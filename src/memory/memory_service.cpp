#include "qt/memory/memory_service.hpp"

#include "qt/memory/scope.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>

namespace qt::memory {
namespace {

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

void append_unique(std::vector<std::string>& values, std::string value) {
  if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(std::move(value));
  }
}

bool redact(std::string& content, const std::regex& expression) {
  const auto replaced = std::regex_replace(content, expression, "[REDACTED]");
  if (replaced == content) return false;
  content = replaced;
  return true;
}

bool redact_json_secret(std::string& content, const std::regex& expression) {
  const auto replaced =
      std::regex_replace(content, expression, "$1[REDACTED]$3");
  if (replaced == content) return false;
  content = replaced;
  return true;
}

double clamp_score(double value) { return std::max(0.0, std::min(1.0, value)); }

}  // namespace

std::string MemoryService::remember(const MemoryScope& scope, MemoryWrite write) {
  validate_scope(scope);
  write.content = normalize_content(std::move(write.content));
  if (write.content.empty()) throw std::invalid_argument("memory content is required");
  if (redact_secrets(write.content)) append_unique(write.tags, "security:redacted");
  if (write.type == MemoryType::Observation) write.type = infer_type(write.content, write.type);
  if (write.status == MemoryStatus::Active && write.provenance.created_by != "user" &&
      write.provenance.created_by != "import") {
    write.status = MemoryStatus::Hypothesis;
  }
  if (write.provenance.evidence_strength <= 0.0) {
    if (!write.provenance.source_hash.empty()) write.provenance.evidence_strength = 0.8;
    else if (!write.provenance.source_uri.empty()) write.provenance.evidence_strength = 0.6;
    else if (!write.source.empty()) write.provenance.evidence_strength = 0.4;
  }
  write.provenance.evidence_strength = clamp_score(write.provenance.evidence_strength);
  write.confidence = clamp_score(write.confidence);
  write.quality_score = clamp_score(write.quality_score);
  append_unique(write.tags, std::string("type:") + to_string(write.type));
  append_unique(write.tags, std::string("source:") + to_string(write.provenance.source_type));
  return memories_.add(scope, std::move(write));
}

std::vector<MemoryEntry> MemoryService::query(const MemoryScope& scope,
                                               const MemoryQuery& query) const {
  validate_scope(scope);
  return memories_.query(scope, query);
}

MemoryEntry MemoryService::get(const MemoryScope& scope, const std::string& id) const {
  validate_scope(scope);
  if (id.empty()) throw std::invalid_argument("memory id is required");
  return memories_.get(scope, id);
}

MemoryEntry MemoryService::update(const MemoryScope& scope, const std::string& id,
                                  MemoryPatch patch) {
  validate_scope(scope);
  if (patch.content) {
    *patch.content = normalize_content(std::move(*patch.content));
    if (patch.content->empty()) throw std::invalid_argument("memory content is required");
    redact_secrets(*patch.content);
  }
  if (patch.confidence) *patch.confidence = clamp_score(*patch.confidence);
  if (patch.quality_score) *patch.quality_score = clamp_score(*patch.quality_score);
  return memories_.update(scope, id, patch);
}

MemoryEntry MemoryService::apply_governed_update(
    const MemoryScope& scope, const std::string& id,
    const MemoryStatus expected_status, MemoryPatch patch,
    std::optional<GraphEdge> edge, AuditEventWrite audit) {
  validate_scope(scope);
  if (id.empty()) throw std::invalid_argument("memory id is required");
  if (patch.content) {
    *patch.content = normalize_content(std::move(*patch.content));
    if (patch.content->empty()) throw std::invalid_argument("memory content is required");
    redact_secrets(*patch.content);
  }
  if (patch.confidence) *patch.confidence = clamp_score(*patch.confidence);
  if (patch.quality_score) *patch.quality_score = clamp_score(*patch.quality_score);
  return memories_.apply_governed_update(scope, id, expected_status, patch,
                                         std::move(edge), std::move(audit));
}

