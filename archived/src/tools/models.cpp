#include "qt/tools/models.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string>

namespace qt::tools {

namespace {

std::optional<ToolResultStatus> parse_status(const std::string& value) {
    static constexpr std::array<std::pair<const char*, ToolResultStatus>, 7> kStatuses{{
        {"success", ToolResultStatus::success},
        {"pending_approval", ToolResultStatus::pending_approval},
        {"invalid_request", ToolResultStatus::invalid_request},
        {"not_found", ToolResultStatus::not_found},
        {"permission_denied", ToolResultStatus::permission_denied},
        {"blocked_by_extension", ToolResultStatus::blocked_by_extension},
        {"execution_error", ToolResultStatus::execution_error},
    }};

    for (const auto& entry : kStatuses) {
        if (value == entry.first) {
            return entry.second;
        }
    }
    return std::nullopt;
}

std::optional<ToolMutationKind> parse_mutation_kind(const std::string& value) {
    static constexpr std::array<std::pair<const char*, ToolMutationKind>, 4> kKinds{{
        {"command", ToolMutationKind::command},
        {"file_append", ToolMutationKind::file_append},
        {"file_edit", ToolMutationKind::file_edit},
        {"file_write", ToolMutationKind::file_write},
    }};

    for (const auto& entry : kKinds) {
        if (value == entry.first) {
            return entry.second;
        }
    }
    return std::nullopt;
}

}  // namespace

const char* to_string(ToolResultStatus status) noexcept {
    switch (status) {
        case ToolResultStatus::success:
            return "success";
        case ToolResultStatus::pending_approval:
            return "pending_approval";
        case ToolResultStatus::invalid_request:
            return "invalid_request";
        case ToolResultStatus::not_found:
            return "not_found";
        case ToolResultStatus::permission_denied:
            return "permission_denied";
        case ToolResultStatus::blocked_by_extension:
            return "blocked_by_extension";
        case ToolResultStatus::execution_error:
            return "execution_error";
    }
    return "execution_error";
}

const char* to_string(ToolMutationKind kind) noexcept {
    switch (kind) {
        case ToolMutationKind::command:
            return "command";
        case ToolMutationKind::file_append:
            return "file_append";
        case ToolMutationKind::file_edit:
            return "file_edit";
        case ToolMutationKind::file_write:
            return "file_write";
    }
    return "file_write";
}

std::string ToolMutationQueue::enqueue(ToolMutation mutation) {
    if (mutation.id.empty()) {
        mutation.id = "mutation-" + std::to_string(next_id_++);
    }
    const std::string id = mutation.id;
    pending_.push_back(std::move(mutation));
    return id;
}

bool ToolMutationQueue::approve(const std::string& id) {
    const auto iterator = std::find_if(
        pending_.begin(), pending_.end(),
        [&id](const ToolMutation& mutation) { return mutation.id == id; });
    if (iterator == pending_.end()) {
        return false;
    }

    approved_.push_back(id);
    pending_.erase(iterator);
    return true;
}

bool ToolMutationQueue::deny(const std::string& id) {
    const auto iterator = std::find_if(
        pending_.begin(), pending_.end(),
        [&id](const ToolMutation& mutation) { return mutation.id == id; });
    if (iterator == pending_.end()) {
        return false;
    }

    pending_.erase(iterator);
    return true;
}

bool ToolMutationQueue::is_approved(const std::string& id) const {
    return std::find(approved_.begin(), approved_.end(), id) != approved_.end();
}

std::optional<ToolMutation> ToolMutationQueue::find(const std::string& id) const {
    const auto iterator = std::find_if(
        pending_.begin(), pending_.end(),
        [&id](const ToolMutation& mutation) { return mutation.id == id; });
    if (iterator == pending_.end()) {
        return std::nullopt;
    }
    return *iterator;
}

std::vector<ToolMutation> ToolMutationQueue::pending() const { return pending_; }

std::size_t ToolMutationQueue::size() const noexcept { return pending_.size(); }

bool is_valid(const ToolDescriptor& descriptor) {
    return !descriptor.name.empty() && !descriptor.description.empty() &&
           descriptor.input_schema.is_object();
}

bool is_valid(const ToolRequest& request) {
    return !request.tool_name.empty() && request.arguments.is_object();
}

bool is_success(const ToolResult& result) noexcept {
    return result.status == ToolResultStatus::success;
}

bool has_only_arguments(const ToolRequest& request,
                        std::initializer_list<const char*> names) {
    if (!request.arguments.is_object()) {
        return false;
    }

    std::set<std::string> allowed;
    for (const char* name : names) {
        if (name == nullptr || *name == '\0') {
            return false;
        }
        allowed.emplace(name);
    }

    for (const auto& item : request.arguments.items()) {
        if (allowed.find(item.key()) == allowed.end()) {
            return false;
        }
    }

    return true;
}

void to_json(Json& json, ToolDescriptor const& descriptor) {
    json = Json{
        {"name", descriptor.name},
        {"description", descriptor.description},
        {"input_schema", descriptor.input_schema},
        {"deterministic", descriptor.deterministic},
        {"destructive", descriptor.destructive},
        {"local_only", descriptor.local_only},
    };
}

void from_json(Json const& json, ToolDescriptor& descriptor) {
    descriptor.name = json.value("name", "");
    descriptor.description = json.value("description", "");
    descriptor.input_schema = json.value("input_schema", Json::object());
    descriptor.deterministic = json.value("deterministic", true);
    descriptor.destructive = json.value("destructive", false);
    descriptor.local_only = json.value("local_only", true);
}

void to_json(Json& json, ToolRequest const& request) {
    json = Json{
        {"tool_name", request.tool_name},
        {"arguments", request.arguments},
    };
    if (request.call_id) {
        json["call_id"] = *request.call_id;
    }
    if (request.approved_mutation_id) {
        json["approved_mutation_id"] = *request.approved_mutation_id;
    }
}

void from_json(Json const& json, ToolRequest& request) {
    request.tool_name = json.value("tool_name", "");
    request.arguments = json.value("arguments", Json::object());
    if (json.contains("call_id") && !json.at("call_id").is_null()) {
        request.call_id = json.at("call_id").get<std::string>();
    } else {
        request.call_id.reset();
    }
    if (json.contains("approved_mutation_id") &&
        !json.at("approved_mutation_id").is_null()) {
        request.approved_mutation_id =
            json.at("approved_mutation_id").get<std::string>();
    } else {
        request.approved_mutation_id.reset();
    }
}

void to_json(Json& json, ToolResult const& result) {
    json = Json{
        {"status", to_string(result.status)},
        {"output", result.output},
    };
    if (result.error_message) {
        json["error_message"] = *result.error_message;
    }
    if (result.call_id) {
        json["call_id"] = *result.call_id;
    }
}

void from_json(Json const& json, ToolResult& result) {
    const auto status_text = json.value("status", "execution_error");
    result.status = parse_status(status_text).value_or(ToolResultStatus::execution_error);
    result.output = json.value("output", Json::object());
    if (json.contains("error_message") && !json.at("error_message").is_null()) {
        result.error_message = json.at("error_message").get<std::string>();
    } else {
        result.error_message.reset();
    }
    if (json.contains("call_id") && !json.at("call_id").is_null()) {
        result.call_id = json.at("call_id").get<std::string>();
    } else {
        result.call_id.reset();
    }
}

void to_json(Json& json, ToolMutation const& mutation) {
    json = Json{
        {"id", mutation.id},
        {"kind", to_string(mutation.kind)},
        {"tool_name", mutation.tool_name},
        {"summary", mutation.summary},
        {"arguments", mutation.arguments},
    };
    if (mutation.call_id) {
        json["call_id"] = *mutation.call_id;
    }
}

void from_json(Json const& json, ToolMutation& mutation) {
    mutation.id = json.value("id", "");
    mutation.kind = parse_mutation_kind(json.value("kind", "file_write"))
                        .value_or(ToolMutationKind::file_write);
    mutation.tool_name = json.value("tool_name", "");
    mutation.summary = json.value("summary", "");
    mutation.arguments = json.value("arguments", Json::object());
    if (json.contains("call_id") && !json.at("call_id").is_null()) {
        mutation.call_id = json.at("call_id").get<std::string>();
    } else {
        mutation.call_id.reset();
    }
}

}  // namespace qt::tools
