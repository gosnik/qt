#include "qt/ai/llama_model_discovery.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace qt::ai {
namespace {

constexpr std::uint32_t kGgufTypeUint8 = 0;
constexpr std::uint32_t kGgufTypeInt8 = 1;
constexpr std::uint32_t kGgufTypeUint16 = 2;
constexpr std::uint32_t kGgufTypeInt16 = 3;
constexpr std::uint32_t kGgufTypeUint32 = 4;
constexpr std::uint32_t kGgufTypeInt32 = 5;
constexpr std::uint32_t kGgufTypeFloat32 = 6;
constexpr std::uint32_t kGgufTypeBool = 7;
constexpr std::uint32_t kGgufTypeString = 8;
constexpr std::uint32_t kGgufTypeArray = 9;
constexpr std::uint32_t kGgufTypeUint64 = 10;
constexpr std::uint32_t kGgufTypeInt64 = 11;
constexpr std::uint32_t kGgufTypeFloat64 = 12;

constexpr std::uint64_t kMaxGgufMetadataEntries = 4096;
constexpr std::uint64_t kMaxGgufStringBytes = 1024 * 1024;
constexpr std::uint64_t kMaxGgufArrayItems = 1024 * 1024;

struct ParsedGgufMetadata {
  Json fields = Json::object();
  std::vector<std::string> warnings;
};

struct GgufParseResult {
  bool ok = false;
  ParsedGgufMetadata metadata;
  std::string error;
};

std::string trim_copy(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() &&
         std::isspace(static_cast<unsigned char>(text[start])) != 0) {
    ++start;
  }
  std::size_t end = text.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return std::string(text.substr(start, end - start));
}

std::string lowercase_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string path_lower_extension(const std::filesystem::path& path) {
  return lowercase_copy(path.extension().generic_string());
}

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

void append_unique(std::vector<std::string>& values, std::string value) {
  if (value.empty()) {
    return;
  }
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(std::move(value));
  }
}

std::string slugify(std::string_view value) {
  std::string slug;
  bool last_dash = false;
  for (const unsigned char ch : value) {
    if (std::isalnum(ch) != 0) {
      slug.push_back(static_cast<char>(std::tolower(ch)));
      last_dash = false;
    } else if (!last_dash && !slug.empty()) {
      slug.push_back('-');
      last_dash = true;
    }
  }
  while (!slug.empty() && slug.back() == '-') {
    slug.pop_back();
  }
  return slug.empty() ? std::string("local-model") : slug;
}

bool read_exact(std::istream& input, char* data, std::size_t size) {
  input.read(data, static_cast<std::streamsize>(size));
  return static_cast<std::size_t>(input.gcount()) == size;
}

template <typename UInt>
bool read_little_unsigned(std::istream& input, UInt& value) {
  static_assert(std::is_unsigned<UInt>::value, "UInt must be unsigned");
  std::array<unsigned char, sizeof(UInt)> bytes {};
  if (!read_exact(input, reinterpret_cast<char*>(bytes.data()), bytes.size())) {
    return false;
  }
  UInt result = 0;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    result |= static_cast<UInt>(bytes[index]) << (index * 8U);
  }
  value = result;
  return true;
}

template <typename Int>
bool read_little_signed(std::istream& input, Int& value) {
  using UInt = typename std::make_unsigned<Int>::type;
  UInt raw = 0;
  if (!read_little_unsigned(input, raw)) {
    return false;
  }
  value = static_cast<Int>(raw);
  return true;
}

bool read_gguf_string(std::istream& input, std::string& value,
                      std::string& error) {
  std::uint64_t size = 0;
  if (!read_little_unsigned(input, size)) {
    error = "truncated GGUF string length";
    return false;
  }
  if (size > kMaxGgufStringBytes) {
    error = "GGUF string exceeds scanner limit";
    return false;
  }
  value.assign(static_cast<std::size_t>(size), '\0');
  if (size == 0) {
    return true;
  }
  if (!read_exact(input, value.data(), static_cast<std::size_t>(size))) {
    error = "truncated GGUF string payload";
    return false;
  }
  return true;
}

std::size_t scalar_type_width(std::uint32_t type) {
  switch (type) {
    case kGgufTypeUint8:
    case kGgufTypeInt8:
    case kGgufTypeBool:
      return 1;
    case kGgufTypeUint16:
    case kGgufTypeInt16:
      return 2;
    case kGgufTypeUint32:
    case kGgufTypeInt32:
    case kGgufTypeFloat32:
      return 4;
    case kGgufTypeUint64:
    case kGgufTypeInt64:
    case kGgufTypeFloat64:
      return 8;
    default:
      return 0;
  }
}

bool skip_bytes(std::istream& input, std::uint64_t bytes, std::string& error) {
  if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
    error = "GGUF value exceeds stream offset limit";
    return false;
  }
  input.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
  if (!input.good()) {
    error = "truncated GGUF value";
    return false;
  }
  return true;
}

bool skip_gguf_value(std::istream& input, std::uint32_t type,
                     std::string& error);

