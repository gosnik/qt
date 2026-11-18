#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace qt::tools {

using Json = nlohmann::json;

enum class ToolResultStatus {
    success,
    pending_approval,
    invalid_request,
    not_found,
    permission_denied,
    blocked_by_extension,
    execution_error,
};

const char* to_string(ToolResultStatus status) noexcept;

struct ToolDescriptor {
    std::string name;
    std::string description;
    Json input_schema = Json::object();
    bool deterministic = true;
    bool destructive = false;
    bool local_only = true;
};

struct ToolRequest {
    std::string tool_name;
    Json arguments = Json::object();
    std::optional<std::string> call_id;
    std::optional<std::string> approved_mutation_id;
};

struct ToolResult {
    ToolResultStatus status = ToolResultStatus::success;
    Json output = Json::object();
    std::optional<std::string> error_message;
    std::optional<std::string> call_id;
};

enum class ToolMutationKind {
    command,
    file_append,
    file_edit,
    file_write,
};

const char* to_string(ToolMutationKind kind) noexcept;

struct ToolMutation {
    std::string id;
    ToolMutationKind kind = ToolMutationKind::file_write;
    std::string tool_name;
    std::optional<std::string> call_id;
    std::string summary;
    Json arguments = Json::object();
};

class ToolMutationQueue {
  public:
    std::string enqueue(ToolMutation mutation);
    bool approve(const std::string& id);
    bool deny(const std::string& id);
    bool is_approved(const std::string& id) const;
    std::optional<ToolMutation> find(const std::string& id) const;
    std::vector<ToolMutation> pending() const;
    std::size_t size() const noexcept;

  private:
    std::vector<ToolMutation> pending_;
    std::vector<std::string> approved_;
    std::size_t next_id_ = 1U;
};

bool is_valid(const ToolDescriptor& descriptor);
bool is_valid(const ToolRequest& request);
bool is_success(const ToolResult& result) noexcept;
bool has_only_arguments(const ToolRequest& request,
                        std::initializer_list<const char*> names);

void to_json(Json& json, ToolDescriptor const& descriptor);
void from_json(Json const& json, ToolDescriptor& descriptor);

void to_json(Json& json, ToolRequest const& request);
void from_json(Json const& json, ToolRequest& request);

void to_json(Json& json, ToolResult const& result);
void from_json(Json const& json, ToolResult& result);

void to_json(Json& json, ToolMutation const& mutation);
void from_json(Json const& json, ToolMutation& mutation);

}  // namespace qt::tools
