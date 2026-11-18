#include "qt/ai/llama_config.hpp"
#include "qt/ai/llama_provider.hpp"
#include "qt/memory/llama_retrieval.hpp"
#include "qt/memory/database.hpp"
#include "qt/memory/recall_service.hpp"
#include "qt/memory/retrieval_engine.hpp"
#include "qt/memory/context_service.hpp"
#include "qt/memory/governance_service.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <sys/resource.h>
#include <nlohmann/json.hpp>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
std::uint64_t rss_kib() {
  std::ifstream in("/proc/self/status");
  std::string key;
  std::uint64_t value = 0;
  while (in >> key) {
    if (key == "VmRSS:") { in >> value; return value; }
    std::string rest; std::getline(in, rest);
  }
  return 0;
}

void require(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}

double norm(const std::vector<float>& v) {
  double sum = 0.0;
  for (float x : v) sum += static_cast<double>(x) * x;
  return std::sqrt(sum);
}

double cosine(const std::vector<float>& a, const std::vector<float>& b) {
  require(a.size() == b.size() && !a.empty(), "embedding dimensions differ");
  double dot = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) dot += static_cast<double>(a[i]) * b[i];
  return dot;
}

qt::memory::MemoryScope eval_scope(const std::string& name) {
  qt::memory::MemoryScope s; s.user = "real-encoder-eval";
  s.kind = name == "global" ? qt::memory::ScopeKind::GlobalUser : qt::memory::ScopeKind::Repository;
  if (name != "global") { s.project = "qtbits"; s.repository = name == "alpha" ? "qt" : "other"; }
  return s;
}
}

