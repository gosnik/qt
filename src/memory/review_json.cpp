#include "qt/memory/review_json.hpp"

#include "qt/memory/memory_service.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>

namespace qt::memory {
namespace {

using Json = nlohmann::json;

std::string safe_text(std::string value) {
  MemoryService::redact_secrets(value);
  return value;
}

Json safe_json(Json value) {
  auto encoded =
      value.dump(-1, ' ', false, Json::error_handler_t::replace);
  MemoryService::redact_secrets(encoded);
  auto parsed = Json::parse(encoded, nullptr, false);
  return parsed.is_discarded() ? Json("[REDACTED]") : parsed;
}

std::int64_t timestamp(const TimePoint value) {
  if (value == TimePoint{}) return 0;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             value.time_since_epoch())
      .count();
}

TimePoint timepoint(const Json& value, const char* field) {
  if (!value.contains(field) || !value.at(field).is_number_integer()) {
    throw std::invalid_argument(std::string(field) +
                                " must be a Unix timestamp in milliseconds");
  }
  return TimePoint{std::chrono::milliseconds(value.at(field).get<std::int64_t>())};
}

Json scope_json(const MemoryScope& scope) {
  return Json{{"user", scope.user},
              {"project", scope.project},
              {"repository", scope.repository},
              {"branch", scope.branch},
              {"session", scope.session},
              {"task", scope.task},
              {"agent", scope.agent}};
}

Json provenance_json(const Provenance& provenance) {
  return Json{{"source_type", to_string(provenance.source_type)},
              {"source_uri", safe_text(provenance.source_uri)},
              {"source_hash", safe_text(provenance.source_hash)},
              {"source_span", safe_text(provenance.source_span)},
              {"created_by", safe_text(provenance.created_by)},
              {"evidence_strength", provenance.evidence_strength}};
}

Json encode_memory(const MemoryEntry& memory) {
  return Json{{"id", memory.id},
              {"scope", scope_json(memory.scope)},
              {"type", to_string(memory.type)},
              {"status", to_string(memory.status)},
              {"content", safe_text(memory.content)},
              {"source", safe_text(memory.source)},
              {"tags", memory.tags},
              {"confidence", memory.confidence},
              {"quality_score", memory.quality_score},
              {"provenance", provenance_json(memory.provenance)},
              {"replacement_id", memory.replacement_id},
              {"superseded_by", memory.superseded_by},
              {"access_count", memory.access_count},
              {"created_at", timestamp(memory.created_at)},
              {"updated_at", timestamp(memory.updated_at)}};
}

Json action_json(const ReviewAction& action) {
  const auto normalized = normalize_review_action(action);
  Json result{{"kind", to_string(normalized.kind)},
              {"actor", normalized.actor},
              {"note", normalized.note},
              {"target_id", normalized.target_id},
              {"evidence_uri", normalized.evidence_uri},
              {"expected_revision", normalized.expected_revision},
              {"idempotency_key", normalized.idempotency_key},
              {"human_authority", normalized.human_authority}};
  if (normalized.deferred_until) {
    result["deferred_until"] = timestamp(*normalized.deferred_until);
  }
  if (normalized.edit) {
    Json edit = Json::object();
    if (normalized.edit->content) edit["content"] = *normalized.edit->content;
    if (normalized.edit->type) edit["type"] = to_string(*normalized.edit->type);
    if (normalized.edit->target_status) {
      edit["target_status"] = to_string(*normalized.edit->target_status);
    }
    if (normalized.edit->confidence) edit["confidence"] = *normalized.edit->confidence;
    if (normalized.edit->scope) edit["scope"] = scope_json(*normalized.edit->scope);
    if (normalized.edit->tags) edit["tags"] = *normalized.edit->tags;
    if (normalized.edit->replacement_id) {
      edit["replacement_id"] = *normalized.edit->replacement_id;
    }
    result["edit"] = std::move(edit);
  }
  Json payload = Json::object();
  if (normalized.deferred_until) payload["until"] = timestamp(*normalized.deferred_until);
  if (!normalized.target_id.empty()) payload["target_id"] = normalized.target_id;
  if (!normalized.evidence_uri.empty()) payload["evidence_uri"] = normalized.evidence_uri;
  if (result.contains("edit")) payload["edit"] = result.at("edit");
  result["payload"] = std::move(payload);
  return result;
}

std::vector<std::string> string_array(const Json& input, const char* field) {
  if (!input.contains(field)) return {};
  if (!input.at(field).is_array()) {
    throw std::invalid_argument(std::string(field) + " must be an array");
  }
  std::vector<std::string> result;
  for (const auto& item : input.at(field)) {
    if (!item.is_string()) {
      throw std::invalid_argument(std::string(field) +
                                  " must contain only strings");
    }
    result.push_back(item.get<std::string>());
  }
  return result;
}

std::string string_value(const Json& input, const char* field,
                         std::string fallback = {}) {
  if (!input.contains(field)) return fallback;
  if (!input.at(field).is_string()) {
    throw std::invalid_argument(std::string(field) + " must be a string");
  }
  return input.at(field).get<std::string>();
}

bool bool_value(const Json& input, const char* field, const bool fallback) {
  if (!input.contains(field)) return fallback;
  if (!input.at(field).is_boolean()) {
    throw std::invalid_argument(std::string(field) + " must be a boolean");
  }
  return input.at(field).get<bool>();
}

std::size_t size_value(const Json& input, const char* field,
                       const std::size_t fallback) {
  if (!input.contains(field)) return fallback;
  if (!input.at(field).is_number_integer()) {
    throw std::invalid_argument(std::string(field) +
                                " must be a non-negative integer");
  }
  std::uint64_t value = 0U;
  if (input.at(field).is_number_unsigned()) {
    value = input.at(field).get<std::uint64_t>();
  } else {
    const auto signed_value = input.at(field).get<std::int64_t>();
    if (signed_value < 0) {
      throw std::invalid_argument(std::string(field) +
                                  " must be a non-negative integer");
    }
    value = static_cast<std::uint64_t>(signed_value);
  }
  if (value > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument(std::string(field) + " is too large");
  }
  return static_cast<std::size_t>(value);
}

double number_value(const Json& input, const char* field) {
  if (!input.contains(field) || !input.at(field).is_number()) {
    throw std::invalid_argument(std::string(field) + " must be a number");
  }
  return input.at(field).get<double>();
}

}  // namespace

