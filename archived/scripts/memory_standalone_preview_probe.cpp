#include "index/retrieval_snapshot.hpp"
#include "index/sqlite_store.hpp"
#include "qt/ai/llama_provider.hpp"
#include "qt/foundation/hash.hpp"
#include "qt/memory/database.hpp"
#include "qt/memory/llama_retrieval.hpp"
#include "qt/memory/retrieval_indexer.hpp"
#include "qt/memory/runtime.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unistd.h>

namespace {
using Json = nlohmann::json;

void require(const bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

qt::memory::MemoryScope scope_for(const std::string& cwd) {
  return qt::memory::derive_runtime_memory_scope(cwd);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc >= 4, "usage: probe <agent-home> <workspace> <embedding-model>");
    const std::filesystem::path agent_home = argv[1];
    const std::filesystem::path workspace = argv[2];
    const std::filesystem::path model_path = argv[3];
    std::filesystem::create_directories(workspace);
    std::ofstream(workspace / "main.cpp")
        << "int preview_target() { return 7; }\n";

    qt::memory::MemoryScope scope = scope_for(workspace.generic_string());
    qt::memory::SQLiteMemoryDatabase database(agent_home);
    qt::ai::LlamaSettings llama;
    llama.models_configured = true;
    llama.embeddings.enabled = true;
    llama.embeddings.pooling = "mean";
    llama.embeddings.normalize = true;
    llama.backend.enabled = true;
    llama.backend.gpu_layers = 0;
    llama.context.context_size = 2048;
    qt::ai::LlamaModelSettings model;
    model.id = "nomic-embed-text-v1.5";
    model.name = model.id;
    model.path = model_path;
    model.supports_embeddings = true;
    llama.models.push_back(model);
    auto provider = std::make_shared<qt::ai::LlamaProvider>(llama);

    qt::memory::RetrievalEngineOptions retrieval;
    qt::memory::bind_llama_retrieval(retrieval, provider, model,
                                     "search_query: ", "search_document: ", true);
    const auto config = retrieval.configure({});
    const auto add = [&](const std::string& content) {
      qt::memory::MemoryWrite write;
      write.type = qt::memory::MemoryType::Semantic;
      write.status = qt::memory::MemoryStatus::Active;
      write.content = content;
      write.source = "standalone-preview-probe";
      write.confidence = 0.95;
      write.provenance.source_type = qt::memory::SourceType::Manual;
      write.provenance.source_id = "standalone-preview";
      write.provenance.evidence_strength = 1.0;
      return database.add(scope, std::move(write));
    };
    const auto memory_id = add(
        "Rotate credentials and reset an expired access token safely.");
    qt::memory::RetrievalIndexer indexer(database, retrieval.embedder);
    for (std::size_t i = 0; i < 1000U && indexer.step(scope).worked; ++i) {}
    database.begin_dense_index(scope, config);
    for (std::size_t i = 0; i < 1000U; ++i) {
      const auto step = indexer.step(scope, {}, config.generation);
      if (!step.worked) break;
      require(step.completed, "dense memory indexing failed: " + step.error);
    }
    require(database.activate_dense_index(scope, config.generation),
            "dense memory generation did not activate");

    qt::index::ProjectScanResult scan;
    scan.root = workspace;
    scan.files.push_back({"main.cpp", qt::index::ProjectFileKind::CxxSource});
    qt::index::CodeIndex code;
    code.functions.push_back({"preview_target", "preview_target", "main.cpp", 1,
                              "function"});
    const auto fingerprint = qt::index::retrieval_source_fingerprint(
        workspace / "main.cpp");
    const std::unordered_map<std::string, std::string> fingerprints{
        {"main.cpp", fingerprint}};
    const auto state_root = agent_home / "index";
    const auto project_key = std::to_string(std::hash<std::string>{}(
        std::filesystem::absolute(workspace).generic_string()));
    const auto index_db = qt::index::sqlite_index_database_path(
        state_root / project_key);
    std::filesystem::create_directories(index_db.parent_path());
    qt::index::save_sqlite_index_snapshot(
        index_db, workspace, scan, code, 7, 0, "standalone-preview", {},
        fingerprints);

    const auto code_snapshot = qt::index::RetrievalSnapshot::build(
        scan, code, 7, fingerprints, false);
    auto code_scope = scope;
    code_scope.task.clear();
    code_scope.session.clear();
    code_scope.conversation.clear();
    code_scope.agent = "qt-code-index:" +
                       qt::foundation::stable_hash_hex(workspace.generic_string());
    database.sync_code_snapshot(code_scope, *code_snapshot);
    for (std::size_t i = 0; i < 1000U && indexer.step(code_scope).worked; ++i) {}
    database.begin_dense_index(code_scope, config);
    for (std::size_t i = 0; i < 1000U; ++i) {
      const auto step = indexer.step(code_scope, {}, config.generation);
      if (!step.worked) break;
      require(step.completed, "dense code indexing failed: " + step.error);
    }
    require(database.activate_dense_index(code_scope, config.generation),
            "dense code generation did not activate");

    const auto pending_id = add("Unindexed operational note awaiting its durable job.");
    {
      qt::memory::MemoryRuntimeOptions pending_options;
      pending_options.agent_home = agent_home.generic_string();
      pending_options.cwd = workspace.generic_string();
      pending_options.initial_scope = scope;
      pending_options.background_processing = false;
      pending_options.automatic_capture = true;
      qt::memory::MemoryRuntime pending_runtime(pending_options);
      qt::ai::Message user;
      user.role = qt::ai::MessageRole::user;
      user.content.push_back(qt::ai::TextContent{"Keep this pending capture for a normal runtime worker.", std::nullopt});
      pending_runtime.enqueue_completed_episode({user}, "probe://pending-capture");
    }
    require(!std::filesystem::is_empty(agent_home / "memory" / "capture_pending"), "capture spool fixture missing");
    const auto status = database.retrieval_index_status(scope);
    require(status.pending > 0, "pending index fixture missing");
    std::cout << Json{{"agent_home", agent_home.generic_string()},
                      {"workspace", workspace.generic_string()},
                      {"memory_id", memory_id}, {"pending_id", pending_id},
                      {"index_database", index_db.generic_string()},
                      {"dense_generation", config.generation},
                      {"pending_jobs", status.pending},
                      {"leased_jobs", status.leased},
                      {"complete_jobs", status.complete}}
                     .dump()
              << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "standalone preview fixture failed: " << error.what() << '\n';
    return 1;
  }
}
