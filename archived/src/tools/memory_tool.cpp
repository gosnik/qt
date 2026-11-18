#include "qt/tools/memory_tool.hpp"

#include "qt/memory/runtime.hpp"
#include "qt/memory/review_json.hpp"
#include "qt/tools/tool_validation.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace qt::tools {
namespace {

Json memory_json(const qt::memory::MemoryEntry& memory) {
  return Json{{"id", memory.id},
              {"type", qt::memory::to_string(memory.type)},
              {"status", qt::memory::to_string(memory.status)},
              {"content", memory.content},
              {"source", memory.source},
              {"tags", memory.tags},
              {"confidence", memory.confidence},
              {"quality_score", memory.quality_score},
              {"access_count", memory.access_count},
              {"success_count", memory.success_count},
              {"failure_count", memory.failure_count},
              {"last_outcome", qt::memory::to_string(memory.last_outcome)},
              {"source_uri", memory.provenance.source_uri},
              {"created_by", memory.provenance.created_by},
              {"evidence_strength", memory.provenance.evidence_strength}};
}

ToolResult exception_result(const ToolRequest& request,
                            const std::exception& error) {
  return make_execution_error(ToolResultStatus::execution_error, error.what(),
                              request);
}

std::vector<std::string> string_array(const Json& value) {
  if (!value.is_array()) {
    throw std::invalid_argument("tags must be an array of strings");
  }
  std::vector<std::string> result;
  result.reserve(value.size());
  for (const auto& item : value) {
    if (!item.is_string()) {
      throw std::invalid_argument("tags must be an array of strings");
    }
    result.push_back(item.get<std::string>());
  }
  return result;
}

class MemoryRecallTool final : public Tool {
 public:
  explicit MemoryRecallTool(std::shared_ptr<qt::memory::MemoryRuntime> runtime)
      : runtime_(std::move(runtime)) {
    descriptor_.name = "memory_recall";
    descriptor_.description =
        "Recall governed built-in memories for the current repository scope.";
    descriptor_.input_schema =
        Json{{"type", "object"},
             {"properties",
              {{"query", {{"type", "string"}}},
               {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 50}}},
               {"include_candidates", {{"type", "boolean"}}},
               {"include_global_user", {{"type", "boolean"}}},
               {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
             {"required", Json::array({"query"})},
             {"additionalProperties", false}};
  }

  const ToolDescriptor& descriptor() const override { return descriptor_; }

  ToolResult invoke(const ToolRequest& request,
                    const ToolContext&) const override {
    if (!is_valid(request) || request.tool_name != descriptor_.name ||
        !has_only_arguments(request, {"query", "limit", "include_candidates",
                                      "include_global_user", "tags"}) ||
        !request.arguments.contains("query") ||
        !request.arguments.at("query").is_string()) {
      return make_invalid_request(
          "memory_recall requires a string query and accepts only query, limit, "
          "include_candidates, include_global_user, and tags",
          request);
    }
    try {
      qt::memory::RecallRequest recall;
      recall.query = request.arguments.at("query").get<std::string>();
      recall.result_limit = request.arguments.value("limit", std::size_t{10});
      recall.candidate_limit = std::max<std::size_t>(recall.result_limit, 20U);
      recall.include_candidates =
          request.arguments.value("include_candidates", true);
      recall.include_global_user =
          request.arguments.value("include_global_user", false);
      if (request.arguments.contains("tags")) {
        recall.tags = string_array(request.arguments.at("tags"));
      }
      if (recall.result_limit == 0U || recall.result_limit > 50U) {
        return make_invalid_request("memory_recall limit must be from 1 to 50",
                                    request);
      }
      const auto recalled = runtime_->recall().recall(runtime_->scope(), recall);
      runtime_->recall().record_access(runtime_->scope(), recalled);
      Json values = Json::array();
      for (const auto& item : recalled) {
        auto value = memory_json(item.memory);
        value["score"] = item.score.total;
        value["warnings"] = item.warnings;
        values.push_back(std::move(value));
      }
      ToolResult result;
      result.call_id = request.call_id;
      result.output = Json{{"memories", std::move(values)},
                           {"count", recalled.size()},
                           {"scope_repository", runtime_->scope().repository}};
      return result;
    } catch (const std::exception& error) {
      return exception_result(request, error);
    }
  }

 private:
  std::shared_ptr<qt::memory::MemoryRuntime> runtime_;
  ToolDescriptor descriptor_;
};

class MemoryRememberTool final : public Tool {
 public:
  explicit MemoryRememberTool(std::shared_ptr<qt::memory::MemoryRuntime> runtime)
      : runtime_(std::move(runtime)) {
    descriptor_.name = "memory_remember";
    descriptor_.description =
        "Create a reviewable candidate or hypothesis in built-in memory.";
    descriptor_.destructive = true;
    descriptor_.input_schema =
        Json{{"type", "object"},
             {"properties",
              {{"content", {{"type", "string"}, {"minLength", 1}}},
               {"type", {{"type", "string"}}},
               {"status", {{"type", "string"},
                            {"enum", Json::array({"candidate", "hypothesis"})}}},
               {"source", {{"type", "string"}}},
               {"source_uri", {{"type", "string"}}},
               {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}},
               {"confidence", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}}},
             {"required", Json::array({"content"})},
             {"additionalProperties", false}};
  }

