#include "qt/tools/mcp_client.hpp"

#include "qt/tools/tool_validation.hpp"

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
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <csignal>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <filesystem>
#include <future>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <type_traits>
#include <thread>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

extern char** environ;

namespace qt::tools {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

using Clock = std::chrono::steady_clock;

struct ParsedHttpUrl {
  std::string scheme;
  std::string host;
  std::string port;
  std::string target = "/";
  bool https = false;
};

struct McpHttpResponse {
  unsigned int status = 0;
  std::map<std::string, std::string> headers;
  std::string body;
};

template <typename Stream>
http::response<http::string_body> read_http_string_response(
    Stream& stream, beast::flat_buffer& buffer) {
  http::response_parser<http::string_body> parser;
  parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
  http::read(stream, buffer, parser);
  return parser.release();
}

struct SseEvent {
  std::optional<std::string> event;
  std::string data;
};

class BackgroundEventLoop {
 public:
  BackgroundEventLoop() : worker_([this] { run(); }) {}
  ~BackgroundEventLoop() { stop(); }

  BackgroundEventLoop(const BackgroundEventLoop&) = delete;
  BackgroundEventLoop& operator=(const BackgroundEventLoop&) = delete;

  template <typename Fn>
  auto submit(Fn&& fn) -> typename std::result_of<Fn()>::type {
    using Result = typename std::result_of<Fn()>::type;
    if (std::this_thread::get_id() == worker_id_) {
      return fn();
    }
    auto task =
        std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
    auto future = task->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back([task] { (*task)(); });
    }
    cv_.notify_one();
    return future.get();
  }

 private:
  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void run() {
    worker_id_ = std::this_thread::get_id();
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_ && queue_.empty()) {
          return;
        }
        task = std::move(queue_.front());
        queue_.pop_front();
      }
      task();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  std::thread worker_;
  std::thread::id worker_id_;
  bool stopping_ = false;
};

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool header_contains(const std::map<std::string, std::string>& headers,
                     std::string_view name) {
  const std::string expected = lower_ascii(std::string(name));
  return std::any_of(headers.begin(), headers.end(), [&](const auto& item) {
    return lower_ascii(item.first) == expected;
  });
}

foundation::Result<ParsedHttpUrl> parse_http_url(const std::string& url) {
  ParsedHttpUrl parsed;
  if (url.rfind("https://", 0) == 0) {
    parsed.scheme = "https";
    parsed.port = "443";
    parsed.https = true;
  } else if (url.rfind("http://", 0) == 0) {
    parsed.scheme = "http";
    parsed.port = "80";
  } else {
    return foundation::Result<ParsedHttpUrl>::failure(
        foundation::ErrorCode::invalid_argument,
        "MCP URL must use http or https: " + url);
  }

  const auto authority_start = parsed.scheme.size() + 3U;
  const auto slash = url.find('/', authority_start);
  const std::string authority =
      slash == std::string::npos ? url.substr(authority_start)
                                 : url.substr(authority_start,
                                              slash - authority_start);
  if (authority.empty()) {
    return foundation::Result<ParsedHttpUrl>::failure(
        foundation::ErrorCode::invalid_argument,
        "MCP URL is missing a host: " + url);
  }

  const auto colon = authority.rfind(':');
  if (colon != std::string::npos && colon + 1U < authority.size()) {
    parsed.host = authority.substr(0, colon);
    parsed.port = authority.substr(colon + 1U);
  } else {
    parsed.host = authority;
  }
  parsed.target = slash == std::string::npos ? "/" : url.substr(slash);
  if (parsed.host.empty()) {
    return foundation::Result<ParsedHttpUrl>::failure(
        foundation::ErrorCode::invalid_argument,
        "MCP URL has an invalid authority: " + url);
  }
  return parsed;
}

void set_mcp_headers(http::request<http::string_body>& request,
                     const ParsedHttpUrl& parsed,
                     const std::map<std::string, std::string>& headers) {
  request.set(http::field::host, parsed.host);
  request.set(http::field::user_agent,
              std::string("qt/") + BOOST_BEAST_VERSION_STRING);
  for (const auto& header : headers) {
    request.set(header.first, header.second);
  }
}

foundation::Result<McpHttpResponse> beast_http_request(
    const std::string& url,
    http::verb verb,
    const std::map<std::string, std::string>& headers,
    const std::string& body,
    int timeout_seconds) {
  const auto parsed_result = parse_http_url(url);
  if (!parsed_result) {
    return foundation::Result<McpHttpResponse>::failure(
        parsed_result.error().code,
        sanitize_mcp_error(parsed_result.error().message),
        parsed_result.error().system_error);
  }
  const ParsedHttpUrl parsed = parsed_result.value();

  try {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    const auto results = resolver.resolve(parsed.host, parsed.port);
    const auto timeout = std::chrono::seconds(std::max(1, timeout_seconds));

    http::request<http::string_body> request{verb, parsed.target, 11};
    set_mcp_headers(request, parsed, headers);
    if (verb != http::verb::get && verb != http::verb::head) {
      request.body() = body;
      request.prepare_payload();
    }

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    if (parsed.https) {
      ssl::context ctx(ssl::context::tls_client);
      ctx.set_default_verify_paths();
      beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
      if (!SSL_set_tlsext_host_name(stream.native_handle(),
                                    parsed.host.c_str())) {
        beast::error_code ec{
            static_cast<int>(::ERR_get_error()),
            net::error::get_ssl_category()};
        return foundation::Result<McpHttpResponse>::failure(
            foundation::ErrorCode::io_error,
            sanitize_mcp_error("configure MCP TLS SNI: " + ec.message()));
      }
      stream.set_verify_mode(ssl::verify_peer);
      stream.set_verify_callback(ssl::host_name_verification(parsed.host));
      beast::get_lowest_layer(stream).expires_after(timeout);
      beast::get_lowest_layer(stream).connect(results);
      stream.handshake(ssl::stream_base::client);
      http::write(stream, request);
      response = read_http_string_response(stream, buffer);
      beast::error_code shutdown_error;
      stream.shutdown(shutdown_error);
      if (shutdown_error == net::error::eof ||
          shutdown_error == ssl::error::stream_truncated) {
        shutdown_error = {};
      }
      if (shutdown_error) {
        return foundation::Result<McpHttpResponse>::failure(
            foundation::ErrorCode::io_error,
            sanitize_mcp_error("shutdown MCP TLS stream: " +
                               shutdown_error.message()));
      }
    } else {
      beast::tcp_stream stream(ioc);
      stream.expires_after(timeout);
      stream.connect(results);
      http::write(stream, request);
      response = read_http_string_response(stream, buffer);
      beast::error_code shutdown_error;
      stream.socket().shutdown(tcp::socket::shutdown_both, shutdown_error);
      if (shutdown_error == net::error::not_connected) {
        shutdown_error = {};
      }
      if (shutdown_error) {
        return foundation::Result<McpHttpResponse>::failure(
            foundation::ErrorCode::io_error,
            sanitize_mcp_error("shutdown MCP HTTP stream: " +
                               shutdown_error.message()));
      }
    }

    McpHttpResponse result;
    result.status = response.result_int();
    result.body = response.body();
    for (const auto& header : response.base()) {
      result.headers.emplace(std::string(header.name_string()),
                             std::string(header.value()));
    }
    return result;
  } catch (const std::exception& error) {
    return foundation::Result<McpHttpResponse>::failure(
        foundation::ErrorCode::io_error,
        sanitize_mcp_error(std::string("MCP HTTP request failed: ") +
                           error.what()));
  }
}

