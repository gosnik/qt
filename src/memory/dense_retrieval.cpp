#include "qt/memory/dense_retrieval.hpp"

#include "qt/memory/scope.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace qt::memory {
namespace {

constexpr std::size_t kMaximumQueryBytes = 32768U;

void append_key(std::ostringstream& key, const std::string& value) {
  key << value.size() << ':' << value << '|';
}

std::string config_key(const DenseIndexConfig& config) {
  std::ostringstream key;
  append_key(key, config.generation);
  append_key(key, config.model_id);
  append_key(key, config.model_fingerprint);
  append_key(key, config.tokenizer_fingerprint);
  append_key(key, config.query_prefix);
  append_key(key, config.document_prefix);
  append_key(key, config.pooling);
  key << config.dimensions << ':' << (config.normalize ? 1 : 0) << '|';
  return key.str();
}

bool cancelled(const RetrievalCancellation& cancellation) {
  return cancellation && cancellation();
}

}  // namespace

DenseRetrievalService::DenseRetrievalService(SQLiteMemoryDatabase& database,
                                             RetrievalEmbedder embedder,
                                             std::size_t cache_capacity,
                                             std::size_t cache_byte_capacity)
    : database_(database),
      embedder_(std::move(embedder)),
      cache_capacity_(std::min<std::size_t>(cache_capacity, 1024U)),
      cache_byte_capacity_(std::min<std::size_t>(cache_byte_capacity, 32U * 1024U * 1024U)) {}

std::vector<DenseSearchHit> DenseRetrievalService::search(
    const MemoryScope& scope, const std::string& query, RecallPolicy policy,
    bool include_global_user, std::size_t limit,
    const RetrievalCancellation& cancellation) {
  if (cancelled(cancellation)) return {};
  if (query.size() > kMaximumQueryBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.last_error = "dense query exceeds 32768 bytes";
    return {};
  }
  // dense_index validates the scope and therefore deliberately remains outside
  // the provider-failure catch below.
  const auto config = database_.dense_index(scope, {}, policy, include_global_user);
  if (!config || limit == 0U) return {};
  const auto key = scope_key(scope) + "|" + config_key(*config) +
                   "|policy:" + std::to_string(static_cast<int>(policy)) +
                   "|global:" + (include_global_user ? "1" : "0") +
                   "|query:" + config->query_prefix + query;
  std::vector<float> embedding;
  bool cached = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = cache_.find(key);
    if (found != cache_.end()) {
      embedding = found->second.embedding;
      found->second.age = ++next_age_;
      ++stats_.cache_hits;
      cached = true;
    }
  }
  if (!cached) {
    if (!embedder_) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.embedding_failures;
      stats_.last_error = "dense embedding provider unavailable";
      return {};
    }
    try {
      embedding = embedder_(*config, config->query_prefix + query, cancellation);
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.embedding_failures;
      stats_.last_error = error.what();
      return {};
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.embedding_failures;
      stats_.last_error = "dense embedding provider failed";
      return {};
    }
    if (embedding.size() != config->dimensions || embedding.empty()) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.embedding_failures;
      stats_.last_error = "dense embedding provider returned the wrong dimension";
      return {};
    }
    double norm = 0.0;
    for (const auto value : embedding) {
      if (!std::isfinite(value)) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.embedding_failures;
        stats_.last_error = "dense embedding provider returned a non-finite value";
        return {};
      }
      norm += static_cast<double>(value) * static_cast<double>(value);
    }
    if (!(norm > 0.0)) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.embedding_failures;
      stats_.last_error = "dense embedding provider returned a zero vector";
      return {};
    }
    if (cancelled(cancellation)) return {};
    if (cache_capacity_ != 0U && cache_byte_capacity_ != 0U) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (cache_capacity_ != 0U && !cancelled(cancellation)) {
        CacheEntry replacement{embedding, ++next_age_, 0};
        auto inserted = cache_.try_emplace(key);
        auto& entry = inserted.first->second;
        cache_bytes_ -= entry.bytes;
        entry = std::move(replacement);
        // Account for retained key/vector capacity and the entry itself. Hash
        // bucket/node allocator overhead is separately bounded by the entry cap.
        entry.bytes = inserted.first->first.capacity() + 1U +
                      entry.embedding.capacity() * sizeof(float) +
                      sizeof(decltype(cache_)::value_type);
        cache_bytes_ += entry.bytes;
        while (cache_.size() > cache_capacity_ || cache_bytes_ > cache_byte_capacity_) {
          auto oldest = cache_.begin();
          for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.age < oldest->second.age) oldest = it;
          }
          cache_bytes_ -= oldest->second.bytes;
          cache_.erase(oldest);
        }
      }
    }
  }
  if (cancelled(cancellation)) return {};
  return database_.search_dense(scope, config->generation, embedding, policy,
                                include_global_user, limit, true);
}

DenseRetrievalStats DenseRetrievalService::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = stats_;
  result.cache_entries = cache_.size();
  result.cache_bytes = cache_bytes_;
  return result;
}

void DenseRetrievalService::clear_cache() {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.clear();
  cache_bytes_ = 0;
}

}  // namespace qt::memory