  const ToolDescriptor& descriptor() const override { return descriptor_; }

  ToolResult invoke(const ToolRequest& request,
                    const ToolContext& context) const override {
    if (!is_valid(request) || request.tool_name != descriptor_.name ||
        !has_only_arguments(request, {"content", "type", "status", "source",
                                      "source_uri", "tags", "confidence"}) ||
        !request.arguments.contains("content") ||
        !request.arguments.at("content").is_string() ||
        request.arguments.at("content").get<std::string>().empty()) {
      return make_invalid_request(
          "memory_remember requires non-empty content and accepts only content, "
          "type, status, source, source_uri, tags, and confidence",
          request);
    }
    const std::string status =
        request.arguments.value("status", std::string("hypothesis"));
    if (status != "candidate" && status != "hypothesis") {
      return make_invalid_request(
          "memory_remember status must be candidate or hypothesis", request);
    }
    if (auto queued = require_mutation_approval(
            ToolMutationKind::command, "create reviewable built-in memory",
            request, context)) {
      return *queued;
    }
    try {
      qt::memory::MemoryWrite write;
      write.content = request.arguments.at("content").get<std::string>();
      write.type = qt::memory::memory_type_from_string(
          request.arguments.value("type", std::string("observation")));
      write.status = qt::memory::memory_status_from_string(status);
      write.source = request.arguments.value("source", std::string("agent:tool"));
      write.confidence = request.arguments.value("confidence", 0.5);
      write.provenance.source_type = qt::memory::SourceType::Chat;
      write.provenance.source_uri =
          request.arguments.value("source_uri", std::string{});
      write.provenance.created_by = "agent";
      if (request.arguments.contains("tags")) {
        write.tags = string_array(request.arguments.at("tags"));
      }
      const auto id = runtime_->memories().remember(runtime_->scope(), write);
      ToolResult result;
      result.call_id = request.call_id;
      result.output = Json{{"id", id}, {"status", status}, {"created", true}};
      return result;
    } catch (const std::invalid_argument& error) {
      return make_invalid_request(error.what(), request);
    } catch (const std::exception& error) {
      return exception_result(request, error);
    }
  }