std::vector<SseEvent> parse_sse_events(std::string_view text) {
  std::vector<SseEvent> events;
  std::optional<std::string> event_name;
  std::string data;
  std::size_t start = 0U;
  while (start <= text.size()) {
    const auto newline = text.find('\n', start);
    std::string line(text.substr(start, newline == std::string_view::npos
                                            ? std::string_view::npos
                                            : newline - start));
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      if (!data.empty()) {
        events.push_back(SseEvent{event_name, data});
      }
      event_name.reset();
      data.clear();
    } else if (line.front() != ':') {
      const auto colon = line.find(':');
      const std::string field =
          colon == std::string::npos ? line : line.substr(0, colon);
      std::string value =
          colon == std::string::npos ? std::string() : line.substr(colon + 1U);
      if (!value.empty() && value.front() == ' ') {
        value.erase(value.begin());
      }
      if (field == "event") {
        event_name = value;
      } else if (field == "data") {
        data += value;
        data.push_back('\n');
      }
    }
    if (newline == std::string_view::npos) {
      break;
    }
    start = newline + 1U;
  }
  if (!data.empty()) {
    events.push_back(SseEvent{event_name, data});
  }
  return events;
}

std::string sanitize_identifier(std::string value) {
  std::string out;
  out.reserve(value.size());
  bool last_underscore = false;
  for (unsigned char ch : value) {
    const bool valid = std::isalnum(ch) != 0;
    if (valid) {
      out.push_back(static_cast<char>(std::tolower(ch)));
      last_underscore = false;
    } else if (!last_underscore) {
      out.push_back('_');
      last_underscore = true;
    }
  }
  while (!out.empty() && out.front() == '_') {
    out.erase(out.begin());
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out.empty() ? "tool" : out;
}

bool safe_environment_name(std::string_view name) {
  static const std::set<std::string_view> kAllowed = {
      "PATH", "HOME", "USER", "LANG", "LC_ALL", "TERM", "SHELL", "TMPDIR"};
  return kAllowed.find(name) != kAllowed.end() ||
         name.rfind("XDG_", 0) == 0;
}

std::map<std::string, std::string> safe_environment(
    const McpServerConfig& config) {
  std::map<std::string, std::string> env;
  for (char** item = environ; item != nullptr && *item != nullptr; ++item) {
    std::string entry(*item);
    const auto pos = entry.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    const std::string name = entry.substr(0, pos);
    if (safe_environment_name(name)) {
      env[name] = entry.substr(pos + 1U);
    }
  }
  for (const auto& item : config.env) {
    env[item.first] = item.second;
  }
  if (env.find("PATH") == env.end()) {
    env["PATH"] = "/usr/local/bin:/usr/bin:/bin";
  }
  return env;
}

std::vector<std::string> environment_vector(
    const std::map<std::string, std::string>& env) {
  std::vector<std::string> result;
  result.reserve(env.size());
  for (const auto& item : env) {
    result.push_back(item.first + "=" + item.second);
  }
  return result;
}

std::string resolve_command(const std::string& command,
                            const std::map<std::string, std::string>& env) {
  if (command.find('/') != std::string::npos) {
    return command;
  }
  const auto path_it = env.find("PATH");
  const std::string path =
      path_it == env.end() ? "/usr/local/bin:/usr/bin:/bin" : path_it->second;
  std::size_t start = 0U;
  while (start <= path.size()) {
    const auto end = path.find(':', start);
    const std::string dir =
        path.substr(start, end == std::string::npos ? std::string::npos
                                                    : end - start);
    const std::string candidate = (dir.empty() ? "." : dir) + "/" + command;
    if (::access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1U;
  }
  return command;
}

std::string resolve_relative_url(const std::string& base,
                                 const std::string& value) {
  if (value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0) {
    return value;
  }
  const auto scheme = base.find("://");
  if (scheme == std::string::npos) {
    return value;
  }
  const auto authority_start = scheme + 3U;
  const auto path_start = base.find('/', authority_start);
  const std::string origin =
      path_start == std::string::npos ? base : base.substr(0, path_start);
  if (!value.empty() && value.front() == '/') {
    return origin + value;
  }
  const auto directory_end =
      path_start == std::string::npos ? base.size() : base.find_last_of('/');
  return base.substr(0, directory_end + 1U) + value;
}

void close_fd(int fd) {
  if (fd >= 0) {
    while (::close(fd) != 0 && errno == EINTR) {
    }
  }
}

bool write_all(int fd, std::string_view data) {
  const char* ptr = data.data();
  std::size_t remaining = data.size();
  while (remaining > 0U) {
    const ssize_t written = ::write(fd, ptr, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    ptr += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

void append_stderr_to_log(int from_fd, const std::string& log_path) {
  if (log_path.empty()) {
    int dev_null = ::open("/dev/null", O_WRONLY);
    if (dev_null >= 0) {
      ::dup2(dev_null, STDERR_FILENO);
      close_fd(dev_null);
    }
    return;
  }
  const int log_fd = ::open(log_path.c_str(), O_CREAT | O_WRONLY | O_APPEND,
                            0600);
  if (log_fd >= 0) {
    ::dup2(log_fd, STDERR_FILENO);
    close_fd(log_fd);
  } else {
    ::dup2(from_fd, STDERR_FILENO);
  }
}

struct JsonResponse {
  Json value;
  std::string error;
};

class StdioSession {
 public:
  StdioSession(McpServerConfig config,
               std::shared_ptr<McpSamplingCallback> sampling_callback)
      : config_(std::move(config)),
        sampling_callback_(std::move(sampling_callback)) {}
  ~StdioSession() { stop(); }

  StdioSession(const StdioSession&) = delete;
  StdioSession& operator=(const StdioSession&) = delete;

  foundation::Result<Json> initialize_and_list_tools() {
    auto started = ensure_started(config_.connect_timeout_seconds);
    if (!started) {
      return started.error();
    }
    if (!initialized_) {
      Json init_params = {
          {"protocolVersion", "2025-03-26"},
          {"capabilities", Json::object()},
          {"clientInfo", {{"name", "qt"}, {"version", "0.0.0"}}},
      };
      auto init = request("initialize", init_params,
                          config_.connect_timeout_seconds);
      if (!init) {
        stop();
        return init.error();
      }
      auto notify_status =
          send(Json{{"jsonrpc", "2.0"},
                    {"method", "notifications/initialized"},
                    {"params", Json::object()}});
      if (!notify_status) {
        stop();
        return notify_status.error();
      }
      initialized_ = true;
    }
    return request("tools/list", Json::object(), config_.timeout_seconds);
  }

  foundation::Result<Json> call_tool(const std::string& name,
                                     const Json& arguments) {
    auto listed = initialize_and_list_tools();
    if (!listed) {
      return listed.error();
    }
    return request("tools/call",
                   Json{{"name", name}, {"arguments", arguments}},
                   config_.timeout_seconds);
  }

 private:
  foundation::Status ensure_started(int timeout_seconds) {
    if (running_) {
      return foundation::Status::success();
    }
    stop();

    if (!config_.command || config_.command->empty()) {
      return foundation::Status::failure(foundation::ErrorCode::invalid_argument,
                                         "MCP stdio server has no command");
    }
    if (!config_.stderr_log_path.empty()) {
      std::error_code ignored;
      std::filesystem::create_directories(
          std::filesystem::path(config_.stderr_log_path).parent_path(),
          ignored);
    }

    int child_stdin[2] = {-1, -1};
    int child_stdout[2] = {-1, -1};
    int child_stderr[2] = {-1, -1};
    if (::pipe(child_stdin) != 0 || ::pipe(child_stdout) != 0 ||
        ::pipe(child_stderr) != 0) {
      close_fd(child_stdin[0]);
      close_fd(child_stdin[1]);
      close_fd(child_stdout[0]);
      close_fd(child_stdout[1]);
      close_fd(child_stderr[0]);
      close_fd(child_stderr[1]);
      return foundation::Status::failure(foundation::ErrorCode::io_error,
                                         "failed to create MCP stdio pipes",
                                         errno);
    }

    const auto env_map = safe_environment(config_);
    std::map<std::string, std::string> child_env = env_map;
    const std::string command = resolve_command(*config_.command, child_env);
    const auto slash = command.find_last_of('/');
    if (slash != std::string::npos) {
      child_env["PATH"] =
          command.substr(0, slash) + ":" + child_env["PATH"];
    }
    const auto env_strings = environment_vector(child_env);
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1U);
    for (const auto& item : env_strings) {
      envp.push_back(const_cast<char*>(item.c_str()));
    }
    envp.push_back(nullptr);

    std::vector<std::string> args;
    args.reserve(config_.args.size() + 1U);
    args.push_back(command);
    args.insert(args.end(), config_.args.begin(), config_.args.end());
    std::vector<char*> argv;
    argv.reserve(args.size() + 1U);
    for (auto& item : args) {
      argv.push_back(item.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
      close_fd(child_stdin[0]);
      close_fd(child_stdin[1]);
      close_fd(child_stdout[0]);
      close_fd(child_stdout[1]);
      close_fd(child_stderr[0]);
      close_fd(child_stderr[1]);
      return foundation::Status::failure(foundation::ErrorCode::io_error,
                                         "failed to fork MCP stdio server",
                                         errno);
    }

    if (pid == 0) {
      ::dup2(child_stdin[0], STDIN_FILENO);
      ::dup2(child_stdout[1], STDOUT_FILENO);
      append_stderr_to_log(child_stderr[1], config_.stderr_log_path);
      close_fd(child_stdin[0]);
      close_fd(child_stdin[1]);
      close_fd(child_stdout[0]);
      close_fd(child_stdout[1]);
      close_fd(child_stderr[0]);
      close_fd(child_stderr[1]);
      ::execve(command.c_str(), argv.data(), envp.data());
      _exit(127);
    }

    close_fd(child_stdin[0]);
    close_fd(child_stdout[1]);
    close_fd(child_stderr[1]);
    stdin_fd_ = child_stdin[1];
    stdout_fd_ = child_stdout[0];
    stderr_fd_ = child_stderr[0];
    pid_ = pid;
    running_ = true;
    initialized_ = false;
    next_id_ = 1;
    reader_ = std::thread([this] { reader_loop(); });
    stderr_reader_ = std::thread([this] { stderr_loop(); });

    const auto deadline =
        Clock::now() + std::chrono::seconds(std::max(1, timeout_seconds));
    while (Clock::now() < deadline) {
      int status = 0;
      const pid_t done = ::waitpid(pid_, &status, WNOHANG);
      if (done == 0) {
        return foundation::Status::success();
      }
      if (done == pid_) {
        running_ = false;
        return foundation::Status::failure(
            foundation::ErrorCode::io_error,
            "MCP stdio server exited during startup: " + config_.name);
      }
      if (done < 0 && errno != EINTR) {
        break;
      }
    }
    return foundation::Status::success();
  }

  foundation::Status send(const Json& message) {
    std::string payload = message.dump();
    payload.push_back('\n');
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!running_ || stdin_fd_ < 0 || !write_all(stdin_fd_, payload)) {
      return foundation::Status::failure(foundation::ErrorCode::io_error,
                                         "failed to write MCP stdio request");
    }
    return foundation::Status::success();
  }

  foundation::Result<Json> request(const std::string& method,
                                   const Json& params,
                                   int timeout_seconds) {
    const int id = next_id_++;
    Json message = {{"jsonrpc", "2.0"},
                    {"id", id},
                    {"method", method},
                    {"params", params}};
    const auto sent = send(message);
    if (!sent) {
      return foundation::Result<Json>::failure(sent.error().code,
                                               sanitize_mcp_error(sent.error().message),
                                               sent.error().system_error);
    }

    std::unique_lock<std::mutex> lock(response_mutex_);
    const auto deadline =
        Clock::now() + std::chrono::seconds(std::max(1, timeout_seconds));
    while (responses_.find(id) == responses_.end()) {
      if (response_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
        responses_.erase(id);
        return foundation::Result<Json>::failure(
            foundation::ErrorCode::io_error,
            "MCP request timed out: " + method + " on " + config_.name);
      }
    }
    JsonResponse response = std::move(responses_.at(id));
    responses_.erase(id);
    if (!response.error.empty()) {
      return foundation::Result<Json>::failure(
          foundation::ErrorCode::io_error, sanitize_mcp_error(response.error));
    }
    return response.value;
  }

  void reader_loop() {
    std::string buffer;
    std::array<char, 4096> chunk{};
    while (running_) {
      const ssize_t read_count =
          ::read(stdout_fd_, chunk.data(), chunk.size());
      if (read_count < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (read_count == 0) {
        break;
      }
      buffer.append(chunk.data(), static_cast<std::size_t>(read_count));
      std::size_t newline = 0U;
      while ((newline = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, newline);
        buffer.erase(0, newline + 1U);
        handle_line(line);
      }
    }
    {
      std::lock_guard<std::mutex> lock(response_mutex_);
      running_ = false;
    }
    response_cv_.notify_all();
  }

  void stderr_loop() {
    std::array<char, 1024> chunk{};
    while (running_) {
      const ssize_t read_count =
          ::read(stderr_fd_, chunk.data(), chunk.size());
      if (read_count < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (read_count == 0) {
        break;
      }
    }
  }

  void handle_line(const std::string& line) {
    if (line.empty()) {
      return;
    }
    Json json;
    try {
      json = Json::parse(line);
    } catch (const Json::parse_error&) {
      return;
    }
    if (!json.is_object() || !json.contains("id")) {
      return;
    }
    if (json.contains("method") && json.at("method").is_string()) {
      const std::string method = json.at("method").get<std::string>();
      if (method == "sampling/createMessage") {
        Json result_or_error;
        bool success = false;
        McpSamplingCallback callback;
        if (sampling_callback_) {
          callback = *sampling_callback_;
        }
        if (callback && config_.sampling.value("enabled", true)) {
          McpSamplingRequest sampling_request;
          sampling_request.server_name = config_.name;
          sampling_request.params =
              json.value("params", Json::object());
          sampling_request.config = config_.sampling;
          const auto sampled = callback(sampling_request);
          if (sampled) {
            result_or_error = sampled.value();
            success = true;
          } else {
            result_or_error = Json{
                {"code", -32000},
                {"message", sanitize_mcp_error(sampled.error().message)},
            };
          }
        } else {
          result_or_error = Json{
              {"code", -32601},
              {"message",
               "MCP sampling/createMessage is not enabled for this server"},
          };
        }
        Json response = {
            {"jsonrpc", "2.0"},
            {"id", json.at("id")},
        };
        if (success) {
          response["result"] = std::move(result_or_error);
        } else {
          response["error"] = std::move(result_or_error);
        }
        const auto status = send(response);
        (void)status;
        return;
      }
    }
    if (!json.at("id").is_number_integer()) {
      return;
    }
    const int id = json.at("id").get<int>();
    JsonResponse response;
    if (json.contains("error")) {
      response.error = json.at("error").dump();
    } else if (json.contains("result")) {
      response.value = json.at("result");
    } else {
      response.value = Json::object();
    }
    {
      std::lock_guard<std::mutex> lock(response_mutex_);
      responses_[id] = std::move(response);
    }
    response_cv_.notify_all();
  }

  void stop() {
    running_ = false;
    close_fd(stdin_fd_);
    stdin_fd_ = -1;
    close_fd(stdout_fd_);
    stdout_fd_ = -1;
    close_fd(stderr_fd_);
    stderr_fd_ = -1;
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      for (int i = 0; i < 20; ++i) {
        int status = 0;
        const pid_t done = ::waitpid(pid_, &status, WNOHANG);
        if (done == pid_) {
          pid_ = -1;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (pid_ > 0) {
        ::kill(pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
      }
    }
    if (reader_.joinable()) {
      reader_.join();
    }
    if (stderr_reader_.joinable()) {
      stderr_reader_.join();
    }
    initialized_ = false;
  }

  const McpServerConfig config_;
  std::shared_ptr<McpSamplingCallback> sampling_callback_;
  mutable std::mutex write_mutex_;
  std::mutex response_mutex_;
  std::condition_variable response_cv_;
  std::map<int, JsonResponse> responses_;
  std::thread reader_;
  std::thread stderr_reader_;
  pid_t pid_ = -1;
  int stdin_fd_ = -1;
  int stdout_fd_ = -1;
  int stderr_fd_ = -1;
  bool running_ = false;
  bool initialized_ = false;
  int next_id_ = 1;
};

class HttpSession {
 public:
  explicit HttpSession(McpServerConfig config) : config_(std::move(config)) {}

  foundation::Result<Json> initialize_and_list_tools() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
      Json init_params = {
          {"protocolVersion", "2025-03-26"},
          {"capabilities", Json::object()},
          {"clientInfo", {{"name", "qt"}, {"version", "0.0.0"}}},
      };
      auto init = request_locked("initialize", init_params,
                                 config_.connect_timeout_seconds);
      if (!init) {
        initialized_ = false;
        return init.error();
      }
      auto notify = notification_locked("notifications/initialized",
                                        Json::object(),
                                        config_.connect_timeout_seconds);
      if (!notify) {
        initialized_ = false;
        return notify.error();
      }
      initialized_ = true;
    }
    return request_locked("tools/list", Json::object(), config_.timeout_seconds);
  }

  foundation::Result<Json> call_tool(const std::string& name,
                                     const Json& arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
      auto init = request_locked(
          "initialize",
          Json{{"protocolVersion", "2025-03-26"},
               {"capabilities", Json::object()},
               {"clientInfo", {{"name", "qt"}, {"version", "0.0.0"}}}},
          config_.connect_timeout_seconds);
      if (!init) {
        return init.error();
      }
      auto notify = notification_locked("notifications/initialized",
                                        Json::object(),
                                        config_.connect_timeout_seconds);
      if (!notify) {
        return notify.error();
      }
      initialized_ = true;
    }
    return request_locked("tools/call",
                          Json{{"name", name}, {"arguments", arguments}},
                          config_.timeout_seconds);
  }

 private:
  foundation::Result<Json> notification_locked(const std::string& method,
                                               const Json& params,
                                               int timeout_seconds) {
    Json message =
        Json{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    auto response = send_http_locked(message, timeout_seconds);
    if (!response) {
      return response.error();
    }
    return Json::object();
  }

  foundation::Result<Json> request_locked(const std::string& method,
                                          const Json& params,
                                          int timeout_seconds) {
    Json message = {{"jsonrpc", "2.0"},
                    {"id", next_id_++},
                    {"method", method},
                    {"params", params}};
    auto response = send_http_locked(message, timeout_seconds);
    if (!response) {
      return response.error();
    }
    const Json& json = response.value();
    if (json.contains("error")) {
      return foundation::Result<Json>::failure(
          foundation::ErrorCode::io_error, sanitize_mcp_error(json.at("error").dump()));
    }
    if (json.contains("result")) {
      return json.at("result");
    }
    return Json::object();
  }

  foundation::Result<Json> send_http_locked(const Json& message,
                                            int timeout_seconds) {
    if (!config_.url || config_.url->empty()) {
      return foundation::Result<Json>::failure(
          foundation::ErrorCode::invalid_argument,
          "MCP HTTP server has no url: " + config_.name);
    }

    std::map<std::string, std::string> headers = config_.headers;
    headers["content-type"] = "application/json";
    headers["accept"] = "application/json, text/event-stream";
    if (!header_contains(headers, "mcp-protocol-version")) {
      headers["mcp-protocol-version"] = "2025-03-26";
    }
    if (!session_id_.empty()) {
      headers["mcp-session-id"] = session_id_;
    }

    auto sent = beast_http_request(*config_.url, http::verb::post, headers,
                                   message.dump(), timeout_seconds);
    if (!sent) {
      return foundation::Result<Json>::failure(sent.error().code,
                                               sent.error().message,
                                               sent.error().system_error);
    }
    const McpHttpResponse response = std::move(sent.value());
    for (const auto& header : response.headers) {
      if (lower_ascii(header.first) == "mcp-session-id" ||
          lower_ascii(header.first) == "mcp-sessionid") {
        session_id_ = header.second;
      }
    }
    if (response.status < 200 || response.status >= 300) {
      return foundation::Result<Json>::failure(
          foundation::ErrorCode::io_error,
          sanitize_mcp_error("MCP HTTP server " + config_.name +
                             " returned status " +
                             std::to_string(response.status) + ": " +
                             response.body));
    }

    const auto parse_json = [&](const std::string& text)
        -> foundation::Result<Json> {
      try {
        return Json::parse(text);
      } catch (const Json::parse_error& error) {
        return foundation::Result<Json>::failure(
            foundation::ErrorCode::encoding_error,
            sanitize_mcp_error(std::string("invalid MCP HTTP JSON response: ") +
                               error.what()));
      }
    };

    const auto content_type = [&]() -> std::string {
      for (const auto& header : response.headers) {
        if (lower_ascii(header.first) == "content-type") {
          return lower_ascii(header.second);
        }
      }
      return {};
    }();
    if (content_type.find("text/event-stream") != std::string::npos ||
        response.body.find("data:") == 0 ||
        response.body.find("\ndata:") != std::string::npos) {
      const auto events = parse_sse_events(response.body);
      for (const auto& event : events) {
        if (event.data.empty()) {
          continue;
        }
        std::string data = event.data;
        while (!data.empty() && (data.back() == '\n' || data.back() == '\r')) {
          data.pop_back();
        }
        if (data.empty()) {
          continue;
        }
        auto parsed = parse_json(data);
        if (parsed) {
          return parsed.value();
        }
      }
      return foundation::Result<Json>::failure(
          foundation::ErrorCode::encoding_error,
          "MCP SSE response did not contain a JSON data event");
    }

    return parse_json(response.body);
  }

  McpServerConfig config_;
  std::mutex mutex_;
  std::string session_id_;
  bool initialized_ = false;
  int next_id_ = 1;
};

class LegacySseSession {
 public:
  LegacySseSession(McpServerConfig config,
                   std::shared_ptr<McpSamplingCallback> sampling_callback)
      : config_(std::move(config)),
        sampling_callback_(std::move(sampling_callback)) {}
  ~LegacySseSession() { stop(); }

  LegacySseSession(const LegacySseSession&) = delete;
  LegacySseSession& operator=(const LegacySseSession&) = delete;

  foundation::Result<Json> initialize_and_list_tools() {
    std::unique_lock<std::mutex> lock(mutex_);
    auto initialized = ensure_initialized_locked(lock);
    if (!initialized) {
      return initialized.error();
    }
    return request_locked(lock, "tools/list", Json::object(),
                          config_.timeout_seconds);
  }

  foundation::Result<Json> call_tool(const std::string& name,
                                     const Json& arguments) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto initialized = ensure_initialized_locked(lock);
    if (!initialized) {
      return initialized.error();
    }
    return request_locked(lock, "tools/call",
                          Json{{"name", name}, {"arguments", arguments}},
                          config_.timeout_seconds);
  }

 private:
  foundation::Status ensure_initialized_locked(
      std::unique_lock<std::mutex>& lock) {
    if (initialized_) {
      return foundation::Status::success();
    }
    auto started = ensure_stream_locked(lock);
    if (!started) {
      return started;
    }
    Json init_params = {
        {"protocolVersion", "2025-03-26"},
        {"capabilities", Json::object()},
        {"clientInfo", {{"name", "qt"}, {"version", "0.0.0"}}},
    };
    auto init = request_locked(lock, "initialize", init_params,
                               config_.connect_timeout_seconds);
    if (!init) {
      initialized_ = false;
      return foundation::Status::failure(init.error().code,
                                         init.error().message,
                                         init.error().system_error);
    }
    auto notified = notification_locked(lock, "notifications/initialized",
                                        Json::object(),
                                        config_.connect_timeout_seconds);
    if (!notified) {
      initialized_ = false;
      return foundation::Status::failure(notified.error().code,
                                         notified.error().message,
                                         notified.error().system_error);
    }
    initialized_ = true;
    return foundation::Status::success();
  }

  foundation::Status ensure_stream_locked(std::unique_lock<std::mutex>& lock) {
    if (running_ && !endpoint_url_.empty()) {
      return foundation::Status::success();
    }
    if (!config_.url || config_.url->empty()) {
      return foundation::Status::failure(foundation::ErrorCode::invalid_argument,
                                         "MCP SSE server has no url: " +
                                             config_.name);
    }
    stop_locked(lock);
    stop_requested_ = false;
    running_ = true;
    endpoint_url_.clear();
    reader_ = std::thread([this] { reader_loop(); });

    const auto deadline =
        Clock::now() +
        std::chrono::seconds(std::max(1, config_.connect_timeout_seconds));
    endpoint_cv_.wait_until(lock, deadline, [this] {
      return !endpoint_url_.empty() || !running_;
    });
    if (endpoint_url_.empty()) {
      return foundation::Status::failure(
          foundation::ErrorCode::io_error,
          "MCP SSE server did not provide an endpoint event: " + config_.name);
    }
    return foundation::Status::success();
  }

  foundation::Result<Json> notification_locked(
      std::unique_lock<std::mutex>& lock,
      const std::string& method,
      const Json& params,
      int timeout_seconds) {
    Json message =
        Json{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    const auto sent = post_message_locked(lock, message, timeout_seconds);
    if (!sent) {
      return foundation::Result<Json>::failure(sent.error().code,
                                               sent.error().message,
                                               sent.error().system_error);
    }
    return Json::object();
  }

  foundation::Result<Json> request_locked(std::unique_lock<std::mutex>& lock,
                                          const std::string& method,
                                          const Json& params,
                                          int timeout_seconds) {
    const int id = next_id_++;
    Json message = {{"jsonrpc", "2.0"},
                    {"id", id},
                    {"method", method},
                    {"params", params}};
    const auto sent = post_message_locked(lock, message, timeout_seconds);
    if (!sent) {
      return foundation::Result<Json>::failure(sent.error().code,
                                               sent.error().message,
                                               sent.error().system_error);
    }
    const auto deadline =
        Clock::now() + std::chrono::seconds(std::max(1, timeout_seconds));
    while (responses_.find(id) == responses_.end()) {
      if (response_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
        responses_.erase(id);
        return foundation::Result<Json>::failure(
            foundation::ErrorCode::io_error,
            "MCP SSE request timed out: " + method + " on " + config_.name);
      }
    }
    JsonResponse response = std::move(responses_.at(id));
    responses_.erase(id);
    if (!response.error.empty()) {
      return foundation::Result<Json>::failure(
          foundation::ErrorCode::io_error, sanitize_mcp_error(response.error));
    }
    return response.value;
  }

  foundation::Status post_message_locked(std::unique_lock<std::mutex>& lock,
                                         const Json& message,
                                         int timeout_seconds) {
    if (endpoint_url_.empty()) {
      const auto started = ensure_stream_locked(lock);
      if (!started) {
        return started;
      }
    }
    const std::string endpoint = endpoint_url_;
    std::map<std::string, std::string> headers = config_.headers;
    headers["content-type"] = "application/json";
    headers["accept"] = "application/json, text/event-stream";
    if (!header_contains(headers, "mcp-protocol-version")) {
      headers["mcp-protocol-version"] = "2025-03-26";
    }
    lock.unlock();
    auto sent = beast_http_request(endpoint, http::verb::post, headers,
                                   message.dump(), timeout_seconds);
    lock.lock();
    if (!sent) {
      return foundation::Status::failure(sent.error().code,
                                         sent.error().message,
                                         sent.error().system_error);
    }
    const McpHttpResponse response = std::move(sent.value());
    if (response.status < 200 || response.status >= 300) {
      return foundation::Status::failure(
          foundation::ErrorCode::io_error,
          sanitize_mcp_error("MCP SSE POST returned status " +
                             std::to_string(response.status) + ": " +
                             response.body));
    }
    return foundation::Status::success();
  }

  void post_message_threadsafe(const Json& message, int timeout_seconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto status = post_message_locked(lock, message, timeout_seconds);
    (void)status;
  }

  void dispatch_sse_event(std::optional<std::string> event,
                          std::string data) {
    while (!data.empty() && (data.back() == '\n' || data.back() == '\r')) {
      data.pop_back();
    }
    if (data.empty()) {
      return;
    }
    if (event == std::optional<std::string>{"endpoint"}) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        endpoint_url_ = resolve_relative_url(*config_.url, data);
      }
      endpoint_cv_.notify_all();
      return;
    }
    Json json;
    try {
      json = Json::parse(data);
    } catch (const Json::parse_error&) {
      return;
    }
    if (!json.is_object() || !json.contains("id")) {
      return;
    }
    if (json.contains("method") && json.at("method").is_string()) {
      const std::string method = json.at("method").get<std::string>();
      if (method == "sampling/createMessage") {
        Json result_or_error;
        bool success = false;
        McpSamplingCallback callback;
        if (sampling_callback_) {
          callback = *sampling_callback_;
        }
        if (callback && config_.sampling.value("enabled", true)) {
          McpSamplingRequest request;
          request.server_name = config_.name;
          request.params = json.value("params", Json::object());
          request.config = config_.sampling;
          const auto sampled = callback(request);
          if (sampled) {
            result_or_error = sampled.value();
            success = true;
          } else {
            result_or_error = Json{
                {"code", -32000},
                {"message", sanitize_mcp_error(sampled.error().message)},
            };
          }
        } else {
          result_or_error = Json{
              {"code", -32601},
              {"message",
               "MCP sampling/createMessage is not enabled for this server"},
          };
        }
        Json response = {{"jsonrpc", "2.0"}, {"id", json.at("id")}};
        if (success) {
          response["result"] = std::move(result_or_error);
        } else {
          response["error"] = std::move(result_or_error);
        }
        post_message_threadsafe(response, config_.timeout_seconds);
        return;
      }
    }
    if (!json.at("id").is_number_integer()) {
      return;
    }
    JsonResponse response;
    if (json.contains("error")) {
      response.error = json.at("error").dump();
    } else if (json.contains("result")) {
      response.value = json.at("result");
    } else {
      response.value = Json::object();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      responses_[json.at("id").get<int>()] = std::move(response);
    }
    response_cv_.notify_all();
  }

  void feed_sse_bytes(std::string& buffer,
                      std::optional<std::string>& event,
                      std::string& data,
                      std::string_view bytes) {
    buffer.append(bytes.data(), bytes.size());
    std::size_t newline = 0U;
    while ((newline = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, newline);
      buffer.erase(0, newline + 1U);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty()) {
        dispatch_sse_event(event, data);
        event.reset();
        data.clear();
        continue;
      }
      if (!line.empty() && line.front() == ':') {
        continue;
      }
      const auto colon = line.find(':');
      const std::string field =
          colon == std::string::npos ? line : line.substr(0, colon);
      std::string value =
          colon == std::string::npos ? std::string() : line.substr(colon + 1U);
      if (!value.empty() && value.front() == ' ') {
        value.erase(value.begin());
      }
      if (field == "event") {
        event = value;
      } else if (field == "data") {
        data += value;
        data.push_back('\n');
      }
    }
  }

  template <typename Stream>
  void read_sse_body(Stream& stream,
                     beast::flat_buffer& buffer,
                     http::response_parser<http::buffer_body>& parser) {
    std::string line_buffer;
    std::optional<std::string> event;
    std::string data;
    std::array<char, 4096> chunk{};
    while (!stop_requested_) {
      parser.get().body().data = chunk.data();
      parser.get().body().size = chunk.size();
      beast::error_code ec;
      http::read_some(stream, buffer, parser, ec);
      const auto used = chunk.size() - parser.get().body().size;
      if (used > 0U) {
        feed_sse_bytes(line_buffer, event, data,
                       std::string_view(chunk.data(), used));
      }
      if (ec == http::error::need_buffer) {
        continue;
      }
      if (ec == http::error::end_of_stream || ec == net::error::eof) {
        break;
      }
      if (ec) {
        break;
      }
    }
  }

  void reader_loop() {
    const auto finish = [this] {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
      }
      endpoint_cv_.notify_all();
      response_cv_.notify_all();
    };
    const auto parsed_result = config_.url ? parse_http_url(*config_.url)
                                           : foundation::Result<ParsedHttpUrl>::failure(
                                                 foundation::ErrorCode::invalid_argument,
                                                 "MCP SSE server has no url: " +
                                                     config_.name);
    if (!parsed_result) {
      finish();
      return;
    }
    const ParsedHttpUrl parsed = parsed_result.value();

    try {
      net::io_context ioc;
      tcp::resolver resolver(ioc);
      const auto results = resolver.resolve(parsed.host, parsed.port);
      std::map<std::string, std::string> headers = config_.headers;
      headers["accept"] = "text/event-stream";
      if (!header_contains(headers, "mcp-protocol-version")) {
        headers["mcp-protocol-version"] = "2025-03-26";
      }
      http::request<http::empty_body> request{http::verb::get, parsed.target,
                                              11};
      request.set(http::field::host, parsed.host);
      request.set(http::field::user_agent,
                  std::string("qt/") + BOOST_BEAST_VERSION_STRING);
      for (const auto& header : headers) {
        request.set(header.first, header.second);
      }

      beast::flat_buffer buffer;
      http::response_parser<http::buffer_body> parser;
      parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
      if (parsed.https) {
        ssl::context ctx(ssl::context::tls_client);
        ctx.set_default_verify_paths();
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
        if (!SSL_set_tlsext_host_name(stream.native_handle(),
                                      parsed.host.c_str())) {
          finish();
          return;
        }
        stream.set_verify_mode(ssl::verify_peer);
        stream.set_verify_callback(ssl::host_name_verification(parsed.host));
        beast::get_lowest_layer(stream).expires_after(
            std::chrono::seconds(std::max(1, config_.connect_timeout_seconds)));
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);
        http::write(stream, request);
        http::read_header(stream, buffer, parser);
        read_sse_body(stream, buffer, parser);
        beast::error_code ignored;
        stream.shutdown(ignored);
      } else {
        beast::tcp_stream stream(ioc);
        stream.expires_after(
            std::chrono::seconds(std::max(1, config_.connect_timeout_seconds)));
        stream.connect(results);
        http::write(stream, request);
        http::read_header(stream, buffer, parser);
        read_sse_body(stream, buffer, parser);
        beast::error_code ignored;
        stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
      }
    } catch (const std::exception&) {
    }
    finish();
  }

  void stop_locked(std::unique_lock<std::mutex>& lock) {
    stop_requested_ = true;
    running_ = false;
    initialized_ = false;
    endpoint_url_.clear();
    lock.unlock();
    if (reader_.joinable()) {
      reader_.join();
    }
    lock.lock();
  }

  void stop() {
    std::unique_lock<std::mutex> lock(mutex_);
    stop_locked(lock);
  }

  const McpServerConfig config_;
  std::shared_ptr<McpSamplingCallback> sampling_callback_;
  std::mutex mutex_;
  std::condition_variable endpoint_cv_;
  std::condition_variable response_cv_;
  std::map<int, JsonResponse> responses_;
  std::thread reader_;
  bool running_ = false;
  bool stop_requested_ = false;
  bool initialized_ = false;
  std::string endpoint_url_;
  int next_id_ = 1;
};

Json mcp_content_to_output(const Json& response) {
  Json output = Json{{"mcp", response}};
  std::ostringstream text;
  if (response.is_object() && response.contains("content") &&
      response.at("content").is_array()) {
    bool first = true;
    for (const auto& item : response.at("content")) {
      if (!item.is_object()) {
        continue;
      }
      if (item.value("type", "") == "text" && item.contains("text") &&
          item.at("text").is_string()) {
        if (!first) {
          text << '\n';
        }
        text << item.at("text").get<std::string>();
        first = false;
      }
    }
  }
  if (text.tellp() > 0) {
    output["text"] = text.str();
  } else {
    output["text"] = response.dump(2);
  }
  return output;
}

bool looks_like_prompt_injection(std::string_view description) {
  const std::string text = lower_ascii(std::string(description));
  return text.find("ignore previous") != std::string::npos ||
         text.find("ignore all previous") != std::string::npos ||
         text.find("system prompt") != std::string::npos ||
         text.find("developer message") != std::string::npos;
}

}  // namespace

