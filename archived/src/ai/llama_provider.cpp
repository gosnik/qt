#include "qt/ai/llama_provider.hpp"

#include <ggml-backend.h>
#include <llama.h>
#include <llama-ext.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>

namespace qt::ai {
namespace {

constexpr std::int64_t kDefaultMaxOutputTokens = 256;
constexpr double kDefaultLlamaTemperature = 0.3;
constexpr double kDefaultLlamaTopP = 0.9;
constexpr std::int64_t kContextPruneHighWatermarkPercent = 70;
constexpr std::int64_t kContextPruneLowWatermarkPercent = 50;

void quiet_llama_log_callback(ggml_log_level level, const char* text,
                              void* user_data) {
  (void)level;
  (void)text;
  (void)user_data;
}

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

std::string read_text_file(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("failed to read llama grammar file: " + path);
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

const char* role_label(MessageRole role) {
  switch (role) {
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

void append_content_text(std::ostringstream& out, const Content& content) {
  if (const auto* text = std::get_if<TextContent>(&content)) {
    out << text->text;
    return;
  }
  if (const auto* thinking = std::get_if<ThinkingContent>(&content)) {
    out << thinking->text;
    return;
  }
  if (const auto* image = std::get_if<ImageContent>(&content)) {
    out << "[image:" << image->media_type << "]";
    return;
  }
  if (const auto* call = std::get_if<ToolCallContent>(&content)) {
    out << "[tool_call name=" << call->name << " id=" << call->id
        << " arguments=" << call->arguments.dump() << "]";
    return;
  }
  if (const auto* result = std::get_if<ToolResultContent>(&content)) {
    Json payload = Json{{"name", result->name},
                        {"id", result->tool_call_id},
                        {"result", result->result}};
    if (result->is_error) {
      payload["is_error"] = true;
    }
    out << "tool_result: " << payload.dump();
  }
}

template <typename T>
void overlay_optional(std::optional<T>& target, const std::optional<T>& value) {
  if (value) {
    target = value;
  }
}

std::optional<double> json_double(const Json& json, const char* key) {
  if (!json.is_object()) {
    return std::nullopt;
  }
  const auto it = json.find(key);
  if (it == json.end() || !it->is_number()) {
    return std::nullopt;
  }
  return it->get<double>();
}

std::optional<int> json_int(const Json& json, const char* key) {
  if (!json.is_object()) {
    return std::nullopt;
  }
  const auto it = json.find(key);
  if (it == json.end() || !it->is_number_integer()) {
    return std::nullopt;
  }
  return it->get<int>();
}

std::optional<std::int64_t> json_int64(const Json& json, const char* key) {
  if (!json.is_object()) {
    return std::nullopt;
  }
  const auto it = json.find(key);
  if (it == json.end() || !it->is_number_integer()) {
    return std::nullopt;
  }
  return it->get<std::int64_t>();
}

std::optional<std::uint32_t> json_uint32(const Json& json, const char* key) {
  if (!json.is_object()) {
    return std::nullopt;
  }
  const auto it = json.find(key);
  if (it == json.end() || !it->is_number_unsigned()) {
    return std::nullopt;
  }
  const auto value = it->get<std::uint64_t>();
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

std::optional<std::string> json_string(const Json& json, const char* key) {
  if (!json.is_object()) {
    return std::nullopt;
  }
  const auto it = json.find(key);
  if (it == json.end() || !it->is_string()) {
    return std::nullopt;
  }
  return it->get<std::string>();
}

std::optional<bool> json_bool(const Json& json, const char* key) {
  if (!json.is_object()) {
    return std::nullopt;
  }
  const auto it = json.find(key);
  if (it == json.end() || !it->is_boolean()) {
    return std::nullopt;
  }
  return it->get<bool>();
}

LlamaGenerationSettings generation_from_json(const Json& json) {
  LlamaGenerationSettings settings;
  settings.temperature = json_double(json, "temperature");
  settings.top_k = json_int(json, "topK");
  settings.top_p = json_double(json, "topP");
  settings.min_p = json_double(json, "minP");
  settings.min_keep = json_int(json, "minKeep");
  settings.seed = json_uint32(json, "seed");
  settings.max_output_tokens = json_int64(json, "maxOutputTokens");
  settings.repeat_penalty = json_double(json, "repeatPenalty");
  settings.presence_penalty = json_double(json, "presencePenalty");
  settings.frequency_penalty = json_double(json, "frequencyPenalty");
  return settings;
}

void overlay_generation_json(LlamaGenerationSettings& target, const Json& json) {
  if (!json.is_object()) {
    return;
  }
  LlamaGenerationSettings overlay = generation_from_json(json);
  target = merge_llama_generation_settings(target, overlay);
}

void overlay_generation_metadata(LlamaGenerationSettings& target,
                                 const Json& metadata) {
  if (!metadata.is_object()) {
    return;
  }
  overlay_generation_json(target, metadata);
  for (const char* key : {"generation", "llamaGeneration"}) {
    const auto it = metadata.find(key);
    if (it != metadata.end()) {
      overlay_generation_json(target, *it);
    }
  }
  const auto llama_it = metadata.find("llama");
  if (llama_it != metadata.end() && llama_it->is_object()) {
    overlay_generation_json(target, *llama_it);
    const auto generation_it = llama_it->find("generation");
    if (generation_it != llama_it->end()) {
      overlay_generation_json(target, *generation_it);
    }
  }
}

struct ResolvedLlamaGrammar {
  bool enabled = false;
  std::string grammar;
  std::string root = "root";
  std::string source;
};

ResolvedLlamaGrammar inline_grammar(std::string grammar, std::string root,
                                    std::string source) {
  ResolvedLlamaGrammar resolved;
  resolved.enabled = !trim_copy(grammar).empty();
  resolved.grammar = std::move(grammar);
  resolved.root = trim_copy(root).empty() ? std::string("root") : std::move(root);
  resolved.source = std::move(source);
  return resolved;
}

ResolvedLlamaGrammar named_or_inline_grammar(
    const LlamaGrammarSettings& settings, const std::string& value,
    const std::string& root, const std::string& source) {
  const std::string trimmed = trim_copy(value);
  if (trimmed.empty() || lowercase_copy(trimmed) == "off" ||
      lowercase_copy(trimmed) == "none") {
    return {};
  }

  const auto file = settings.files.find(trimmed);
  if (file != settings.files.end()) {
    return inline_grammar(read_text_file(file->second), root,
                          "file:" + file->second);
  }
  return inline_grammar(trimmed, root, source);
}

void apply_grammar_json(ResolvedLlamaGrammar& target,
                        const LlamaGrammarSettings& settings,
                        const Json& json) {
  if (json.is_null()) {
    return;
  }
  if (json.is_string()) {
    target = named_or_inline_grammar(settings, json.get<std::string>(),
                                     target.root, "inline");
    return;
  }
  if (!json.is_object()) {
    return;
  }

  if (const auto enabled = json_bool(json, "enabled");
      enabled && !*enabled) {
    target = {};
    return;
  }
  if (const auto root = json_string(json, "root")) {
    const std::string trimmed = trim_copy(*root);
    if (!trimmed.empty()) {
      target.root = trimmed;
    }
  }
  if (const auto name = json_string(json, "name")) {
    target = named_or_inline_grammar(settings, *name, target.root,
                                     "configured:" + *name);
  }
  if (const auto file = json_string(json, "file")) {
    target = inline_grammar(read_text_file(*file), target.root,
                            "file:" + *file);
  }
  for (const char* key : {"inline", "grammar", "text"}) {
    if (const auto grammar = json_string(json, key)) {
      target = inline_grammar(*grammar, target.root, "inline");
    }
  }
}

void apply_grammar_metadata(ResolvedLlamaGrammar& target,
                            const LlamaGrammarSettings& settings,
                            const Json& metadata) {
  if (!metadata.is_object()) {
    return;
  }
  const auto grammar = metadata.find("grammar");
  if (grammar != metadata.end()) {
    apply_grammar_json(target, settings, *grammar);
  }
  const auto llama = metadata.find("llama");
  if (llama != metadata.end() && llama->is_object()) {
    const auto llama_grammar = llama->find("grammar");
    if (llama_grammar != llama->end()) {
      apply_grammar_json(target, settings, *llama_grammar);
    }
  }
}

ResolvedLlamaGrammar resolve_llama_grammar(
    const LlamaSettings& settings, const CompletionRequest& request,
    const ProviderOptions& options) {
  ResolvedLlamaGrammar resolved;
  resolved.root = settings.grammar.root.value_or("root");
  if (settings.grammar.default_grammar) {
    resolved = named_or_inline_grammar(settings.grammar,
                                       *settings.grammar.default_grammar,
                                       resolved.root, "default");
  }
  apply_grammar_metadata(resolved, settings.grammar, options.metadata);
  apply_grammar_metadata(resolved, settings.grammar, request.metadata);
  return resolved;
}

std::int64_t resolved_max_output_tokens(
    const LlamaGenerationSettings& settings) {
  return settings.max_output_tokens.value_or(kDefaultMaxOutputTokens);
}

Json sampler_step_parameters(const LlamaGenerationSettings& settings) {
  Json json = Json::object();
  to_json(json, settings);
  return json;
}

std::uint32_t sampler_seed(const LlamaGenerationSettings& settings) {
  return settings.seed.value_or(LLAMA_DEFAULT_SEED);
}

std::uint32_t checked_u32(std::int64_t value, const char* field) {
  if (value < 0 ||
      value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error(std::string("llama ") + field +
                             " exceeds uint32 range");
  }
  return static_cast<std::uint32_t>(value);
}

llama_split_mode split_mode_from_settings(const std::optional<std::string>& value) {
  if (!value) {
    return LLAMA_SPLIT_MODE_LAYER;
  }
  const std::string normalized = lowercase_copy(trim_copy(*value));
  if (normalized == "none") {
    return LLAMA_SPLIT_MODE_NONE;
  }
  if (normalized == "row") {
    return LLAMA_SPLIT_MODE_ROW;
  }
  return LLAMA_SPLIT_MODE_LAYER;
}

enum llama_pooling_type pooling_from_settings(
    const std::optional<std::string>& value) {
  if (!value) {
    return LLAMA_POOLING_TYPE_UNSPECIFIED;
  }
  const std::string normalized = lowercase_copy(trim_copy(*value));
  if (normalized == "none") {
    return LLAMA_POOLING_TYPE_NONE;
  }
  if (normalized == "mean") {
    return LLAMA_POOLING_TYPE_MEAN;
  }
  if (normalized == "cls") {
    return LLAMA_POOLING_TYPE_CLS;
  }
  if (normalized == "last") {
    return LLAMA_POOLING_TYPE_LAST;
  }
  if (normalized == "rank") {
    return LLAMA_POOLING_TYPE_RANK;
  }
  return LLAMA_POOLING_TYPE_UNSPECIFIED;
}

bool retains_kv_cache(const LlamaKvCacheSettings& settings) {
  return lowercase_copy(settings.policy.value_or("session")) == "session";
}

template <typename T>
Json optional_status_value(const std::optional<T>& value) {
  return value ? Json(*value) : Json(nullptr);
}

Json backend_settings_status(const LlamaBackendSettings& settings) {
  return Json{{"gpuLayers", optional_status_value(settings.gpu_layers)},
              {"mainGpu", optional_status_value(settings.main_gpu)},
              {"splitMode", optional_status_value(settings.split_mode)},
              {"offloadKqv", optional_status_value(settings.offload_kqv)},
              {"useMmap", optional_status_value(settings.use_mmap)},
              {"useMlock", optional_status_value(settings.use_mlock)},
              {"tensorSplitConfigured", settings.tensor_split_configured},
              {"tensorSplitCount", settings.tensor_split.size()}};
}

Json memory_breakdown_data_json(const llama_memory_breakdown_data& data) {
  const auto model = static_cast<std::uint64_t>(data.model);
  const auto context = static_cast<std::uint64_t>(data.context);
  const auto compute = static_cast<std::uint64_t>(data.compute);
  return Json{{"modelBytes", model},
              {"contextBytes", context},
              {"computeBytes", compute},
              {"allocatedBytes", model + context + compute}};
}

const char* backend_device_type_name(enum ggml_backend_dev_type type) {
  switch (type) {
    case GGML_BACKEND_DEVICE_TYPE_CPU:
      return "cpu";
    case GGML_BACKEND_DEVICE_TYPE_GPU:
      return "gpu";
    case GGML_BACKEND_DEVICE_TYPE_IGPU:
      return "igpu";
    case GGML_BACKEND_DEVICE_TYPE_ACCEL:
      return "accelerator";
    case GGML_BACKEND_DEVICE_TYPE_META:
      return "meta";
  }
  return "unknown";
}

Json llama_memory_status_json(const llama_model* model,
                              const llama_context* context) {
  Json memory = Json{{"devices", Json::array()},
                     {"host", memory_breakdown_data_json({})},
                     {"buffers", Json::array()}};
  if (model == nullptr || context == nullptr) {
    return memory;
  }

  std::vector<ggml_backend_dev_t> devices;
  const int32_t device_count = llama_model_n_devices(model);
  devices.reserve(device_count > 0 ? static_cast<std::size_t>(device_count)
                                   : 0U);
  for (int32_t index = 0; index < device_count; ++index) {
    ggml_backend_dev_t device = llama_model_get_device(model, index);
    if (device != nullptr) {
      devices.push_back(device);
    }
  }

  std::vector<llama_memory_breakdown_data> device_memory(devices.size());
  llama_memory_breakdown_data host_memory;
  std::set<ggml_backend_buffer_type_t> seen_buffer_types;
  const llama_memory_breakdown breakdown = llama_get_memory_breakdown(context);

  for (const auto& entry : breakdown) {
    ggml_backend_buffer_type_t buffer_type = entry.first;
    const llama_memory_breakdown_data& data = entry.second;
    if (ggml_backend_buft_is_host(buffer_type)) {
      host_memory.model += data.model;
      host_memory.context += data.context;
      host_memory.compute += data.compute;
      seen_buffer_types.insert(buffer_type);
      continue;
    }

    ggml_backend_dev_t device = ggml_backend_buft_get_device(buffer_type);
    if (device == nullptr) {
      continue;
    }
    for (std::size_t index = 0; index < devices.size(); ++index) {
      if (devices[index] == device) {
        device_memory[index].model += data.model;
        device_memory[index].context += data.context;
        device_memory[index].compute += data.compute;
        seen_buffer_types.insert(buffer_type);
        break;
      }
    }
  }

  for (std::size_t index = 0; index < devices.size(); ++index) {
    ggml_backend_dev_t device = devices[index];
    std::size_t free_bytes = 0;
    std::size_t total_bytes = 0;
    ggml_backend_dev_memory(device, &free_bytes, &total_bytes);

    Json device_json = memory_breakdown_data_json(device_memory[index]);
    const enum ggml_backend_dev_type type = ggml_backend_dev_type(device);
    device_json["index"] = index;
    device_json["name"] = ggml_backend_dev_name(device);
    device_json["description"] = ggml_backend_dev_description(device);
    device_json["type"] = backend_device_type_name(type);
    device_json["gpuMemory"] = type == GGML_BACKEND_DEVICE_TYPE_GPU ||
                               type == GGML_BACKEND_DEVICE_TYPE_IGPU;
    device_json["freeBytes"] = static_cast<std::uint64_t>(free_bytes);
    device_json["totalBytes"] = static_cast<std::uint64_t>(total_bytes);
    const auto allocated = device_json.value("allocatedBytes", std::uint64_t{0});
    const auto used = total_bytes >= free_bytes
                          ? static_cast<std::uint64_t>(total_bytes - free_bytes)
                          : std::uint64_t{0};
    device_json["usedBytes"] = used;
    device_json["unaccountedBytes"] =
        used >= allocated ? used - allocated : std::uint64_t{0};
    memory["devices"].push_back(std::move(device_json));
  }

  memory["host"] = memory_breakdown_data_json(host_memory);
  for (const auto& entry : breakdown) {
    if (seen_buffer_types.count(entry.first) != 0U) {
      continue;
    }
    Json buffer_json = memory_breakdown_data_json(entry.second);
    buffer_json["name"] = ggml_backend_buft_name(entry.first);
    memory["buffers"].push_back(std::move(buffer_json));
  }
  return memory;
}

void normalize_embedding(std::vector<float>& embedding) {
  double norm_squared = 0.0;
  for (const float value : embedding) {
    norm_squared += static_cast<double>(value) * static_cast<double>(value);
  }
  if (norm_squared <= 0.0) {
    return;
  }
  const double scale = 1.0 / std::sqrt(norm_squared);
  for (float& value : embedding) {
    value = static_cast<float>(static_cast<double>(value) * scale);
  }
}

struct BackendGuardState {
  std::mutex mutex;
  std::size_t ref_count = 0;
};

BackendGuardState& backend_guard_state() {
  static BackendGuardState state;
  return state;
}

class LlamaBackendGuard {
 public:
  LlamaBackendGuard() {
    auto& state = backend_guard_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.ref_count == 0) {
      llama_log_set(quiet_llama_log_callback, nullptr);
      llama_backend_init();
    }
    ++state.ref_count;
  }

  ~LlamaBackendGuard() {
    auto& state = backend_guard_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.ref_count == 0) {
      return;
    }
    --state.ref_count;
    if (state.ref_count == 0) {
      llama_backend_free();
    }
  }

  LlamaBackendGuard(const LlamaBackendGuard&) = delete;
  LlamaBackendGuard& operator=(const LlamaBackendGuard&) = delete;
};

struct LlamaModelDeleter {
  void operator()(llama_model* model) const noexcept {
    if (model != nullptr) {
      llama_model_free(model);
    }
  }
};

struct LlamaContextDeleter {
  void operator()(llama_context* context) const noexcept {
    if (context != nullptr) {
      llama_free(context);
    }
  }
};

struct LlamaSamplerDeleter {
  void operator()(llama_sampler* sampler) const noexcept {
    if (sampler != nullptr) {
      llama_sampler_free(sampler);
    }
  }
};

using LlamaModelPtr = std::unique_ptr<llama_model, LlamaModelDeleter>;
using LlamaContextPtr = std::unique_ptr<llama_context, LlamaContextDeleter>;
using LlamaSamplerPtr = std::unique_ptr<llama_sampler, LlamaSamplerDeleter>;

class LlamaOperationCancelled final : public std::runtime_error {
 public:
  LlamaOperationCancelled() : std::runtime_error("llama embedding cancelled") {}
};

// Keep exceptions inside the C++ boundary and latch cancellation even if the
// caller's predicate subsequently changes. Serialize caller/worker evaluation
// so a cancellation predicate never has to be reentrant.
class LlamaCancellation final {
 public:
  explicit LlamaCancellation(std::function<bool()> callback) : callback_(std::move(callback)) {}
  bool poll() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return true;
    try { stopped_ = callback_ && callback_(); }
    catch (...) { stopped_ = true; }
    return stopped_;
  }
  void check() {
    if (poll()) throw LlamaOperationCancelled();
  }
 private:
  std::function<bool()> callback_;
  std::mutex mutex_;
  bool stopped_ = false;
};

class RealLlamaBackendAdapter final : public LlamaBackendAdapter {
 public:
  RealLlamaBackendAdapter() = default;
  ~RealLlamaBackendAdapter() override { unload(); }

  void load(const LlamaBackendLoadRequest& request) override {
    std::lock_guard<std::mutex> lock(mutex_);
    LlamaCancellation cancellation(request.cancelled);
    cancellation.check();
    unload_unlocked();
    // Declare the guard before the model/context so failure destroys resources
    // before releasing the backend, including cancellation and context failure.
    auto guard = std::make_unique<LlamaBackendGuard>();
    tensor_split_storage_.clear();

    llama_model_params model_params = llama_model_default_params();
    if (request.cancelled) {
      model_params.progress_callback = [](float, void* data) noexcept {
        return !static_cast<LlamaCancellation*>(data)->poll();
      };
      model_params.progress_callback_user_data = &cancellation;
    }
    if (request.backend.gpu_layers) {
      model_params.n_gpu_layers = *request.backend.gpu_layers;
    }
    if (request.backend.main_gpu) {
      model_params.main_gpu = *request.backend.main_gpu;
    }
    model_params.split_mode = split_mode_from_settings(request.backend.split_mode);
    if (request.backend.tensor_split_configured) {
      tensor_split_storage_.reserve(request.backend.tensor_split.size());
      for (const double value : request.backend.tensor_split) {
        tensor_split_storage_.push_back(static_cast<float>(value));
      }
      model_params.tensor_split = tensor_split_storage_.data();
    }
    if (request.backend.use_mmap) {
      model_params.use_mmap = *request.backend.use_mmap;
    }
    if (request.backend.use_mlock) {
      model_params.use_mlock = *request.backend.use_mlock;
    }

    LlamaModelPtr model(llama_model_load_from_file(request.model.path.c_str(),
                                                   model_params));
    cancellation.check();
    if (!model) {
      throw std::runtime_error("failed to load llama model: " + request.model.path);
    }

    llama_context_params context_params = llama_context_default_params();
    if (request.context.context_size) {
      context_params.n_ctx =
          checked_u32(*request.context.context_size, "contextSize");
    }
    if (request.context.batch_size) {
      context_params.n_batch =
          checked_u32(*request.context.batch_size, "batchSize");
    }
    if (request.context.ubatch_size) {
      context_params.n_ubatch =
          checked_u32(*request.context.ubatch_size, "ubatchSize");
    }
    if (request.context.threads) {
      context_params.n_threads = *request.context.threads;
    }
    if (request.context.batch_threads) {
      context_params.n_threads_batch = *request.context.batch_threads;
    }
    context_params.embeddings = request.embeddings.enabled.value_or(false);
    context_params.pooling_type = pooling_from_settings(request.embeddings.pooling);
    if (request.backend.offload_kqv) {
      context_params.offload_kqv = *request.backend.offload_kqv;
    }

    LlamaContextPtr context(llama_init_from_model(model.get(), context_params));
    cancellation.check();
    if (!context) {
      throw std::runtime_error("failed to create llama context for model: " +
                               request.model.id);
    }

    auto retained_request = request;
    retained_request.cancelled = {};
    guard_ = std::move(guard);
    model_ = std::move(model);
    context_ = std::move(context);
    model_id_ = request.model.id;
    load_request_ = std::move(retained_request);
    sequence_position_ = 0;
  }

  void unload() noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    unload_unlocked();
  }

  std::optional<std::string> loaded_model_id() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return model_id_;
  }

