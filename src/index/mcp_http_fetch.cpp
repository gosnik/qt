#include "index/mcp_http_fetch.hpp"

#include "index/string_util.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace qt::index {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;
using json = nlohmann::json;

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string target = "/";
    bool https = false;
};

struct FetchResponse {
    unsigned int status = 0;
    std::string body;
    std::string location;
};

bool is_public_address(const net::ip::address& address) {
    if (address.is_unspecified() || address.is_loopback() || address.is_multicast()) {
        return false;
    }
    if (address.is_v4()) {
        const auto bytes = address.to_v4().to_bytes();
        return bytes[0] != 0U && bytes[0] != 10U && bytes[0] != 127U &&
               !(bytes[0] == 100U && bytes[1] >= 64U && bytes[1] <= 127U) &&
               !(bytes[0] == 169U && bytes[1] == 254U) &&
               !(bytes[0] == 172U && bytes[1] >= 16U && bytes[1] <= 31U) &&
               !(bytes[0] == 192U && bytes[1] == 168U) &&
               !(bytes[0] == 198U && (bytes[1] == 18U || bytes[1] == 19U)) &&
               bytes[0] < 224U;
    }
    const auto bytes = address.to_v6().to_bytes();
    const bool unique_local = (bytes[0] & 0xFEU) == 0xFCU;
    const bool link_local = bytes[0] == 0xFEU && (bytes[1] & 0xC0U) == 0x80U;
    const bool v4_mapped =
        std::all_of(bytes.begin(), bytes.begin() + 10, [](unsigned char byte) {
            return byte == 0U;
        }) &&
        bytes[10] == 0xFFU && bytes[11] == 0xFFU;
    if (unique_local || link_local) {
        return false;
    }
    if (v4_mapped) {
        const std::array<unsigned char, 4> v4{{bytes[12], bytes[13], bytes[14],
                                               bytes[15]}};
        return v4[0] != 0U && v4[0] != 10U && v4[0] != 127U &&
               !(v4[0] == 169U && v4[1] == 254U) &&
               !(v4[0] == 172U && v4[1] >= 16U && v4[1] <= 31U) &&
               !(v4[0] == 192U && v4[1] == 168U) && v4[0] < 224U;
    }
    return true;
}

template <typename Stream>
http::response<http::string_body> read_fetch_string_response(
    Stream& stream, beast::flat_buffer& buffer, std::size_t body_limit) {
    http::response_parser<http::string_body> parser;
    parser.body_limit(static_cast<std::uint64_t>(body_limit));
    http::read(stream, buffer, parser);
    return parser.release();
}

std::size_t query_size(const HttpRequest& request, const std::string& key, std::size_t fallback) {
    const auto found = request.query.find(key);
    if (found == request.query.end() || found->second.empty()) {
        return fallback;
    }
    return static_cast<std::size_t>(std::strtoull(found->second.c_str(), nullptr, 10));
}

bool query_bool(const HttpRequest& request, const std::string& key, bool fallback) {
    const auto found = request.query.find(key);
    if (found == request.query.end() || found->second.empty()) {
        return fallback;
    }
    return found->second != "0" && found->second != "false";
}

std::string query_value(const HttpRequest& request, const std::string& key, const std::string& fallback = {}) {
    const auto found = request.query.find(key);
    return found == request.query.end() || found->second.empty() ? fallback : found->second;
}

