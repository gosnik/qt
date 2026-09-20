#include "qt/memory/context_service.hpp"
#include "qt/memory/ingestion_service.hpp"
#include "qt/memory/prehandler_protocol.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace qt::ai {
const char* to_string(const MessageRole value) {
  switch (value) {
    case MessageRole::system:
      return "system";
    case MessageRole::developer:
      return "developer";
    case MessageRole::user:
      return "user";
    case MessageRole::assistant:
      return "assistant";
    case MessageRole::tool:
      return "tool";
  }
  return "user";
}
}  // namespace qt::ai

namespace {

using namespace qt::memory;

void require(const bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

bool valid_utf8(const std::string& value) {
  for (std::size_t index = 0U; index < value.size();) {
    const auto lead = static_cast<unsigned char>(value[index]);
    std::size_t continuation = 0U;
    if (lead <= 0x7fU) {
      ++index;
      continue;
    }
    if ((lead & 0xe0U) == 0xc0U) {
      continuation = 1U;
    } else if ((lead & 0xf0U) == 0xe0U) {
      continuation = 2U;
    } else if ((lead & 0xf8U) == 0xf0U) {
      continuation = 3U;
    } else {
      return false;
    }
    if (index + continuation >= value.size()) return false;
    for (std::size_t offset = 1U; offset <= continuation; ++offset) {
      if ((static_cast<unsigned char>(value[index + offset]) & 0xc0U) !=
          0x80U) {
        return false;
      }
    }
    index += continuation + 1U;
  }
  return true;
}

class ProbeBlocks final : public BlockRepository {
 public:
  ContextBlock get_block(const MemoryScope&, const std::string& id) const override {
    for (const auto& block : values_) {
      if (block.id == id) return block;
    }
    return {id, {}, {}};
  }

  ContextBlock update_block(const MemoryScope&, const std::string& id,
                            std::string content) override {
    ContextBlock block{id, std::move(content), {}};
    values_.push_back(block);
    return block;
  }

  std::vector<ContextBlock> blocks(const MemoryScope&) const override {
    return values_;
  }

 private:
  std::vector<ContextBlock> values_{{"project", "Pinned build guidance.", {}}};
};

class ProbeEvents final : public SessionEventRepository {
 public:
  std::string record_session_event(const MemoryScope&,
                                   SessionEventWrite write) override {
    values_.insert(values_.begin(),
                   {"episode-written", std::move(write.event_type),
                    std::move(write.source), std::move(write.payload_json), {},
                    std::move(write.deduplication_key)});
    return values_.front().id;
  }

  SessionEventRecordResult record_session_event_once(
      const MemoryScope& scope, SessionEventWrite write) override {
    return {record_session_event(scope, std::move(write)), true};
  }

  std::vector<SessionEvent> session_events(
      const MemoryScope&, const std::string& event_type, std::size_t limit,
      std::size_t offset = 0U) const override {
    std::vector<SessionEvent> result;
    for (const auto& event : values_) {
      if (!event_type.empty() && event.event_type != event_type) continue;
      if (offset != 0U) {
        --offset;
        continue;
      }
      if (result.size() == limit) break;
      result.push_back(event);
    }
    return result;
  }

 private:
  std::vector<SessionEvent> values_{
      {"episode-1", "completed_episode", "qt:agent_end",
       R"({"content":"The prior turn confirmed the native verification command."})",
       {}, "episode-deduplication-key"}};
};

RecalledMemory recalled(std::string id, const MemoryType type,
                        std::string content) {
  RecalledMemory result;
  result.memory.id = std::move(id);
  result.memory.type = type;
  result.memory.status = MemoryStatus::Validated;
  result.memory.content = std::move(content);
  result.memory.confidence = 0.95;
  return result;
}

void verify_transcript_chunking() {
  const std::string transcript =
      "# Codex Session Transcript\n\n## Events\n\n"
      "### User\nDecision: keep deterministic transcript boundaries.\n\n"
      "### Assistant\nProcedure: run the focused probe. password=private-value\n"
      "### Unicode\nDecision: preserve 🧠🧠🧠🧠🧠 boundaries.\n";
  const auto first =
      MemoryIngestionService::chunk_transcript(transcript, 96U);
  const auto second =
      MemoryIngestionService::chunk_transcript(transcript, 96U);
  require(first == second && first.size() >= 2U,
          "transcript chunking is not deterministic or structural");
  for (const auto& chunk : first) {
    require(!chunk.empty() && chunk.size() <= 96U,
            "transcript chunk exceeded its configured bound");
    require(valid_utf8(chunk),
            "transcript chunk split a multibyte UTF-8 sequence");
    require(chunk.find("# Codex Session Transcript") != std::string::npos,
            "transcript chunk lost its source context");
    require(chunk.find("private-value") == std::string::npos,
            "transcript chunking leaked a secret");
  }
  const auto single_byte =
      MemoryIngestionService::chunk_transcript("🧠", 1U);
  require(single_byte == std::vector<std::string>{"?"} &&
              valid_utf8(single_byte.front()),
          "one-byte transcript chunks did not replace a wider code point");
}

void verify_context_categories() {
  ProbeBlocks blocks;
  ProbeEvents events;
  ContextService context(blocks, &events);
  MemoryScope scope;
  scope.kind = ScopeKind::Exact;
  scope.project = "probe";
  scope.repository = "probe-repository";
  scope.session = "probe-session";

  auto procedure = recalled("procedure-1", MemoryType::Procedural,
                            "Run the focused verification before release.");
  procedure.memory.provenance.source_uri = "runbook://release";
  auto fact = recalled("memory-1", MemoryType::Decision,
                       "The native verification profile is authoritative.");
  fact.memory.provenance.source_uri = "docs://verification";
  auto drifted = recalled("stale-1", MemoryType::Semantic,
                          "STALE-CONTENT-MUST-NOT-BE-INJECTED");
  drifted.memory.provenance.source_uri = "stale://private-evidence";
  drifted.warnings = {"known source drift; content excluded"};
  drifted.eligible_for_context = false;

  ContextOptions options;
  options.token_budget = 1000U;
  const ContextPack pack =
      context.build(scope, {procedure, fact, drifted}, options);
  for (const char* heading : {"## Pinned blocks", "## Procedures",
                              "## Relevant memories",
                              "## Recent raw episodes", "## Evidence links",
                              "## Governance warnings"}) {
    require(pack.markdown.find(heading) != std::string::npos,
            std::string("context pack omitted category ") + heading);
  }
  require(pack.markdown.find("STALE-CONTENT-MUST-NOT-BE-INJECTED") ==
                  std::string::npos &&
              pack.markdown.find("stale://private-evidence") ==
                  std::string::npos,
          "known-stale content or provenance entered context");
  require(pack.markdown.find("known source drift; content excluded") !=
              std::string::npos,
          "known-stale warning was not allocated independently");
  require(std::find(pack.memory_ids.begin(), pack.memory_ids.end(), "stale-1") ==
              pack.memory_ids.end(),
          "warning-only stale memory was exposed as injected content");
  require(pack.markdown.find("prior turn confirmed") != std::string::npos,
          "recent raw episode was not allocated independently");
  require(pack.markdown.find("docs://verification") != std::string::npos,
          "evidence link was not allocated independently");
  require(!pack.required_context_markdown.empty() &&
              pack.markdown.find(pack.required_context_markdown) !=
                  std::string::npos &&
              pack.required_context_markdown.find("prior turn confirmed") !=
                  std::string::npos &&
              pack.required_context_markdown.find("docs://verification") !=
                  std::string::npos,
          "required protocol supplement omitted raw episodes or evidence links");
  require(pack.required_memory_ids ==
              std::vector<std::string>({"procedure-1", "memory-1"}),
          "required evidence memory IDs were not preserved exactly");
  require(pack.markdown.size() <= options.token_budget * options.bytes_per_token &&
              pack.estimated_tokens <= options.token_budget,
          "context category allocation exceeded the total budget");
}

void verify_required_prehandler_context() {
  MemoryPrehandlerRequest request;
  request.logical_input_sequence = 7U;
  request.context_token_budget = 128U;
  request.required_context_markdown =
      "## Recent raw episodes\n[Episode probe] password=private-value\n\n"
      "## Evidence links\n- [Memory probe] uri=test://evidence\n";
  auto required_memory =
      recalled("probe", MemoryType::Decision, "Required evidence memory.");
  request.candidates = {required_memory};
  request.required_memory_ids = {required_memory.memory.id};
  const auto encoded = encode_memory_prehandler_request(request);
  const std::string required =
      encoded.at("required_context_markdown").get<std::string>();
  require(required.find("private-value") == std::string::npos &&
              required.find("[REDACTED]") != std::string::npos,
          "pre-handler protocol did not redact required context");

  const std::string context = "Child-selected context.\n" + required;
  qt::ai::Json contribution{
      {"version", kMemoryPrehandlerProtocolVersion},
      {"logical_input_sequence", request.logical_input_sequence},
      {"context_markdown", context},
      {"memory_ids", qt::ai::Json::array({required_memory.memory.id})},
      {"block_ids", qt::ai::Json::array()},
      {"warnings", qt::ai::Json::array()},
      {"estimated_tokens", (context.size() + 3U) / 4U},
      {"omission_reason", ""},
      {"candidate_pack_hash", memory_candidate_pack_hash(request)},
      {"output_hash", ""}};
  require(decode_and_validate_memory_contribution(contribution, request).ok(),
          "pre-handler rejected preserved required context");
  contribution["memory_ids"] = qt::ai::Json::array();
  require(!decode_and_validate_memory_contribution(contribution, request),
          "pre-handler accepted a dropped required evidence memory ID");
  contribution["memory_ids"] =
      qt::ai::Json::array({required_memory.memory.id});
  contribution["context_markdown"] = "Child-selected context.";
  contribution["estimated_tokens"] = std::size_t{6U};
  require(!decode_and_validate_memory_contribution(contribution, request),
          "pre-handler accepted dropped required context");
}

}  // namespace

int main() {
  try {
    verify_transcript_chunking();
    verify_context_categories();
    verify_required_prehandler_context();
    std::cout << "memory context probe: ok\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "memory context probe: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