  bool loaded_embeddings_enabled() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return load_request_.embeddings.enabled.value_or(false);
  }

  std::vector<LlamaGpuMemory> gpu_memory() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    LlamaBackendGuard guard;
    std::vector<LlamaGpuMemory> memory;
    std::size_t gpu_index = 0;
    const std::size_t count = ggml_backend_dev_count();
    for (std::size_t index = 0; index < count; ++index) {
      ggml_backend_dev_t device = ggml_backend_dev_get(index);
      if (device == nullptr) {
        continue;
      }
      const auto type = ggml_backend_dev_type(device);
      if (type != GGML_BACKEND_DEVICE_TYPE_GPU &&
          type != GGML_BACKEND_DEVICE_TYPE_IGPU) {
        continue;
      }
      std::size_t free_bytes = 0;
      std::size_t total_bytes = 0;
      ggml_backend_dev_memory(device, &free_bytes, &total_bytes);
      memory.push_back(
          {gpu_index++, static_cast<std::uint64_t>(free_bytes),
           static_cast<std::uint64_t>(total_bytes)});
    }
    return memory;
  }

  LlamaRuntimeStatus runtime_status() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    LlamaRuntimeStatus status;
    status.loaded_model_id = model_id_;
    status.model_loaded = model_ != nullptr;
    status.context_loaded = context_ != nullptr;
    status.embeddings_enabled = load_request_.embeddings.enabled.value_or(false);
    status.sequence_position = sequence_position_;
    status.gpu_offload_supported = llama_supports_gpu_offload();
    status.max_devices = llama_max_devices();
    status.backend = backend_settings_status(load_request_.backend);
    if (model_) {
      status.model_size_bytes =
          static_cast<std::uint64_t>(llama_model_size(model_.get()));
    }
    if (context_) {
      status.state_size_bytes =
          static_cast<std::uint64_t>(llama_state_get_size(context_.get()));
      status.context_size = llama_n_ctx(context_.get());
    }
    status.memory = llama_memory_status_json(model_.get(), context_.get());
    status.metadata = Json{{"source", "llama-ext"}};
    return status;
  }

  LlamaBackendGenerateResult generate(
      const LlamaBackendGenerateRequest& request,
      const LlamaTokenCallback& on_token) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!model_ || !context_) {
      throw std::runtime_error("llama model is not loaded");
    }

    LlamaBackendGenerateResult result;
    if (request.aborted) {
      result.stop_reason = StopReason::aborted;
      return result;
    }

    const llama_vocab* vocab = llama_model_get_vocab(model_.get());
    std::vector<llama_token> prompt_tokens = tokenize(vocab, request.prompt);
    const std::uint32_t context_size = llama_n_ctx(context_.get());
    llama_memory_t memory = llama_get_memory(context_.get());
    Json context_pressure = maybe_prune_context_pressure(
        memory, context_size,
        static_cast<std::int64_t>(prompt_tokens.size()) +
            request.max_output_tokens,
        retains_kv_cache(request.kv_cache));
    const std::int64_t base_position =
        retains_kv_cache(request.kv_cache) ? sequence_position_ : 0;
    if (context_size > 0 &&
        base_position + static_cast<std::int64_t>(prompt_tokens.size()) +
                request.max_output_tokens >
            static_cast<std::int64_t>(context_size)) {
      throw std::runtime_error("llama context overflow: prompt tokens " +
                               std::to_string(prompt_tokens.size()) +
                               " plus max output " +
                               std::to_string(request.max_output_tokens) +
                               " exceed context " +
                               std::to_string(context_size));
    }

    if (!retains_kv_cache(request.kv_cache)) {
      llama_memory_clear(memory, true);
      sequence_position_ = 0;
    }
    if (!prompt_tokens.empty()) {
      const std::uint32_t batch_size =
          std::max<std::uint32_t>(1U, llama_n_batch(context_.get()));
      for (std::size_t offset = 0; offset < prompt_tokens.size();
           offset += batch_size) {
        const auto count = static_cast<int32_t>(
            std::min<std::size_t>(batch_size, prompt_tokens.size() - offset));
        llama_batch batch = llama_batch_get_one(prompt_tokens.data() + offset,
                                                count);
        const int32_t status = llama_decode(context_.get(), batch);
        if (status == 2) {
          result.stop_reason = StopReason::aborted;
          return result;
        }
        if (status != 0) {
          throw std::runtime_error("llama prompt decode failed with status " +
                                   std::to_string(status));
        }
      }
      sequence_position_ += static_cast<std::int64_t>(prompt_tokens.size());
    }

    LlamaSamplerPtr sampler = make_sampler(vocab, request.sampler_plan);
    std::int64_t generated_tokens = 0;
    result.stop_reason = StopReason::max_tokens;

    while (generated_tokens < request.max_output_tokens) {
      const llama_token token =
          llama_sampler_sample(sampler.get(), context_.get(), -1);
      if (llama_vocab_is_eog(vocab, token)) {
        result.stop_reason = StopReason::end_turn;
        break;
      }

      const std::string piece = token_to_piece(vocab, token);
      result.text += piece;
      if (on_token) {
        on_token(piece);
      }

      llama_token next = token;
      llama_batch batch = llama_batch_get_one(&next, 1);
      const int32_t status = llama_decode(context_.get(), batch);
      ++generated_tokens;
      ++sequence_position_;
      if (status == 2) {
        result.stop_reason = StopReason::aborted;
        break;
      }
      if (status != 0) {
        throw std::runtime_error("llama token decode failed with status " +
                                 std::to_string(status));
      }
    }

    result.usage.input_tokens =
        static_cast<std::int64_t>(prompt_tokens.size());
    result.usage.output_tokens = generated_tokens;
    normalize(result.usage);
    result.metadata = Json{{"contextSize", context_size},
                           {"model", request.model_id},
                           {"kvCache",
                            Json{{"policy",
                                  request.kv_cache.policy.value_or("session")},
                                 {"sequencePosition", sequence_position_},
                                 {"retained",
                                  retains_kv_cache(request.kv_cache)},
                                 {"contextPressure",
                                  std::move(context_pressure)}}},
                           {"systemInfo", llama_print_system_info()}};
    return result;
  }

  LlamaEmbeddingResult embed(const LlamaEmbeddingRequest& request) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (request.cancelled && request.cancelled())
      throw std::runtime_error("llama embedding cancelled");
    if (!model_ || !context_) {
      throw std::runtime_error("llama model is not loaded");
    }
    if (!load_request_.embeddings.enabled.value_or(false)) {
      throw std::runtime_error("llama embedding mode is not enabled");
    }

    const llama_vocab* vocab = llama_model_get_vocab(model_.get());
    const auto tokens = tokenize(vocab, request.text);
    if (tokens.empty()) {
      throw std::runtime_error("llama embedding text produced no tokens");
    }
    const std::uint32_t context_size = llama_n_ctx(context_.get());
    if (context_size > 0 && tokens.size() > context_size) {
      throw std::runtime_error("llama embedding input exceeds context " +
                               std::to_string(context_size));
    }

    llama_memory_clear(llama_get_memory(context_.get()), true);
    llama_set_embeddings(context_.get(), true);

    struct AbortGuard {
      llama_context* context;
      ~AbortGuard() { llama_set_abort_callback(context, nullptr, nullptr); }
    } abort_guard{context_.get()};
    llama_set_abort_callback(context_.get(), [](void* data) noexcept {
      const auto* callback = static_cast<const std::function<bool()>*>(data);
      try { return *callback && (*callback)(); } catch (...) { return true; }
    }, const_cast<std::function<bool()>*>(&request.cancelled));

    llama_batch batch =
        llama_batch_init(static_cast<int32_t>(tokens.size()), 0, 1);
    struct BatchGuard {
      llama_batch& batch;
      ~BatchGuard() { llama_batch_free(batch); }
    } batch_guard{batch};
    for (std::size_t index = 0; index < tokens.size(); ++index) {
      batch.token[index] = tokens[index];
      batch.pos[index] = static_cast<llama_pos>(index);
      batch.n_seq_id[index] = 1;
      batch.seq_id[index][0] = 0;
      batch.logits[index] = 1;
    }
    batch.n_tokens = static_cast<int32_t>(tokens.size());
    const int32_t status = llama_model_has_encoder(model_.get()) && !llama_model_has_decoder(model_.get())
        ? llama_encode(context_.get(), batch) : llama_decode(context_.get(), batch);
    if (status != 0) {
      throw std::runtime_error("llama embedding decode failed with status " +
                               std::to_string(status));
    }
    if (request.cancelled && request.cancelled()) {
      throw std::runtime_error("llama embedding cancelled");
    }

    const enum llama_pooling_type pooling = llama_pooling_type(context_.get());
    const int32_t embedding_dimensions = llama_model_n_embd_out(model_.get());
    const float* raw_embedding = nullptr;
    if (pooling == LLAMA_POOLING_TYPE_NONE) {
      raw_embedding =
          llama_get_embeddings_ith(context_.get(),
                                   static_cast<int32_t>(tokens.size() - 1U));
    } else {
      raw_embedding = llama_get_embeddings_seq(context_.get(), 0);
    }
    if (raw_embedding == nullptr) {
      throw std::runtime_error("failed to read llama embeddings");
    }

    LlamaEmbeddingResult result;
    result.embedding.assign(raw_embedding, raw_embedding + embedding_dimensions);
    if (request.normalize) {
      normalize_embedding(result.embedding);
    }
    result.usage.input_tokens = static_cast<std::int64_t>(tokens.size());
    normalize(result.usage);
    const char* pooling_name = "unknown";
    switch (pooling) {
      case LLAMA_POOLING_TYPE_NONE: pooling_name = "none"; break;
      case LLAMA_POOLING_TYPE_MEAN: pooling_name = "mean"; break;
      case LLAMA_POOLING_TYPE_CLS: pooling_name = "cls"; break;
      case LLAMA_POOLING_TYPE_LAST: pooling_name = "last"; break;
      case LLAMA_POOLING_TYPE_RANK: pooling_name = "rank"; break;
      default: break;
    }
    result.metadata = Json{{"model", request.model_id},
                           {"dimensions", embedding_dimensions},
                           {"normalized", request.normalize},
                           {"pooling", pooling_name}};
    return result;
  }

  LlamaKvCacheState kv_cache_control(
      const LlamaKvCacheOperation& operation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    LlamaKvCacheState state;
    state.loaded_model_id = model_id_;
    state.seq_id = operation.seq_id;
    if (!context_) {
      return state;
    }

    llama_memory_t memory = llama_get_memory(context_.get());
    switch (operation.kind) {
      case LlamaKvCacheOperationKind::kInspect:
        break;
      case LlamaKvCacheOperationKind::kClear:
        llama_memory_clear(memory, true);
        sequence_position_ = 0;
        state.mutated = true;
        break;
      case LlamaKvCacheOperationKind::kKeep:
        llama_memory_seq_keep(memory, operation.seq_id);
        state.mutated = true;
        break;
      case LlamaKvCacheOperationKind::kDrop:
        state.mutated = llama_memory_seq_rm(memory, operation.seq_id,
                                            operation.from, operation.to);
        if (state.mutated && operation.seq_id == 0 && operation.from <= 0 &&
            operation.to < 0) {
          sequence_position_ = 0;
        }
        break;
      case LlamaKvCacheOperationKind::kShift:
        if (!llama_memory_can_shift(memory)) {
          state.metadata["error"] = "llama memory does not support shifting";
          break;
        }
        llama_memory_seq_add(memory, operation.seq_id, operation.from,
                             operation.to, operation.delta);
        sequence_position_ += operation.delta;
        if (sequence_position_ < 0) {
          sequence_position_ = 0;
        }
        state.mutated = true;
        break;
    }

    state.position_min = llama_memory_seq_pos_min(memory, operation.seq_id);
    state.position_max = llama_memory_seq_pos_max(memory, operation.seq_id);
    state.sequence_position = sequence_position_;
    state.can_shift = llama_memory_can_shift(memory);
    state.metadata["loaded"] = model_id_.has_value();
    return state;
  }

 private:
  void unload_unlocked() noexcept {
    context_.reset();
    model_.reset();
    model_id_.reset();
    tensor_split_storage_.clear();
    sequence_position_ = 0;
    load_request_ = {};
    guard_.reset();
  }

  static std::vector<llama_token> tokenize(const llama_vocab* vocab,
                                           const std::string& text) {
    int32_t count =
        llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()),
                       nullptr, 0, true, true);
    if (count == std::numeric_limits<int32_t>::min()) {
      throw std::runtime_error("llama tokenization overflow");
    }
    if (count < 0) {
      count = -count;
    }
    std::vector<llama_token> tokens(static_cast<std::size_t>(count));
    if (count == 0) {
      return tokens;
    }
    const int32_t actual =
        llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()),
                       tokens.data(), count, true, true);
    if (actual < 0) {
      throw std::runtime_error("llama tokenization failed");
    }
    tokens.resize(static_cast<std::size_t>(actual));
    return tokens;
  }

  Json maybe_prune_context_pressure(llama_memory_t memory,
                                    std::uint32_t context_size,
                                    std::int64_t required_tokens,
                                    bool retain_kv_cache) {
    Json pressure{{"highWatermarkPercent", kContextPruneHighWatermarkPercent},
                  {"lowWatermarkPercent", kContextPruneLowWatermarkPercent},
                  {"prunedTokens", 0},
                  {"beforeSequencePosition", sequence_position_},
                  {"afterSequencePosition", sequence_position_}};
    if (!retain_kv_cache || context_size == 0 || sequence_position_ <= 0) {
      return pressure;
    }

    const std::int64_t context =
        static_cast<std::int64_t>(context_size);
    const std::int64_t high = std::max<std::int64_t>(
        1, (context * kContextPruneHighWatermarkPercent) / 100);
    const std::int64_t low = std::max<std::int64_t>(
        1, (context * kContextPruneLowWatermarkPercent) / 100);
    const std::int64_t projected = sequence_position_ + required_tokens;
    pressure["projectedSequencePosition"] = projected;
    if (projected <= high) {
      return pressure;
    }
    if (!llama_memory_can_shift(memory)) {
      pressure["skipped"] = "llama memory does not support shifting";
      return pressure;
    }

    const std::int64_t prune_target =
        std::min<std::int64_t>(sequence_position_, std::max<std::int64_t>(
                                                       1, projected - low));
    const auto to = static_cast<std::int32_t>(
        std::min<std::int64_t>(prune_target,
                               std::numeric_limits<std::int32_t>::max()));
    const bool removed = llama_memory_seq_rm(memory, 0, 0, to);
    if (!removed) {
      pressure["skipped"] = "no matching KV items were removed";
      return pressure;
    }
    llama_memory_seq_add(memory, 0, to, -1, -to);
    sequence_position_ = std::max<std::int64_t>(0, sequence_position_ - to);
    pressure["prunedTokens"] = to;
    pressure["afterSequencePosition"] = sequence_position_;
    return pressure;
  }

  static std::string token_to_piece(const llama_vocab* vocab,
                                    llama_token token) {
    char buffer[128];
    int32_t written =
        llama_token_to_piece(vocab, token, buffer, sizeof(buffer), 0, false);
    if (written >= 0) {
      return std::string(buffer, buffer + written);
    }
    std::string dynamic_buffer(static_cast<std::size_t>(-written), '\0');
    written = llama_token_to_piece(vocab, token, dynamic_buffer.data(),
                                   static_cast<int32_t>(dynamic_buffer.size()),
                                   0, false);
    if (written < 0) {
      throw std::runtime_error("llama token detokenization failed");
    }
    dynamic_buffer.resize(static_cast<std::size_t>(written));
    return dynamic_buffer;
  }

  static LlamaSamplerPtr make_sampler(
      const llama_vocab* vocab,
      const std::vector<LlamaSamplerPlanStep>& sampler_plan) {
    llama_sampler_chain_params params = llama_sampler_chain_default_params();
    LlamaSamplerPtr chain(llama_sampler_chain_init(params));
    if (!chain) {
      throw std::runtime_error("failed to create llama sampler chain");
    }

    for (const auto& step : sampler_plan) {
      llama_sampler* sampler = nullptr;
      if (step.name == "penalties") {
        sampler = llama_sampler_init_penalties(
            step.parameters.value("lastN", -1),
            static_cast<float>(step.parameters.value("repeatPenalty", 1.0)),
            static_cast<float>(step.parameters.value("frequencyPenalty", 0.0)),
            static_cast<float>(step.parameters.value("presencePenalty", 0.0)));
      } else if (step.name == "topK") {
        sampler = llama_sampler_init_top_k(step.parameters.value("k", 0));
      } else if (step.name == "topP") {
        sampler = llama_sampler_init_top_p(
            static_cast<float>(step.parameters.value("p", 1.0)),
            step.parameters.value("minKeep", static_cast<std::size_t>(1)));
      } else if (step.name == "minP") {
        sampler = llama_sampler_init_min_p(
            static_cast<float>(step.parameters.value("p", 0.0)),
            step.parameters.value("minKeep", static_cast<std::size_t>(1)));
      } else if (step.name == "temperature") {
        sampler = llama_sampler_init_temp(
            static_cast<float>(step.parameters.value("temperature", 0.8)));
      } else if (step.name == "greedy") {
        sampler = llama_sampler_init_greedy();
      } else if (step.name == "dist") {
        sampler = llama_sampler_init_dist(
            step.parameters.value("seed",
                                  static_cast<std::uint32_t>(LLAMA_DEFAULT_SEED)));
      } else if (step.name == "grammar") {
        const std::string grammar =
            step.parameters.value("grammar", std::string{});
        const std::string root =
            step.parameters.value("root", std::string{"root"});
        sampler = llama_sampler_init_grammar(vocab, grammar.c_str(),
                                             root.c_str());
      }

      if (sampler == nullptr) {
        throw std::runtime_error("failed to create llama sampler: " + step.name);
      }
      llama_sampler_chain_add(chain.get(), sampler);
    }

    return chain;
  }

  mutable std::mutex mutex_;
  std::unique_ptr<LlamaBackendGuard> guard_;
  LlamaModelPtr model_;
  LlamaContextPtr context_;
  std::optional<std::string> model_id_;
  LlamaBackendLoadRequest load_request_;
  std::vector<float> tensor_split_storage_;
  std::int64_t sequence_position_ = 0;
};