 private:
  std::shared_ptr<qt::memory::MemoryRuntime> runtime_;
  ToolDescriptor descriptor_;
};

class MemoryForgetTool final : public Tool {
 public:
  explicit MemoryForgetTool(std::shared_ptr<qt::memory::MemoryRuntime> runtime)
      : runtime_(std::move(runtime)) {
    descriptor_.name = "memory_forget";
    descriptor_.description =
        "Permanently delete one in-scope memory after explicit confirmation and approval.";
    descriptor_.destructive = true;
    descriptor_.input_schema =
        Json{{"type", "object"},
             {"properties", {{"id", {{"type", "string"}, {"minLength", 1}}},
                             {"confirm", {{"type", "boolean"}}}}},
             {"required", Json::array({"id", "confirm"})},
             {"additionalProperties", false}};
  }

  const ToolDescriptor& descriptor() const override { return descriptor_; }

  ToolResult invoke(const ToolRequest& request,
                    const ToolContext& context) const override {
    if (!is_valid(request) || request.tool_name != descriptor_.name ||
        !has_only_arguments(request, {"id", "confirm"}) ||
        !request.arguments.contains("id") ||
        !request.arguments.at("id").is_string() ||
        request.arguments.at("id").get<std::string>().empty() ||
        !request.arguments.value("confirm", false)) {
      return make_invalid_request(
          "memory_forget requires a non-empty id and confirm=true", request);
    }
    const auto id = request.arguments.at("id").get<std::string>();
    if (auto queued = require_mutation_approval(
            ToolMutationKind::command, "permanently forget memory " + id,
            request, context)) {
      return *queued;
    }
    try {
      const bool removed = runtime_->memories().forget(runtime_->scope(), id);
      ToolResult result;
      result.call_id = request.call_id;
      result.status = removed ? ToolResultStatus::success
                              : ToolResultStatus::not_found;
      result.output = Json{{"id", id}, {"forgotten", removed}};
      if (!removed) result.error_message = "memory not found in current scope";
      return result;
    } catch (const std::exception& error) {
      return exception_result(request, error);
    }
  }

 private:
  std::shared_ptr<qt::memory::MemoryRuntime> runtime_;
  ToolDescriptor descriptor_;
};

class MemoryReviewListTool final : public Tool {
 public:
  explicit MemoryReviewListTool(
      std::shared_ptr<qt::memory::MemoryRuntime> runtime)
      : runtime_(std::move(runtime)) {
    descriptor_.name = "memory_review_list";
    descriptor_.description =
        "List the indexed, priority-ordered memory review queue. Use this "
        "instead of text search for review IDs, kinds, priority, status, or tags.";
    descriptor_.input_schema =
        Json{{"type", "object"},
             {"properties",
              {{"statuses", {{"type", "array"}, {"items", {{"type", "string"}}}}},
               {"kinds", {{"type", "array"}, {"items", {{"type", "string"}}}}},
               {"priorities", {{"type", "array"}, {"items", {{"type", "string"}}}}},
               {"source_types", {{"type", "array"}, {"items", {{"type", "string"}}}}},
               {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}},
               {"dream_cycles", {{"type", "array"}, {"items", {{"type", "string"}}}}},
               {"scope", {{"type", "string"},
                          {"enum", {"all", "exact", "durable"}}}},
               {"stale", {{"type", "boolean"}}},
               {"conflict", {{"type", "boolean"}}},
               {"created_after", {{"type", "integer"}}},
               {"created_before", {{"type", "integer"}}},
               {"decided_after", {{"type", "integer"}}},
               {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
               {"offset", {{"type", "integer"}, {"minimum", 0}}},
               {"oldest_first", {{"type", "boolean"}}}}},
             {"additionalProperties", false}};
  }

  const ToolDescriptor& descriptor() const override { return descriptor_; }

