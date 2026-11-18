#include "qt/memory/ingestion_service.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace qt::memory {
namespace {

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool meaningful(const std::string& line) {
  if (line.size() < 12) return false;
  const auto lower = lower_copy(line);
  static const char* signals[] = {
      "decision", "decided", "must ", "must not", "always ", "never ",
      "root cause", "fixed by", "regression", "procedure", "steps:",
      "preference", "i prefer", "correction", "actually", "todo", "next step"};
  for (const auto* signal : signals) {
    if (lower.find(signal) != std::string::npos) return true;
  }
  return false;
}

std::string canonical_key(const std::string& value) {
  std::string key;
  for (unsigned char ch : value) {
    if (std::isalnum(ch) != 0) key.push_back(static_cast<char>(std::tolower(ch)));
  }
  return key;
}

std::string trim(std::string value) {
  const auto is_space = [](const unsigned char ch) {
    return std::isspace(ch) != 0;
  };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), [&](const char ch) {
                return !is_space(static_cast<unsigned char>(ch));
              }));
  value.erase(
      std::find_if(value.rbegin(), value.rend(), [&](const char ch) {
        return !is_space(static_cast<unsigned char>(ch));
      }).base(),
      value.end());
  return value;
}

std::size_t utf8_width(const unsigned char lead) {
  if (lead <= 0x7fU) return 1U;
  if ((lead & 0xe0U) == 0xc0U) return 2U;
  if ((lead & 0xf0U) == 0xe0U) return 3U;
  if ((lead & 0xf8U) == 0xf0U) return 4U;
  return 0U;
}

std::string sanitize_utf8(const std::string& value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0U; index < value.size();) {
    const std::size_t width =
        utf8_width(static_cast<unsigned char>(value[index]));
    bool valid = width != 0U && index + width <= value.size();
    for (std::size_t offset = 1U; valid && offset < width; ++offset) {
      valid = (static_cast<unsigned char>(value[index + offset]) & 0xc0U) ==
              0x80U;
    }
    if (!valid) {
      result.push_back('?');
      ++index;
      continue;
    }
    result.append(value, index, width);
    index += width;
  }
  return result;
}

std::size_t utf8_chunk_end(const std::string& value, const std::size_t offset,
                           const std::size_t maximum_bytes) {
  std::size_t end = maximum_bytes >= value.size() - offset
                        ? value.size()
                        : offset + maximum_bytes;
  if (end == value.size()) return end;
  while (end > offset &&
         (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U) {
    --end;
  }
  if (end != offset) return end;
  return std::min(value.size(),
                  offset + utf8_width(static_cast<unsigned char>(value[offset])));
}

std::string bounded_utf8(const std::string& value,
                         const std::size_t maximum_bytes) {
  const std::size_t end = utf8_chunk_end(value, 0U, maximum_bytes);
  return end > maximum_bytes ? std::string("?") : value.substr(0U, end);
}

void append_chunk_unit(std::vector<std::string>& chunks, std::string& current,
                       const std::string& unit,
                       const std::size_t max_characters) {
  const std::string cleaned = trim(unit);
  if (cleaned.empty()) return;
  if (!current.empty() &&
      current.size() + cleaned.size() + 2U > max_characters) {
    chunks.push_back(trim(std::move(current)));
    current.clear();
  }
  if (cleaned.size() <= max_characters) {
    if (!current.empty()) current += "\n\n";
    current += cleaned;
    return;
  }
  if (!current.empty()) {
    chunks.push_back(trim(std::move(current)));
    current.clear();
  }
  for (std::size_t offset = 0U; offset < cleaned.size();) {
    const std::size_t end =
        utf8_chunk_end(cleaned, offset, max_characters);
    chunks.push_back(end - offset > max_characters
                         ? std::string("?")
                         : trim(cleaned.substr(offset, end - offset)));
    offset = end;
  }
}

}  // namespace