bool skip_gguf_array(std::istream& input, std::string& error) {
  std::uint32_t element_type = 0;
  std::uint64_t count = 0;
  if (!read_little_unsigned(input, element_type) ||
      !read_little_unsigned(input, count)) {
    error = "truncated GGUF array header";
    return false;
  }
  if (count > kMaxGgufArrayItems) {
    error = "GGUF array exceeds scanner limit";
    return false;
  }
  const std::size_t width = scalar_type_width(element_type);
  if (width != 0) {
    return skip_bytes(input, count * static_cast<std::uint64_t>(width), error);
  }
  for (std::uint64_t index = 0; index < count; ++index) {
    if (!skip_gguf_value(input, element_type, error)) {
      return false;
    }
  }
  return true;
}

bool skip_gguf_value(std::istream& input, std::uint32_t type,
                     std::string& error) {
  if (type == kGgufTypeString) {
    std::string ignored;
    return read_gguf_string(input, ignored, error);
  }
  if (type == kGgufTypeArray) {
    return skip_gguf_array(input, error);
  }
  const std::size_t width = scalar_type_width(type);
  if (width == 0) {
    error = "unsupported GGUF metadata type: " + std::to_string(type);
    return false;
  }
  return skip_bytes(input, width, error);
}

bool read_gguf_json_value(std::istream& input, std::uint32_t type, Json& value,
                          std::string& error) {
  switch (type) {
    case kGgufTypeUint8: {
      std::uint8_t raw = 0;
      if (!read_little_unsigned(input, raw)) {
        error = "truncated GGUF uint8";
        return false;
      }
      value = raw;
      return true;
    }
    case kGgufTypeInt8: {
      std::int8_t raw = 0;
      if (!read_little_signed(input, raw)) {
        error = "truncated GGUF int8";
        return false;
      }
      value = raw;
      return true;
    }
    case kGgufTypeUint16: {
      std::uint16_t raw = 0;
      if (!read_little_unsigned(input, raw)) {
        error = "truncated GGUF uint16";
        return false;
      }
      value = raw;
      return true;
    }
    case kGgufTypeInt16: {
      std::int16_t raw = 0;
      if (!read_little_signed(input, raw)) {
        error = "truncated GGUF int16";
        return false;
      }
      value = raw;
      return true;
    }
    case kGgufTypeUint32: {
      std::uint32_t raw = 0;
      if (!read_little_unsigned(input, raw)) {
        error = "truncated GGUF uint32";
        return false;
      }
      value = raw;
      return true;
    }
    case kGgufTypeInt32: {
      std::int32_t raw = 0;
      if (!read_little_signed(input, raw)) {
        error = "truncated GGUF int32";
        return false;
      }
      value = raw;
      return true;
    }
    case kGgufTypeBool: {
      std::uint8_t raw = 0;
      if (!read_little_unsigned(input, raw)) {
        error = "truncated GGUF bool";
        return false;
      }
      value = raw != 0;
      return true;
    }
    case kGgufTypeString: {
      std::string raw;
      if (!read_gguf_string(input, raw, error)) {
        return false;
      }
      value = raw;
      return true;
    }
    case kGgufTypeUint64: {
      std::uint64_t raw = 0;
      if (!read_little_unsigned(input, raw)) {
        error = "truncated GGUF uint64";
        return false;
      }
      value = raw;
      return true;
    }
    case kGgufTypeInt64: {
      std::int64_t raw = 0;
      if (!read_little_signed(input, raw)) {
        error = "truncated GGUF int64";
        return false;
      }
      value = raw;
      return true;
    }
    case kGgufTypeFloat32:
    case kGgufTypeFloat64:
    case kGgufTypeArray:
      return skip_gguf_value(input, type, error);
    default:
      error = "unsupported GGUF metadata type: " + std::to_string(type);
      return false;
  }
}

GgufParseResult parse_gguf_metadata(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return GgufParseResult{false, {}, "unable to open GGUF file"};
  }

  char magic[4] {};
  if (!read_exact(input, magic, sizeof(magic)) ||
      std::string_view(magic, sizeof(magic)) != "GGUF") {
    return GgufParseResult{false, {}, "missing GGUF magic"};
  }

  std::uint32_t version = 0;
  std::uint64_t tensor_count = 0;
  std::uint64_t metadata_count = 0;
  if (!read_little_unsigned(input, version) ||
      !read_little_unsigned(input, tensor_count) ||
      !read_little_unsigned(input, metadata_count)) {
    return GgufParseResult{false, {}, "truncated GGUF header"};
  }
  if (metadata_count > kMaxGgufMetadataEntries) {
    return GgufParseResult{false, {}, "GGUF metadata entry count exceeds scanner limit"};
  }

  ParsedGgufMetadata metadata;
  metadata.fields["gguf.version"] = version;
  metadata.fields["gguf.tensor_count"] = tensor_count;
  metadata.fields["gguf.metadata_count"] = metadata_count;

  for (std::uint64_t index = 0; index < metadata_count; ++index) {
    std::string key;
    std::string error;
    if (!read_gguf_string(input, key, error)) {
      return GgufParseResult{false, std::move(metadata), error};
    }
    std::uint32_t type = 0;
    if (!read_little_unsigned(input, type)) {
      return GgufParseResult{false, std::move(metadata),
                             "truncated GGUF metadata type"};
    }
    Json value;
    if (!read_gguf_json_value(input, type, value, error)) {
      return GgufParseResult{false, std::move(metadata), error};
    }
    if (!value.is_null()) {
      metadata.fields[key] = std::move(value);
    }
  }

  return GgufParseResult{true, std::move(metadata), {}};
}