std::string sanitize_mcp_error(std::string message) {
  const std::vector<std::string> markers = {
      "sk-", "ghp_", "github_pat_", "Bearer ", "token=", "key=", "API_KEY=",
      "password=", "secret="};
  for (const auto& marker : markers) {
    std::size_t pos = 0U;
    while ((pos = message.find(marker, pos)) != std::string::npos) {
      const std::size_t value_start = pos + marker.size();
      std::size_t value_end = value_start;
      while (value_end < message.size() &&
             !std::isspace(static_cast<unsigned char>(message[value_end])) &&
             message[value_end] != '"' && message[value_end] != '\'' &&
             message[value_end] != '&') {
        ++value_end;
      }
      message.replace(pos, value_end - pos, marker + "[REDACTED]");
      pos += marker.size() + 10U;
    }
  }
  return message;
}

std::string mcp_registered_tool_name(std::string server_name,
                                     std::string remote_name) {
  return "mcp_" + sanitize_identifier(std::move(server_name)) + "_" +
         sanitize_identifier(std::move(remote_name));
}

struct McpClientManager::Impl {
  explicit Impl(std::vector<McpServerConfig> configs)
      : configs(std::move(configs)),
        sampling_callback(std::make_shared<McpSamplingCallback>()) {}

  StdioSession& session_for(const McpServerConfig& config) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto found = sessions.find(config.name);
    if (found == sessions.end()) {
      found = sessions
                  .emplace(config.name,
                           std::make_unique<StdioSession>(
                               config, sampling_callback))
                  .first;
    }
    return *found->second;
  }

  HttpSession& http_session_for(const McpServerConfig& config) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto found = http_sessions.find(config.name);
    if (found == http_sessions.end()) {
      found = http_sessions
                  .emplace(config.name,
                           std::make_unique<HttpSession>(config))
                  .first;
    }
    return *found->second;
  }

  LegacySseSession& sse_session_for(const McpServerConfig& config) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto found = sse_sessions.find(config.name);
    if (found == sse_sessions.end()) {
      found = sse_sessions
                  .emplace(config.name,
                           std::make_unique<LegacySseSession>(
                               config, sampling_callback))
                  .first;
    }
    return *found->second;
  }

  std::vector<McpRemoteTool> discover_tools_on_loop(
      std::vector<std::string>* diagnostics) {
    std::vector<McpRemoteTool> discovered;
    for (const auto& config : configs) {
      if (!config.enabled) {
        continue;
      }
      if (config.transport != "stdio" &&
          config.transport != "streamable-http" && config.transport != "http" &&
          config.transport != "sse") {
        if (diagnostics != nullptr) {
          diagnostics->push_back("MCP server " + config.name +
                                 " uses unsupported transport " +
                                 config.transport);
        }
        continue;
      }

      foundation::Result<Json> listed =
          foundation::Result<Json>::failure(foundation::ErrorCode::io_error,
                                            "not attempted");
      for (int attempt = 0; attempt < 5; ++attempt) {
        if (config.transport == "stdio") {
          listed = session_for(config).initialize_and_list_tools();
        } else if (config.transport == "sse") {
          listed = sse_session_for(config).initialize_and_list_tools();
        } else {
          listed = http_session_for(config).initialize_and_list_tools();
        }
        if (listed) {
          break;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100 * (1 << attempt)));
      }
      if (!listed) {
        if (diagnostics != nullptr) {
          diagnostics->push_back("MCP tool discovery failed for " +
                                 config.name + ": " +
                                 sanitize_mcp_error(listed.error().message));
        }
        continue;
      }
      const Json& result = listed.value();
      const auto tools = result.find("tools");
      if (tools == result.end() || !tools->is_array()) {
        continue;
      }
      for (const auto& tool : *tools) {
        if (!tool.is_object() || !tool.contains("name") ||
            !tool.at("name").is_string()) {
          continue;
        }
        McpRemoteTool remote;
        remote.server_name = config.name;
        remote.remote_name = tool.at("name").get<std::string>();
        remote.registered_name =
            mcp_registered_tool_name(remote.server_name, remote.remote_name);
        remote.description =
            tool.value("description", "MCP tool " + remote.remote_name +
                                          " from " + remote.server_name);
        if (tool.contains("inputSchema") && tool.at("inputSchema").is_object()) {
          remote.input_schema = tool.at("inputSchema");
        }
        if (looks_like_prompt_injection(remote.description) &&
            diagnostics != nullptr) {
          diagnostics->push_back("MCP tool " + remote.registered_name +
                                 " description contains prompt-like text");
        }
        discovered.push_back(std::move(remote));
      }
    }
    return discovered;
  }

  ToolResult call_tool_on_loop(const std::string& server_name,
                               const std::string& remote_tool_name,
                               const ToolRequest& request) {
    const auto found =
        std::find_if(configs.begin(), configs.end(), [&](const auto& config) {
          return config.name == server_name;
        });
    if (found == configs.end()) {
      return make_execution_error(ToolResultStatus::not_found,
                                  "MCP server not found: " + server_name,
                                  request);
    }
    if (found->transport != "stdio" && found->transport != "streamable-http" &&
        found->transport != "http" && found->transport != "sse") {
      return make_execution_error(
          ToolResultStatus::execution_error,
          "MCP transport is not implemented for server " + server_name + ": " +
              found->transport,
          request);
    }

    foundation::Result<Json> response =
        foundation::Result<Json>::failure(foundation::ErrorCode::io_error,
                                          "not attempted");
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (found->transport == "stdio") {
        response = session_for(*found).call_tool(remote_tool_name,
                                                 request.arguments);
      } else if (found->transport == "sse") {
        response = sse_session_for(*found).call_tool(remote_tool_name,
                                                     request.arguments);
      } else {
        response = http_session_for(*found).call_tool(remote_tool_name,
                                                      request.arguments);
      }
      if (response) {
        break;
      }
      std::this_thread::sleep_for(
          std::chrono::milliseconds(100 * (1 << attempt)));
    }
    if (!response) {
      return make_execution_error(ToolResultStatus::execution_error,
                                  sanitize_mcp_error(response.error().message),
                                  request);
    }

    ToolResult result;
    result.status = ToolResultStatus::success;
    result.call_id = request.call_id;
    result.output = mcp_content_to_output(response.value());
    if (response.value().is_object() &&
        response.value().value("isError", false)) {
      result.status = ToolResultStatus::execution_error;
      result.error_message =
          result.output.value("text", "MCP tool returned an error");
    }
    return result;
  }

  std::vector<McpServerConfig> configs;
  mutable std::mutex mutex;
  mutable std::map<std::string, std::unique_ptr<StdioSession>> sessions;
  mutable std::map<std::string, std::unique_ptr<HttpSession>> http_sessions;
  mutable std::map<std::string, std::unique_ptr<LegacySseSession>> sse_sessions;
  std::shared_ptr<McpSamplingCallback> sampling_callback;
  BackgroundEventLoop event_loop;
};

