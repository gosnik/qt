#include "qt/memory/retrieval_indexer.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace qt::memory {
namespace {
bool continuation(unsigned char c) { return (c & 0xc0U) == 0x80U; }
bool cancelled(const RetrievalCancellation& check) { return check && check(); }
}  // namespace

std::vector<RetrievalChunk> RetrievalIndexer::render(const std::string& text) {
  if (text.empty()) throw std::invalid_argument("cannot index empty content");
  if (text.size() > 1024U * 1024U) throw std::invalid_argument("index owner content exceeds one MiB");
  std::vector<RetrievalChunk> chunks;
  constexpr std::size_t window = 2048;
  constexpr std::size_t overlap = 128;
  std::size_t begin = 0;
  while (begin < text.size()) {
    std::size_t end = std::min(text.size(), begin + window);
    if (end < text.size()) {
      while (end > begin && continuation(static_cast<unsigned char>(text[end]))) --end;
      const auto newline = text.rfind('\n', end - 1);
      if (newline != std::string::npos && newline > begin + window / 2) end = newline + 1;
    }
    if (end <= begin) throw std::invalid_argument("unable to partition malformed text");
    RetrievalChunk chunk;
    chunk.ordinal = chunks.size(); chunk.text = text.substr(begin, end - begin);
    chunk.source_span = "bytes:" + std::to_string(begin) + ":" + std::to_string(end);
    chunks.push_back(std::move(chunk));
    if (end == text.size()) break;
    begin = end > begin + overlap ? end - overlap : end;
    while (begin < end && continuation(static_cast<unsigned char>(text[begin]))) ++begin;
  }
  return chunks;
}

RetrievalIndexStep RetrievalIndexer::step(const MemoryScope& scope,
    const RetrievalCancellation& cancellation, std::optional<std::string> dense_generation,
    const RetrievalIndexWait& wait_until_ready) {
  RetrievalIndexStep result;
  if (cancelled(cancellation)) return result;
  const auto job = database_.claim_index_job(scope, std::chrono::seconds{300}, dense_generation);
  if (!job) return result;
  result.worked = true;
  result.owner_key = job->owner_key;
  try {
    const auto checkpoint = [&] {
      do {
        if (cancelled(cancellation)) throw std::runtime_error("indexing cancelled");
        if (!database_.renew_index_job(scope, *job, std::chrono::seconds{300}))
          throw std::runtime_error("indexing lease superseded");
      } while (wait_until_ready && !wait_until_ready(std::chrono::seconds{1}));
      if (cancelled(cancellation)) throw std::runtime_error("indexing cancelled");
    };
    checkpoint();
    if (job->representation == "chunks") {
      auto chunks = render(job->content);
      checkpoint();
      result.completed = database_.publish_chunks(scope, *job, std::move(chunks));
    } else if (job->representation == "dense") {
      if (!embedder_) throw std::runtime_error("embedding provider unavailable");
      const auto config = database_.dense_index(scope, job->generation);
      if (!config) throw std::runtime_error("embedding generation unavailable");
      if (config->dimensions == 0 || config->dimensions > 65536U)
        throw std::runtime_error("embedding dimension exceeds indexing bounds");
      const auto chunks = database_.retrieval_chunks(scope, job->owner_key);
      if (chunks.empty() || chunks.size() > 4096U)
        throw std::runtime_error("dense chunk count exceeds indexing bounds");
      // Keep one vector (at most 256 KiB) in the indexing batch. An anonymous
      // spool avoids holding every vector or a database write lock during model
      // calls. Closing it, including process exit, discards unpublished work;
      // the durable lease/job is the recovery authority.
      const auto close_spool = [](std::FILE* file) { std::fclose(file); };
      std::unique_ptr<std::FILE, decltype(close_spool)> spool(std::tmpfile(), close_spool);
      if (!spool) throw std::runtime_error("unable to create dense indexing spool");
      for (const auto& chunk : chunks) {
        checkpoint();
        const auto values = embedder_(*config, config->document_prefix + chunk.text, cancellation);
        if (values.size() != config->dimensions)
          throw std::runtime_error("embedding provider returned the wrong dimension");
        if (std::fwrite(values.data(), sizeof(float), values.size(), spool.get()) != values.size())
          throw std::runtime_error("unable to write dense indexing spool");
      }
      checkpoint();
      if (std::fflush(spool.get()) != 0 || std::fseek(spool.get(), 0, SEEK_SET) != 0)
        throw std::runtime_error("unable to rewind dense indexing spool");
      std::size_t next = 0;
      result.completed = database_.publish_dense_vectors(scope, *job, chunks.size(), [&] {
        if (cancelled(cancellation)) throw std::runtime_error("indexing cancelled");
        DenseChunkVector vector;
        vector.chunk_id = chunks.at(next++).id;
        vector.values.resize(config->dimensions);
        if (std::fread(vector.values.data(), sizeof(float), vector.values.size(), spool.get()) != vector.values.size())
          throw std::runtime_error("unable to read dense indexing spool");
        return vector;
      });
      if (result.completed) (void)database_.activate_dense_index(scope, job->generation);
    } else {
      throw std::runtime_error("unsupported retrieval representation");
    }
    if (!result.completed) result.error = "indexing result superseded";
  } catch (const std::exception& error) {
    result.error = error.what();
    (void)database_.fail_index_job(scope, *job, result.error);
  }
  return result;
}

}  // namespace qt::memory