std::optional<std::int64_t> metadata_int(const Json& metadata,
                                         const char* key) {
  if (!metadata.is_object()) {
    return std::nullopt;
  }
  const auto it = metadata.find(key);
  if (it == metadata.end()) {
    return std::nullopt;
  }
  if (it->is_number_integer()) {
    return it->get<std::int64_t>();
  }
  if (it->is_number_unsigned()) {
    const auto value = it->get<std::uint64_t>();
    if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return static_cast<std::int64_t>(value);
    }
  }
  return std::nullopt;
}

std::optional<std::string> metadata_string(const Json& metadata,
                                           const char* key) {
  if (!metadata.is_object()) {
    return std::nullopt;
  }
  const auto it = metadata.find(key);
  if (it == metadata.end() || !it->is_string()) {
    return std::nullopt;
  }
  const std::string value = trim_copy(it->get<std::string>());
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

std::string metadata_text_corpus(const std::filesystem::path& path,
                                 const Json& metadata) {
  std::ostringstream corpus;
  corpus << path.filename().generic_string() << ' '
         << path.parent_path().filename().generic_string() << ' '
         << path.parent_path().parent_path().filename().generic_string();
  if (metadata.is_object()) {
    for (auto it = metadata.begin(); it != metadata.end(); ++it) {
      corpus << ' ' << it.key();
      if (it->is_string()) {
        corpus << ' ' << it->get<std::string>();
      }
    }
  }
  return lowercase_copy(corpus.str());
}

struct PatternRule {
  const char* pattern;
  const char* usage;
};

std::optional<std::string> first_matching_usage(
    std::string_view corpus, const std::vector<PatternRule>& rules,
    std::vector<std::string>& matched) {
  for (const auto& rule : rules) {
    if (contains(corpus, rule.pattern)) {
      append_unique(matched, rule.pattern);
      return std::string(rule.usage);
    }
  }
  return std::nullopt;
}

bool matches_any(std::string_view corpus, const std::vector<const char*>& patterns,
                 std::vector<std::string>& matched) {
  bool found = false;
  for (const char* pattern : patterns) {
    if (contains(corpus, pattern)) {
      append_unique(matched, pattern);
      found = true;
    }
  }
  return found;
}

std::optional<double> model_size_billions(std::string_view corpus) {
  const std::string text(corpus);
  const std::regex pattern(R"((\d+(?:\.\d+)?)([bm]))",
                           std::regex_constants::icase);
  std::smatch match;
  if (!std::regex_search(text, match, pattern) || match.size() < 3) {
    return std::nullopt;
  }
  const double amount = std::stod(match[1].str());
  const std::string unit = lowercase_copy(match[2].str());
  return unit == "m" ? amount / 1000.0 : amount;
}

std::string quantization_from_file_type(std::int64_t file_type) {
  switch (file_type) {
    case 0:
      return "F32";
    case 1:
      return "F16";
    case 2:
      return "Q4_0";
    case 3:
      return "Q4_1";
    case 7:
      return "Q8_0";
    case 8:
      return "Q5_0";
    case 9:
      return "Q5_1";
    case 10:
      return "Q2_K";
    case 11:
      return "Q3_K_S";
    case 12:
      return "Q3_K_M";
    case 13:
      return "Q3_K_L";
    case 14:
      return "Q4_K_S";
    case 15:
      return "Q4_K_M";
    case 16:
      return "Q5_K_S";
    case 17:
      return "Q5_K_M";
    case 18:
      return "Q6_K";
    case 32:
      return "BF16";
    case 36:
      return "TQ1_0";
    case 37:
      return "TQ2_0";
    case 40:
      return "Q1_0";
    case 41:
      return "Q2_0";
    default:
      return {};
  }
}

std::string quantization_from_filename(std::string_view corpus) {
  static const std::vector<const char*> quantizations = {
      "q8_0",   "q6_k",   "q5_k_m", "q5_k_s", "q4_k_m", "q4_k_s",
      "q4_0",   "q4_1",   "q3_k_l", "q3_k_m", "q3_k_s", "tq1_0",
      "tq2_0",  "q1_0",   "q2_0",   "q2_k",
      "iq4_nl", "iq4_xs", "iq3_m",  "iq3_s",  "iq2_m",  "iq2_s",
      "f16",    "bf16",   "f32",
  };
  std::string normalized(corpus);
  std::replace(normalized.begin(), normalized.end(), '-', '_');
  std::replace(normalized.begin(), normalized.end(), '.', '_');
  for (const char* quantization : quantizations) {
    if (contains(normalized, quantization)) {
      std::string result = quantization;
      std::transform(result.begin(), result.end(), result.begin(),
                     [](unsigned char ch) {
                       return static_cast<char>(std::toupper(ch));
                     });
      return result;
    }
  }
  return {};
}

std::string infer_quantization(const std::string& corpus, const Json& metadata,
                               std::optional<std::int64_t>& file_type) {
  if (const auto type = metadata_int(metadata, "general.file_type")) {
    file_type = *type;
    const std::string from_type = quantization_from_file_type(*type);
    if (!from_type.empty()) {
      return from_type;
    }
  }
  return quantization_from_filename(corpus);
}

std::string embedding_quantization_preference(const std::string& quantization) {
  if (quantization == "F16" || quantization == "BF16" || quantization == "F32") {
    return "best_quality_more_ram";
  }
  if (quantization == "Q8_0") {
    return "recommended_default";
  }
  if (quantization == "Q6_K") {
    return "good_quality";
  }
  if (quantization == "Q5_K_M") {
    return "acceptable";
  }
  if (quantization == "Q4_K_M") {
    return "usable_if_constrained";
  }
  if (quantization.empty()) {
    return "unknown";
  }
  return "avoid_unless_memory_is_extremely_constrained";
}

std::string sentence_handler_quantization_preference(
    const std::string& quantization, std::optional<double> size_b) {
  if (quantization == "Q4_K_M" || quantization == "Q5_K_M") {
    return "recommended_speed_quality_balance";
  }
  if (quantization == "Q8_0") {
    return size_b && *size_b <= 1.0 ? "high_quality_for_tiny_model"
                                    : "high_quality_more_ram";
  }
  if (quantization == "F16" || quantization == "BF16" || quantization == "F32") {
    return "best_quality_more_ram";
  }
  if (quantization.empty()) {
    return "unknown";
  }
  return "usable";
}

LlamaDiscoveredModelCapability classify_capability(
    std::string_view corpus, const Json& metadata,
    std::vector<std::string>& matched, std::string& best_usage) {
  static const std::vector<PatternRule> reranker_rules = {
      {"bge-reranker-base", "rerank_memory_search_results"},
      {"bge-reranker-large", "rerank_memory_search_results"},
      {"jina-reranker", "rerank_memory_search_results"},
      {"reranker", "rerank_memory_search_results"},
      {"re-ranker", "rerank_memory_search_results"},
      {"cross-encoder", "rerank_memory_search_results"},
  };
  if (const auto usage = first_matching_usage(corpus, reranker_rules, matched)) {
    best_usage = *usage;
    return LlamaDiscoveredModelCapability::reranker;
  }

  static const std::vector<PatternRule> embedding_rules = {
      {"bge-small-en-v1.5", "default_sentence_embedding"},
      {"all-minilm-l6-v2", "fast_short_sentence_embedding"},
      {"all-minilm", "fast_short_sentence_embedding"},
      {"mxbai-embed-xsmall-v1", "ultra_light_embedding"},
      {"nomic-embed-text-v1.5", "paragraph_or_document_embedding"},
      {"jina-embeddings-v2", "long_context_embedding"},
      {"snowflake-arctic-embed", "general_embedding"},
      {"gte-small", "general_embedding"},
      {"e5-small", "query_document_embedding"},
      {"sentence-transformers", "general_embedding"},
      {"nomic-embed", "paragraph_or_document_embedding"},
      {"jina-embeddings", "long_context_embedding"},
      {"mxbai-embed", "general_embedding"},
      {"bge", "general_embedding"},
      {"minilm", "fast_short_sentence_embedding"},
      {"e5", "query_document_embedding"},
      {"gte", "general_embedding"},
  };
  if (const auto usage = first_matching_usage(corpus, embedding_rules, matched)) {
    best_usage = *usage;
    return LlamaDiscoveredModelCapability::embedding;
  }
  static const std::vector<PatternRule> high_capacity_memory_rules = {
      {"ternary-bonsai-27b", "memory_context_compilation_and_consolidation_review"},
      {"ternary bonsai 27b", "memory_context_compilation_and_consolidation_review"},
  };
  if (const auto usage =
          first_matching_usage(corpus, high_capacity_memory_rules, matched)) {
    best_usage = *usage;
    return LlamaDiscoveredModelCapability::chat;
  }
  static const std::vector<PatternRule> sentence_rules = {
      {"qwen3-0.6b", "local_sentence_classification_and_extraction"},
      {"qwen2.5-0.5b-instruct", "local_sentence_classification_and_extraction"},
      {"qwen2-0.5b-instruct", "local_sentence_classification_and_extraction"},
      {"smollm2-360m-instruct", "ultra_light_sentence_handler"},
      {"smollm2-1.7b-instruct", "better_local_summary_and_rewrite"},
      {"tinyllama-1.1b-chat", "lightweight_chat_or_rewrite"},
      {"phi-3-mini", "higher_quality_local_reasoning"},
      {"gemma-2-2b", "higher_quality_local_rewrite"},
  };
  if (const auto usage = first_matching_usage(corpus, sentence_rules, matched)) {
    best_usage = *usage;
    return LlamaDiscoveredModelCapability::sentence_handler;
  }

  static const std::vector<const char*> handler_markers = {
      "smol", "smollm", "tinyllama"};
  if (matches_any(corpus, handler_markers, matched)) {
    best_usage = contains(corpus, "smollm2-1.7b")
                     ? "better_local_summary_and_rewrite"
                     : "ultra_light_sentence_handler";
    return LlamaDiscoveredModelCapability::sentence_handler;
  }

  const bool has_local_llm_marker =
      contains(corpus, "qwen") || contains(corpus, "phi") ||
      contains(corpus, "gemma") || contains(corpus, "llama");
  const bool has_instruction_marker =
      contains(corpus, "instruct") || contains(corpus, "chat");
  if (has_local_llm_marker && has_instruction_marker) {
    const auto size = model_size_billions(corpus);
    if (size && *size <= 3.0) {
      append_unique(matched, "small-local-instruct");
      best_usage = "local_sentence_classification_and_extraction";
      return LlamaDiscoveredModelCapability::sentence_handler;
    }
  }

  static const std::vector<const char*> chat_markers = {
      "qwen", "qwen2", "qwen2.5", "qwen3", "phi", "gemma", "llama",
      "instruct", "chat"};
  if (matches_any(corpus, chat_markers, matched)) {
    best_usage = "local_chat";
    return LlamaDiscoveredModelCapability::chat;
  }

  // Decoder-only models also expose an embedding dimension in GGUF metadata;
  // that is the token embedding width, not evidence that the model is an
  // embedding-only encoder. Only encoder architectures may use metadata as a
  // final embedding-model signal after the explicit name rules above.
  const std::string architecture = lowercase_copy(
      metadata_string(metadata, "general.architecture").value_or(""));
  if (architecture == "bert" || architecture == "nomic-bert") {
    append_unique(matched, architecture);
    best_usage = "general_embedding";
    return LlamaDiscoveredModelCapability::embedding;
  }

  best_usage = "local_llama_model";
  return LlamaDiscoveredModelCapability::other;
}

std::vector<std::string> usage_notes_for(
    LlamaDiscoveredModelCapability capability) {
  switch (capability) {
    case LlamaDiscoveredModelCapability::embedding:
      return {
          "Use for sentence, memory chunk, and query vectors.",
          "Prefer this over chat models for primary memory search.",
          "Hybrid retrieval hint: 0.55 vector, 0.25 lexical, 0.15 graph, 0.05 recency_or_importance.",
      };
    case LlamaDiscoveredModelCapability::reranker:
      return {
          "Use after initial vector/ngram retrieval.",
          "Retrieve top 20-100 candidates, rerank, then keep top 5-20.",
      };
    case LlamaDiscoveredModelCapability::sentence_handler:
      return {
          "Use for memory worthiness, entity extraction, canonical rewrites, contradiction checks, graph edge suggestions, and context summaries.",
          "Do not use as the primary vector search model.",
      };
    case LlamaDiscoveredModelCapability::chat:
      return {"Use for local chat or rewrite tasks when a generative model is needed."};
    case LlamaDiscoveredModelCapability::other:
      return {"GGUF model with no known memory-system specialization inferred."};
  }
  return {};
}

std::optional<std::int64_t> infer_context_length(const Json& metadata) {
  for (const char* key : {"llama.context_length", "bert.context_length",
                          "nomic-bert.context_length",
                          "qwen2.context_length", "qwen3.context_length",
                          "qwen35.context_length"}) {
    if (const auto value = metadata_int(metadata, key)) {
      return value;
    }
  }
  return std::nullopt;
}

std::optional<std::int64_t> infer_block_count(const Json& metadata) {
  if (const auto architecture =
          metadata_string(metadata, "general.architecture")) {
    const std::string key = *architecture + ".block_count";
    if (const auto value = metadata_int(metadata, key.c_str())) {
      return value;
    }
  }
  for (auto entry = metadata.begin(); entry != metadata.end(); ++entry) {
    constexpr std::string_view suffix = ".block_count";
    if (entry.key().size() >= suffix.size() &&
        entry.key().compare(entry.key().size() - suffix.size(), suffix.size(),
                            suffix) == 0) {
      if (const auto value = metadata_int(metadata, entry.key().c_str())) {
        return value;
      }
    }
  }
  return std::nullopt;
}

Json discovery_runtime_hints(LlamaDiscoveredModelCapability capability) {
  if (capability == LlamaDiscoveredModelCapability::embedding) {
    return Json{{"hybridScore",
                 Json{{"vectorSimilarity", 0.55},
                      {"ngramOrBm25", 0.25},
                      {"graphProximity", 0.15},
                      {"recencyOrImportance", 0.05}}},
                {"preferredForPrimaryMemorySearch", true}};
  }
  if (capability == LlamaDiscoveredModelCapability::reranker) {
    return Json{{"initialCandidatesMin", 20},
                {"initialCandidatesMax", 100},
                {"finalContextMin", 5},
                {"finalContextMax", 20}};
  }
  if (capability == LlamaDiscoveredModelCapability::sentence_handler) {
    return Json{{"primaryEmbeddingSearch", false},
                {"tasks",
                 Json::array({"memory_worthiness_classification",
                              "entity_extraction",
                              "canonical_memory_rewrite",
                              "contradiction_detection",
                              "graph_edge_suggestion",
                              "context_compression"})}};
  }
  return Json::object();
}

Json discovery_metadata_json(const LlamaDiscoveredModel& model) {
  Json json = Json{{"source", "cli.localModelDiscovery"},
                   {"capability", to_string(model.capability)},
                   {"bestUsage", model.best_usage},
                   {"format", model.format},
                   {"path", model.path},
                   {"matchedPatterns", model.matched_patterns},
                   {"usageNotes", model.usage_notes},
                   {"runtimeHints", discovery_runtime_hints(model.capability)}};
  if (!model.quantization.empty()) {
    json["quantization"] = model.quantization;
  }
  if (!model.quantization_preference.empty()) {
    json["quantizationPreference"] = model.quantization_preference;
  }
  if (model.context_length) {
    json["contextLength"] = *model.context_length;
  }
  if (model.block_count) {
    json["blockCount"] = *model.block_count;
  }
  if (model.embedding_length) {
    json["embeddingLength"] = *model.embedding_length;
  }
  if (model.gguf_file_type) {
    json["ggufFileType"] = *model.gguf_file_type;
  }
  if (!model.gguf_metadata.empty()) {
    json["ggufMetadata"] = model.gguf_metadata;
  }
  if (!model.warnings.empty()) {
    json["warnings"] = model.warnings;
  }
  return json;
}

Json zero_cost_metadata() {
  return Json{{"input", 0.0},
              {"output", 0.0},
              {"cacheRead", 0.0},
              {"cacheWrite", 0.0},
              {"input_usd_per_million", 0.0},
              {"output_usd_per_million", 0.0},
              {"cache_read_usd_per_million", 0.0},
              {"cache_write_usd_per_million", 0.0}};
}

ProviderCapabilities discovered_model_capabilities(
    LlamaDiscoveredModelCapability capability) {
  ProviderCapabilities capabilities;
  capabilities.streaming = capability != LlamaDiscoveredModelCapability::embedding &&
                           capability != LlamaDiscoveredModelCapability::reranker;
  capabilities.tool_calls = capabilities.streaming;
  capabilities.vision = false;
  capabilities.reasoning =
      capability == LlamaDiscoveredModelCapability::chat ||
      capability == LlamaDiscoveredModelCapability::sentence_handler;
  capabilities.session_caching = capabilities.streaming;
  return capabilities;
}

std::optional<LlamaModelSettings> find_settings_model(
    const LlamaSettings* settings, const std::string& id) {
  if (settings == nullptr) {
    return std::nullopt;
  }
  for (const auto& model : settings->models) {
    if (lowercase_copy(model.id) == lowercase_copy(id)) {
      return model;
    }
  }
  return std::nullopt;
}

Json llama_metadata_for_discovered_model(const LlamaDiscoveredModel& model,
                                         const LlamaSettings* settings) {
  const auto settings_model = find_settings_model(settings, model.id)
                                  .value_or(llama_model_settings_from_discovery(model));
  Json generation = Json::object();
  if (settings != nullptr) {
    to_json(generation, merge_llama_generation_settings(settings->generation,
                                                        settings_model.generation));
  }
  return Json{{"model", settings_model}, {"generation", generation}};
}

void append_file_if_model(std::vector<std::filesystem::path>& paths,
                          const std::filesystem::path& path) {
  if (path_lower_extension(path) == ".gguf") {
    paths.push_back(path);
  }
}

std::vector<std::filesystem::path> collect_candidate_paths(
    const LlamaModelDiscoveryOptions& options,
    std::vector<std::string>& diagnostics) {
  std::vector<std::filesystem::path> paths;
  for (const auto& root_text : options.roots) {
    const std::string root_trimmed = trim_copy(root_text);
    if (root_trimmed.empty()) {
      diagnostics.push_back("local model discovery skipped empty root");
      continue;
    }
    const std::filesystem::path root(root_trimmed);
    std::error_code error;
    const auto status = std::filesystem::status(root, error);
    if (error || !std::filesystem::exists(status)) {
      diagnostics.push_back("local model discovery path does not exist: " +
                            root.generic_string());
      continue;
    }
    if (std::filesystem::is_regular_file(status)) {
      append_file_if_model(paths, root);
      continue;
    }
    if (!std::filesystem::is_directory(status)) {
      diagnostics.push_back("local model discovery path is not a file or directory: " +
                            root.generic_string());
      continue;
    }
    if (options.recursive) {
      std::filesystem::recursive_directory_iterator it(
          root, std::filesystem::directory_options::skip_permission_denied, error);
      std::filesystem::recursive_directory_iterator end;
      while (!error && it != end) {
        std::error_code entry_error;
        if (it->is_regular_file(entry_error) && !entry_error) {
          append_file_if_model(paths, it->path());
        }
        it.increment(error);
      }
      if (error) {
        diagnostics.push_back("local model discovery could not fully scan " +
                              root.generic_string() + ": " + error.message());
      }
    } else {
      std::filesystem::directory_iterator it(
          root, std::filesystem::directory_options::skip_permission_denied, error);
      std::filesystem::directory_iterator end;
      while (!error && it != end) {
        std::error_code entry_error;
        if (it->is_regular_file(entry_error) && !entry_error) {
          append_file_if_model(paths, it->path());
        }
        it.increment(error);
      }
      if (error) {
        diagnostics.push_back("local model discovery could not scan " +
                              root.generic_string() + ": " + error.message());
      }
    }
  }

  std::sort(paths.begin(), paths.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.generic_string() < rhs.generic_string();
  });
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

LlamaDiscoveredModel inspect_model_file(const std::filesystem::path& path) {
  LlamaDiscoveredModel model;
  model.path = path.lexically_normal().generic_string();

  const auto parsed = parse_gguf_metadata(path);
  if (parsed.ok) {
    model.gguf_metadata = parsed.metadata.fields;
    model.warnings = parsed.metadata.warnings;
  } else {
    model.warnings.push_back("GGUF metadata parse failed: " + parsed.error);
  }

  model.name = metadata_string(model.gguf_metadata, "general.name")
                   .value_or(path.stem().generic_string());
  const std::string corpus = metadata_text_corpus(path, model.gguf_metadata);
  model.block_count = infer_block_count(model.gguf_metadata);
  model.context_length = infer_context_length(model.gguf_metadata);
  model.embedding_length = metadata_int(model.gguf_metadata, "embedding.length");
  model.quantization =
      infer_quantization(corpus, model.gguf_metadata, model.gguf_file_type);

  model.capability = classify_capability(
      corpus, model.gguf_metadata, model.matched_patterns, model.best_usage);
  if (model.capability == LlamaDiscoveredModelCapability::embedding) {
    model.quantization_preference =
        embedding_quantization_preference(model.quantization);
  } else if (model.capability ==
             LlamaDiscoveredModelCapability::sentence_handler) {
    model.quantization_preference = sentence_handler_quantization_preference(
        model.quantization, model_size_billions(corpus));
  } else if (model.capability == LlamaDiscoveredModelCapability::reranker) {
    model.quantization_preference = "precision_over_latency";
  } else {
    model.quantization_preference =
        model.quantization.empty() ? "unknown" : "model_dependent";
  }
  model.usage_notes = usage_notes_for(model.capability);
  return model;
}

void assign_unique_ids(std::vector<LlamaDiscoveredModel>& models) {
  std::set<std::string> used;
  for (auto& model : models) {
    std::string base = slugify(std::filesystem::path(model.path).stem().string());
    std::string candidate = base;
    int suffix = 2;
    while (!used.insert(lowercase_copy(candidate)).second) {
      candidate = base + "-" + std::to_string(suffix++);
    }
    model.id = std::move(candidate);
  }
}

}  // namespace