std::set<std::string> allowed_tool_names(const CompletionRequest& request) {
  std::set<std::string> names;
  for (const auto& tool : request.tools) {
    if (!tool.name.empty()) {
      names.insert(tool.name);
    }
  }
  return names;
}

std::optional<ToolCallContent> tool_call_from_json(const Json& parsed,
                                                   const CompletionRequest& request,
                                                   const std::string& raw_text) {
  if (!parsed.is_object()) {
    return std::nullopt;
  }

  const auto parse_arguments = [](Json arguments) -> std::optional<Json> {
    if (arguments.is_null()) {
      return Json::object();
    }
    if (arguments.is_string()) {
      try {
        arguments = Json::parse(arguments.get<std::string>());
      } catch (const std::exception&) {
        return std::nullopt;
      }
    }
    if (!arguments.is_object()) {
      return std::nullopt;
    }
    return arguments;
  };

  const auto make_call = [&](std::string id, std::string name,
                             Json arguments) -> std::optional<ToolCallContent> {
    if (name.empty()) {
      return std::nullopt;
    }
    auto parsed_arguments = parse_arguments(std::move(arguments));
    if (!parsed_arguments) {
      return std::nullopt;
    }
    if (id.empty()) {
      id = "llama_" + std::to_string(std::hash<std::string>{}(raw_text));
    }
    return ToolCallContent{
        std::move(id), std::move(name), std::move(*parsed_arguments),
        std::nullopt};
  };

  const auto calls = parsed.find("tool_calls");
  if (calls != parsed.end() && calls->is_array() && !calls->empty()) {
    const auto& first = calls->front();
    if (first.is_object()) {
      const std::string id = first.value("id", std::string{});
      const auto function = first.find("function");
      if (function != first.end() && function->is_object()) {
        const std::string name = function->value("name", std::string{});
        const Json arguments = function->value("arguments", Json::object());
        return make_call(id, name, arguments);
      }
      const std::string name = first.value("name", std::string{});
      const Json arguments = first.value("arguments", Json::object());
      return make_call(id, name, arguments);
    }
  }

  const auto function_call = parsed.find("function_call");
  if (function_call != parsed.end() && function_call->is_object()) {
    const std::string name = function_call->value("name", std::string{});
    const Json arguments = function_call->value("arguments", Json::object());
    return make_call("", name, arguments);
  }

  const auto call = parsed.find("tool_call");
  if (call != parsed.end() && call->is_object()) {
    const std::string name = call->value("name", std::string{});
    const Json arguments = call->value("arguments", Json::object());
    return make_call(call->value("id", std::string{}), name, arguments);
  }

  const auto names = allowed_tool_names(request);
  if (!names.empty() && parsed.size() == 1U) {
    const auto first = parsed.begin();
    if (names.count(first.key()) != 0U) {
      return make_call("", first.key(), first.value());
    }
  }

  return std::nullopt;
}