  ToolResult invoke(const ToolRequest& request,
                    const ToolContext&) const override {
    if (!is_valid(request) || request.tool_name != descriptor_.name ||
        !has_only_arguments(request, {"statuses", "kinds", "priorities",
                                      "source_types", "tags", "dream_cycles",
                                      "scope", "stale", "conflict",
                                      "created_after", "created_before",
                                      "decided_after", "limit", "offset",
                                      "oldest_first"})) {
      return make_invalid_request("invalid memory_review_list request", request);
    }
    try {
      auto query = qt::memory::review_query_from_json(request.arguments);
      if (query.limit > 100U) {
        return make_invalid_request("memory_review_list limit must be from 1 to 100",
                                    request);
      }
      const auto reviews = runtime_->review_items(query);
      Json items = Json::array();
      for (const auto& review : reviews) {
        items.push_back(qt::memory::review_item_json(review));
      }
      ToolResult result;
      result.call_id = request.call_id;
      result.output = Json{{"items", std::move(items)}, {"count", reviews.size()}};
      return result;
    } catch (const std::invalid_argument& error) {
      return make_invalid_request(error.what(), request);
    } catch (const std::exception& error) {
      return exception_result(request, error);
    }
  }

 private:
  std::shared_ptr<qt::memory::MemoryRuntime> runtime_;
  ToolDescriptor descriptor_;
};

class MemoryReviewShowTool final : public Tool {
 public:
  explicit MemoryReviewShowTool(std::shared_ptr<qt::memory::MemoryRuntime> runtime)
      : runtime_(std::move(runtime)) {
    descriptor_.name = "memory_review_show";
    descriptor_.description =
        "Hydrate one indexed memory review with its typed candidate, governed "
        "subject, sources, graph context, recall impact, and risk explanation.";
    descriptor_.input_schema =
        Json{{"type", "object"},
             {"properties", {{"id", {{"type", "string"}}}}},
             {"required", Json::array({"id"})},
             {"additionalProperties", false}};
  }

  const ToolDescriptor& descriptor() const override { return descriptor_; }

  ToolResult invoke(const ToolRequest& request, const ToolContext&) const override {
    if (!is_valid(request) || request.tool_name != descriptor_.name ||
        !has_only_arguments(request, {"id"}) ||
        !request.arguments.contains("id") || !request.arguments.at("id").is_string()) {
      return make_invalid_request("memory_review_show requires string id", request);
    }
    try {
      ToolResult result;
      result.call_id = request.call_id;
      result.output = qt::memory::hydrated_review_json(
          runtime_->review_detail(request.arguments.at("id").get<std::string>()));
      return result;
    } catch (const std::invalid_argument& error) {
      return make_invalid_request(error.what(), request);
    } catch (const std::exception& error) {
      return exception_result(request, error);
    }
  }

 private:
  std::shared_ptr<qt::memory::MemoryRuntime> runtime_;
  ToolDescriptor descriptor_;
};

class MemoryReviewPreviewTool final : public Tool {
 public:
  explicit MemoryReviewPreviewTool(std::shared_ptr<qt::memory::MemoryRuntime> runtime)
      : runtime_(std::move(runtime)) {
    descriptor_.name = "memory_review_preview";
    descriptor_.description =
        "Prepare a non-mutating governed review action preview. This can suggest "
        "an outcome but cannot authorize or apply it.";
    descriptor_.input_schema =
        Json{{"type", "object"},
             {"properties",
              {{"id", {{"type", "string"}}},
               {"action", {{"type", "string"}}},
               {"note", {{"type", "string"}}},
               {"target_id", {{"type", "string"}}},
               {"evidence_uri", {{"type", "string"}}},
               {"deferred_until", {{"type", "integer"}}},
               {"edit", {{"type", "object"}}}}},
             {"required", Json::array({"id", "action"})},
             {"additionalProperties", false}};
  }

  const ToolDescriptor& descriptor() const override { return descriptor_; }