std::string to_string(LlamaDiscoveredModelCapability capability) {
  switch (capability) {
    case LlamaDiscoveredModelCapability::embedding:
      return "embedding";
    case LlamaDiscoveredModelCapability::reranker:
      return "reranker";
    case LlamaDiscoveredModelCapability::sentence_handler:
      return "sentence_handler";
    case LlamaDiscoveredModelCapability::chat:
      return "chat";
    case LlamaDiscoveredModelCapability::other:
      return "other";
  }
  return "other";
}

std::string to_string(LlamaMemoryTask task) {
  switch (task) {
    case LlamaMemoryTask::document_embedding:
      return "document_embedding";
    case LlamaMemoryTask::short_embedding:
      return "short_embedding";
    case LlamaMemoryTask::worthiness:
      return "worthiness";
    case LlamaMemoryTask::extraction:
      return "extraction";
    case LlamaMemoryTask::recall_prehandler:
      return "recall_prehandler";
    case LlamaMemoryTask::context_compilation:
      return "context_compilation";
  }
  return "unknown";
}

const LlamaModelSettings* select_llama_model_for_memory_task(
    const LlamaSettings& settings, LlamaMemoryTask task) {
  const auto preference = [task](const LlamaModelSettings& model) {
    const std::string corpus = lowercase_copy(
        model.id + " " + model.name + " " + model.path);
    const auto has = [&](std::string_view value) {
      return corpus.find(value) != std::string::npos;
    };
    int score = -1;
    switch (task) {
      case LlamaMemoryTask::document_embedding:
        if (has("nomic-embed-text-v1.5")) score = 500;
        else if (has("bge-small-en-v1.5")) score = 400;
        break;
      case LlamaMemoryTask::short_embedding:
        if (has("bge-small-en-v1.5")) score = 500;
        else if (has("nomic-embed-text-v1.5")) score = 400;
        break;
      case LlamaMemoryTask::worthiness:
        if (has("smollm2-135m-instruct")) score = 500;
        else if (has("qwen2.5-0.5b-instruct") ||
                 has("qwen2-5-0-5b-instruct")) score = 400;
        break;
      case LlamaMemoryTask::extraction:
        if (has("qwen2.5-0.5b-instruct") ||
            has("qwen2-5-0-5b-instruct")) score = 500;
        else if (has("qwen2.5-3b-instruct") ||
                 has("qwen2-5-3b-instruct")) score = 400;
        break;
      case LlamaMemoryTask::recall_prehandler:
        if (has("qwen2.5-3b-instruct") ||
            has("qwen2-5-3b-instruct")) score = 600;
        else if (has("qwen2.5-0.5b-instruct") ||
                 has("qwen2-5-0-5b-instruct")) score = 450;
        else if (has("smollm2-135m-instruct")) score = 300;
        break;
      case LlamaMemoryTask::context_compilation:
        if (has("ternary-bonsai-27b") || has("ternary bonsai 27b")) {
          score = 1100;
        } else if (has("qwen2.5-3b-instruct") ||
            has("qwen2-5-3b-instruct")) score = 600;
        else if (has("qwen2.5-0.5b-instruct") ||
                 has("qwen2-5-0-5b-instruct")) score = 450;
        else if (has("smollm2-135m-instruct")) score = 300;
        break;
    }
    if (score < 0) return score;
    if (has("q8_0") || has("q8-0")) score += 30;
    if (has("q4_k_m") || has("q4-k-m")) {
      score += task == LlamaMemoryTask::context_compilation ? 40 : 20;
    }
    if (has("f16")) score += 5;
    return score;
  };

  const LlamaModelSettings* selected = nullptr;
  int selected_score = -1;
  for (const auto& model : settings.models) {
    std::error_code error;
    if (model.path.empty() ||
        !std::filesystem::is_regular_file(model.path, error)) {
      continue;
    }
    const int score = preference(model);
    if (score > selected_score ||
        (score == selected_score && selected != nullptr &&
         model.id < selected->id)) {
      selected = &model;
      selected_score = score;
    }
  }
  return selected;
}