std::optional<Json> parse_first_json_object(std::string_view text) {
  for (std::size_t start = 0; start < text.size(); ++start) {
    if (text[start] != '{') {
      continue;
    }

    std::size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t index = start; index < text.size(); ++index) {
      const char ch = text[index];
      if (in_string) {
        if (escaped) {
          escaped = false;
        } else if (ch == '\\') {
          escaped = true;
        } else if (ch == '"') {
          in_string = false;
        }
        continue;
      }

      if (ch == '"') {
        in_string = true;
        continue;
      }
      if (ch == '{') {
        ++depth;
      } else if (ch == '}') {
        if (depth == 0) {
          break;
        }
        --depth;
        if (depth == 0) {
          try {
            return Json::parse(text.substr(start, index - start + 1U));
          } catch (const std::exception&) {
            break;
          }
        }
      }
    }
  }
  return std::nullopt;
}

CompletionResponse make_response(const LlamaBackendGenerateRequest& request,
                                 const CompletionRequest& completion_request,
                                 const LlamaBackendGenerateResult& result) {
  CompletionResponse response;
  response.message.role = MessageRole::assistant;
  response.stop_reason = result.stop_reason;
  if (!result.text.empty()) {
    if (const auto parsed = parse_first_json_object(trim_copy(result.text))) {
      if (auto tool_call = tool_call_from_json(parsed, completion_request,
                                               result.text)) {
        response.message.content.push_back(std::move(*tool_call));
        response.stop_reason = StopReason::tool_call;
      }
    }
  }
  if (response.message.content.empty()) {
    response.message.content.push_back(TextContent{result.text, std::nullopt});
  }
  response.usage = result.usage;
  response.response_model = request.model_id;
  response.metadata = result.metadata;
  response.metadata["provider"] = kLlamaProviderId;
  response.metadata["api"] = kLlamaProviderApi;
  response.metadata["model"] = request.model_id;
  response.metadata["generation"] = request.generation;
  response.metadata["kvCache"] = request.kv_cache;
  if (request.metadata.contains("grammar")) {
    response.metadata["grammar"] = request.metadata.at("grammar");
  }
  response.metadata["samplerPlan"] = Json::array();
  for (const auto& step : request.sampler_plan) {
    response.metadata["samplerPlan"].push_back(
        Json{{"name", step.name}, {"parameters", step.parameters}});
  }
  return response;
}