  ToolResult invoke(const ToolRequest& request, const ToolContext&) const override {
    if (!is_valid(request) || request.tool_name != descriptor_.name ||
        !has_only_arguments(request, {"id", "action", "note", "target_id",
                                      "evidence_uri", "deferred_until", "edit"}) ||
        !request.arguments.contains("id") || !request.arguments.at("id").is_string()) {
      return make_invalid_request(
          "memory_review_preview requires string id and action", request);
    }
    try {
      const auto id = request.arguments.at("id").get<std::string>();
      auto action = qt::memory::review_action_from_json(request.arguments,
                                                        "agent:suggestion", false);
      ToolResult result;
      result.call_id = request.call_id;
      result.output = qt::memory::review_preview_json(
          runtime_->preview_review(id, std::move(action)));
      return result;
    } catch (const std::invalid_argument& error) {
      return make_invalid_request(error.what(), request);
    } catch (const std::exception& error) {
      return exception_result(request, error);
    }
  }

 private:
  std::shared_ptr<qt::memory::MemoryRuntime> runtime_;
  ToolDescriptor descriptor_;
};

class MemoryReviewApplyTool final : public Tool {
 public:
  explicit MemoryReviewApplyTool(std::shared_ptr<qt::memory::MemoryRuntime> runtime)
      : runtime_(std::move(runtime)) {
    descriptor_.name = "memory_review_apply";
    descriptor_.description =
        "Apply one typed memory review action only after explicit user approval. "
        "Use memory_review_preview first and pass its revision and token.";
    descriptor_.destructive = true;
    descriptor_.input_schema =
        Json{{"type", "object"},
             {"properties",
              {{"id", {{"type", "string"}}},
               {"action", {{"type", "string"}}},
               {"note", {{"type", "string"}}},
               {"target_id", {{"type", "string"}}},
               {"evidence_uri", {{"type", "string"}}},
               {"deferred_until", {{"type", "integer"}}},
               {"expected_revision", {{"type", "integer"}}},
               {"idempotency_key", {{"type", "string"}}},
               {"edit", {{"type", "object"}}}}},
             {"required", Json::array({"id", "action", "expected_revision",
                                        "idempotency_key"})},
             {"additionalProperties", false}};
  }

  const ToolDescriptor& descriptor() const override { return descriptor_; }

  ToolResult invoke(const ToolRequest& request,
                    const ToolContext& context) const override {
    if (!is_valid(request) || request.tool_name != descriptor_.name ||
        !has_only_arguments(request, {"id", "action", "note", "target_id",
                                      "evidence_uri", "deferred_until",
                                      "expected_revision", "idempotency_key", "edit"}) ||
        !request.arguments.contains("id") || !request.arguments.at("id").is_string()) {
      return make_invalid_request(
          "memory_review_apply requires id, action, expected_revision, and idempotency_key",
          request);
    }
    const auto id = request.arguments.at("id").get<std::string>();
    if (auto queued = require_mutation_approval(
            ToolMutationKind::command, "apply memory review " + id, request,
            context)) {
      return *queued;
    }
    try {
      auto action = qt::memory::review_action_from_json(request.arguments,
                                                        "user:tool-approval", true);
      ToolResult result;
      result.call_id = request.call_id;
      result.output = qt::memory::review_application_json(
          runtime_->apply_review_action(id, std::move(action)));
      return result;
    } catch (const std::invalid_argument& error) {
      return make_invalid_request(error.what(), request);
    } catch (const std::exception& error) {
      return exception_result(request, error);
    }
  }

 private:
  std::shared_ptr<qt::memory::MemoryRuntime> runtime_;
  ToolDescriptor descriptor_;
};

class MemoryFeedbackTool final : public Tool {
 public:
  explicit MemoryFeedbackTool(std::shared_ptr<qt::memory::MemoryRuntime> runtime)
      : runtime_(std::move(runtime)) {
    descriptor_.name = "memory_feedback";
    descriptor_.description =
        "Record an outcome for an in-scope procedural memory.";
    descriptor_.destructive = true;
    descriptor_.input_schema =
        Json{{"type", "object"},
             {"properties",
              {{"id", {{"type", "string"}, {"minLength", 1}}},
               {"outcome", {{"type", "string"},
                             {"enum", Json::array({"success", "failure", "partial", "unused"})}}},
               {"task_id", {{"type", "string"}}},
               {"notes", {{"type", "string"}}},
               {"evidence_uri", {{"type", "string"}}}}},
             {"required", Json::array({"id", "outcome"})},
             {"additionalProperties", false}};
  }