Json review_memory_json(const MemoryEntry& memory) { return encode_memory(memory); }

Json review_item_json(const ReviewItem& item) {
  return Json{{"id", item.id},
              {"kind", item.kind},
              {"candidate_version", item.candidate_version},
              {"revision", item.revision},
              {"status", to_string(item.status)},
              {"priority", item.priority},
              {"reason", safe_text(item.reason)},
              {"subject_id", item.subject_id},
              {"provenance", provenance_json(item.provenance)},
              {"candidate", safe_json(review_candidate_json(decode_review_candidate(item)))},
              {"decision", safe_text(item.decision)},
              {"decided_by", safe_text(item.decided_by)},
              {"decision_note", safe_text(item.decision_note)},
              {"deferred_until", timestamp(item.deferred_until)},
              {"application_status", item.application_status},
              {"application_error", safe_text(item.application_error)},
              {"action_key", item.action_key},
              {"queue_score", review_queue_score(
                                  item, std::chrono::system_clock::now())},
              {"queue_signals",
               {{"critical_type", item.queue_signals.critical_type},
                {"trust_risk", item.queue_signals.trust_risk},
                {"access_count", item.queue_signals.access_count},
                {"recent_context", item.queue_signals.recent_context},
                {"procedure_failures",
                 item.queue_signals.procedure_failures}}},
              {"tags", safe_json(item.tags)},
              {"created_at", timestamp(item.created_at)},
              {"updated_at", timestamp(item.updated_at)},
              {"decided_at", timestamp(item.decided_at)}};
}

Json hydrated_review_json(const HydratedReview& review) {
  Json sources = Json::array();
  for (const auto& source : review.sources) sources.push_back(encode_memory(source));
  Json related = Json::array();
  for (const auto& memory : review.related) related.push_back(encode_memory(memory));
  Json edges = Json::array();
  for (const auto& edge : review.edges) {
    edges.push_back(Json{{"id", edge.id},
                         {"from_id", edge.from_id},
                         {"to_id", edge.to_id},
                         {"type", edge.type},
                         {"confidence", edge.confidence},
                         {"source", edge.source},
                         {"created_at", timestamp(edge.created_at)}});
  }
  Json procedure_runs = Json::array();
  for (const auto& run : review.procedure_runs) {
    procedure_runs.push_back(
        Json{{"id", run.id},
             {"procedure_id", run.procedure_id},
             {"task_id", run.task_id},
             {"outcome", to_string(run.outcome)},
             {"notes", safe_text(run.notes)},
             {"evidence_uri", safe_text(run.evidence_uri)},
             {"created_at", timestamp(run.created_at)}});
  }
  Json result{{"review", review_item_json(review.item)},
              {"candidate", safe_json(review_candidate_json(review.candidate))},
              {"sources", std::move(sources)},
              {"related", std::move(related)},
              {"edges", std::move(edges)},
              {"procedure_runs", std::move(procedure_runs)},
              {"risk", {{"score", review.risk.score},
                          {"class", review.risk.risk_class},
                          {"components", review.risk.components},
                          {"explanations", safe_json(review.risk.explanations)}}},
              {"recall", {{"strict", review.recall.strict},
                            {"balanced", review.recall.balanced},
                            {"broad", review.recall.broad},
                            {"warnings", safe_json(review.recall.warnings)}}},
              {"blocking_diagnostics", safe_json(review.blocking_diagnostics)}};
  result["subject"] = review.subject ? encode_memory(*review.subject) : Json(nullptr);
  return result;
}