bool content_has_tool_required_intent(const std::vector<Content>& content) {
  std::ostringstream text;
  for (const auto& item : content) {
    text << '\n';
    append_content_text(text, item);
  }
  const std::string normalized = lowercase_copy(text.str());
  for (const char* token :
       {"file", "files", "directory", "directories", "folder", "folders",
        "list", "read", "search", "find", "grep", "model", "models"}) {
    if (normalized.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool request_requires_tool_choice(const CompletionRequest& request) {
  if (request.tools.empty()) {
    return false;
  }
  for (const auto& message : request.messages) {
    if (message.role == MessageRole::user &&
        content_has_tool_required_intent(message.content)) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::optional<LlamaInstanceTag> parse_llama_instance_tag(std::string_view value) {
  if (value == "embed") {
    return LlamaInstanceTag::kEmbed;
  }
  if (value == "rank") {
    return LlamaInstanceTag::kRank;
  }
  if (value == "instruct") {
    return LlamaInstanceTag::kInstruct;
  }
  if (value == "chat") {
    return LlamaInstanceTag::kChat;
  }
  return std::nullopt;
}

std::string_view llama_instance_tag_name(LlamaInstanceTag tag) noexcept {
  switch (tag) {
    case LlamaInstanceTag::kEmbed:
      return "embed";
    case LlamaInstanceTag::kRank:
      return "rank";
    case LlamaInstanceTag::kInstruct:
      return "instruct";
    case LlamaInstanceTag::kChat:
      return "chat";
  }
  return "chat";
}

std::string serialize_llama_prompt(const CompletionRequest& request) {
  std::ostringstream out;
  const auto content_text = [](const std::vector<Content>& content) {
    std::ostringstream text;
    for (std::size_t index = 0; index < content.size(); ++index) {
      if (index > 0) {
        text << "\n";
      }
      append_content_text(text, content[index]);
    }
    return text.str();
  };

  const auto openai_message = [&](const Message& message) {
    Json item = Json{{"role", std::string(role_label(message.role))}};
    if (message.name && !message.name->empty()) {
      item["name"] = *message.name;
    }
    if (message.role == MessageRole::tool && !message.content.empty()) {
      if (const auto* result =
              std::get_if<ToolResultContent>(&message.content.front())) {
        item["tool_call_id"] = result->tool_call_id;
        item["name"] = result->name;
        item["content"] = result->result.dump();
        return item;
      }
    }
    item["content"] = content_text(message.content);
    return item;
  };

  const auto openai_tool = [](const ToolDefinition& tool) {
    return Json{{"type", "function"},
                {"function",
                 Json{{"name", tool.name},
                      {"description", tool.description},
                      {"parameters", tool.input_schema}}}};
  };

  if (!request.tools.empty()) {
    const bool force_tool_choice = request_requires_tool_choice(request);
    Json messages = Json::array(
        {Json{{"role", "system"},
              {"content",
               "You are a local model controller. You must either answer "
               "directly or request a tool call using valid JSON arguments."}},
         Json{{"role", "developer"},
              {"content",
               "Filesystem operations must be performed with tools. Never "
               "invent file listings or file contents. For model discovery, "
               "call list_directory first, then classify discovered models. "
               "For search, grep, or find-text requests, call search_text. "
               "For user text like 'search <term> <path>', call search_text "
               "with arguments {\"query\":\"<term>\",\"path\":\"<path>\","
               "\"literal\":true}. "
               "For reading a named file, call read_text. For listing a "
               "directory, call list_directory. "
               "When a tool is needed, return an assistant message with "
               "OpenAI-style tool_calls only. Do not include markdown, prose, "
               "or role labels around the tool call."}}});
    if (force_tool_choice) {
      messages.push_back(
          Json{{"role", "developer"},
               {"content",
                "The current user request requires one of the available tools. "
                "Return only an OpenAI-style tool_calls JSON object. Do not "
                "answer directly before the tool result is available. Include "
                "all required tool arguments from the user request."}});
    }
    for (const auto& message : request.messages) {
      messages.push_back(openai_message(message));
    }

    Json tools = Json::array();
    for (const auto& tool : request.tools) {
      tools.push_back(openai_tool(tool));
    }

    Json payload = Json{{"messages", std::move(messages)},
                        {"tools", std::move(tools)},
                        {"tool_choice", force_tool_choice ? "required" : "auto"},
                        {"parallel_tool_calls", false}};
    out << "OpenAI-compatible chat completion request JSON:\n"
        << payload.dump(2) << "\n\n"
        << "Return the assistant response. ";
    if (force_tool_choice) {
      out << "This request requires a tool. Return exactly one JSON object "
             "shaped like {\"tool_calls\":[{\"id\":\"call_1\","
             "\"type\":\"function\",\"function\":{\"name\":\"tool_name\","
             "\"arguments\":\"{}\"}}]} and no prose.\n";
    } else {
      out << "If calling a tool, return exactly one JSON object shaped like "
             "{\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
             "\"function\":{\"name\":\"tool_name\",\"arguments\":\"{}\"}}]}. "
             "If no tool is needed, return plain assistant text.\n";
    }
  } else {
    Json messages = Json::array();
    for (const auto& message : request.messages) {
      messages.push_back(openai_message(message));
    }
    out << "OpenAI-compatible chat completion request JSON:\n"
        << Json{{"messages", std::move(messages)}}.dump(2) << "\n\n"
        << "Return the assistant response text.\n";
  }
  out << "assistant: ";
  return out.str();
}

LlamaGenerationSettings effective_llama_generation_settings(
    const LlamaSettings& settings, const LlamaModelSettings& model,
    const CompletionRequest& request, const ProviderOptions& options) {
  LlamaGenerationSettings result =
      merge_llama_generation_settings(settings.generation, model.generation);

  overlay_generation_metadata(result, options.metadata);
  overlay_generation_metadata(result, request.metadata);
  overlay_optional(result.temperature, request.temperature);
  overlay_optional(result.max_output_tokens, request.max_output_tokens);
  if (!result.temperature) {
    result.temperature = kDefaultLlamaTemperature;
  }
  if (!result.top_p) {
    result.top_p = kDefaultLlamaTopP;
  }
  return result;
}

std::vector<LlamaSamplerPlanStep> make_llama_sampler_plan(
    const LlamaGenerationSettings& settings) {
  std::vector<LlamaSamplerPlanStep> plan;
  if (settings.repeat_penalty || settings.presence_penalty ||
      settings.frequency_penalty) {
    plan.push_back({"penalties",
                    Json{{"lastN", -1},
                         {"repeatPenalty", settings.repeat_penalty.value_or(1.0)},
                         {"presencePenalty",
                          settings.presence_penalty.value_or(0.0)},
                         {"frequencyPenalty",
                          settings.frequency_penalty.value_or(0.0)}}});
  }
  if (settings.top_k) {
    plan.push_back({"topK", Json{{"k", *settings.top_k}}});
  }
  if (settings.top_p) {
    plan.push_back({"topP",
                    Json{{"p", *settings.top_p},
                         {"minKeep", settings.min_keep.value_or(1)}}});
  }
  if (settings.min_p) {
    plan.push_back({"minP",
                    Json{{"p", *settings.min_p},
                         {"minKeep", settings.min_keep.value_or(1)}}});
  }
  if (settings.temperature) {
    plan.push_back(
        {"temperature", Json{{"temperature", *settings.temperature}}});
  }
  if (settings.temperature && *settings.temperature <= 0.0) {
    plan.push_back({"greedy", sampler_step_parameters(settings)});
  } else {
    plan.push_back({"dist", Json{{"seed", sampler_seed(settings)}}});
  }
  return plan;
}

LlamaProvider::LlamaProvider(LlamaSettings settings,
                             std::unique_ptr<LlamaBackendAdapter> backend)
    : settings_(std::move(settings)),
      automatic_gpu_offload_(!settings_.backend.gpu_layers.has_value()) {
  backends_[LlamaInstanceTag::kChat] =
      backend ? std::move(backend) : make_real_llama_backend_adapter();
  worker_ = std::thread([this]() { worker_loop(); });
  descriptor_.id = kLlamaProviderId;
  descriptor_.display_name = "llama.cpp";
  descriptor_.api = kLlamaProviderApi;
  descriptor_.transport = ProviderTransport::auto_detect;
  descriptor_.capabilities.streaming = true;
  descriptor_.capabilities.tool_calls = true;
  descriptor_.capabilities.vision = false;
  descriptor_.capabilities.reasoning = false;
  descriptor_.capabilities.session_caching =
      settings_.kv_cache.policy.value_or("session") != "off";
}

LlamaProvider::~LlamaProvider() { stop_worker(); }

const ProviderDescriptor& LlamaProvider::descriptor() const {
  return descriptor_;
}

CompletionResponse LlamaProvider::complete(const CompletionRequest& request,
                                           const ProviderOptions& options) {
  return run_tagged(LlamaInstanceTag::kChat, request, options);
}

void LlamaProvider::stream(const CompletionRequest& request,
                           const ProviderOptions& options, EventSink& sink) {
  StreamEvent start;
  start.kind = EventKind::message_start;
  start.role = MessageRole::assistant;
  sink.push(start);

  auto future = enqueue_model_call([this, request, options, &sink]() {
    const auto& model = require_model(request.model.name);
    auto& backend = ensure_loaded(model, LlamaInstanceTag::kChat);
    const auto generate_request = make_generate_request(request, options, model);
    const auto result = backend.generate(
        generate_request, [&](std::string_view token) {
          StreamEvent delta;
          delta.kind = EventKind::content_delta;
          delta.delta = std::string(token);
          delta.content = TextContent{std::string(token), std::nullopt};
          delta.metadata = Json{{"content_index", 0}, {"content_type", "text"}};
          sink.push(delta);
        });
    return std::pair<LlamaBackendGenerateRequest, LlamaBackendGenerateResult>{
        generate_request, result};
  });
  const auto completed = future.get();
  const auto& result = completed.second;

  StreamEvent done_content;
  done_content.kind = EventKind::content_done;
  done_content.content = TextContent{result.text, std::nullopt};
  done_content.metadata = Json{{"content_index", 0}, {"content_type", "text"}};
  sink.push(done_content);

  StreamEvent usage;
  usage.kind = EventKind::usage;
  usage.usage = result.usage;
  sink.push(usage);

  StreamEvent done;
  done.kind = EventKind::message_done;
  done.stop_reason = result.stop_reason;
  done.metadata = result.metadata;
  sink.push(done);
}

void LlamaProvider::load_model(const std::string& model_id) {
  load_model(model_id, LlamaInstanceTag::kChat);
}

void LlamaProvider::load_model(const std::string& model_id,
                               LlamaInstanceTag tag) {
  enqueue_model_call([this, model_id, tag]() {
    (void)ensure_loaded(require_model(model_id), tag,
                        tag == LlamaInstanceTag::kEmbed);
  }).get();
}

void LlamaProvider::unload() noexcept {
  try {
    enqueue_model_call([this]() {
      std::lock_guard<std::mutex> lock(backends_mutex_);
      for (auto& entry : backends_) {
        if (entry.second) {
          entry.second->unload();
        }
      }
    }).get();
  } catch (...) {
  }
}

std::optional<std::string> LlamaProvider::loaded_model_id() const {
  return loaded_model_id(LlamaInstanceTag::kChat);
}

std::optional<std::string> LlamaProvider::loaded_model_id(
    LlamaInstanceTag tag) const {
  return const_cast<LlamaProvider*>(this)
      ->enqueue_model_call([this, tag]() -> std::optional<std::string> {
        std::lock_guard<std::mutex> lock(backends_mutex_);
        const auto found = backends_.find(tag);
        if (found == backends_.end() || !found->second) {
          return std::nullopt;
        }
        return found->second->loaded_model_id();
      })
      .get();
}

LlamaRuntimeStatus LlamaProvider::runtime_status() const {
  return runtime_status(LlamaInstanceTag::kChat);
}

LlamaRuntimeStatus LlamaProvider::runtime_status(LlamaInstanceTag tag) const {
  return const_cast<LlamaProvider*>(this)
      ->enqueue_model_call([this, tag]() {
        std::lock_guard<std::mutex> lock(backends_mutex_);
        const auto found = backends_.find(tag);
        if (found == backends_.end() || !found->second) {
          LlamaRuntimeStatus status;
          status.metadata["tag"] = llama_instance_tag_name(tag);
          return status;
        }
        auto status = found->second->runtime_status();
        status.metadata["tag"] = llama_instance_tag_name(tag);
        const auto offload = offload_status_.find(tag);
        if (offload != offload_status_.end()) {
          status.metadata["offload"] = offload->second;
        }
        return status;
      })
      .get();
}

LlamaEmbeddingResult LlamaProvider::embed(const LlamaEmbeddingRequest& request) {
  return embed(request, LlamaInstanceTag::kEmbed);
}

LlamaEmbeddingResult LlamaProvider::embed(const LlamaEmbeddingRequest& request,
                                          LlamaInstanceTag tag) {
  if (request.queue_timeout.count() < 0)
    throw std::invalid_argument("llama embedding queue timeout must be nonnegative");
  const auto cancellation = std::make_shared<LlamaCancellation>(request.cancelled);
  cancellation->check();
  LlamaEmbeddingRequest queued_request = request;
  if (request.cancelled)
    queued_request.cancelled = [cancellation] { return cancellation->poll(); };
  const auto identity = std::make_shared<int>(0);
  const auto queued_at = std::chrono::steady_clock::now();
  auto future = enqueue_model_call([this, request = std::move(queued_request), tag, queued_at]() {
    if (request.cancelled && request.cancelled())
      throw std::runtime_error("llama embedding cancelled");
    // The worker may dequeue between waiter polls. Enforce the deadline here
    // too, before loading a model or invoking the backend.
    if (std::chrono::steady_clock::now() - queued_at >= request.queue_timeout)
      throw std::runtime_error("llama embedding queue timeout");
    const auto& model = require_model(request.model_id);
    auto& backend = ensure_loaded(model, tag, true, request.cancelled);
    LlamaEmbeddingRequest resolved = request;
    resolved.normalize = settings_.embeddings.normalize.value_or(request.normalize);
    auto result = backend.embed(resolved);
    result.metadata["tag"] = llama_instance_tag_name(tag);
    return result;
  }, identity);
  while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
    std::exception_ptr failure;
    try {
      cancellation->check();
    } catch (...) {
      failure = std::current_exception();
    }
    const bool timed_out =
        std::chrono::steady_clock::now() - queued_at >= request.queue_timeout;
    bool pending = false;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      const auto call = std::find_if(queued_model_calls_.begin(), queued_model_calls_.end(),
          [&](const QueuedModelCall& value) { return value.identity == identity; });
      pending = call != queued_model_calls_.end();
      // Removal and worker dequeue use the same lock. After removal no backend
      // can invoke the request's cancellation callback or retain its payload.
      if (pending && (failure || timed_out)) queued_model_calls_.erase(call);
    }
    if (failure) {
      // An active operation may hold callbacks that borrow the caller's state.
      // Join it before returning; cooperative decode cancellation remains the
      // active-operation contract, and queue timeout never interrupts a decode.
      if (!pending) future.wait();
      std::rethrow_exception(failure);
    }
    if (pending && timed_out)
      throw std::runtime_error("llama embedding queue timeout");
    if (!pending) return future.get();
  }
  return future.get();
}

LlamaKvCacheState LlamaProvider::kv_cache_control(
    const LlamaKvCacheOperation& operation) {
  return kv_cache_control(operation, LlamaInstanceTag::kChat);
}

LlamaKvCacheState LlamaProvider::kv_cache_control(
    const LlamaKvCacheOperation& operation, LlamaInstanceTag tag) {
  return enqueue_model_call([this, operation, tag]() {
    auto& backend = backend_for_tag(tag);
    auto state = backend.kv_cache_control(operation);
    state.metadata["tag"] = llama_instance_tag_name(tag);
    return state;
  }).get();
}

CompletionResponse LlamaProvider::run_tagged(
    LlamaInstanceTag tag, const CompletionRequest& request,
    const ProviderOptions& options) {
  return enqueue_model_call([this, tag, request, options]() {
    const auto& model = require_model(request.model.name);
    auto& backend = ensure_loaded(model, tag);
    const auto generate_request = make_generate_request(request, options, model);
    const auto result = backend.generate(generate_request, {});
    auto response = make_response(generate_request, request, result);
    response.metadata["llamaInstanceTag"] = llama_instance_tag_name(tag);
    return response;
  }).get();
}

void LlamaProvider::worker_loop() noexcept {
  while (true) {
    std::function<void()> call;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this]() {
        return stopping_worker_ || !queued_model_calls_.empty();
      });
      if (stopping_worker_ && queued_model_calls_.empty()) {
        break;
      }
      call = std::move(queued_model_calls_.front().run);
      queued_model_calls_.pop_front();
    }
    call();
  }
}

