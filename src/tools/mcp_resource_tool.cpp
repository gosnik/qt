#include "qt/tools/mcp_resource_tool.hpp"

#include "qt/tools/tool_validation.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace qt::tools {
namespace {

const Json* mcp_resources_metadata(const ToolContext& context) {
    const auto found = context.metadata.find("mcp_resources");
    if (found == context.metadata.end() || !found->is_object()) {
        return nullptr;
    }
    return &*found;
}

bool matches_server(const std::string& server, const std::optional<std::string>& filter) {
    return !filter || *filter == server;
}

std::optional<std::string> optional_string_arg(const ToolRequest& request,
                                               const char* name,
                                               const char* tool_name) {
    if (!request.arguments.contains(name)) {
        return std::nullopt;
    }
    if (!request.arguments.at(name).is_string()) {
        throw std::invalid_argument(std::string(tool_name) + " requires string " + name);
    }
    const auto value = request.arguments.at(name).get<std::string>();
    return value.empty() ? std::optional<std::string>{} : std::optional<std::string>{value};
}

std::optional<std::string> optional_string_alias_arg(
    const ToolRequest& request, const char* preferred, const char* alias,
    const char* tool_name) {
    const auto preferred_value = optional_string_arg(request, preferred, tool_name);
    const auto alias_value = optional_string_arg(request, alias, tool_name);
    if (preferred_value && alias_value && *preferred_value != *alias_value) {
        throw std::invalid_argument(std::string(tool_name) + " accepts " +
                                    preferred + " or " + alias + ", not both");
    }
    return preferred_value ? preferred_value : alias_value;
}

Json normalize_resource(const std::string& server, Json resource) {
    if (!resource.is_object()) {
        resource = Json{{"uri", resource.dump()}};
    }
    resource["server"] = server;
    return resource;
}

Json resource_contents_for(const Json& server_resources, const std::string& uri) {
    const auto contents = server_resources.find("contents");
    if (contents != server_resources.end() && contents->is_object()) {
        const auto content = contents->find(uri);
        if (content != contents->end()) {
            if (content->is_object() && content->contains("contents")) {
                return *content;
            }
            if (content->is_array()) {
                return Json{{"contents", *content}};
            }
            if (content->is_string()) {
                return Json{{"contents", Json::array({Json{{"type", "text"},
                                                           {"text", content->get<std::string>()},
                                                           {"uri", uri}}})}};
            }
            return Json{{"contents", Json::array({Json{{"type", "json"},
                                                       {"json", *content},
                                                       {"uri", uri}}})}};
        }
    }
    const auto resources = server_resources.find("resources");
    if (resources != server_resources.end() && resources->is_array()) {
        for (const auto& resource : *resources) {
            if (!resource.is_object() || !resource.contains("uri") ||
                !resource.at("uri").is_string() ||
                resource.at("uri").get<std::string>() != uri) {
                continue;
            }
            if (resource.contains("contents")) {
                return Json{{"contents", resource.at("contents")}};
            }
            if (resource.contains("text") && resource.at("text").is_string()) {
                return Json{{"contents", Json::array({Json{{"type", "text"},
                                                           {"text", resource.at("text")},
                                                           {"uri", uri}}})}};
            }
            return Json{{"contents", Json::array({Json{{"type", "json"},
                                                       {"json", resource},
                                                       {"uri", uri}}})}};
        }
    }
    return Json();
}

}  // namespace

ListMcpResourcesTool::ListMcpResourcesTool() {
    descriptor_.name = "list_mcp_resources";
    descriptor_.description =
        "List MCP resources exposed by integrated MCP servers.";
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {
             {"server",
              {
                  {"type", "string"},
                  {"description", "Optional MCP server name."},
              }},
             {"mcp_name",
              {
                  {"type", "string"},
                  {"description", "Crush-compatible alias for server."},
              }},
             {"cursor",
              {
                  {"type", "string"},
                  {"description", "Optional server-specific pagination cursor."},
              }},
         }},
        {"additionalProperties", false},
    };
}

const ToolDescriptor& ListMcpResourcesTool::descriptor() const { return descriptor_; }