McpClientManager::McpClientManager(std::vector<McpServerConfig> servers)
    : impl_(std::make_shared<Impl>(std::move(servers))) {}

McpClientManager::~McpClientManager() = default;

std::vector<McpRemoteTool> McpClientManager::discover_tools(
    std::vector<std::string>* diagnostics) {
  return impl_->event_loop.submit([this, diagnostics] {
    return impl_->discover_tools_on_loop(diagnostics);
  });
}

void McpClientManager::set_sampling_callback(McpSamplingCallback callback) {
  if (!impl_ || !impl_->sampling_callback) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  *impl_->sampling_callback = std::move(callback);
}

ToolResult McpClientManager::call_tool(const std::string& server_name,
                                       const std::string& remote_tool_name,
                                       const ToolRequest& request) const {
  return impl_->event_loop.submit([this, &server_name, &remote_tool_name,
                                   &request] {
    return impl_->call_tool_on_loop(server_name, remote_tool_name, request);
  });
}

McpProxyTool::McpProxyTool(McpRemoteTool remote_tool,
                           std::shared_ptr<McpClientManager> manager)
    : remote_tool_(std::move(remote_tool)), manager_(std::move(manager)) {
  descriptor_.name = remote_tool_.registered_name;
  descriptor_.description =
      "MCP " + remote_tool_.server_name + "/" + remote_tool_.remote_name +
      ": " + remote_tool_.description;
  descriptor_.input_schema = remote_tool_.input_schema;
  descriptor_.deterministic = false;
  descriptor_.destructive = true;
  descriptor_.local_only = false;
}