void LlamaProvider::stop_worker() noexcept {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (stopping_worker_) {
      return;
    }
    queued_model_calls_.push_back({[this]() {
      std::lock_guard<std::mutex> lock(backends_mutex_);
      for (auto& entry : backends_) {
        if (entry.second) {
          entry.second->unload();
        }
      }
    }, {}});
    stopping_worker_ = true;
  }
  queue_cv_.notify_one();
  if (worker_.joinable()) {
    worker_.join();
  }
}

const LlamaModelSettings& LlamaProvider::require_model(
    const std::string& model_id) const {
  for (const auto& model : settings_.models) {
    if (model.id == model_id) {
      return model;
    }
  }
  throw std::runtime_error("unknown llama model: " + model_id);
}

LlamaBackendAdapter& LlamaProvider::ensure_loaded(
    const LlamaModelSettings& model, LlamaInstanceTag tag,
    bool embeddings_required, const std::function<bool()>& cancelled) {
  LlamaCancellation cancellation(cancelled);
  cancellation.check();
  auto& backend = backend_for_tag(tag);
  // Embedding capability and embedding execution mode are distinct. Decoder
  // models expose an embedding width too, and older discovery persisted that
  // as supports_embeddings=true. Generation must still create a logits
  // context; only explicit embed operations enable embedding mode.
  const bool embeddings_enabled = embeddings_required;
  if (backend.loaded_model_id() == model.id &&
      backend.loaded_embeddings_enabled() == embeddings_enabled) {
    return backend;
  }
  LlamaBackendLoadRequest request;
  request.model = model;
  request.backend = settings_.backend;
  request.context = settings_.context;
  if (!request.context.context_size && model.context_window) {
    request.context.context_size = model.context_window;
  }
  request.embeddings = settings_.embeddings;
  request.embeddings.enabled = embeddings_enabled;
  if (cancelled) request.cancelled = [&cancellation] { return cancellation.poll(); };

  auto devices = automatic_gpu_offload_ ? backend.gpu_memory()
                                        : std::vector<LlamaGpuMemory>{};
  if (settings_.backend.split_mode.value_or("layer") == "none" &&
      !devices.empty()) {
    const auto main_gpu = settings_.backend.main_gpu.value_or(0);
    const auto selected = std::find_if(
        devices.begin(), devices.end(), [main_gpu](const auto& device) {
          return device.index == static_cast<std::size_t>(main_gpu);
        });
    const auto device = selected != devices.end() ? *selected : devices.front();
    devices.assign(1, device);
  }

  std::error_code size_error;
  const auto model_size = std::filesystem::file_size(model.path, size_error);
  LlamaOffloadPlanInput plan_input;
  if (!automatic_gpu_offload_) {
    plan_input.configured_gpu_layers = settings_.backend.gpu_layers;
  }
  plan_input.block_count = model.block_count;
  plan_input.model_size_bytes =
      size_error ? 0 : static_cast<std::uint64_t>(model_size);
  plan_input.devices = std::move(devices);
  const auto plan = make_llama_offload_plan(plan_input);

  Json offload = {
      {"policy", plan.automatic ? "automatic" : "configured"},
      {"modelBytes", plan_input.model_size_bytes},
      {"gpuFreeBytes", plan.free_bytes},
      {"gpuUsableBytes", plan.usable_bytes},
      {"gpuReservedBytes", plan.reserved_bytes},
      {"attemptedGpuLayers", Json::array()}};
  if (plan.estimated_block_count > 0) {
    offload["blockCount"] = plan.estimated_block_count;
    offload["blockCountEstimated"] = plan.block_count_estimated;
  }

  std::exception_ptr last_error;
  bool load_started = false;
  const auto cancel_load = [&] {
    if (load_started) backend.unload();
    offload["loadCancelled"] = true;
    offload_status_[tag] = std::move(offload);
  };
  for (const int gpu_layers : plan.gpu_layer_attempts) {
    request.backend.gpu_layers = gpu_layers;
    try {
      cancellation.check();
      offload["attemptedGpuLayers"].push_back(gpu_layers);
      load_started = true;
      backend.load(request);
      cancellation.check();
      offload["selectedGpuLayers"] = gpu_layers;
      offload["cpuFallback"] =
          gpu_layers == 0 && plan.gpu_layer_attempts.front() > 0;
      offload_status_[tag] = std::move(offload);
      return backend;
    } catch (const LlamaOperationCancelled&) {
      cancel_load();
      throw;
    } catch (...) {
      last_error = std::current_exception();
      // Adapters can report a generic load failure after observing the borrowed
      // predicate. A cancellation must never become another GPU/CPU retry.
      if (cancellation.poll()) {
        cancel_load();
        throw LlamaOperationCancelled();
      }
    }
  }
  offload["loadFailed"] = true;
  offload_status_[tag] = std::move(offload);
  std::rethrow_exception(last_error);
}

