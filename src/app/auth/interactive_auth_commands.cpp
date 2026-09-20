#include "qt/app/auth/interactive_auth_commands.hpp"

#include "qt/ai/auth_storage.hpp"
#include "qt/app/auth/auth_flow.hpp"
#include "qt/tui/widgets/selectors/login_dialog.hpp"
#include "qt/tui/widgets/selectors/select.hpp"

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
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace qt::app {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

std::string lowercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string trim_ascii_copy(std::string value) {
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), [](char ch) {
                return std::isspace(static_cast<unsigned char>(ch)) == 0;
              }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [](char ch) {
                return std::isspace(static_cast<unsigned char>(ch)) == 0;
              }).base(),
              value.end());
  return value;
}

std::uint32_t rotr32(std::uint32_t value, unsigned int bits) {
  return (value >> bits) | (value << (32U - bits));
}

std::array<std::uint8_t, 32> sha256_bytes(std::string_view input) {
  constexpr std::array<std::uint32_t, 64> k = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
      0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
      0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
      0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
      0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
      0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
      0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
      0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  std::uint32_t h0 = 0x6a09e667U;
  std::uint32_t h1 = 0xbb67ae85U;
  std::uint32_t h2 = 0x3c6ef372U;
  std::uint32_t h3 = 0xa54ff53aU;
  std::uint32_t h4 = 0x510e527fU;
  std::uint32_t h5 = 0x9b05688cU;
  std::uint32_t h6 = 0x1f83d9abU;
  std::uint32_t h7 = 0x5be0cd19U;

  std::vector<std::uint8_t> message(input.begin(), input.end());
  const std::uint64_t bit_length =
      static_cast<std::uint64_t>(message.size()) * 8ULL;
  message.push_back(0x80U);
  while ((message.size() % 64U) != 56U) {
    message.push_back(0U);
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<std::uint8_t>(bit_length >> shift));
  }

  for (std::size_t offset = 0; offset < message.size(); offset += 64U) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16U; ++i) {
      const std::size_t j = offset + (i * 4U);
      w[i] = (static_cast<std::uint32_t>(message[j]) << 24U) |
             (static_cast<std::uint32_t>(message[j + 1U]) << 16U) |
             (static_cast<std::uint32_t>(message[j + 2U]) << 8U) |
             static_cast<std::uint32_t>(message[j + 3U]);
    }
    for (std::size_t i = 16U; i < 64U; ++i) {
      const std::uint32_t s0 =
          rotr32(w[i - 15U], 7U) ^ rotr32(w[i - 15U], 18U) ^
          (w[i - 15U] >> 3U);
      const std::uint32_t s1 =
          rotr32(w[i - 2U], 17U) ^ rotr32(w[i - 2U], 19U) ^
          (w[i - 2U] >> 10U);
      w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }

    std::uint32_t a = h0;
    std::uint32_t b = h1;
    std::uint32_t c = h2;
    std::uint32_t d = h3;
    std::uint32_t e = h4;
    std::uint32_t f = h5;
    std::uint32_t g = h6;
    std::uint32_t h = h7;

    for (std::size_t i = 0; i < 64U; ++i) {
      const std::uint32_t s1 =
          rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
      const std::uint32_t ch = (e & f) ^ ((~e) & g);
      const std::uint32_t temp1 = h + s1 + ch + k[i] + w[i];
      const std::uint32_t s0 =
          rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
    h5 += f;
    h6 += g;
    h7 += h;
  }

  const std::array<std::uint32_t, 8> words = {h0, h1, h2, h3,
                                              h4, h5, h6, h7};
  std::array<std::uint8_t, 32> digest{};
  for (std::size_t i = 0; i < words.size(); ++i) {
    digest[i * 4U] = static_cast<std::uint8_t>(words[i] >> 24U);
    digest[(i * 4U) + 1U] = static_cast<std::uint8_t>(words[i] >> 16U);
    digest[(i * 4U) + 2U] = static_cast<std::uint8_t>(words[i] >> 8U);
    digest[(i * 4U) + 3U] = static_cast<std::uint8_t>(words[i]);
  }
  return digest;
}

std::vector<std::uint8_t> random_bytes(std::size_t count) {
  std::vector<std::uint8_t> bytes(count);
  std::random_device device;
  for (auto& byte : bytes) {
    byte = static_cast<std::uint8_t>(device() & 0xffU);
  }
  return bytes;
}