int main(int argc, char** argv) {
  try {
    const std::string model_path = argc > 1 ? argv[1] : "models/nomic-embed-text-v1.5.Q8_0.gguf";
    require(std::filesystem::exists(model_path), "model file does not exist: " + model_path);
    qt::ai::LlamaSettings settings;
    settings.models_configured = true;
    settings.embeddings.enabled = true;
    settings.embeddings.pooling = "mean";
    settings.embeddings.normalize = true;
    settings.backend.enabled = true;
    const int gpu_layers = argc > 2 ? std::stoi(argv[2]) : 0;
    const std::size_t scale_records = argc > 3 ? static_cast<std::size_t>(std::stoul(argv[3])) : 100U;
    settings.backend.gpu_layers = gpu_layers;
    settings.context.context_size = 2048;
    qt::ai::LlamaModelSettings model;
    model.id = "nomic-embed-text-v1.5";
    model.name = model.id;
    model.path = model_path;
    model.supports_embeddings = true;
    settings.models.push_back(model);
    auto provider = std::make_shared<qt::ai::LlamaProvider>(settings);

    qt::memory::RetrievalEngineOptions options;
    qt::memory::bind_llama_retrieval(options, provider, model,
                                     "search_query: ", "search_document: ", true);
    const auto before = rss_kib();
    const auto config = options.configure({});
    const auto after_load = rss_kib();
    const auto runtime = provider->runtime_status(qt::ai::LlamaInstanceTag::kEmbed);
    require(config.dimensions > 0, "zero embedding dimensions");
    std::cout << "model=" << model_path << '\n'
              << "file_bytes=" << std::filesystem::file_size(model_path) << '\n'
              << "dimensions=" << config.dimensions << '\n'
              << "pooling=" << config.pooling << '\n'
              << "normalized=" << (config.normalize ? "true" : "false") << '\n'
              << "query_prefix=" << config.query_prefix << '\n'
              << "document_prefix=" << config.document_prefix << '\n'
              << "gpu_layers=" << gpu_layers << '\n'
              << "gpu_offload_supported=" << (runtime.gpu_offload_supported ? "true" : "false") << '\n'
              << "runtime_memory=" << runtime.memory.dump() << '\n'
              << "generation=" << config.generation << '\n'
              << "rss_before_kib=" << before << '\n'
              << "rss_after_load_kib=" << after_load << '\n';

    const auto embed = [&](const std::string& text) {
      return options.embedder(config, text, {});
    };
    const auto q = embed("search_query: how do I reset an access token?");
    const auto positive = embed("search_document: rotate credentials and reset an expired access token");
    const auto distractor = embed("search_document: compile the user interface with a native toolchain");
    require(q.size() == config.dimensions, "query vector has wrong dimensions");
    require(std::isfinite(norm(q)) && std::abs(norm(q) - 1.0) < 0.02,
            "normalized query vector is not unit length");
    const double positive_score = cosine(q, positive);
    const double distractor_score = cosine(q, distractor);
    require(positive_score > distractor_score, "semantic positive did not outrank distractor");
    std::cout << "positive_cosine=" << positive_score << '\n'
              << "distractor_cosine=" << distractor_score << '\n'
              << "heldout_nonlexical_top1=" << (positive_score > distractor_score ? "true" : "false") << '\n';

    // Production path: SQLite owner/chunk/vector indexing, DenseRetrievalService,
    // then RecallService dense fusion. The query intentionally shares no token
    // with either stored document, so an FTS-only path cannot satisfy it.
    const auto db_root = std::filesystem::temp_directory_path() /
                         ("qt-memory-v2-encoder-" + std::to_string(::getpid()));
    std::filesystem::remove_all(db_root);
    qt::memory::SQLiteMemoryDatabase database(db_root);
    qt::memory::MemoryScope scope;
    scope.kind = qt::memory::ScopeKind::Exact; scope.user = "encoder-probe";
    scope.project = "retrieval"; scope.repository = "probe"; scope.branch = "main";
    auto add = [&](const std::string& text) {
      qt::memory::MemoryWrite write;
      write.type = qt::memory::MemoryType::Semantic;
      write.status = qt::memory::MemoryStatus::Active;
      write.content = text; write.source = "real-encoder-probe";
      write.confidence = 0.95; write.provenance.source_id = "probe";
      write.provenance.source_type = qt::memory::SourceType::Manual;
      write.provenance.evidence_strength = 1.0;
      return database.add(scope, std::move(write));
    };
    const auto expected_id = add("Rotate credentials and reset an expired access token safely.");
    (void)add("Compile the user interface with a native toolchain and linker.");
    qt::memory::RetrievalEngineOptions engine_options;
    engine_options.enabled = true; engine_options.dense_enabled = true;
    engine_options.configure = options.configure; engine_options.embedder = options.embedder;
    engine_options.validate_embedding = options.validate_embedding;
    qt::memory::RetrievalEngine engine(database, scope, engine_options);
    std::vector<qt::memory::MemoryEntry> recalled;
    for (int attempt = 0; attempt < 80; ++attempt) {
      if (database.dense_index(scope)) {
        recalled = engine.search(scope, "credential lifecycle", qt::memory::RecallPolicy::Balanced,
                                 false, 2);
        if (!recalled.empty()) break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    require(!recalled.empty() && recalled.front().id == expected_id,
            "production dense retrieval did not rank the semantic hit first");
    qt::memory::RecallService recall(database, database, database, database, database,
      [&](const qt::memory::MemoryScope& requested, const qt::memory::RecallRequest& request) {
        return engine.search_candidates(requested, request.query, request.policy,
                             request.include_global_user, request.candidate_limit,
                             request.cancelled);
      });
    qt::memory::RecallRequest recall_request;
    recall_request.query = "credential lifecycle"; recall_request.dense_enabled = true;
    recall_request.result_limit = 2;
    const auto fused = recall.recall(scope, recall_request);
    require(!fused.empty() && fused.front().memory.id == expected_id &&
            fused.front().score.dense_rank != 0 && fused.front().dense_match &&
            fused.front().dense_match->chunk.owner_key == "memory:" + expected_id &&
            fused.front().dense_match->chunk.owner_revision ==
                database.retrieval_revision(scope, expected_id) &&
            !fused.front().dense_match->chunk.id.empty() &&
            !fused.front().dense_match->generation.empty() &&
            std::isfinite(fused.front().dense_match->similarity),
            "RecallService did not retain the production dense hit");
    qt::memory::MemoryQuery lexical_only;
    lexical_only.text = "credential lifecycle";
    lexical_only.policy = qt::memory::RecallPolicy::Balanced;
    lexical_only.limit = 10;
    require(database.query(scope, lexical_only).empty(),
            "dedicated nonlexical query unexpectedly matched lexical search");
    std::cout << "production_dense_recall=true\n"
              << "production_dense_id=" << recalled.front().id << '\n';

    bool cancelled_seen = false;
    try {
      qt::ai::LlamaEmbeddingRequest request;
      request.model_id = model.id;
      request.text = "search_query: cancellation check";
      request.cancelled = [&] { cancelled_seen = true; return true; };
      (void)provider->embed(request, qt::ai::LlamaInstanceTag::kEmbed);
    } catch (...) {
      // Providers may reject before entering llama.cpp; either path is fail-closed.
    }
    require(cancelled_seen, "provider did not observe cancellation callback");

    // Label-preserving four-way evaluation over the checked-in V2 corpus.
    const auto fixture_path = std::filesystem::path("scripts/fixtures/memory_v2_retrieval.json");
    std::ifstream fixture_input(fixture_path);
    const auto fixture = nlohmann::json::parse(fixture_input);
    qt::memory::SQLiteMemoryDatabase eval_db(std::filesystem::temp_directory_path() /
                                             ("qt-memory-v2-eval-" + std::to_string(::getpid())));
    std::map<std::string, std::string> ids, labels;
    std::set<std::string> fixture_scopes;
    for (const auto& item : fixture.at("memories")) {
      qt::memory::MemoryWrite w; w.type = qt::memory::MemoryType::Decision;
      w.status = qt::memory::memory_status_from_string(item.at("status"));
      w.content = item.at("content"); w.source = "memory-v2-baseline";
      w.confidence = item.value("confidence", 0.5); w.quality_score = item.value("quality", 0.0);
      w.provenance.source_type = qt::memory::SourceType::Manual;
      w.provenance.source_uri = "test://memory-v2/" + item.at("id").get<std::string>();
      w.provenance.source_hash = "fixture:" + item.at("id").get<std::string>();
      const auto id = eval_db.add(eval_scope(item.at("scope")), std::move(w));
      ids[item.at("id")] = id; labels[id] = item.at("id");
      fixture_scopes.insert(item.at("scope").get<std::string>());
    }
    const auto index_start = std::chrono::steady_clock::now();
    qt::memory::RetrievalIndexer indexer(eval_db, options.embedder);
    for (const auto& name : fixture_scopes) {
      const auto indexed_scope = eval_scope(name);
      for (std::size_t jobs = 0;; ++jobs) {
        require(jobs < 1000, "chunk indexing did not drain");
        const auto step = indexer.step(indexed_scope);
        if (!step.worked) break;
        require(step.completed, "fixture chunk indexing failed: " + step.error);
      }
      eval_db.begin_dense_index(indexed_scope, config);
      for (std::size_t jobs = 0;; ++jobs) {
        require(jobs < 1000, "dense indexing did not drain");
        const auto step = indexer.step(indexed_scope, {}, config.generation);
        if (!step.worked) break;
        require(step.completed, "fixture dense indexing failed: " + step.error);
      }
      require(eval_db.activate_dense_index(indexed_scope, config.generation),
              "fixture dense generation did not activate");
    }
    const auto index_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - index_start).count();
    qt::memory::DenseRetrievalService dense_service(eval_db, options.embedder);
    const auto graph_edge = [&](const std::string& a, const std::string& b, const std::string& type) {
      qt::memory::GraphEdge e; e.from_id = ids.at(a); e.to_id = ids.at(b); e.type = type;
      e.source = "fixture:explicit-recovery-relationship"; e.confidence = 0.95; eval_db.add_edge(eval_scope("alpha"), e);
    };
    graph_edge("alpha-multi-hop-decision", "alpha-retry-incident", "caused_by");
    graph_edge("alpha-retry-incident", "alpha-retry-recovery", "resolved_by");
    struct Mode { const char* name; bool dense; bool graph; };
    const Mode modes[] = {{"bm25", false, false}, {"bm25_graph", false, true},
                          {"bm25_dense", true, false}, {"combined", true, true}};
    nlohmann::json mode_reports = nlohmann::json::object();
    qt::memory::ContextService context(eval_db, nullptr, &eval_db, {}, &eval_db);
    for (const auto mode : modes) {
      dense_service.clear_cache();
      nlohmann::json cases = nlohmann::json::array(); double r1=0,r3=0,r5=0,hit1=0,hit3=0,hit5=0,mrr=0;
      std::size_t forbidden=0, forbidden_eligible=0, eligible=0, forbidden_injected=0;
      double total_ms=0; std::vector<double> latencies;
      std::vector<double> warm_latencies;
      std::size_t context_injected = 0, context_omitted = 0;
      for (const auto& item : fixture.at("queries")) {
        qt::memory::DenseCandidateProvider provider_cb;
        if (mode.dense) provider_cb = [&](const qt::memory::MemoryScope& s, const qt::memory::RecallRequest& req) {
          const auto hits = dense_service.search(s, req.query, req.policy,
              req.include_global_user, req.candidate_limit, req.cancelled);
          return eval_db.resolve_dense_memory_candidates(
              s, hits, req.policy, req.include_global_user);
        };
        qt::memory::RecallService service(eval_db, eval_db, eval_db, eval_db, eval_db, provider_cb);
        qt::memory::RecallRequest req; req.query = item.at("query"); req.policy = qt::memory::RecallPolicy::Broad;
        req.include_global_user = item.at("scope") == "alpha" && item.at("id") == "security-scope";
        req.dense_enabled = mode.dense; req.graph.enabled = mode.graph; req.result_limit = 20; req.candidate_limit = 50;
        const auto start = std::chrono::steady_clock::now();
        const auto result = service.recall(eval_scope(item.at("scope")), req);
        qt::memory::ContextOptions context_options; context_options.policy = req.policy;
        context_options.include_global_user = req.include_global_user;
        const auto context_pack = context.build(eval_scope(item.at("scope")), result, context_options);
        context_injected += context_pack.memory_ids.size(); context_omitted += context_pack.omitted;
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
        total_ms += elapsed; latencies.push_back(elapsed);
        for (int repetition = 0; repetition < 3; ++repetition) {
          const auto warm_start = std::chrono::steady_clock::now();
          const auto warm_result = service.recall(eval_scope(item.at("scope")), req);
          (void)context.build(eval_scope(item.at("scope")), warm_result, context_options);
          warm_latencies.push_back(std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - warm_start).count());
        }
        std::set<std::string> rel; for (const auto& x : item.at("relevant")) rel.insert(x.get<std::string>());
        std::set<std::string> forb; for (const auto& x : item.value("forbidden", nlohmann::json::array())) forb.insert(x.get<std::string>());
        std::size_t first=0; bool bad=false; std::size_t rel1=0, rel3=0, rel5=0; nlohmann::json ranking=nlohmann::json::array();
        for (std::size_t i=0;i<result.size();++i) { const auto label=labels[result[i].memory.id]; ranking.push_back({{"rank",i+1},{"label",label},{"eligible_for_context",result[i].eligible_for_context}}); if(rel.count(label)&&!first) first=i+1; if(rel.count(label)&&i<1) rel1++; if(rel.count(label)&&i<3) rel3++; if(rel.count(label)&&i<5) rel5++; if(forb.count(label)) { bad=true; forbidden_eligible += result[i].eligible_for_context ? 1 : 0; } if(result[i].eligible_for_context) eligible++; }
        nlohmann::json injected_labels = nlohmann::json::array();
        nlohmann::json forbidden_labels = nlohmann::json::array();
        for (const auto& id : context_pack.memory_ids) {
          const auto& label = labels.at(id);
          injected_labels.push_back(label);
          if (forb.count(label)) { ++forbidden_injected; forbidden_labels.push_back(label); }
        }
        if(first==1) hit1++; if(first&&first<=3) hit3++; if(first&&first<=5) hit5++; if(first) mrr += 1.0/first; if(bad) forbidden++;
        r1 += static_cast<double>(rel1) / rel.size(); r3 += static_cast<double>(rel3) / rel.size(); r5 += static_cast<double>(rel5) / rel.size();
        cases.push_back({{"id",item.at("id")},{"first_relevant_rank",first},{"forbidden_hit",bad},{"ranking",ranking},
                         {"injected_labels",injected_labels},{"forbidden_injected_labels",forbidden_labels}});
      }
      const double n = static_cast<double>(cases.size());
      std::sort(latencies.begin(), latencies.end());
      const auto percentile = [&](double p) { return latencies.empty() ? 0.0 : latencies[std::min<std::size_t>(latencies.size()-1, static_cast<std::size_t>(p * latencies.size()))]; };
      mode_reports[mode.name] = {{"metrics",{{"recall_at_1",r1/n},{"recall_at_3",r3/n},{"recall_at_5",r5/n},{"hit_at_1",hit1/n},{"hit_at_3",hit3/n},{"hit_at_5",hit5/n},{"mrr",mrr/n},{"forbidden_candidate_rate",forbidden/n},{"forbidden_eligible_count",forbidden_eligible},{"eligible_candidate_count",eligible},{"forbidden_injected_count",forbidden_injected},{"context_memory_injected_count",context_injected},{"context_omitted_count",context_omitted},{"latency_avg_ms",total_ms/n}}},{"cases",cases}};
      mode_reports[mode.name]["metrics"]["latency_p50_ms"] = percentile(0.50);
      mode_reports[mode.name]["metrics"]["latency_p95_ms"] = percentile(0.95);
      std::sort(warm_latencies.begin(), warm_latencies.end());
      const auto warm_percentile = [&](double p) {
        const auto index = static_cast<std::size_t>(std::ceil(p * warm_latencies.size())) - 1;
        return warm_latencies.at(std::min(index, warm_latencies.size() - 1));
      };
      mode_reports[mode.name]["metrics"]["warm_latency_p50_ms"] = warm_percentile(0.50);
      mode_reports[mode.name]["metrics"]["warm_latency_p95_ms"] = warm_percentile(0.95);
      mode_reports[mode.name]["metrics"]["warm_samples"] = warm_latencies.size();
    }
    mode_reports["resources"] = {{"index_ms", index_ms}, {"indexed_memories", ids.size()}};
    mode_reports["resources"]["ranking_revision"] = "owner-rrf-v2";
    const auto file_bytes = [&](const std::filesystem::path& path) {
      std::error_code error; return std::filesystem::exists(path, error) ? std::filesystem::file_size(path, error) : 0ULL;
    };
    const auto disk_bytes = file_bytes(eval_db.path()) + file_bytes(eval_db.path().string() + "-wal") + file_bytes(eval_db.path().string() + "-shm");
    struct rusage usage_before_scale{};
    getrusage(RUSAGE_SELF, &usage_before_scale);
    mode_reports["resources"]["peak_rss_kib"] = usage_before_scale.ru_maxrss;
    mode_reports["resources"]["sqlite_bytes_including_wal_shm"] = disk_bytes;
    std::cout << "four_way_report=" << mode_reports.dump() << '\n';
    // Separate scale workload: deterministic short source-like records, kept
    // out of the labelled relevance corpus and its quality metrics.
    const auto scale_root = std::filesystem::temp_directory_path() /
                            ("qt-memory-v2-scale-" + std::to_string(::getpid()));
    std::filesystem::remove_all(scale_root);
    qt::memory::SQLiteMemoryDatabase scale_db(scale_root);
    qt::memory::MemoryScope scale_scope = eval_scope("alpha");
    std::vector<double> embedding_ms;
    auto scale_embedder = [&](const qt::memory::DenseIndexConfig& current,
                              const std::string& text, const qt::memory::RetrievalCancellation& cancelled) {
      const auto start = std::chrono::steady_clock::now();
      const auto value = options.embedder(current, text, cancelled);
      embedding_ms.push_back(std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start).count());
      return value;
    };
    qt::memory::RetrievalEngineOptions scale_options;
    scale_options.enabled = true; scale_options.dense_enabled = true;
    scale_options.configure = options.configure; scale_options.embedder = scale_embedder;
    scale_options.validate_embedding = options.validate_embedding;
    for (std::size_t i = 0; i < scale_records; ++i) {
      qt::memory::MemoryWrite w; w.type = qt::memory::MemoryType::Observation;
      w.status = qt::memory::MemoryStatus::Active;
      w.content = "Source fact " + std::to_string(i) + ": parser module records branch-safe token " +
                  std::to_string(i % 17) + " for repository maintenance.";
      w.source = "scale-workload"; w.provenance.source_type = qt::memory::SourceType::File;
      w.provenance.source_uri = "file://src/scale_fixture_" + std::to_string(i) + ".cpp";
      w.provenance.evidence_strength = 1.0; scale_db.add(scale_scope, std::move(w));
    }
    const auto scale_start = std::chrono::steady_clock::now();
    qt::memory::RetrievalEngine scale_engine(scale_db, scale_scope, scale_options);
    bool scale_complete = false;
    for (int attempt = 0; attempt < 1200; ++attempt) {
      const auto status = scale_engine.status();
      if (status.jobs.pending == 0 && status.jobs.leased == 0 && status.jobs.failed == 0 &&
          status.jobs.chunks == scale_records &&
          scale_db.dense_index(scale_scope)) { scale_complete = true; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(scale_complete, "scale workload did not activate a complete dense generation");
    scale_engine.stop();
    const auto scale_elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - scale_start).count();
    const auto scale_config = scale_db.dense_index(scale_scope);
    require(scale_config.has_value(), "scale dense generation missing after worker stop");
    const auto scale_query = options.embedder(*scale_config, scale_config->query_prefix + "parser branch token", {});
    const auto scale_hits = scale_db.search_dense(scale_scope, scale_config->generation, scale_query,
                                                  qt::memory::RecallPolicy::Strict, false, scale_records);
    require(scale_hits.size() == scale_records && scale_db.size(scale_scope) == scale_records &&
            embedding_ms.size() == scale_records, "scale workload vector/chunk count mismatch");
    std::sort(embedding_ms.begin(), embedding_ms.end());
    const auto scale_percentile = [&](double p) { return embedding_ms.empty() ? 0.0 : embedding_ms[std::min<std::size_t>(embedding_ms.size()-1, static_cast<std::size_t>(p * embedding_ms.size()))]; };
    const auto scale_file = [&](const std::filesystem::path& path) { std::error_code e; return std::filesystem::exists(path, e) ? std::filesystem::file_size(path, e) : 0ULL; };
    const auto scale_disk = scale_file(scale_db.path()) + scale_file(scale_db.path().string() + "-wal") + scale_file(scale_db.path().string() + "-shm");
    struct rusage scale_usage{};
    getrusage(RUSAGE_SELF, &scale_usage);
    std::cout << "scale_records=" << scale_records << '\n'
              << "scale_index_ms=" << scale_elapsed << '\n'
              << "scale_chunks=" << scale_db.retrieval_index_status(scale_scope).chunks << '\n'
              << "scale_vector_count=" << scale_hits.size() << '\n'
              << "scale_vectors_searchable=true\n"
              << "scale_embedding_count=" << embedding_ms.size() << '\n'
              << "scale_embedding_p50_ms=" << scale_percentile(0.50) << '\n'
              << "scale_embedding_p95_ms=" << scale_percentile(0.95) << '\n'
              << "scale_disk_bytes_total=" << scale_disk << '\n'
              << "scale_token_count=unavailable\n"
              << "scale_report={\"records\":" << scale_records << ",\"chunks\":"
              << scale_db.retrieval_index_status(scale_scope).chunks << ",\"index_ms\":"
              << scale_elapsed << ",\"embedding_p50_ms\":" << scale_percentile(0.50)
              << ",\"embedding_p95_ms\":" << scale_percentile(0.95)
              << ",\"disk_bytes_total\":" << scale_disk << ",\"peak_rss_kib\":"
              << scale_usage.ru_maxrss << ",\"vector_count\":" << scale_hits.size()
              << ",\"embedding_count\":" << embedding_ms.size()
              << ",\"token_count\":null,\"token_count_reason\":\"provider_usage_exposes_no_tokenizer_count\"}\n";
    std::cout << "peak_rss_kib=" << scale_usage.ru_maxrss << '\n'
              << "evaluation_db_bytes_total=" << disk_bytes << '\n';
    std::cout << "cancellation_observed=" << (cancelled_seen ? "true" : "false") << '\n'
              << "gpu_tested=" << ((gpu_layers > 0 && runtime.gpu_offload_supported) ? "true" : "false") << '\n'
              << "design_memory_limit_bytes=" << (8ULL * 1024ULL * 1024ULL * 1024ULL) << '\n';
    // Supplemental authority scenario, AFTER the frozen quality and resource
    // measurements. No fixture labels or baseline lifecycle states are changed.
    const auto governed_scope = eval_scope("alpha");
    const auto old_policy = ids.at("alpha-cache-policy-old");
    const auto new_policy = ids.at("alpha-cache-policy-new");
    const auto contains_id = [](const std::vector<std::string>& values, const std::string& id) {
      return std::find(values.begin(), values.end(), id) != values.end();
    };
    const auto dense_provider = [&](const qt::memory::MemoryScope& scope,
                                    const qt::memory::RecallRequest& request) {
      const auto hits = dense_service.search(scope, request.query, request.policy,
          request.include_global_user, request.candidate_limit, request.cancelled);
      return eval_db.resolve_dense_memory_candidates(
          scope, hits, request.policy, request.include_global_user);
    };
    std::map<std::string, std::vector<qt::memory::RecalledMemory>> captured;
    qt::memory::RecallRequest governed_request;
    governed_request.query = "What is the approved cache policy for source evidence?";
    governed_request.policy = qt::memory::RecallPolicy::Broad;
    governed_request.result_limit = 20; governed_request.candidate_limit = 50;
    qt::memory::ContextOptions governed_context;
    governed_context.policy = governed_request.policy;
    for (const auto mode : modes) {
      qt::memory::DenseCandidateProvider callback;
      if (mode.dense) callback = dense_provider;
      qt::memory::RecallService service(eval_db, eval_db, eval_db, eval_db, eval_db, callback);
      governed_request.dense_enabled = mode.dense; governed_request.graph.enabled = mode.graph;
      auto results = service.recall(governed_scope, governed_request);
      const auto packed = context.build(governed_scope, results, governed_context);
      require(contains_id(packed.memory_ids, old_policy) && contains_id(packed.memory_ids, new_policy),
              std::string(mode.name) + ": supplemental pre-resolution control missing policy");
      captured.emplace(mode.name, std::move(results));
    }
    const auto dense_before = dense_provider(governed_scope, governed_request);
    require(std::any_of(dense_before.begin(), dense_before.end(), [&](const auto& candidate) {
      return candidate.memory.id == old_policy && candidate.hit.chunk.owner_key == "memory:" + old_policy &&
          candidate.hit.chunk.owner_revision == eval_db.retrieval_revision(governed_scope, old_policy) &&
          !candidate.hit.chunk.id.empty() && !candidate.hit.generation.empty() &&
          std::isfinite(candidate.hit.similarity);
    }), "supplemental control did not publish old policy vector");
    const auto revisions_before = eval_db.revisions(governed_scope, old_policy).size();
    qt::memory::MemoryService governed_memories(eval_db);
    qt::memory::GovernanceService governance(governed_memories, eval_db, eval_db);
    qt::memory::LifecycleRequest resolution;
    resolution.id = old_policy; resolution.status = qt::memory::MemoryStatus::Superseded;
    resolution.replacement_id = new_policy; resolution.actor = "supplemental-policy-owner";
    resolution.reason = "Explicit policy-owner decision replaces the earlier cache evidence rule";
    resolution.has_evidence = true;
    resolution.evidence_uri = "test://supplemental/policy-owner/cache-resolution";
    const auto resolved = governance.transition(governed_scope, resolution);
    require(resolved.status == qt::memory::MemoryStatus::Superseded &&
            resolved.replacement_id == new_policy, "governed policy state not published");
    const auto revisions_after = eval_db.revisions(governed_scope, old_policy).size();
    require(revisions_after > revisions_before, "governed change did not retain revision history");
    const auto audits = eval_db.audit_events(governed_scope, 100);
    const auto audit = std::find_if(audits.begin(), audits.end(), [&](const auto& value) {
      return value.before_id == old_policy && value.after_id == new_policy &&
          value.actor == resolution.actor && value.reason == resolution.reason &&
          value.action == "memory.lifecycle.superseded" && value.evidence_uri == resolution.evidence_uri;
    });
    require(audit != audits.end(), "governed change lacks evidence-bearing audit");
    qt::memory::GraphQuery relationship; relationship.memory_id = old_policy;
    const auto edges = eval_db.query_edges(governed_scope, relationship);
    const auto edge = std::find_if(edges.begin(), edges.end(), [&](const auto& value) {
      return value.from_id == old_policy && value.to_id == new_policy && value.type == "superseded_by";
    });
    require(edge != edges.end(), "governed replacement relationship missing");
    // Deliberately run no indexer after mutation: eligibility cannot wait for it.
    const auto pending = eval_db.retrieval_index_status(governed_scope).pending;
    require(pending > 0, "supplemental scenario did not leave lifecycle indexing pending");
    const auto dense_after = dense_provider(governed_scope, governed_request);
    require(std::none_of(dense_after.begin(), dense_after.end(), [&](const auto& candidate) {
      return candidate.memory.id == old_policy;
    }), "superseded vector remained eligible before reindex");
    nlohmann::json supplemental = {{"scenario", "explicit_policy_supersession"},
        {"evidence_uri", resolution.evidence_uri}, {"audit_id", audit->id},
        {"relationship_id", edge->id}, {"revisions_before", revisions_before},
        {"revisions_after", revisions_after}, {"pending_jobs_after_resolution", pending},
        {"indexer_steps_after_resolution", 0}, {"old_vector_verified_before", true},
        {"old_vector_excluded_after", true}, {"modes", nlohmann::json::object()}};
    for (const auto mode : modes) {
      qt::memory::DenseCandidateProvider callback;
      if (mode.dense) callback = dense_provider;
      qt::memory::RecallService service(eval_db, eval_db, eval_db, eval_db, eval_db, callback);
      governed_request.dense_enabled = mode.dense; governed_request.graph.enabled = mode.graph;
      const auto stale_pack = context.build(governed_scope, captured.at(mode.name), governed_context);
      require(!contains_id(stale_pack.memory_ids, old_policy) && contains_id(stale_pack.memory_ids, new_policy),
              std::string(mode.name) + ": stale capture bypassed final policy authority");
      const auto fresh = service.recall(governed_scope, governed_request);
      require(std::none_of(fresh.begin(), fresh.end(), [&](const auto& item) {
        return item.memory.id == old_policy && item.eligible_for_context;
      }), std::string(mode.name) + ": superseded policy remained eligible");
      const auto packed = context.build(governed_scope, fresh, governed_context);
      require(!contains_id(packed.memory_ids, old_policy) && contains_id(packed.memory_ids, new_policy),
              std::string(mode.name) + ": governed policy context incorrect before reindex");
      if (mode.dense) {
        // Simulate an in-flight provider returning its pre-resolution snapshot.
        qt::memory::RecallService stale_dense(eval_db, eval_db, eval_db, eval_db, eval_db,
            [&](const qt::memory::MemoryScope&, const qt::memory::RecallRequest&) {
              return dense_before;
            });
        const auto delayed = stale_dense.recall(governed_scope, governed_request);
        require(std::none_of(delayed.begin(), delayed.end(), [&](const auto& item) {
          return item.memory.id == old_policy && item.eligible_for_context;
        }), std::string(mode.name) + ": frozen dense candidate bypassed current authority");
        const auto delayed_pack = context.build(governed_scope, delayed, governed_context);
        require(!contains_id(delayed_pack.memory_ids, old_policy) &&
                contains_id(delayed_pack.memory_ids, new_policy),
                std::string(mode.name) + ": frozen dense context lost replacement authority");
      }
      std::vector<std::string> injected;
      for (const auto& id : packed.memory_ids) injected.push_back(labels.at(id));
      supplemental["modes"][mode.name] = {{"injected_labels", injected},
          {"stale_capture_revalidated", true}, {"replacement_injected", true},
          {"superseded_injected", false}};
      supplemental["modes"][mode.name]["frozen_dense_candidate_revalidated"] =
          mode.dense ? nlohmann::json(true) : nlohmann::json(nullptr);
    }
    std::cout << "governed_policy_report=" << supplemental.dump() << '\n';
    provider->unload();
    engine.stop();
    std::filesystem::remove_all(db_root);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "real encoder probe failed: " << error.what() << '\n';
    return 1;
  }
}
