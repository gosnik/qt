#pragma once

#include "qt/foundation/error.hpp"
#include "qt/tools/registry.hpp"
#include "qt/tools/tool.hpp"

#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace qt::tools {

struct McpServerConfig {
  std::string name;
  bool enabled = true;
  std::string transport = "stdio";
  std::optional<std::string> command;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  std::optional<std::string> url;
  std::map<std::string, std::string> headers;
  Json sampling = Json::object();
  int timeout_seconds = 120;
  int connect_timeout_seconds = 60;
  std::string stderr_log_path;
};

struct McpSamplingRequest {
  std::string server_name;
  Json params = Json::object();
  Json config = Json::object();
};

using McpSamplingCallback =
    std::function<foundation::Result<Json>(const McpSamplingRequest&)>;

struct McpRemoteTool {
  std::string server_name;
  std::string remote_name;
  std::string registered_name;
  std::string description;
  Json input_schema = Json{{"type", "object"}, {"additionalProperties", true}};
};

std::string sanitize_mcp_error(std::string message);
std::string mcp_registered_tool_name(std::string server_name,
                                     std::string remote_name);

class McpClientManager : public std::enable_shared_from_this<McpClientManager> {
 public:
  explicit McpClientManager(std::vector<McpServerConfig> servers);
  ~McpClientManager();

  McpClientManager(const McpClientManager&) = delete;
  McpClientManager& operator=(const McpClientManager&) = delete;

  [[nodiscard]] std::vector<McpRemoteTool> discover_tools(
      std::vector<std::string>* diagnostics);

  void set_sampling_callback(McpSamplingCallback callback);

  ToolResult call_tool(const std::string& server_name,
                       const std::string& remote_tool_name,
                       const ToolRequest& request) const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

class McpProxyTool final : public Tool {
 public:
  McpProxyTool(McpRemoteTool remote_tool,
               std::shared_ptr<McpClientManager> manager);

  const ToolDescriptor& descriptor() const override;
  ToolResult invoke(const ToolRequest& request,
                    const ToolContext& context) const override;

 private:
  McpRemoteTool remote_tool_;
  std::shared_ptr<McpClientManager> manager_;
  ToolDescriptor descriptor_;
};

void register_mcp_tools(ToolRegistry& registry,
                        const std::shared_ptr<McpClientManager>& manager,
                        std::vector<std::string>* diagnostics,
                        std::function<bool(const std::string&)> enabled = {});

}  // namespace qt::tools
