#include "qt/tools/mcp_client.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Json = nlohmann::json;

struct ChildProcess {
  pid_t pid = -1;

  ~ChildProcess() { stop(); }

  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  ChildProcess(ChildProcess&& other) noexcept : pid(other.pid) {
    other.pid = -1;
  }

  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      stop();
      pid = other.pid;
      other.pid = -1;
    }
    return *this;
  }

  void stop() {
    if (pid <= 0) {
      return;
    }
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {
      int status = 0;
      const pid_t done = ::waitpid(pid, &status, WNOHANG);
      if (done == pid) {
        pid = -1;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGKILL);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    pid = -1;
  }
};

[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
  if (!condition) {
    fail(message);
  }
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

void write_file(const std::filesystem::path& path, std::string_view text) {
  std::ofstream output(path);
  output << text;
}

ChildProcess start_python(const std::string& python,
                          const std::filesystem::path& script,
                          const std::vector<std::string>& args) {
  const pid_t pid = ::fork();
  if (pid < 0) {
    fail("fork failed");
  }
  if (pid == 0) {
    std::vector<std::string> owned;
    owned.reserve(args.size() + 2U);
    owned.push_back(python);
    owned.push_back(script.string());
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1U);
    for (auto& item : owned) {
      argv.push_back(item.data());
    }
    argv.push_back(nullptr);
    ::execvp(python.c_str(), argv.data());
    _exit(127);
  }
  ChildProcess child;
  child.pid = pid;
  return child;
}