std::string html_entity_decode(std::string text) {
    const std::vector<std::pair<std::string, std::string>> entities{{"&amp;", "&"},
                                                                    {"&lt;", "<"},
                                                                    {"&gt;", ">"},
                                                                    {"&quot;", "\""},
                                                                    {"&#39;", "'"},
                                                                    {"&nbsp;", " "}};
    for (const auto& [from, to] : entities) {
        std::size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    return text;
}

std::string strip_html_for_fetch(std::string html) {
    html = std::regex_replace(html, std::regex(R"(<script\b[^>]*>.*?</script>)", std::regex::icase), " ");
    html = std::regex_replace(html, std::regex(R"(<style\b[^>]*>.*?</style>)", std::regex::icase), " ");
    html = std::regex_replace(html, std::regex(R"(<\s*(br|p|div|section|article|tr|li|h[1-6])\b[^>]*>)", std::regex::icase), "\n");
    html = std::regex_replace(html, std::regex(R"(</\s*(p|div|section|article|tr|li|h[1-6])\s*>)", std::regex::icase), "\n");
    html = std::regex_replace(html, std::regex(R"(<[^>]+>)"), " ");
    html = html_entity_decode(std::move(html));

    std::stringstream input(html);
    std::string output;
    std::string line;
    bool previous_blank = false;
    while (std::getline(input, line)) {
        line = trim_ascii(std::regex_replace(line, std::regex(R"(\s+)"), " "));
        if (line.empty()) {
            if (!previous_blank && !output.empty()) {
                output.push_back('\n');
                previous_blank = true;
            }
            continue;
        }
        if (!output.empty() && output.back() != '\n') {
            output.push_back('\n');
        }
        output += line;
        previous_blank = false;
    }
    return output;
}

ParsedUrl parse_fetch_url(const std::string& url) {
    ParsedUrl parsed;
    if (url.rfind("https://", 0) == 0) {
        parsed.scheme = "https";
        parsed.port = "443";
        parsed.https = true;
    } else if (url.rfind("http://", 0) == 0) {
        parsed.scheme = "http";
        parsed.port = "80";
    } else {
        throw std::invalid_argument("fetch url must use http:// or https://");
    }
    const auto authority_start = parsed.scheme.size() + 3U;
    const auto slash = url.find('/', authority_start);
    const std::string authority = slash == std::string::npos
                                      ? url.substr(authority_start)
                                      : url.substr(authority_start, slash - authority_start);
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos && colon + 1U < authority.size()) {
        parsed.host = authority.substr(0, colon);
        parsed.port = authority.substr(colon + 1U);
    } else {
        parsed.host = authority;
    }
    parsed.target = slash == std::string::npos ? "/" : url.substr(slash);
    if (parsed.host.empty()) {
        throw std::invalid_argument("fetch url is missing a host");
    }
    return parsed;
}

std::string resolve_location(const std::string& base, const std::string& location) {
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return location;
    }
    const auto parsed = parse_fetch_url(base);
    const std::string origin = parsed.scheme + "://" + parsed.host +
                               ((parsed.https && parsed.port == "443") ||
                                        (!parsed.https && parsed.port == "80")
                                    ? std::string()
                                    : ":" + parsed.port);
    if (!location.empty() && location.front() == '/') {
        return origin + location;
    }
    const auto slash = parsed.target.find_last_of('/');
    const std::string directory = slash == std::string::npos
                                      ? "/"
                                      : parsed.target.substr(0, slash + 1U);
    return origin + directory + location;
}