  const ToolDescriptor& descriptor() const override { return descriptor_; }

  ToolResult invoke(const ToolRequest& request,
                    const ToolContext& context) const override {
    if (!is_valid(request) || request.tool_name != descriptor_.name ||
        !has_only_arguments(request, {"id", "outcome", "task_id", "notes",
                                      "evidence_uri"}) ||
        !request.arguments.contains("id") ||
        !request.arguments.at("id").is_string() ||
        !request.arguments.contains("outcome") ||
        !request.arguments.at("outcome").is_string()) {
      return make_invalid_request(
          "memory_feedback requires string id and outcome", request);
    }
    if (auto queued = require_mutation_approval(
            ToolMutationKind::command,
            "record procedural memory outcome for " +
                request.arguments.at("id").get<std::string>(),
            request, context)) {
      return *queued;
    }
    try {
      qt::memory::ProcedureFeedback feedback;
      feedback.procedure_id = request.arguments.at("id").get<std::string>();
      feedback.outcome = qt::memory::procedure_outcome_from_string(
          request.arguments.at("outcome").get<std::string>());
      if (feedback.outcome == qt::memory::ProcedureOutcome::Unknown) {
        return make_invalid_request("memory_feedback outcome is invalid", request);
      }
      feedback.task_id = request.arguments.value("task_id", std::string{});
      feedback.notes = request.arguments.value("notes", std::string{});
      feedback.evidence_uri =
          request.arguments.value("evidence_uri", std::string{});
      const auto recorded = runtime_->record_feedback(std::move(feedback));
      ToolResult result;
      result.call_id = request.call_id;
      result.output = Json{{"run_id", recorded.first.id},
                           {"memory", memory_json(recorded.second)}};
      return result;
    } catch (const std::invalid_argument& error) {
      return make_invalid_request(error.what(), request);
    } catch (const std::exception& error) {
      return exception_result(request, error);
    }
  }

 private:
  std::shared_ptr<qt::memory::MemoryRuntime> runtime_;
  ToolDescriptor descriptor_;
};

}  // namespace

void register_memory_tools(
    ToolRegistry& registry,
    std::shared_ptr<qt::memory::MemoryRuntime> runtime,
    const std::function<bool(const std::string&)>& enabled) {
  if (!runtime) return;
  if (enabled("memory_recall")) {
    registry.register_tool(std::make_unique<MemoryRecallTool>(runtime));
  }
  if (enabled("memory_remember")) {
    registry.register_tool(std::make_unique<MemoryRememberTool>(runtime));
  }
  if (enabled("memory_forget")) {
    registry.register_tool(std::make_unique<MemoryForgetTool>(runtime));
  }
  if (enabled("memory_review") || enabled("memory_review_list")) {
    registry.register_tool(std::make_unique<MemoryReviewListTool>(runtime));
  }
  if (enabled("memory_review") || enabled("memory_review_show")) {
    registry.register_tool(std::make_unique<MemoryReviewShowTool>(runtime));
  }
  if (enabled("memory_review") || enabled("memory_review_preview")) {
    registry.register_tool(std::make_unique<MemoryReviewPreviewTool>(runtime));
  }
  if (enabled("memory_review") || enabled("memory_review_apply")) {
    registry.register_tool(std::make_unique<MemoryReviewApplyTool>(runtime));
  }
  if (enabled("memory_feedback")) {
    registry.register_tool(std::make_unique<MemoryFeedbackTool>(runtime));
  }
}

}  // namespace qt::tools