Json review_preview_json(const ReviewMutationPreview& preview) {
  Json changes = Json::array();
  for (const auto& change : preview.changes) {
    const auto before = Json::parse(change.before_json, nullptr, false);
    const auto after = Json::parse(change.after_json, nullptr, false);
    changes.push_back(Json{{"record_type", change.record_type},
                           {"record_id", change.record_id},
                           {"operation", change.operation},
                           {"before_json", before.is_discarded()
                                               ? Json(safe_text(change.before_json))
                                               : safe_json(before)},
                           {"after_json", after.is_discarded()
                                              ? Json(safe_text(change.after_json))
                                              : safe_json(after)}});
  }
  return Json{{"review_id", preview.review_id},
              {"review_revision", preview.review_revision},
              {"idempotency_token", preview.idempotency_token},
              {"action", safe_json(action_json(preview.action))},
              {"changes", std::move(changes)},
              {"will_do", preview.will_do},
              {"will_not_do", preview.will_not_do},
              {"warnings", safe_json(preview.warnings)},
              {"requires_human_authority", preview.requires_human_authority},
              {"applicable", preview.applicable}};
}

Json review_application_json(const ReviewApplicationResult& result) {
  return Json{{"review_id", result.review_id},
              {"status", to_string(result.status)},
              {"decision_persisted", result.decision_persisted},
              {"domain_mutations_applied", result.domain_mutations_applied},
              {"idempotent_replay", result.idempotent_replay},
              {"revision", result.revision},
              {"action_id", result.action_id},
              {"changed_ids", result.changed_ids},
              {"failed_ids", result.failed_ids},
              {"error", safe_text(result.error)}};
}

Json review_action_record_json(const ReviewActionRecord& record) {
  const auto parsed_result = Json::parse(record.result_json, nullptr, false);
  return Json{{"id", record.id},
              {"review_id", record.review_id},
              {"action_key", record.action_key},
              {"action", record.action},
              {"actor", record.actor},
              {"note", safe_text(record.note)},
              {"before_status", to_string(record.before_status)},
              {"after_status", to_string(record.after_status)},
              {"before_revision", record.before_revision},
              {"after_revision", record.after_revision},
              {"application_status", to_string(record.application_status)},
              {"application_error", safe_text(record.application_error)},
              {"result", parsed_result.is_discarded()
                             ? Json(safe_text(record.result_json))
                             : safe_json(parsed_result)},
              {"replay_count", record.replay_count},
              {"created_at", timestamp(record.created_at)}};
}

Json review_batch_preview_json(const ReviewBatchPreview& preview) {
  Json items = Json::array();
  for (const auto& item : preview.items) {
    items.push_back(review_preview_json(item));
  }
  return Json{{"idempotency_token", preview.idempotency_token},
              {"items", std::move(items)},
              {"warnings", preview.warnings},
              {"applicable", preview.applicable}};
}

Json review_batch_result_json(const ReviewBatchResult& result) {
  Json items = Json::array();
  for (const auto& item : result.items) {
    Json value{{"review_id", item.review_id},
               {"error_code", item.error_code},
               {"error", safe_text(item.error)}};
    value["result"] = item.result ? review_application_json(*item.result)
                                  : Json(nullptr);
    items.push_back(std::move(value));
  }
  return Json{{"idempotency_token", result.idempotency_token},
              {"results", std::move(items)},
              {"applied_count", result.applied_count},
              {"failed_count", result.failed_count},
              {"partial_failure", result.partial_failure}};
}

Json review_batch_job_json(const ReviewBatchJob& job) {
  Json value{{"job_id", job.id},
             {"status", to_string(job.status)},
             {"total_count", job.total_count},
             {"processed_count", job.processed_count},
             {"error_code", job.error_code},
             {"error", safe_text(job.error)}};
  value["result"] = job.result ? review_batch_result_json(*job.result)
                               : Json(nullptr);
  return value;
}