FetchResponse beast_fetch_once(const std::string& url,
                               const std::string& user_agent,
                               std::chrono::seconds timeout,
                               std::size_t body_limit,
                               bool public_only) {
    const auto parsed = parse_fetch_url(url);
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    const auto results = resolver.resolve(parsed.host, parsed.port);
    if (public_only &&
        std::any_of(results.begin(), results.end(), [](const auto& entry) {
            return !is_public_address(entry.endpoint().address());
        })) {
        throw std::invalid_argument(
            "fetch refused a private, local, or reserved network address");
    }
    http::request<http::empty_body> request{http::verb::get, parsed.target, 11};
    request.set(http::field::host, parsed.host);
    request.set(http::field::user_agent, user_agent);
    request.set(http::field::accept, "*/*");

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    if (parsed.https) {
        ssl::context ctx(ssl::context::tls_client);
        ctx.set_default_verify_paths();
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed.host.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            throw std::runtime_error("configure fetch TLS SNI: " + ec.message());
        }
        stream.set_verify_mode(ssl::verify_peer);
        stream.set_verify_callback(ssl::host_name_verification(parsed.host));
        beast::get_lowest_layer(stream).expires_after(timeout);
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);
        http::write(stream, request);
        response = read_fetch_string_response(stream, buffer, body_limit);
        beast::error_code shutdown_error;
        stream.shutdown(shutdown_error);
        if (shutdown_error == net::error::eof || shutdown_error == ssl::error::stream_truncated) {
            shutdown_error = {};
        }
        if (shutdown_error) {
            throw std::runtime_error("shutdown fetch TLS stream: " + shutdown_error.message());
        }
    } else {
        beast::tcp_stream stream(ioc);
        stream.expires_after(timeout);
        stream.connect(results);
        http::write(stream, request);
        response = read_fetch_string_response(stream, buffer, body_limit);
        beast::error_code shutdown_error;
        stream.socket().shutdown(tcp::socket::shutdown_both, shutdown_error);
        if (shutdown_error == net::error::not_connected) {
            shutdown_error = {};
        }
        if (shutdown_error) {
            throw std::runtime_error("shutdown fetch HTTP stream: " + shutdown_error.message());
        }
    }
    FetchResponse result;
    result.status = response.result_int();
    result.body = response.body();
    if (response.base().find(http::field::location) != response.base().end()) {
        result.location = std::string(response.base()[http::field::location]);
    }
    return result;
}

FetchResponse beast_fetch_following_redirects(const std::string& initial_url,
                                              const std::string& user_agent,
                                              std::chrono::seconds timeout,
                                              std::size_t body_limit,
                                              bool public_only) {
    std::string url = initial_url;
    for (int redirect = 0; redirect < 6; ++redirect) {
        auto response =
            beast_fetch_once(url, user_agent, timeout, body_limit, public_only);
        if ((response.status == 301 || response.status == 302 || response.status == 303 ||
             response.status == 307 || response.status == 308) &&
            !response.location.empty()) {
            url = resolve_location(url, response.location);
            continue;
        }
        return response;
    }
    throw std::runtime_error("fetch exceeded redirect limit");
}

}  // namespace

json fetch_payload(const HttpRequest& request) {
    const auto url = query_value(request, "url");
    if (url.empty()) {
        throw std::invalid_argument("fetch requires url");
    }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        throw std::invalid_argument("fetch url must use http:// or https://");
    }
    const auto max_length = query_size(request, "max_length", 5000);
    const auto start_index = query_size(request, "start_index", 0);
    const bool raw = query_bool(request, "raw", false);
    const auto timeout = std::min<std::size_t>(query_size(request, "timeout_seconds", 20), 120);
    const auto body_limit = std::clamp<std::size_t>(
        query_size(request, "max_response_bytes", 2U * 1024U * 1024U),
        1024U, 8U * 1024U * 1024U);
    const bool public_only = query_bool(request, "public_only", false);
    const auto user_agent = query_value(request,
                                        "user_agent",
                                        "ModelContextProtocol/1.0 (Autonomous; +https://github.com/modelcontextprotocol/servers)");

    const auto response = beast_fetch_following_redirects(
        url, user_agent, std::chrono::seconds(static_cast<int>(timeout)),
        body_limit, public_only);
    auto content = response.body;
    if (!raw) {
        content = strip_html_for_fetch(std::move(content));
    }
    const auto total_length = content.size();
    const auto start = std::min(start_index, total_length);
    auto sliced = content.substr(start);
    bool truncated = false;
    if (max_length != 0 && sliced.size() > max_length) {
        sliced.resize(max_length);
        truncated = true;
    }
    return {{"url", url},
            {"content", sliced},
            {"raw", raw},
            {"start_index", start},
            {"max_length", max_length},
            {"max_response_bytes", body_limit},
            {"public_only", public_only},
            {"total_length", total_length},
            {"truncated", truncated},
            {"next_start_index", start + sliced.size()},
            {"status", response.status},
            {"transport", "boost.beast"},
            {"query", request.query}};
}

}  // namespace qt::index