const ToolDescriptor& McpProxyTool::descriptor() const { return descriptor_; }

ToolResult McpProxyTool::invoke(const ToolRequest& request,
                                const ToolContext&) const {
  if (!is_valid(request)) {
    return make_invalid_request("invalid tool request", request);
  }
  if (request.tool_name != descriptor_.name) {
    return make_invalid_request("tool request name does not match " +
                                    descriptor_.name,
                                request);
  }
  if (!manager_) {
    return make_execution_error(ToolResultStatus::execution_error,
                                "MCP client manager is unavailable", request);
  }
  return manager_->call_tool(remote_tool_.server_name, remote_tool_.remote_name,
                             request);
}

void register_mcp_tools(ToolRegistry& registry,
                        const std::shared_ptr<McpClientManager>& manager,
                        std::vector<std::string>* diagnostics,
                        std::function<bool(const std::string&)> enabled) {
  if (!manager) {
    return;
  }
  const auto tools = manager->discover_tools(diagnostics);
  for (const auto& tool : tools) {
    if (enabled && !enabled(tool.registered_name)) {
      continue;
    }
    if (!registry.register_tool(std::make_unique<McpProxyTool>(tool, manager)) &&
        diagnostics != nullptr) {
      diagnostics->push_back("MCP tool registration skipped duplicate name: " +
                             tool.registered_name);
    }
  }
}

}  // namespace qt::tools