std::string base64url_encode(const std::vector<std::uint8_t>& bytes) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string encoded;
  encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);
  std::uint32_t buffer = 0U;
  int bits = 0;
  for (const auto byte : bytes) {
    buffer = (buffer << 8U) | byte;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      encoded.push_back(alphabet[(buffer >> bits) & 0x3fU]);
    }
  }
  if (bits > 0) {
    encoded.push_back(alphabet[(buffer << (6 - bits)) & 0x3fU]);
  }
  return encoded;
}

std::string base64url_encode(const std::array<std::uint8_t, 32>& bytes) {
  return base64url_encode(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

std::string lowercase_hex_encode(const std::vector<std::uint8_t>& bytes) {
  static constexpr char alphabet[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(bytes.size() * 2U);
  for (const auto byte : bytes) {
    encoded.push_back(alphabet[(byte >> 4U) & 0x0fU]);
    encoded.push_back(alphabet[byte & 0x0fU]);
  }
  return encoded;
}

struct OAuthPkceMaterial {
  std::string verifier;
  std::string challenge;
  std::string state;
};

OAuthPkceMaterial make_oauth_pkce_material() {
  OAuthPkceMaterial material;
  material.verifier = base64url_encode(random_bytes(32U));
  material.challenge = base64url_encode(sha256_bytes(material.verifier));
  material.state = lowercase_hex_encode(random_bytes(16U));
  return material;
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string percent_encode_form(const std::string& value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  for (const unsigned char ch : value) {
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
        ch == '~') {
      encoded.push_back(static_cast<char>(ch));
    } else {
      encoded.push_back('%');
      encoded.push_back(hex[(ch >> 4U) & 0x0fU]);
      encoded.push_back(hex[ch & 0x0fU]);
    }
  }
  return encoded;
}

std::string form_body(
    const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string body;
  for (const auto& field : fields) {
    if (!body.empty()) {
      body.push_back('&');
    }
    body += percent_encode_form(field.first);
    body.push_back('=');
    body += percent_encode_form(field.second);
  }
  return body;
}

std::string json_string_escape(const std::string& value) {
  std::string escaped;
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (ch < 0x20U) {
          static constexpr char hex[] = "0123456789abcdef";
          escaped += "\\u00";
          escaped.push_back(hex[(ch >> 4U) & 0x0fU]);
          escaped.push_back(hex[ch & 0x0fU]);
        } else {
          escaped.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  return escaped;
}

std::string oauth_token_request_body(
    const OAuthAuthorizationPlan& plan,
    const OAuthPkceMaterial& pkce,
    const OAuthAuthorizationCode& authorization) {
  const std::string provider = lowercase_ascii(plan.provider);
  if (provider == "anthropic") {
    return std::string("{\"grant_type\":\"authorization_code\",") +
           "\"client_id\":\"9d1c250a-e61b-44d9-88ed-594d1962f5e\"," +
           "\"code\":\"" + json_string_escape(authorization.code) + "\"," +
           "\"state\":\"" +
           json_string_escape(authorization.state.value_or(pkce.verifier)) +
           "\"," +
           "\"redirect_uri\":\"" + json_string_escape(plan.redirect_uri) +
           "\"," +
           "\"code_verifier\":\"" + json_string_escape(pkce.verifier) + "\"}";
  }

  return form_body({{"grant_type", "authorization_code"},
                    {"client_id", "app_EMoamEEZ73f0CkXaXp7hrann"},
                    {"code", authorization.code},
                    {"code_verifier", pkce.verifier},
                    {"redirect_uri", plan.redirect_uri}});
}

std::string oauth_token_content_type(const OAuthAuthorizationPlan& plan) {
  return lowercase_ascii(plan.provider) == "anthropic"
             ? "application/json"
             : "application/x-www-form-urlencoded";
}

struct ParsedHttpsUrl {
  std::string host;
  std::string port = "443";
  std::string target = "/";
};

foundation::Result<ParsedHttpsUrl> parse_https_url(const std::string& url) {
  constexpr std::string_view scheme = "https://";
  if (url.rfind(std::string(scheme), 0) != 0) {
    return foundation::Result<ParsedHttpsUrl>::failure(
        foundation::ErrorCode::invalid_argument,
        "OAuth token URL must use https: " + url);
  }
  const std::string without_scheme = url.substr(scheme.size());
  const auto slash = without_scheme.find('/');
  const std::string authority =
      slash == std::string::npos ? without_scheme : without_scheme.substr(0, slash);
  if (authority.empty()) {
    return foundation::Result<ParsedHttpsUrl>::failure(
        foundation::ErrorCode::invalid_argument,
        "OAuth token URL is missing a host: " + url);
  }

  ParsedHttpsUrl parsed;
  parsed.target = slash == std::string::npos ? "/" : without_scheme.substr(slash);
  const auto colon = authority.rfind(':');
  if (colon != std::string::npos && colon + 1U < authority.size()) {
    parsed.host = authority.substr(0, colon);
    parsed.port = authority.substr(colon + 1U);
  } else {
    parsed.host = authority;
  }
  if (parsed.host.empty() || parsed.port.empty()) {
    return foundation::Result<ParsedHttpsUrl>::failure(
        foundation::ErrorCode::invalid_argument,
        "OAuth token URL has an invalid authority: " + url);
  }
  return parsed;
}

struct BeastHttpResponse {
  unsigned int status = 0;
  std::string body;
};

template <typename Stream>
http::response<http::string_body> read_auth_string_response(
    Stream& stream, beast::flat_buffer& buffer) {
  http::response_parser<http::string_body> parser;
  parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
  http::read(stream, buffer, parser);
  return parser.release();
}

foundation::Result<BeastHttpResponse> beast_https_post(
    const std::string& url,
    const std::string& content_type,
    const std::string& body,
    std::chrono::seconds timeout = std::chrono::seconds(30)) {
  const auto parsed = parse_https_url(url);
  if (!parsed) {
    return foundation::Result<BeastHttpResponse>::failure(
        parsed.error().code, parsed.error().message, parsed.error().system_error);
  }

  try {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tls_client);
    ctx.set_default_verify_paths();

    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
    if (!SSL_set_tlsext_host_name(stream.native_handle(),
                                  parsed.value().host.c_str())) {
      beast::error_code ec{
          static_cast<int>(::ERR_get_error()),
          net::error::get_ssl_category()};
      return foundation::Result<BeastHttpResponse>::failure(
          foundation::ErrorCode::io_error,
          "configure OAuth TLS SNI: " + ec.message());
    }
    stream.set_verify_mode(ssl::verify_peer);
    stream.set_verify_callback(
        ssl::host_name_verification(parsed.value().host));

    tcp::resolver resolver(ioc);
    auto const results =
        resolver.resolve(parsed.value().host, parsed.value().port);
    beast::get_lowest_layer(stream).expires_after(timeout);
    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> request{
        http::verb::post, parsed.value().target, 11};
    request.set(http::field::host, parsed.value().host);
    request.set(http::field::user_agent,
                std::string("qt/") + BOOST_BEAST_VERSION_STRING);
    request.set(http::field::accept, "application/json");
    request.set(http::field::content_type, content_type);
    request.body() = body;
    request.prepare_payload();

    http::write(stream, request);

    beast::flat_buffer buffer;
    auto response = read_auth_string_response(stream, buffer);

    beast::error_code shutdown_error;
    stream.shutdown(shutdown_error);
    if (shutdown_error == net::error::eof ||
        shutdown_error == ssl::error::stream_truncated) {
      shutdown_error = {};
    }
    if (shutdown_error) {
      return foundation::Result<BeastHttpResponse>::failure(
          foundation::ErrorCode::io_error,
          "shutdown OAuth TLS stream: " + shutdown_error.message());
    }

    BeastHttpResponse result;
    result.status = response.result_int();
    result.body = response.body();
    return result;
  } catch (const std::exception& ex) {
    return foundation::Result<BeastHttpResponse>::failure(
        foundation::ErrorCode::io_error,
        std::string("OAuth token HTTPS request failed: ") + ex.what());
  }
}

class SocketHandle {
 public:
  explicit SocketHandle(int fd = -1) : fd_(fd) {}
  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;
  SocketHandle(SocketHandle&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }
  SocketHandle& operator=(SocketHandle&& other) noexcept {
    if (this != &other) {
      close();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  ~SocketHandle() { close(); }

  int get() const noexcept { return fd_; }
  explicit operator bool() const noexcept { return fd_ >= 0; }

  void close() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_ = -1;
};

std::string read_http_request(int fd) {
  std::string request;
  std::array<char, 4096> buffer{};
  while (request.find("\r\n\r\n") == std::string::npos &&
         request.find("\n\n") == std::string::npos &&
         request.size() < 32768U) {
    const ssize_t read_count = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (read_count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("read OAuth callback request: ") +
                               std::strerror(errno));
    }
    if (read_count == 0) {
      break;
    }
    request.append(buffer.data(), static_cast<std::size_t>(read_count));
  }
  return request;
}

std::string http_request_line(const std::string& request) {
  const auto end = request.find('\n');
  std::string line = end == std::string::npos ? request : request.substr(0, end);
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

void write_all(int fd, std::string_view text) {
  while (!text.empty()) {
    const ssize_t written = ::send(fd, text.data(), text.size(), 0);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("write OAuth callback response: ") +
                               std::strerror(errno));
    }
    text.remove_prefix(static_cast<std::size_t>(written));
  }
}

foundation::Result<OAuthCallbackResponsePlan> wait_for_oauth_callback(
    const OAuthAuthorizationPlan& plan,
    const std::string& expected_state,
    std::chrono::seconds timeout = std::chrono::seconds(180)) {
  SocketHandle server(::socket(AF_INET, SOCK_STREAM, 0));
  if (!server) {
    return foundation::Result<OAuthCallbackResponsePlan>::failure(
        foundation::ErrorCode::io_error,
        std::string("create OAuth callback socket: ") + std::strerror(errno));
  }

  int reuse = 1;
  (void)::setsockopt(server.get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(plan.callback_port);
  if (::inet_pton(AF_INET, plan.callback_host.c_str(), &address.sin_addr) != 1) {
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  }

  if (::bind(server.get(), reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0) {
    return foundation::Result<OAuthCallbackResponsePlan>::failure(
        foundation::ErrorCode::io_error,
        std::string("bind OAuth callback server on ") + plan.callback_host +
            ":" + std::to_string(plan.callback_port) + ": " +
            std::strerror(errno));
  }
  if (::listen(server.get(), 1) != 0) {
    return foundation::Result<OAuthCallbackResponsePlan>::failure(
        foundation::ErrorCode::io_error,
        std::string("listen for OAuth callback: ") + std::strerror(errno));
  }

  fd_set read_fds;
  FD_ZERO(&read_fds);
  FD_SET(server.get(), &read_fds);
  timeval wait{};
  wait.tv_sec = timeout.count();
  wait.tv_usec = 0;
  const int ready = ::select(server.get() + 1, &read_fds, nullptr, nullptr, &wait);
  if (ready < 0) {
    return foundation::Result<OAuthCallbackResponsePlan>::failure(
        foundation::ErrorCode::io_error,
        std::string("wait for OAuth callback: ") + std::strerror(errno));
  }
  if (ready == 0) {
    return foundation::Result<OAuthCallbackResponsePlan>::failure(
        foundation::ErrorCode::io_error,
        "OAuth callback timed out waiting for browser redirect");
  }

  SocketHandle client(::accept(server.get(), nullptr, nullptr));
  if (!client) {
    return foundation::Result<OAuthCallbackResponsePlan>::failure(
        foundation::ErrorCode::io_error,
        std::string("accept OAuth callback: ") + std::strerror(errno));
  }

  OAuthCallbackHttpRequest request;
  request.provider = plan.provider;
  request.expected_state = expected_state;
  request.request_line = http_request_line(read_http_request(client.get()));
  auto response = plan_oauth_callback_http_response(request);
  if (!response) {
    return foundation::Result<OAuthCallbackResponsePlan>::failure(
        response.error().code, response.error().message,
        response.error().system_error);
  }

  try {
    write_all(client.get(), response.value().raw_response);
  } catch (const std::exception& ex) {
    return foundation::Result<OAuthCallbackResponsePlan>::failure(
        foundation::ErrorCode::io_error, ex.what());
  }
  if (!response.value().callback.completed ||
      !response.value().callback.authorization) {
    return foundation::Result<OAuthCallbackResponsePlan>::failure(
        foundation::ErrorCode::io_error,
        response.value().callback.message.empty()
            ? "OAuth callback did not complete"
            : response.value().callback.message);
  }
  return foundation::Result<OAuthCallbackResponsePlan>::success(
      response.value().callback);
}

foundation::Result<qt::ai::ProviderAuthRecord> exchange_oauth_code(
    const OAuthAuthorizationPlan& plan,
    const OAuthPkceMaterial& pkce,
    const OAuthAuthorizationCode& authorization) {
  const auto response = beast_https_post(
      plan.token_url, oauth_token_content_type(plan),
      oauth_token_request_body(plan, pkce, authorization));
  if (!response) {
    return foundation::Result<qt::ai::ProviderAuthRecord>::failure(
        response.error().code, response.error().message,
        response.error().system_error);
  }

  if (response.value().status < 200 || response.value().status >= 300) {
    return foundation::Result<qt::ai::ProviderAuthRecord>::failure(
        foundation::ErrorCode::io_error,
        "OAuth token exchange failed (" +
            std::to_string(response.value().status) + "): " +
            response.value().body);
  }

  qt::ai::Json body;
  try {
    body = qt::ai::Json::parse(response.value().body);
  } catch (const std::exception& ex) {
    return foundation::Result<qt::ai::ProviderAuthRecord>::failure(
        foundation::ErrorCode::io_error,
        std::string("OAuth token exchange returned invalid JSON: ") + ex.what());
  }

  OAuthTokenResponseRequest parse_request;
  parse_request.provider = plan.provider;
  parse_request.response = std::move(body);
  parse_request.now_ms = now_ms();
  auto parsed = parse_oauth_token_response(parse_request);
  if (!parsed) {
    return foundation::Result<qt::ai::ProviderAuthRecord>::failure(
        parsed.error().code, parsed.error().message,
        parsed.error().system_error);
  }
  return parsed;
}

std::string auth_status_description(
    const AuthSelectorProvider& provider,
    const InteractiveAuthCommandContext& context) {
  const auto status =
      describe_auth_provider_status(context.services.auth_storage, provider.id);
  const auto* stored =
      qt::ai::find_provider_auth_record(context.services.auth_storage,
                                        provider.id);
  if (stored != nullptr) {
    const bool matching = (provider.auth_type == AuthSelectorAuthType::oauth &&
                           stored->token.has_value()) ||
                          (provider.auth_type == AuthSelectorAuthType::api_key &&
                           stored->api_key.has_value());
    if (matching) {
      return provider.auth_type == AuthSelectorAuthType::oauth
                 ? "subscription configured"
                 : "API key configured";
    }
    return stored->token ? "subscription configured" : "API key configured";
  }

  if (provider.auth_type == AuthSelectorAuthType::oauth) {
    return "unconfigured";
  }

  switch (status.source) {
    case AuthProviderStatusSource::environment_api_key:
      return "env: " + status.label;
    case AuthProviderStatusSource::models_json_key:
      return "key in models.json";
    case AuthProviderStatusSource::models_json_command:
      return "command in models.json";
    case AuthProviderStatusSource::auth_storage_api_key:
      return "API key configured";
    case AuthProviderStatusSource::auth_storage_token:
      return "subscription configured";
    case AuthProviderStatusSource::none:
      break;
  }
  return "unconfigured";
}

std::vector<qt::tui::SelectItem> provider_select_items(
    const std::vector<AuthSelectorProvider>& providers,
    const InteractiveAuthCommandContext& context) {
  std::vector<qt::tui::SelectItem> items;
  items.reserve(providers.size());
  for (const auto& provider : providers) {
    items.push_back(qt::tui::SelectItem{
        provider.id, provider.name, auth_status_description(provider, context)});
  }
  return items;
}

foundation::Result<InteractiveSlashCommandResponse> handle_login_command(
    InteractiveTerminalSession& ui, const InteractiveShellEvent& event,
    InteractiveAuthCommandContext& context) {
  InteractiveSlashCommandResponse response;
  response.handled = true;
  response.append_default_output = false;

  const auto providers_for =
      [&](std::optional<AuthSelectorAuthType> auth_type) {
        AuthLoginSelectorRequest request;
        request.catalog = context.services.model_catalog;
        request.oauth_providers = built_in_oauth_provider_options();
        request.auth_type = auth_type;
        return build_auth_login_provider_options(request);
      };

  auto show_bedrock_setup =
      std::make_shared<std::function<void(AuthSelectorProvider)>>();
  *show_bedrock_setup = [&ui](AuthSelectorProvider provider) {
    auto dialog = std::make_shared<qt::tui::LoginDialogComponent>(
        provider.id, provider.name,
        [&ui](bool, std::string) { ui.hide_overlay(); },
        "Configure " + provider.name);
    dialog->show_info(
        {"Amazon Bedrock uses AWS credentials from your environment or AWS profile.",
         "Set AWS_PROFILE, AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY, or AWS_BEARER_TOKEN_BEDROCK.",
         "No API key is stored for Amazon Bedrock."});
    ui.show_overlay(dialog);
  };

  auto show_oauth_setup =
      std::make_shared<std::function<void(AuthSelectorProvider)>>();
  *show_oauth_setup = [&ui, &context](AuthSelectorProvider provider) {
    const auto pkce = make_oauth_pkce_material();
    OAuthAuthorizationPlanRequest plan_request;
    plan_request.provider = provider.id;
    plan_request.verifier = pkce.verifier;
    plan_request.challenge = pkce.challenge;
    plan_request.state = pkce.state;
    const auto plan = plan_oauth_authorization(plan_request);
    auto dialog = std::make_shared<qt::tui::LoginDialogComponent>(
        provider.id, provider.name,
        [&ui](bool, std::string) { ui.hide_overlay(); });
    if (!plan) {
      dialog->show_info({"OAuth login for " + provider.name +
                         " is not available in this build.",
                         plan.error().message});
      ui.show_overlay(dialog);
      return;
    }
    if (!plan.value().uses_callback_server) {
      dialog->show_auth(
          plan.value().authorize_url,
          "Subscription login for " + provider.name +
              " uses a device-code flow that is not wired into this dialog yet.");
      ui.show_overlay(dialog);
      return;
    }
    if (!context.post_ui) {
      dialog->show_info({"OAuth login for " + provider.name +
                         " cannot start because the UI dispatcher is unavailable."});
      ui.show_overlay(dialog);
      return;
    }
    const OAuthAuthorizationPlan plan_value = plan.value();
    const std::string provider_name = provider.name;
    dialog->show_auth(
        plan_value.authorize_url,
        "Subscription login for " + provider.name +
            " requires the OAuth browser flow. Waiting for browser callback.");
    ui.show_overlay(dialog);
    auto post_ui = context.post_ui;
    auto* context_ptr = &context;
    std::thread([plan_value, pkce, provider_name, post_ui, context_ptr]() {
      const auto callback = wait_for_oauth_callback(plan_value, pkce.state);
      if (!callback) {
        post_ui([message = callback.error().message](
                    InteractiveTerminalSession& ui) {
          ui.append_transcript_line("error: " + message);
        });
        return;
      }
      const auto token = exchange_oauth_code(
          plan_value, pkce, *callback.value().authorization);
      if (!token) {
        post_ui([message = token.error().message](
                    InteractiveTerminalSession& ui) {
          ui.append_transcript_line("error: " + message);
        });
        return;
      }
      post_ui([context_ptr, provider_name, record = token.value()](
                  InteractiveTerminalSession& ui) mutable {
        context_ptr->services.auth_storage =
            qt::ai::upsert_provider_auth_record(
                context_ptr->services.auth_storage, std::move(record));
        const auto saved = qt::ai::save_auth_storage(
            context_ptr->services.auth_storage_path,
            context_ptr->services.auth_storage);
        if (!saved) {
          ui.append_transcript_line("error: " + saved.error().message);
          return;
        }
        ui.hide_overlay();
        const auto status = context_ptr->reload_runtime(
            ui, "Saved subscription login for " + provider_name +
                    ". Credentials saved to " +
                    context_ptr->services.auth_storage_path);
        if (!status) {
          ui.append_transcript_line("error: " + status.error().message);
        }
      });
    }).detach();
  };

  auto begin_login =
      std::make_shared<std::function<void(AuthSelectorProvider)>>();
  *begin_login = [&ui, &context, show_bedrock_setup, show_oauth_setup](
                     AuthSelectorProvider provider) {
    const std::string provider_id = provider.id;
    const std::string provider_name = provider.name;
    if (provider.auth_type == AuthSelectorAuthType::oauth) {
      (*show_oauth_setup)(std::move(provider));
      return;
    }
    if (lowercase_ascii(provider_id) == "amazon-bedrock") {
      (*show_bedrock_setup)(std::move(provider));
      return;
    }
    context.prompt_for_secret(
        ui, "Login to " + provider_name, "Enter API key:", "",
        [&, provider_id, provider_name](const std::string& value) {
          const std::string secret = trim_ascii_copy(value);
          if (secret.empty()) {
            ui.append_transcript_line("error: API key cannot be empty");
            return;
          }
          qt::ai::ProviderAuthRecord record;
          record.provider = provider_id;
          record.api_key = secret;
          context.services.auth_storage =
              qt::ai::upsert_provider_auth_record(context.services.auth_storage,
                                                  record);
          const auto saved = qt::ai::save_auth_storage(
              context.services.auth_storage_path, context.services.auth_storage);
          if (!saved) {
            ui.append_transcript_line("error: " + saved.error().message);
            return;
          }
          const auto status = context.reload_runtime(
              ui, "Saved API key for " + provider_name +
                      ". Credentials saved to " +
                      context.services.auth_storage_path);
          if (!status) {
            ui.append_transcript_line("error: " + status.error().message);
          }
        });
  };

  auto show_auth_type_selector = std::make_shared<std::function<void()>>();
  auto show_provider_selector =
      std::make_shared<std::function<void(AuthSelectorAuthType)>>();

  *show_provider_selector = [&ui, &context, providers_for, begin_login,
                             show_auth_type_selector](
                                AuthSelectorAuthType auth_type) {
    const auto providers = providers_for(auth_type);
    if (providers.empty()) {
      ui.append_transcript_lines({
          auth_type == AuthSelectorAuthType::oauth
              ? "No subscription providers available."
              : "No API key providers available."});
      return;
    }

    auto provider_options =
        std::make_shared<std::vector<AuthSelectorProvider>>(providers);
    auto select = std::make_shared<qt::tui::Select>(
        std::optional<std::string>{
            auth_type == AuthSelectorAuthType::oauth
                ? "Select provider to configure:"
                : "Select provider to configure:"},
        provider_select_items(providers, context), 0, 8U);
    select->set_select_callback([&ui, provider_options, begin_login](
                                    const qt::tui::SelectItem& item) {
      ui.hide_overlay();
      const auto it = std::find_if(
          provider_options->begin(), provider_options->end(),
          [&](const AuthSelectorProvider& provider) {
            return provider.id == item.value;
          });
      if (it == provider_options->end()) {
        ui.append_transcript_line("error: provider not found: " + item.value);
        return;
      }
      (*begin_login)(*it);
    });
    select->set_cancel_callback([&ui, show_auth_type_selector]() {
      ui.hide_overlay();
      if (*show_auth_type_selector) {
        (*show_auth_type_selector)();
      }
    });
    ui.show_overlay(select);
  };

  *show_auth_type_selector = [&ui, show_provider_selector]() {
    std::vector<qt::tui::SelectItem> items{
        qt::tui::SelectItem{"oauth", "Use a subscription",
                            "OAuth subscription providers"},
        qt::tui::SelectItem{"api_key", "Use an API key",
                            "Store a provider API key"}};
    auto select = std::make_shared<qt::tui::Select>(
        std::optional<std::string>{"Select authentication method:"},
        std::move(items), 0, 2U);
    select->set_select_callback([&ui, show_provider_selector](
                                    const qt::tui::SelectItem& item) {
      ui.hide_overlay();
      if (*show_provider_selector) {
        (*show_provider_selector)(item.value == "oauth"
                                      ? AuthSelectorAuthType::oauth
                                      : AuthSelectorAuthType::api_key);
      }
    });
    select->set_cancel_callback([&ui]() { ui.hide_overlay(); });
    ui.show_overlay(select);
  };

  if (!event.arguments.empty()) {
    const auto wanted = lowercase_ascii(trim_ascii_copy(event.arguments));
    const auto providers = providers_for(std::nullopt);
    const auto it = std::find_if(
        providers.begin(), providers.end(),
        [&](const AuthSelectorProvider& provider) {
          return lowercase_ascii(provider.id) == wanted;
        });
    if (it == providers.end()) {
      return foundation::Result<InteractiveSlashCommandResponse>::failure(
          foundation::ErrorCode::not_found,
          "unknown login provider: " + event.arguments);
    }
    (*begin_login)(*it);
    return foundation::Result<InteractiveSlashCommandResponse>::success(
        std::move(response));
  }

  (*show_auth_type_selector)();
  return foundation::Result<InteractiveSlashCommandResponse>::success(
      std::move(response));
}

foundation::Result<InteractiveSlashCommandResponse> handle_logout_command(
    InteractiveTerminalSession& ui, const InteractiveShellEvent& event,
    InteractiveAuthCommandContext& context) {
  InteractiveSlashCommandResponse response;
  response.handled = true;
  response.append_default_output = false;

  AuthLogoutSelectorRequest request;
  request.catalog = context.services.model_catalog;
  request.auth_storage = context.services.auth_storage;
  request.oauth_providers = built_in_oauth_provider_options();
  const auto providers = build_auth_logout_provider_options(request);
  if (providers.empty()) {
    response.transcript_lines = {
        "No stored credentials to remove. /logout only removes credentials "
        "saved by /login; environment variables and models.json config are "
        "unchanged."};
    return foundation::Result<InteractiveSlashCommandResponse>::success(
        std::move(response));
  }

  const auto perform_logout = [&](const AuthSelectorProvider& provider) {
    context.services.auth_storage = qt::ai::remove_provider_auth_record(
        context.services.auth_storage, provider.id);
    const auto saved = qt::ai::save_auth_storage(
        context.services.auth_storage_path, context.services.auth_storage);
    if (!saved) {
      ui.append_transcript_line("error: " + saved.error().message);
      return;
    }
    const std::string message =
        provider.auth_type == AuthSelectorAuthType::oauth
            ? "Logged out of " + provider.name
            : "Removed stored API key for " + provider.name +
                  ". Environment variables and models.json config are unchanged.";
    const auto status = context.reload_runtime(
        ui, message);
    if (!status) {
      ui.append_transcript_line("error: " + status.error().message);
    }
  };

  if (!event.arguments.empty()) {
    const auto wanted = lowercase_ascii(event.arguments);
    const auto it = std::find_if(
        providers.begin(), providers.end(),
        [&](const AuthSelectorProvider& provider) {
          return lowercase_ascii(provider.id) == wanted;
        });
    if (it == providers.end()) {
      return foundation::Result<InteractiveSlashCommandResponse>::failure(
          foundation::ErrorCode::not_found,
          "unknown logout provider: " + event.arguments);
    }
    perform_logout(*it);
    return foundation::Result<InteractiveSlashCommandResponse>::success(
        std::move(response));
  }

  std::vector<qt::tui::SelectItem> items;
  for (const auto& provider : providers) {
    items.push_back(qt::tui::SelectItem{
        provider.id, provider.name,
        provider.auth_type == AuthSelectorAuthType::oauth
            ? "subscription configured"
            : "API key configured"});
  }
  auto provider_options =
      std::make_shared<std::vector<AuthSelectorProvider>>(providers);
  auto select = std::make_shared<qt::tui::Select>(
      std::optional<std::string>{"Select provider to logout:"},
      std::move(items), 0, 10U);
  select->set_select_callback([&ui, provider_options, perform_logout, select](
                                  const qt::tui::SelectItem& item) {
    ui.hide_overlay();
    const auto it = std::find_if(
        provider_options->begin(), provider_options->end(),
        [&](const AuthSelectorProvider& provider) {
          return provider.id == item.value;
        });
    if (it == provider_options->end()) {
      ui.append_transcript_line("error: provider not found: " + item.value);
      return;
    }
    perform_logout(*it);
  });
  select->set_cancel_callback([&ui]() { ui.hide_overlay(); });
  ui.show_overlay(select);
  return foundation::Result<InteractiveSlashCommandResponse>::success(
      std::move(response));
}

}  // namespace

foundation::Result<std::optional<InteractiveSlashCommandResponse>>
try_handle_interactive_auth_command(
    InteractiveTerminalSession& ui, const InteractiveShellEvent& event,
    InteractiveAuthCommandContext& context) {
  if (event.command == "login") {
    const auto handled = handle_login_command(ui, event, context);
    if (!handled) {
      return foundation::Result<std::optional<InteractiveSlashCommandResponse>>::failure(
          handled.error().code, handled.error().message,
          handled.error().system_error);
    }
    return foundation::Result<std::optional<InteractiveSlashCommandResponse>>::success(
        std::optional<InteractiveSlashCommandResponse>(std::move(handled.value())));
  }

  if (event.command == "logout") {
    const auto handled = handle_logout_command(ui, event, context);
    if (!handled) {
      return foundation::Result<std::optional<InteractiveSlashCommandResponse>>::failure(
          handled.error().code, handled.error().message,
          handled.error().system_error);
    }
    return foundation::Result<std::optional<InteractiveSlashCommandResponse>>::success(
        std::optional<InteractiveSlashCommandResponse>(std::move(handled.value())));
  }

  return foundation::Result<std::optional<InteractiveSlashCommandResponse>>::success(
      std::nullopt);
}

}  // namespace qt::app