LlamaModelDiscoveryResult discover_llama_models(
    const LlamaModelDiscoveryOptions& options) {
  LlamaModelDiscoveryResult result;
  const auto paths = collect_candidate_paths(options, result.diagnostics);
  result.models.reserve(paths.size());
  for (const auto& path : paths) {
    auto model = inspect_model_file(path);
    for (const auto& warning : model.warnings) {
      result.diagnostics.push_back(model.path + ": " + warning);
    }
    result.models.push_back(std::move(model));
  }
  assign_unique_ids(result.models);
  return result;
}

LlamaModelSettings llama_model_settings_from_discovery(
    const LlamaDiscoveredModel& model) {
  LlamaModelSettings settings;
  settings.id = model.id;
  settings.name = model.name;
  settings.path = model.path;
  settings.block_count = model.block_count;
  settings.context_window = model.context_length;
  settings.supports_embeddings =
      model.capability == LlamaDiscoveredModelCapability::embedding;
  return settings;
}

ModelCatalog model_catalog_from_llama_discovery(
    const LlamaModelDiscoveryResult& discovery, const LlamaSettings* settings) {
  ModelCatalog catalog;
  if (discovery.models.empty()) {
    return catalog;
  }

  ModelCatalogProviderRecord provider;
  provider.descriptor.id = kLlamaProviderId;
  provider.descriptor.display_name = "llama.cpp";
  provider.descriptor.api = kLlamaProviderApi;
  provider.descriptor.transport = ProviderTransport::auto_detect;
  provider.descriptor.capabilities.streaming = true;
  provider.descriptor.capabilities.tool_calls = true;
  provider.descriptor.capabilities.vision = false;
  provider.descriptor.capabilities.reasoning = false;
  provider.descriptor.capabilities.session_caching = true;
  provider.metadata = Json{{"local", true},
                           {"direct", true},
                           {"source", "cli.localModelDiscovery"},
                           {"api", kLlamaProviderApi}};
  catalog.providers.push_back(std::move(provider));

  for (const auto& discovered : discovery.models) {
    ModelCatalogModelRecord record;
    record.ref = ModelRef{kLlamaProviderId, kLlamaProviderApi, discovered.id};
    record.display_name = discovered.name;
    record.capabilities = discovered_model_capabilities(discovered.capability);
    record.metadata = Json{{"input", Json::array({"text"})},
                           {"cost", zero_cost_metadata()},
                           {"path", discovered.path},
                           {"local", true},
                           {"direct", true},
                           {"format", discovered.format},
                           {"localDiscovery",
                            discovery_metadata_json(discovered)},
                           {"llama",
                            llama_metadata_for_discovered_model(discovered,
                                                                settings)}};
    if (discovered.context_length) {
      record.metadata["contextWindow"] = *discovered.context_length;
    }
    if (discovered.capability == LlamaDiscoveredModelCapability::embedding) {
      record.metadata["supportsEmbeddings"] = true;
    }
    catalog.models.push_back(std::move(record));
  }

  return normalized(catalog);
}