std::string wait_for_port_file(const std::filesystem::path& path) {
  for (int i = 0; i < 100; ++i) {
    if (std::filesystem::exists(path)) {
      const std::string text = read_file(path);
      if (!text.empty()) {
        return text;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  fail("timed out waiting for server port file: " + path.string());
}

qt::tools::ToolRequest request(Json arguments = Json::object()) {
  qt::tools::ToolRequest value;
  value.arguments = std::move(arguments);
  return value;
}

std::string result_text(const qt::tools::ToolResult& result) {
  if (result.output.is_object() && result.output.contains("text") &&
      result.output.at("text").is_string()) {
    return result.output.at("text").get<std::string>();
  }
  return result.output.dump();
}

std::vector<qt::tools::McpRemoteTool> discover(
    qt::tools::McpClientManager& manager) {
  std::vector<std::string> diagnostics;
  auto tools = manager.discover_tools(&diagnostics);
  if (tools.empty()) {
    std::ostringstream message;
    message << "MCP discovery returned no tools";
    for (const auto& diagnostic : diagnostics) {
      message << "\n" << diagnostic;
    }
    fail(message.str());
  }
  return tools;
}

std::string find_registered(const std::vector<qt::tools::McpRemoteTool>& tools,
                            const std::string& remote_name) {
  for (const auto& tool : tools) {
    if (tool.remote_name == remote_name) {
      return tool.registered_name;
    }
  }
  fail("missing remote MCP tool: " + remote_name);
}

void write_stdio_server(const std::filesystem::path& path) {
  write_file(path, R"PY(
import json
import os
import sys

mode = sys.argv[1]
state_file = sys.argv[2] if len(sys.argv) > 2 else ""

if mode == "flaky":
    count = 0
    if os.path.exists(state_file):
        count = int(open(state_file).read() or "0")
    count += 1
    open(state_file, "w").write(str(count))
    if count < 3:
        sys.exit(1)

def send(message):
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()

def result_for_tool(name, args):
    if name == "env":
        secret = os.environ.get("SECRET_SHOULD_NOT_PASS", "missing")
        allowed = os.environ.get("MCP_ALLOWED", "missing")
        return {"content": [{"type": "text", "text": f"secret={secret};allowed={allowed}"}]}
    if name == "sample":
        send({
            "jsonrpc": "2.0",
            "id": 900,
            "method": "sampling/createMessage",
            "params": {
                "messages": [{"role": "user", "content": {"type": "text", "text": "sample please"}}],
                "maxTokens": 8
            }
        })
        while True:
            line = sys.stdin.readline()
            if not line:
                return {"content": [{"type": "text", "text": "missing sampling response"}]}
            response = json.loads(line)
            if response.get("id") == 900:
                result = response.get("result", {})
                content = result.get("content", {})
                text = content.get("text", json.dumps(result))
                return {"content": [{"type": "text", "text": text}]}
    return {"content": [{"type": "text", "text": json.dumps(args, sort_keys=True)}]}

tools = [
    {"name": "env", "description": "Return filtered environment", "inputSchema": {"type": "object"}},
    {"name": "sample", "description": "Request sampling", "inputSchema": {"type": "object"}},
    {"name": "echo", "description": "Echo arguments", "inputSchema": {"type": "object"}}
]

for line in sys.stdin:
    if not line.strip():
        continue
    message = json.loads(line)
    method = message.get("method")
    if method == "initialize":
        send({"jsonrpc": "2.0", "id": message.get("id"), "result": {"protocolVersion": "2025-03-26", "capabilities": {}}})
    elif method == "notifications/initialized":
        continue
    elif method == "tools/list":
        send({"jsonrpc": "2.0", "id": message.get("id"), "result": {"tools": tools}})
    elif method == "tools/call":
        params = message.get("params", {})
        send({"jsonrpc": "2.0", "id": message.get("id"), "result": result_for_tool(params.get("name"), params.get("arguments", {}))})
    else:
        send({"jsonrpc": "2.0", "id": message.get("id"), "error": {"code": -32601, "message": "unknown"}})
)PY");
}

void write_http_sse_server(const std::filesystem::path& path) {
  write_file(path, R"PY(
import json
import queue
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

mode = sys.argv[1]
port_file = sys.argv[2]
events = queue.Queue()
pending_sample_call = {"id": None}
tools = [
    {"name": "echo", "description": "Echo arguments", "inputSchema": {"type": "object"}},
    {"name": "sample", "description": "Request sampling", "inputSchema": {"type": "object"}}
]

def rpc_response(message):
    method = message.get("method")
    if method == "initialize":
        return {"jsonrpc": "2.0", "id": message.get("id"), "result": {"protocolVersion": "2025-03-26", "capabilities": {}}}
    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": message.get("id"), "result": {"tools": tools}}
    if method == "tools/call":
        params = message.get("params", {})
        if mode == "sse" and params.get("name") == "sample":
            pending_sample_call["id"] = message.get("id")
            return {
                "jsonrpc": "2.0",
                "id": 901,
                "method": "sampling/createMessage",
                "params": {
                    "messages": [{"role": "user", "content": {"type": "text", "text": "sample via sse"}}],
                    "maxTokens": 8
                }
            }
        return {"jsonrpc": "2.0", "id": message.get("id"), "result": {"content": [{"type": "text", "text": json.dumps(params.get("arguments", {}), sort_keys=True)}]}}
    return {"jsonrpc": "2.0", "id": message.get("id"), "result": {}}

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if mode != "sse" or self.path != "/sse":
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(b"event: endpoint\ndata: /message\n\n")
        self.wfile.flush()
        while True:
            payload = events.get()
            self.wfile.write(("data: " + json.dumps(payload) + "\n\n").encode())
            self.wfile.flush()

    def do_POST(self):
        length = int(self.headers.get("content-length", "0"))
        message = json.loads(self.rfile.read(length) or b"{}")
        if mode == "sse" and message.get("id") == 901 and "result" in message:
            result = message.get("result", {})
            content = result.get("content", {})
            text = content.get("text", json.dumps(result))
            events.put({
                "jsonrpc": "2.0",
                "id": pending_sample_call["id"],
                "result": {"content": [{"type": "text", "text": text}]}
            })
            self.send_response(202)
            self.end_headers()
            return
        response = rpc_response(message)
        if mode == "sse":
            events.put(response)
            self.send_response(202)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("mcp-session-id", "probe-session")
        body = json.dumps(response).encode()
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
open(port_file, "w").write(str(server.server_port))
server.serve_forever()
)PY");
}

void test_stdio(const std::string& python,
                const std::filesystem::path& server_script) {
  ::setenv("SECRET_SHOULD_NOT_PASS", "sk-should-not-leak", 1);
  qt::tools::McpServerConfig config;
  config.name = "stdio-probe";
  config.transport = "stdio";
  config.command = python;
  config.args = {server_script.string(), "stdio"};
  config.env = {{"MCP_ALLOWED", "allowed"}};
  config.timeout_seconds = 5;
  config.connect_timeout_seconds = 5;

  qt::tools::McpClientManager manager({config});
  const auto tools = discover(manager);
  const auto env_result = manager.call_tool(
      "stdio-probe", "env", request(Json::object()));
  require(env_result.status == qt::tools::ToolResultStatus::success,
          "stdio env tool call failed");
  const std::string text = result_text(env_result);
  require(text.find("secret=missing") != std::string::npos,
          "stdio subprocess received filtered secret env: " + text);
  require(text.find("allowed=allowed") != std::string::npos,
          "stdio subprocess did not receive explicit env: " + text);
  (void)find_registered(tools, "env");
}

void test_retry(const std::string& python,
                const std::filesystem::path& server_script,
                const std::filesystem::path& state_file) {
  qt::tools::McpServerConfig config;
  config.name = "retry-probe";
  config.transport = "stdio";
  config.command = python;
  config.args = {server_script.string(), "flaky", state_file.string()};
  config.timeout_seconds = 5;
  config.connect_timeout_seconds = 5;

  qt::tools::McpClientManager manager({config});
  const auto tools = discover(manager);
  require(!tools.empty(), "retry probe did not discover tools");
  require(read_file(state_file) == "3",
          "retry probe did not reconnect with exponential attempts");
}

void test_sampling(const std::string& python,
                   const std::filesystem::path& server_script) {
  qt::tools::McpServerConfig config;
  config.name = "sampling-probe";
  config.transport = "stdio";
  config.command = python;
  config.args = {server_script.string(), "stdio"};
  config.timeout_seconds = 5;
  config.connect_timeout_seconds = 5;
  config.sampling = Json{{"enabled", true}};

  qt::tools::McpClientManager manager({config});
  manager.set_sampling_callback([](const qt::tools::McpSamplingRequest& request) {
    require(request.server_name == "sampling-probe",
            "sampling callback received wrong server");
    return qt::foundation::Result<Json>(Json{
        {"role", "assistant"},
        {"content", {{"type", "text"}, {"text", "sampled response"}}},
        {"model", "probe-model"},
    });
  });
  (void)discover(manager);
  const auto result = manager.call_tool("sampling-probe", "sample",
                                       request(Json::object()));
  require(result.status == qt::tools::ToolResultStatus::success,
          "sampling tool call failed");
  require(result_text(result).find("sampled response") != std::string::npos,
          "sampling response was not returned through MCP stdio");
}

void test_http(const std::string& python,
               const std::filesystem::path& server_script,
               const std::filesystem::path& port_file) {
  ChildProcess child = start_python(python, server_script,
                                    {"http", port_file.string()});
  const std::string port = wait_for_port_file(port_file);
  qt::tools::McpServerConfig config;
  config.name = "http-probe";
  config.transport = "streamable-http";
  config.url = "http://127.0.0.1:" + port + "/mcp";
  config.timeout_seconds = 5;
  config.connect_timeout_seconds = 5;

  qt::tools::McpClientManager manager({config});
  const auto tools = discover(manager);
  require(!tools.empty(), "HTTP probe did not discover tools");
  const auto result = manager.call_tool(
      "http-probe", "echo", request(Json{{"value", "http"}}));
  require(result.status == qt::tools::ToolResultStatus::success,
          "HTTP MCP tool call failed");
  require(result_text(result).find("http") != std::string::npos,
          "HTTP MCP tool result did not include arguments");
  child.stop();
}

void test_sse(const std::string& python,
              const std::filesystem::path& server_script,
              const std::filesystem::path& port_file) {
  ChildProcess child = start_python(python, server_script,
                                    {"sse", port_file.string()});
  const std::string port = wait_for_port_file(port_file);
  qt::tools::McpServerConfig config;
  config.name = "sse-probe";
  config.transport = "sse";
  config.url = "http://127.0.0.1:" + port + "/sse";
  config.timeout_seconds = 5;
  config.connect_timeout_seconds = 5;

  qt::tools::McpClientManager manager({config});
  manager.set_sampling_callback([](const qt::tools::McpSamplingRequest& request) {
    require(request.server_name == "sse-probe",
            "SSE sampling callback received wrong server");
    return qt::foundation::Result<Json>(Json{
        {"role", "assistant"},
        {"content", {{"type", "text"}, {"text", "sse sampled response"}}},
        {"model", "probe-model"},
    });
  });
  const auto tools = discover(manager);
  require(!tools.empty(), "SSE probe did not discover tools");
  const auto result = manager.call_tool(
      "sse-probe", "echo", request(Json{{"value", "sse"}}));
  require(result.status == qt::tools::ToolResultStatus::success,
          "SSE MCP tool call failed");
  require(result_text(result).find("sse") != std::string::npos,
          "SSE MCP tool result did not include arguments");
  const auto sampled = manager.call_tool("sse-probe", "sample",
                                        request(Json::object()));
  require(sampled.status == qt::tools::ToolResultStatus::success,
          "SSE MCP sampling tool call failed");
  require(result_text(sampled).find("sse sampled response") != std::string::npos,
          "SSE MCP sampling response was not returned");
  child.stop();
}

void test_redaction() {
  const std::string sanitized = qt::tools::sanitize_mcp_error(
      "failed Bearer abcdef token=mytoken key=mykey API_KEY=envvalue password=passvalue secret=secretvalue sk-live");
  require(sanitized.find("abcdef") == std::string::npos,
          "Bearer credential was not redacted");
  require(sanitized.find("mytoken") == std::string::npos,
          "token credential was not redacted");
  require(sanitized.find("mykey") == std::string::npos,
          "key credential was not redacted");
  require(sanitized.find("envvalue") == std::string::npos,
          "API_KEY credential was not redacted");
  require(sanitized.find("passvalue") == std::string::npos,
          "password credential was not redacted");
  require(sanitized.find("secretvalue") == std::string::npos,
          "secret credential was not redacted");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 3) {
      fail("usage: mcp_client_probe <python> <workdir>");
    }
    const std::string python = argv[1];
    const std::filesystem::path workdir = argv[2];
    std::filesystem::remove_all(workdir);
    std::filesystem::create_directories(workdir);
    const auto stdio_server = workdir / "mcp_stdio_server.py";
    const auto http_server = workdir / "mcp_http_sse_server.py";
    write_stdio_server(stdio_server);
    write_http_sse_server(http_server);

    test_redaction();
    test_stdio(python, stdio_server);
    test_retry(python, stdio_server, workdir / "retry-state.txt");
    test_sampling(python, stdio_server);
    test_http(python, http_server, workdir / "http-port.txt");
    test_sse(python, http_server, workdir / "sse-port.txt");
    std::cout << "mcp client probe passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "mcp client probe failed: " << error.what() << "\n";
    return 1;
  }
}