LlamaBackendAdapter& LlamaProvider::backend_for_tag(LlamaInstanceTag tag) {
  std::lock_guard<std::mutex> lock(backends_mutex_);
  auto& backend = backends_[tag];
  if (!backend) {
    backend = make_real_llama_backend_adapter();
  }
  return *backend;
}

LlamaBackendGenerateRequest LlamaProvider::make_generate_request(
    const CompletionRequest& request, const ProviderOptions& options,
    const LlamaModelSettings& model) const {
  LlamaBackendGenerateRequest generate;
  generate.model_id = model.id;
  generate.prompt = serialize_llama_prompt(request);
  generate.generation = effective_llama_generation_settings(
      settings_, model, request, options);
  generate.kv_cache = settings_.kv_cache;
  generate.max_output_tokens = resolved_max_output_tokens(generate.generation);
  generate.aborted = options.aborted;
  generate.sampler_plan = make_llama_sampler_plan(generate.generation);
  generate.metadata = Json{{"request", request.metadata},
                           {"options", options.metadata}};
  const auto grammar = resolve_llama_grammar(settings_, request, options);
  if (grammar.enabled) {
    generate.sampler_plan.insert(
        generate.sampler_plan.begin(),
        LlamaSamplerPlanStep{
            "grammar",
            Json{{"grammar", grammar.grammar},
                 {"root", grammar.root},
                 {"source", grammar.source}},
        });
    generate.metadata["grammar"] = Json{{"root", grammar.root},
                                        {"source", grammar.source}};
  }
  return generate;
}

std::unique_ptr<LlamaBackendAdapter> make_real_llama_backend_adapter() {
  return std::make_unique<RealLlamaBackendAdapter>();
}

std::shared_ptr<Provider> make_llama_provider(const LlamaSettings& settings) {
  return std::make_shared<LlamaProvider>(settings);
}

}  // namespace qt::ai