Json review_statistics_json(const ReviewStatistics& value) {
  return Json{{"pending", value.pending},
              {"urgent", value.urgent},
              {"due_deferred", value.due_deferred},
              {"reviewed", value.reviewed},
              {"approved", value.approved},
              {"edited_and_approved", value.edited_and_approved},
              {"rejected", value.rejected},
              {"deferred", value.deferred},
              {"resurfaced", value.resurfaced},
              {"reopened", value.reopened},
              {"merged", value.merged},
              {"superseded", value.superseded},
              {"validated", value.validated},
              {"failed", value.failed},
              {"application_errors", value.application_errors},
              {"idempotent_replays", value.idempotent_replays},
              {"compensated", value.compensated},
              {"dreaming_pending", value.dreaming_pending},
              {"dreaming_approved", value.dreaming_approved},
              {"dreaming_rejected", value.dreaming_rejected},
              {"oldest_pending_age_seconds", value.oldest_pending_age_seconds},
              {"median_pending_age_seconds", value.median_pending_age_seconds},
              {"time_to_first_decision_p50_seconds", value.time_to_first_decision_p50_seconds},
              {"time_to_first_decision_p95_seconds", value.time_to_first_decision_p95_seconds},
              {"review_duration_p50_seconds", value.review_duration_p50_seconds},
              {"review_duration_p95_seconds", value.review_duration_p95_seconds},
              {"unresolved_critical_conflicts", value.unresolved_critical_conflicts},
              {"stale_validated_subjects", value.stale_validated_subjects},
              {"unprovenanced_active_subjects", value.unprovenanced_active_subjects},
              {"high_use_low_trust_subjects", value.high_use_low_trust_subjects},
              {"batch_runs", value.batch_runs},
              {"batch_items", value.batch_items},
              {"batch_applied_items", value.batch_applied_items},
              {"batch_failed_items", value.batch_failed_items},
              {"batch_partial_failures", value.batch_partial_failures},
              {"batch_retries", value.batch_retries},
              {"batch_idempotent_replays", value.batch_idempotent_replays},
              {"batch_compensations", value.batch_compensations},
              {"batch_application_errors", value.batch_application_errors},
              {"pending_by_kind", value.pending_by_kind},
              {"pending_by_priority", value.pending_by_priority},
              {"pending_by_scope", value.pending_by_scope},
              {"pending_by_risk_class", value.pending_by_risk_class},
              {"by_kind", value.by_kind},
              {"by_priority", value.by_priority},
              {"by_status", value.by_status},
              {"by_source_type", value.by_source_type},
              {"by_application_status", value.by_application_status},
              {"by_outcome", value.by_outcome},
              {"oldest_pending_age_by_kind_seconds", value.oldest_pending_age_by_kind_seconds},
              {"median_pending_age_by_kind_seconds", value.median_pending_age_by_kind_seconds},
              {"oldest_pending_age_by_priority_seconds", value.oldest_pending_age_by_priority_seconds},
              {"median_pending_age_by_priority_seconds", value.median_pending_age_by_priority_seconds},
              {"oldest_pending_age_by_scope_seconds", value.oldest_pending_age_by_scope_seconds},
              {"median_pending_age_by_scope_seconds", value.median_pending_age_by_scope_seconds},
              {"oldest_pending_age_by_risk_class_seconds", value.oldest_pending_age_by_risk_class_seconds},
              {"median_pending_age_by_risk_class_seconds", value.median_pending_age_by_risk_class_seconds}};
}

