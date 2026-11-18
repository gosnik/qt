#include "qt/memory/llama_retrieval.hpp"

#include "qt/foundation/hash.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace qt::memory {
namespace {
struct Binding {
  std::shared_ptr<ai::LlamaProvider> provider;
  ai::LlamaModelSettings model;
  std::string query_prefix;
  std::string document_prefix;
  bool normalize = true;
  std::mutex mutex;
  std::optional<DenseIndexConfig> config;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type modified;

  void check_source() const {
    if (std::filesystem::file_size(model.path) != size ||
        std::filesystem::last_write_time(model.path) != modified)
      throw std::runtime_error("embedding model changed; restart retrieval to rebuild its generation");
  }
};

std::string fingerprint(const std::string& path, const RetrievalCancellation& cancelled) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read configured embedding model");
  std::array<char, 65536> buffer{};
  auto hash = foundation::kStableHash64Offset;
  std::uint64_t bytes = 0;
  while (input) {
    if (cancelled && cancelled()) throw std::runtime_error("embedding initialization cancelled");
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    bytes += static_cast<std::uint64_t>(count);
    for (std::streamsize i = 0; i < count; ++i) {
      hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
      hash *= foundation::kStableHash64Prime;
    }
  }
  if (!input.eof() || bytes == 0) throw std::runtime_error("unable to fingerprint embedding model");
  return "gguf-fnv1a64:" + foundation::hex_u64(hash) + ":" + std::to_string(bytes);
}
}  // namespace

void bind_llama_retrieval(RetrievalEngineOptions& options,
    std::shared_ptr<ai::LlamaProvider> provider, ai::LlamaModelSettings model,
    std::string query_prefix, std::string document_prefix, bool normalize) {
  if (!provider || model.id.empty() || model.path.empty())
    throw std::invalid_argument("dense retrieval requires a configured local embedding model");
  auto binding = std::make_shared<Binding>();
  binding->provider = std::move(provider); binding->model = std::move(model);
  binding->query_prefix = std::move(query_prefix);
  binding->document_prefix = std::move(document_prefix); binding->normalize = normalize;
  options.validate_embedding = [binding] {
    std::lock_guard<std::mutex> lock(binding->mutex);
    if (binding->config) binding->check_source();
  };
  options.configure = [binding](const RetrievalCancellation& cancelled) {
    std::lock_guard<std::mutex> lock(binding->mutex);
    if (binding->config) { binding->check_source(); return *binding->config; }
    binding->size = std::filesystem::file_size(binding->model.path);
    binding->modified = std::filesystem::last_write_time(binding->model.path);
    DenseIndexConfig config;
    config.model_id = binding->model.id;
    config.model_fingerprint = fingerprint(binding->model.path, cancelled);
    // GGUF carries tokenizer metadata; pinning all bytes covers it as well.
    config.tokenizer_fingerprint = config.model_fingerprint;
    config.query_prefix = binding->query_prefix;
    config.document_prefix = binding->document_prefix;
    config.normalize = binding->normalize;
    binding->check_source();
    ai::LlamaEmbeddingRequest request;
    request.model_id = config.model_id; request.text = config.query_prefix + "memory retrieval";
    request.normalize = config.normalize; request.cancelled = cancelled;
    const auto result = binding->provider->embed(request, ai::LlamaInstanceTag::kEmbed);
    binding->check_source();
    if (cancelled && cancelled()) throw std::runtime_error("embedding initialization cancelled");
    config.dimensions = result.embedding.size();
    config.pooling = result.metadata.value("pooling", std::string{});
    double norm = 0;
    for (const auto value : result.embedding) {
      if (!std::isfinite(value)) throw std::runtime_error("embedding provider returned non-finite values");
      norm += static_cast<double>(value) * value;
    }
    if (config.dimensions == 0 || config.dimensions > 65536 || !(norm > 0) ||
        (config.pooling != "mean" && config.pooling != "cls" &&
         config.pooling != "last" && config.pooling != "none") ||
        result.metadata.value("normalized", !config.normalize) != config.normalize)
      throw std::runtime_error("embedding provider did not establish a compatible vector contract");
    const ai::Json identity = {config.model_id, config.model_fingerprint,
        config.tokenizer_fingerprint, config.query_prefix, config.document_prefix,
        config.pooling, config.dimensions, config.normalize, "llama-embedding-v1"};
    config.generation = "dense-v1-" + foundation::stable_hash_hex(identity.dump());
    binding->config = config;
    return config;
  };
  options.embedder = [binding](const DenseIndexConfig& requested, const std::string& text,
                              const RetrievalCancellation& cancelled) {
    {
      std::lock_guard<std::mutex> lock(binding->mutex);
      if (!binding->config || binding->config->generation != requested.generation)
        throw std::runtime_error("requested embedding space is not loaded");
      binding->check_source();
    }
    ai::LlamaEmbeddingRequest request;
    request.model_id = requested.model_id; request.text = text;
    request.normalize = requested.normalize; request.cancelled = cancelled;
    const auto result = binding->provider->embed(request, ai::LlamaInstanceTag::kEmbed);
    {
      std::lock_guard<std::mutex> lock(binding->mutex); binding->check_source();
    }
    if (result.embedding.size() != requested.dimensions ||
        result.metadata.value("pooling", std::string{}) != requested.pooling ||
        result.metadata.value("normalized", !requested.normalize) != requested.normalize)
      throw std::runtime_error("embedding provider changed its vector contract");
    return result.embedding;
  };
}

}  // namespace qt::memory