std::vector<std::string> MemoryIngestionService::chunk_transcript(
    const std::string& content, const std::size_t max_characters) {
  if (max_characters == 0U) {
    throw std::invalid_argument(
        "transcript chunk maximum must be non-zero");
  }
  std::string redacted = content;
  MemoryService::redact_secrets(redacted);
  redacted = sanitize_utf8(redacted);

  std::vector<std::string> units;
  std::istringstream input(redacted);
  std::ostringstream current;
  std::string line;
  bool saw_section = false;
  while (std::getline(input, line)) {
    if (line.rfind("### ", 0U) == 0U) {
      saw_section = true;
      const std::string section = trim(current.str());
      if (!section.empty()) units.push_back(section);
      current.str({});
      current.clear();
    }
    current << line << '\n';
  }
  const std::string tail = trim(current.str());
  if (!tail.empty()) units.push_back(tail);

  if (!saw_section) {
    units.clear();
    std::istringstream paragraphs(redacted);
    std::string paragraph;
    while (std::getline(paragraphs, line)) {
      if (trim(line).empty()) {
        const std::string completed = trim(std::move(paragraph));
        if (!completed.empty()) units.push_back(completed);
        paragraph.clear();
        continue;
      }
      if (!paragraph.empty()) paragraph.push_back('\n');
      paragraph += line;
    }
    const std::string completed = trim(std::move(paragraph));
    if (!completed.empty()) units.push_back(completed);
  }

  std::vector<std::string> chunks;
  std::string chunk;
  for (const auto& unit : units) {
    append_chunk_unit(chunks, chunk, unit, max_characters);
  }
  if (!trim(chunk).empty()) chunks.push_back(trim(std::move(chunk)));
  if (chunks.empty() && !trim(redacted).empty()) {
    append_chunk_unit(chunks, chunk, redacted, max_characters);
    if (!trim(chunk).empty()) chunks.push_back(trim(std::move(chunk)));
  }

  if (redacted.find("# Codex Session Transcript") != std::string::npos) {
    const std::string transcript_prefix =
        "# Codex Session Transcript\n\n## Events\n\n";
    std::vector<std::string> contextualized;
    for (auto& item : chunks) {
      if (item.find("# Codex Session Transcript") == std::string::npos) {
        if (transcript_prefix.size() >= max_characters) {
          contextualized.push_back(std::move(item));
          continue;
        }
        const std::size_t available = max_characters - transcript_prefix.size();
        for (std::size_t offset = 0U; offset < item.size();) {
          const std::size_t end = utf8_chunk_end(item, offset, available);
          contextualized.push_back(transcript_prefix +
                                   (end - offset > available
                                        ? std::string("?")
                                        : trim(item.substr(offset,
                                                           end - offset))));
          offset = end;
        }
      } else {
        contextualized.push_back(std::move(item));
      }
    }
    chunks = std::move(contextualized);
  }
  return chunks;
}

std::vector<IngestionCandidate> MemoryIngestionService::extract(
    const IngestionRequest& request) const {
  std::vector<IngestionCandidate> result;
  std::unordered_set<std::string> seen;
  for (const auto& chunk : chunk_transcript(request.content)) {
    std::istringstream input(chunk);
    std::string line;
    while (std::getline(input, line)) {
      line = MemoryService::normalize_content(std::move(line));
      if (!meaningful(line)) continue;
      IngestionCandidate candidate;
      candidate.content = bounded_utf8(line, 1200U);
      candidate.type = MemoryService::infer_type(candidate.content);
      candidate.confidence = 0.72;
      candidate.source_line = bounded_utf8(line, 200U);
      candidate.tags = request.tags;
      candidate.tags.push_back(std::string("processed:") +
                               to_string(candidate.type));
      const auto key = canonical_key(candidate.content);
      if (candidate.confidence >= request.minimum_confidence &&
          seen.insert(key).second) {
        result.push_back(std::move(candidate));
      }
    }
  }
  if (result.empty() && request.fallback_to_raw) {
    auto content = MemoryService::normalize_content(request.content);
    if (!content.empty()) {
      IngestionCandidate candidate;
      candidate.content = bounded_utf8(content, 1200U);
      candidate.type = MemoryService::infer_type(candidate.content);
      candidate.confidence = 0.6;
      candidate.source_line = bounded_utf8(content, 200U);
      candidate.tags = request.tags;
      candidate.tags.push_back("processed:fallback");
      if (candidate.confidence >= request.minimum_confidence)
        result.push_back(std::move(candidate));
    }
  }
  return result;
}

IngestionResult MemoryIngestionService::ingest(const MemoryScope& scope,
                                               const IngestionRequest& request,
                                               bool persist_candidates) {
  IngestionResult result;
  result.candidates = extract(request);
  if (!persist_candidates) return result;
  for (const auto& candidate : result.candidates) {
    MemoryQuery existing;
    existing.text = candidate.content;
    existing.limit = 10;
    existing.policy = RecallPolicy::Strict;
    bool duplicate = false;
    const auto key = canonical_key(candidate.content);
    for (const auto& memory : memories_.query(scope, existing)) {
      if (canonical_key(memory.content) == key) { duplicate = true; break; }
    }
    if (duplicate) continue;
    MemoryWrite write;
    write.type = candidate.type;
    write.status = MemoryStatus::Candidate;
    write.provenance = request.provenance;
    write.source = request.source;
    write.content = candidate.content;
    write.tags = candidate.tags;
    write.confidence = candidate.confidence;
    result.stored_ids.push_back(memories_.remember(scope, std::move(write)));
  }
  return result;
}

}  // namespace qt::memory