ToolResult ListMcpResourcesTool::invoke(const ToolRequest& request,
                                        const ToolContext& context) const {
    if (!is_valid(request)) {
        return make_invalid_request("invalid tool request", request);
    }
    if (request.tool_name != descriptor_.name) {
        return make_invalid_request(
            "tool request name does not match list_mcp_resources", request);
    }
    if (!has_only_arguments(request, {"server", "mcp_name", "cursor"})) {
        return make_invalid_request(
            "list_mcp_resources accepts only server, mcp_name, and cursor arguments", request);
    }

    std::optional<std::string> server_filter;
    std::optional<std::string> cursor;
    try {
        server_filter = optional_string_alias_arg(request, "server", "mcp_name",
                                                 descriptor_.name.c_str());
        cursor = optional_string_arg(request, "cursor", descriptor_.name.c_str());
    } catch (const std::invalid_argument& error) {
        return make_invalid_request(error.what(), request);
    }

    const Json* metadata = mcp_resources_metadata(context);
    Json resources = Json::array();
    if (metadata != nullptr) {
        std::vector<std::string> servers;
        for (const auto& item : metadata->items()) {
            servers.push_back(item.key());
        }
        std::sort(servers.begin(), servers.end());
        for (const auto& server : servers) {
            if (!matches_server(server, server_filter)) {
                continue;
            }
            const auto& value = metadata->at(server);
            const Json* server_resources = &value;
            if (value.is_object() && value.contains("resources")) {
                server_resources = &value.at("resources");
            }
            if (!server_resources->is_array()) {
                continue;
            }
            for (const auto& resource : *server_resources) {
                resources.push_back(normalize_resource(server, resource));
            }
        }
    }

    if (server_filter && metadata != nullptr && !metadata->contains(*server_filter)) {
        return make_execution_error(ToolResultStatus::not_found,
                                    "MCP server not found: " + *server_filter,
                                    request);
    }

    ToolResult result;
    result.status = ToolResultStatus::success;
    result.call_id = request.call_id;
    result.output = Json{{"server", server_filter ? Json(*server_filter) : Json(nullptr)},
                         {"resources", std::move(resources)},
                         {"nextCursor", nullptr},
                         {"cursor", cursor ? Json(*cursor) : Json(nullptr)}};
    result.output["text"] =
        result.output.at("resources").empty()
            ? "No MCP resources available"
            : result.output.at("resources").dump(2);
    return result;
}

ReadMcpResourceTool::ReadMcpResourceTool() {
    descriptor_.name = "read_mcp_resource";
    descriptor_.description = "Read a resource from an integrated MCP server.";
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {
             {"server",
              {
                  {"type", "string"},
                  {"description", "MCP server name."},
              }},
             {"mcp_name",
              {
                  {"type", "string"},
                  {"description", "Crush-compatible alias for server."},
              }},
             {"uri",
              {
                  {"type", "string"},
                  {"description", "Resource URI to read."},
              }},
         }},
        {"required", Json::array({"server", "uri"})},
        {"additionalProperties", false},
    };
}

const ToolDescriptor& ReadMcpResourceTool::descriptor() const { return descriptor_; }

ToolResult ReadMcpResourceTool::invoke(const ToolRequest& request,
                                       const ToolContext& context) const {
    if (!is_valid(request)) {
        return make_invalid_request("invalid tool request", request);
    }
    if (request.tool_name != descriptor_.name) {
        return make_invalid_request(
            "tool request name does not match read_mcp_resource", request);
    }
    if (!has_only_arguments(request, {"server", "mcp_name", "uri"})) {
        return make_invalid_request(
            "read_mcp_resource accepts only server, mcp_name, and uri arguments", request);
    }
    std::optional<std::string> server;
    try {
        server = optional_string_alias_arg(request, "server", "mcp_name",
                                          descriptor_.name.c_str());
    } catch (const std::invalid_argument& error) {
        return make_invalid_request(error.what(), request);
    }
    const auto uri = require_string_argument(request, "uri", descriptor_.name);
    if (!uri) {
        return make_invalid_request(uri.error().summary(), request);
    }
    if (!server || server->empty() || uri.value().empty()) {
        return make_invalid_request("read_mcp_resource requires non-empty server and uri",
                                    request);
    }

    const Json* metadata = mcp_resources_metadata(context);
    if (metadata == nullptr || !metadata->contains(*server)) {
        return make_execution_error(ToolResultStatus::not_found,
                                    "MCP server not found: " + *server,
                                    request);
    }
    const Json payload = resource_contents_for(metadata->at(*server), uri.value());
    if (payload.is_null()) {
        return make_execution_error(ToolResultStatus::not_found,
                                    "MCP resource not found: " + uri.value(),
                                    request);
    }

    ToolResult result;
    result.status = ToolResultStatus::success;
    result.call_id = request.call_id;
    result.output = payload;
    result.output["server"] = *server;
    result.output["uri"] = uri.value();
    result.output["text"] = payload.dump(2);
    return result;
}

}  // namespace qt::tools