ReviewQuery review_query_from_json(const Json& input) {
  if (!input.is_object()) throw std::invalid_argument("review query must be an object");
  ReviewQuery query;
  if (input.contains("statuses")) {
    query.statuses.clear();
    for (const auto& status : string_array(input, "statuses")) {
      query.statuses.push_back(review_status_from_string(status));
    }
  }
  query.kinds = string_array(input, "kinds");
  query.priorities = string_array(input, "priorities");
  for (const auto& source_type : string_array(input, "source_types")) {
    const auto parsed = source_type_from_string(source_type);
    if (parsed == SourceType::Unknown && source_type != "unknown") {
      throw std::invalid_argument("invalid review source type: " + source_type);
    }
    query.source_types.push_back(parsed);
  }
  query.tags = string_array(input, "tags");
  query.dream_cycles = string_array(input, "dream_cycles");
  if (input.contains("stale")) query.stale = bool_value(input, "stale", false);
  if (input.contains("conflict")) {
    query.conflict = bool_value(input, "conflict", false);
  }
  if (input.contains("created_after")) query.created_after = timepoint(input, "created_after");
  if (input.contains("created_before")) query.created_before = timepoint(input, "created_before");
  if (input.contains("decided_after")) query.decided_after = timepoint(input, "decided_after");
  if (input.contains("deferred_due_before")) {
    query.deferred_due_before = timepoint(input, "deferred_due_before");
  }
  query.include_due_deferred = bool_value(input, "include_due_deferred", true);
  query.oldest_first = bool_value(input, "oldest_first", false);
  const auto scope = string_value(input, "scope", "all");
  if (scope == "all") query.scope = ReviewQuery::ScopeSelector::All;
  else if (scope == "exact") query.scope = ReviewQuery::ScopeSelector::Exact;
  else if (scope == "durable") {
    query.scope = ReviewQuery::ScopeSelector::Durable;
  } else {
    throw std::invalid_argument(
        "review query scope must be all, exact, or durable");
  }
  query.limit = size_value(input, "limit", std::size_t{50});
  query.offset = size_value(input, "offset", std::size_t{0});
  if (query.limit == 0U || query.limit > 1000U) {
    throw std::invalid_argument("review query limit must be from 1 to 1000");
  }
  return query;
}

ReviewAction review_action_from_json(const Json& input, std::string actor,
                                     const bool human_authority) {
  if (!input.is_object() || !input.contains("action") ||
      !input.at("action").is_string()) {
    throw std::invalid_argument("review action requires string action");
  }
  ReviewAction action;
  action.kind = review_action_kind_from_string(input.at("action").get<std::string>());
  action.actor = std::move(actor);
  action.human_authority = human_authority;
  action.note = string_value(input, "note");
  action.target_id = string_value(input, "target_id");
  action.evidence_uri = string_value(input, "evidence_uri");
  action.expected_revision = size_value(input, "expected_revision", 0U);
  action.idempotency_key = string_value(input, "idempotency_key");
  if (input.contains("deferred_until")) {
    action.deferred_until = timepoint(input, "deferred_until");
  }
  const Json* payload = nullptr;
  if (input.contains("payload")) {
    if (!input.at("payload").is_object()) {
      throw std::invalid_argument("review action payload must be an object");
    }
    payload = &input.at("payload");
    if (payload->contains("target_id")) {
      action.target_id = string_value(*payload, "target_id");
    }
    if (payload->contains("evidence_uri")) {
      action.evidence_uri = string_value(*payload, "evidence_uri");
    }
    if (payload->contains("until")) {
      action.deferred_until = timepoint(*payload, "until");
    }
  }
  const Json* edit_owner =
      payload != nullptr && payload->contains("edit") ? payload : &input;
  if (edit_owner->contains("edit")) {
    const auto& value = edit_owner->at("edit");
    if (!value.is_object()) throw std::invalid_argument("edit must be an object");
    ReviewEdit edit;
    if (value.contains("content")) edit.content = string_value(value, "content");
    if (value.contains("type")) edit.type = memory_type_from_string(string_value(value, "type"));
    if (value.contains("target_status")) {
      edit.target_status = memory_status_from_string(string_value(value, "target_status"));
    }
    if (value.contains("confidence")) edit.confidence = number_value(value, "confidence");
    if (value.contains("scope")) {
      const auto& scope_value = value.at("scope");
      if (!scope_value.is_object()) {
        throw std::invalid_argument("edit.scope must be an object");
      }
      MemoryScope scope;
      scope.kind = scope_kind_from_string(
          string_value(scope_value, "kind", "exact"));
      scope.user = string_value(scope_value, "user");
      scope.organization = string_value(scope_value, "organization");
      scope.project = string_value(scope_value, "project");
      scope.repository = string_value(scope_value, "repository");
      scope.repository_display = string_value(scope_value, "repository_display");
      scope.branch = string_value(scope_value, "branch");
      scope.task = string_value(scope_value, "task");
      scope.agent = string_value(scope_value, "agent");
      scope.session = string_value(scope_value, "session");
      scope.conversation = string_value(scope_value, "conversation");
      edit.scope = std::move(scope);
    }
    if (value.contains("tags")) edit.tags = string_array(value, "tags");
    if (value.contains("replacement_id")) edit.replacement_id = string_value(value, "replacement_id");
    action.edit = std::move(edit);
  }
  return normalize_review_action(std::move(action));
}

}  // namespace qt::memory