void enrich_model_catalog_with_llama_discovery(
    ModelCatalog& catalog, const LlamaModelDiscoveryResult& discovery) {
  for (const auto& discovered : discovery.models) {
    auto model = std::find_if(
        catalog.models.begin(), catalog.models.end(),
        [&](const ModelCatalogModelRecord& candidate) {
          return lowercase_copy(candidate.ref.provider) ==
                     lowercase_copy(kLlamaProviderId) &&
                 lowercase_copy(candidate.ref.name) == lowercase_copy(discovered.id);
        });
    if (model == catalog.models.end()) {
      continue;
    }
    model->capabilities = discovered_model_capabilities(discovered.capability);
    model->metadata["localDiscovery"] = discovery_metadata_json(discovered);
    model->metadata["format"] = discovered.format;
    model->metadata["path"] = discovered.path;
    model->metadata["local"] = true;
    model->metadata["direct"] = true;
    if (discovered.context_length) {
      model->metadata["contextWindow"] = *discovered.context_length;
    }
    if (discovered.capability == LlamaDiscoveredModelCapability::embedding) {
      model->metadata["supportsEmbeddings"] = true;
    } else {
      model->metadata.erase("supportsEmbeddings");
    }
  }
  catalog = normalized(catalog);
}

void to_json(Json& json, const LlamaDiscoveredModel& model) {
  json = Json{{"id", model.id},
              {"name", model.name},
              {"path", model.path},
              {"format", model.format},
              {"capability", to_string(model.capability)},
              {"bestUsage", model.best_usage},
              {"matchedPatterns", model.matched_patterns},
              {"usageNotes", model.usage_notes},
              {"warnings", model.warnings}};
  if (!model.quantization.empty()) {
    json["quantization"] = model.quantization;
  }
  if (!model.quantization_preference.empty()) {
    json["quantizationPreference"] = model.quantization_preference;
  }
  if (model.context_length) {
    json["contextLength"] = *model.context_length;
  }
  if (model.block_count) {
    json["blockCount"] = *model.block_count;
  }
  if (model.embedding_length) {
    json["embeddingLength"] = *model.embedding_length;
  }
  if (model.gguf_file_type) {
    json["ggufFileType"] = *model.gguf_file_type;
  }
  if (!model.gguf_metadata.empty()) {
    json["ggufMetadata"] = model.gguf_metadata;
  }
}

void to_json(Json& json, const LlamaModelDiscoveryResult& result) {
  json = Json{{"models", result.models}, {"diagnostics", result.diagnostics}};
}

}  // namespace qt::ai
