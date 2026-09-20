#pragma once

#include "qt/ai/llama_provider.hpp"
#include "qt/memory/retrieval_engine.hpp"

namespace qt::memory {

// The captured provider remains alive until both callbacks and workers release it.
void bind_llama_retrieval(RetrievalEngineOptions&,
    std::shared_ptr<ai::LlamaProvider>, ai::LlamaModelSettings,
    std::string query_prefix, std::string document_prefix, bool normalize);

}  // namespace qt::memory