bool MemoryService::forget(const MemoryScope& scope, const std::string& id) {
  validate_scope(scope);
  return memories_.forget(scope, id);
}

std::vector<MemoryRevision> MemoryService::revisions(const MemoryScope& scope,
                                                     const std::string& id) const {
  validate_scope(scope);
  return memories_.revisions(scope, id);
}

std::string MemoryService::normalize_content(std::string content) {
  std::string out;
  out.reserve(content.size());
  std::size_t newline_run = 0;
  for (char ch : content) {
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (newline_run < 2) out.push_back(ch);
      ++newline_run;
    } else {
      newline_run = 0;
      out.push_back(ch);
    }
  }
  const auto non_space = [](unsigned char ch) { return std::isspace(ch) == 0; };
  out.erase(out.begin(), std::find_if(out.begin(), out.end(), non_space));
  out.erase(std::find_if(out.rbegin(), out.rend(), non_space).base(), out.end());
  return out;
}

bool MemoryService::redact_secrets(std::string& content) {
  bool changed = false;
  changed = redact_json_secret(
      content,
      std::regex(
          R"REGEX(((?:"(?:password|secret|api[_-]?key|access[_-]?token|refresh[_-]?token|token|authorization)"\s*:\s*"))([^"]*)((")))REGEX",
          std::regex::icase)) || changed;
  changed = redact(content, std::regex(R"(sk-(proj-)?[A-Za-z0-9_-]{20,})")) || changed;
  changed = redact(content, std::regex(R"(sk-ant-[A-Za-z0-9_-]{20,})")) || changed;
  changed = redact(content, std::regex(R"(ghp_[A-Za-z0-9]{20,})")) || changed;
  changed = redact(content, std::regex(R"(github_pat_[A-Za-z0-9_]{22,})")) || changed;
  changed = redact(content, std::regex(R"(Bearer\s+[A-Za-z0-9._~+/=-]{20,})", std::regex::icase)) || changed;
  changed = redact(content, std::regex(R"(AKIA[0-9A-Z]{16})")) || changed;
  changed = redact(content, std::regex(R"((password|secret)\s*[:=]\s*[^\s]+)", std::regex::icase)) || changed;
  changed = redact(content, std::regex(
      R"(/(?:home|Users|var|tmp|etc|workspace|root|mnt)/[^\s\"'<>|]+)")) || changed;
  changed = redact(content, std::regex(
      R"([A-Za-z]:\\[^\s\"'<>|]+)")) || changed;
  changed = redact(content, std::regex(
      R"(-----BEGIN (RSA |EC |DSA |OPENSSH )?PRIVATE KEY-----[\s\S]*?-----END (RSA |EC |DSA |OPENSSH )?PRIVATE KEY-----)")) || changed;
  return changed;
}

MemoryType MemoryService::infer_type(const std::string& content, MemoryType fallback) {
  const auto lower = lower_copy(content);
  if (lower.rfind("todo", 0) == 0 || lower.rfind("task", 0) == 0 ||
      lower.find("next step") != std::string::npos) return MemoryType::Todo;
  if (lower.rfind("bug", 0) == 0 || lower.rfind("issue", 0) == 0 ||
      lower.find("root cause") != std::string::npos ||
      lower.find("fixed by") != std::string::npos) return MemoryType::Bug;
  if (lower.rfind("decision:", 0) == 0 || lower.find("we decided") != std::string::npos ||
      lower.find("decided to") != std::string::npos) return MemoryType::Decision;
  if (lower.rfind("procedure:", 0) == 0 || lower.find("checklist") != std::string::npos ||
      lower.find("steps:") != std::string::npos) return MemoryType::Procedural;
  if (lower.rfind("constraint:", 0) == 0 || lower.find("must not") != std::string::npos ||
      lower.find("must always") != std::string::npos) return MemoryType::Constraint;
  if (lower.rfind("preference:", 0) == 0 || lower.find("i prefer") != std::string::npos)
    return MemoryType::Preference;
  return fallback;
}

}  // namespace qt::memory
