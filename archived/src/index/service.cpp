#include "index/service.hpp"

#include "index/build_metadata.hpp"
#include "index/cancellation.hpp"
#include "index/compilation_database.hpp"
#include "index/project_scan.hpp"
#include "index/sqlite_store.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace qt::index {
namespace {

std::string time_to_string(std::chrono::system_clock::time_point time) {
    if (time == std::chrono::system_clock::time_point{}) {
        return {};
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        time.time_since_epoch()).count();
    return std::to_string(seconds);
}

RetrievalSearchHit search_hit_for_node(const RetrievalCodeNode& node, double rank,
                                       std::size_t generation) {
    RetrievalSearchHit hit;
    hit.node_id = node.id;
    hit.path = node.path.generic_string();
    hit.kind = node.kind;
    hit.rank = rank;
    hit.generation = generation;
    hit.source_fingerprint = node.source_fingerprint;
    return hit;
}

void low_priority_service_checkpoint(bool low_priority) {
    scan_cancellation_checkpoint();
    if (!low_priority) {
        return;
    }
    std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
}

unsigned clamp_progress_percent(unsigned value) {
    return value > 100U ? 100U : value;
}

}  // namespace

BackgroundService::BackgroundService(ServiceOptions options) : options_(std::move(options)) {
    options_.project_root = std::filesystem::absolute(options_.project_root);
    if (options_.state_directory.empty()) {
        options_.state_directory = options_.project_root / ".index";
    }
    if (options_.compile_commands_output.empty()) {
        options_.compile_commands_output = options_.project_root / "compile_commands.json";
    }
    if (options_.tree_sitter.output_directory.empty()) {
        options_.tree_sitter.output_directory = options_.state_directory / "tree-sitter";
    }
    status_.sqlite_database = sqlite_index_database_path(options_.state_directory);
    if (!options_.defer_snapshot_load) {
        load_persisted_snapshot();
    }
}

void BackgroundService::load_persisted_snapshot() {
    std::string load_error;
    auto snapshot = load_sqlite_index_snapshot(status_.sqlite_database, options_.project_root, &load_error);
    if (!snapshot) {
        if (!load_error.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.last_error = "sqlite index load failed: " + load_error;
        }
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    scan_ = snapshot->scan;
    code_index_ = snapshot->code_index;
    if (options_.retrieval_enabled && snapshot->scan_fingerprints_complete) {
        retrieval_snapshot_ = RetrievalSnapshot::build(
            scan_, code_index_, snapshot->generation, snapshot->scan_fingerprints, false);
    } else if (options_.retrieval_enabled) {
        retrieval_snapshot_.reset();
    }
    status_.snapshot_loaded = true;
    status_.index_stale = true;
    status_.sqlite_fts_available = snapshot->fts_available;
    status_.generation = snapshot->generation;
    status_.files = snapshot->scan.files.size();
    status_.compile_commands = snapshot->compile_commands;
    status_.build_metadata_summary = snapshot->build_metadata_summary;
    status_.last_scan_finished = snapshot->last_scan_finished;
    status_.tree_sitter = snapshot->tree_sitter;
}

BackgroundService::~BackgroundService() {
    stop();
}

void BackgroundService::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.running) {
        return;
    }
    stop_requested_.store(false, std::memory_order_relaxed);
    status_.running = true;
    scan_requested_ = true;
    worker_ = std::thread(&BackgroundService::worker_loop, this);
    wake_.notify_all();
}

void BackgroundService::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!status_.running && !worker_.joinable()) {
            return;
        }
        stop_requested_.store(true, std::memory_order_relaxed);
        status_.running = false;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void BackgroundService::request_scan() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scan_requested_ = true;
    }
    wake_.notify_all();
}

void BackgroundService::run_once() {
    run_once_impl(false);
}

void BackgroundService::run_once_impl(bool force_low_priority) {
    ScopedScanCancellation cancellation(stop_requested_);
    ServiceStatus next = status();
    next.scan_active = true;
    next.scan_progress_percent = 0;
    next.scan_phase = "starting";
    next.last_scan_started = std::chrono::system_clock::now();
    next.last_error.clear();
    update_status(next);
    const auto progress = [this](unsigned percent, std::string phase) {
        ServiceStatus status = this->status();
        status.scan_active = true;
        status.scan_progress_percent = clamp_progress_percent(percent);
        status.scan_phase = std::move(phase);
        this->update_status(std::move(status));
    };

    try {
        const bool low_priority_scan = force_low_priority || next.generation > 0;
        std::filesystem::create_directories(options_.state_directory);
        low_priority_service_checkpoint(low_priority_scan);
        progress(5, "scanning project files");
        const auto scan = scan_project_tree(options_.project_root, low_priority_scan);
        std::unordered_map<std::string, std::string> scan_fingerprints;
        if (options_.retrieval_enabled) {
            for (const auto& file : scan.files) {
                scan_fingerprints.emplace(file.path.generic_string(),
                                          retrieval_source_fingerprint(scan.root / file.path));
            }
        }
        low_priority_service_checkpoint(low_priority_scan);
        progress(25, "scanning build metadata");
        const auto metadata = scan_build_metadata(scan, low_priority_scan);
        low_priority_service_checkpoint(low_priority_scan);
        progress(45, "writing compilation database");
        const auto entries =
            write_compilation_database(scan, metadata, options_.compile_commands_output, low_priority_scan);
        low_priority_service_checkpoint(low_priority_scan);
        progress(60, "building code index");
        const auto code_index = build_code_index(scan, low_priority_scan);
        low_priority_service_checkpoint(low_priority_scan);
        progress(80, "generating tree-sitter metadata");
        const auto tree_sitter =
            generate_tree_sitter_metadata(scan, options_.tree_sitter, low_priority_scan);

        next = status();
        next.scan_active = false;
        next.scan_progress_percent = 100;
        next.scan_phase = "complete";
        next.generation += 1;
        next.files = scan.files.size();
        next.compile_commands = entries;
        next.build_metadata_summary = build_metadata_summary(metadata);
        const auto code_summary = nlohmann::json::parse(code_index_summary_json(code_index));
        next.build_metadata_summary += " code_index=classes:" +
                                       std::to_string(code_summary.value("classes", 0)) +
                                       ",functions:" +
                                       std::to_string(code_summary.value("functions", 0)) +
                                       ",call_graph_edges:" +
                                       std::to_string(code_summary.value("call_graph_edges", 0)) +
                                       ",skipped_files:" +
                                       std::to_string(code_summary.value("skipped_files", 0));
        next.last_scan_finished = std::chrono::system_clock::now();
        next.last_error.clear();
        next.tree_sitter = tree_sitter;
        next.snapshot_loaded = true;
        next.index_stale = false;
        next.sqlite_database = sqlite_index_database_path(options_.state_directory);
        save_sqlite_index_snapshot(next.sqlite_database,
                                   options_.project_root,
                                   scan,
                                   code_index,
                                   next.generation,
                                   entries,
                                   next.build_metadata_summary,
                                   tree_sitter,
                                   scan_fingerprints);
        std::string reload_error;
        const auto reloaded =
            load_sqlite_index_snapshot(next.sqlite_database, options_.project_root, &reload_error);
        next.sqlite_fts_available = reloaded.has_value() && reloaded->fts_available;
        std::shared_ptr<const RetrievalSnapshot> retrieval;
        if (options_.retrieval_enabled) {
            retrieval = RetrievalSnapshot::build(
                scan, code_index, next.generation, scan_fingerprints, false);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            code_index_ = code_index;
            scan_ = scan;
            scan_fingerprints_ = scan_fingerprints;
            retrieval_snapshot_ = std::move(retrieval);
            status_ = next;
        }
    } catch (const std::exception& ex) {
        next = status();
        next.scan_active = false;
        if (std::string(ex.what()) == "index scan cancelled") {
            next.scan_phase = "cancelled";
        } else {
            next.scan_phase = "error";
        }
        next.last_scan_finished = std::chrono::system_clock::now();
        next.last_error = ex.what();
        update_status(next);
    }
}

ServiceStatus BackgroundService::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

const ServiceOptions& BackgroundService::options() const {
    return options_;
}

CodeIndex BackgroundService::code_index() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return code_index_;
}

ProjectScanResult BackgroundService::project_scan() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return scan_;
}

std::shared_ptr<const RetrievalSnapshot> BackgroundService::retrieval_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return retrieval_snapshot_;
}

RetrievalSearchResult BackgroundService::retrieval_search(const std::string& query,
                                                          std::size_t limit) const {
    RetrievalSearchResult result;
    result.query = query;
    if (query.empty()) {
        result.diagnostic = "search query is empty";
        return result;
    }
    if (limit == 0) return result;
    limit = std::min<std::size_t>(limit, 1000U);
    std::shared_ptr<const RetrievalSnapshot> snapshot;
    std::filesystem::path database;
    std::size_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = retrieval_snapshot_;
        database = status_.sqlite_database;
        generation = status_.generation;
    }
    if (!snapshot || snapshot->stale() || generation == 0 || snapshot->generation() != generation) {
        result.stale = true;
        result.diagnostic = "retrieval snapshot is stale";
        return result;
    }
    result.generation = generation;
    nlohmann::json payload;
    try {
        std::string fts_query;
        std::string word;
        std::size_t words = 0;
        const auto flush_word = [&] {
            if (word.empty() || words >= 32) { word.clear(); return; }
            if (!fts_query.empty()) fts_query += " OR ";
            fts_query += "\"" + word + "\"";
            word.clear(); ++words;
        };
        for (const unsigned char ch : query.substr(0, 16384)) {
            if (std::isalnum(ch) || ch == '_' || ch >= 128) word.push_back(static_cast<char>(ch));
            else flush_word();
        }
        flush_word();
        if (fts_query.empty()) throw std::invalid_argument("search has no lexical terms");
        payload = sqlite_fts_search_payload_pinned(database, options_.project_root, fts_query, limit,
                                                   generation);
    } catch (const std::exception& ex) {
        result.diagnostic = ex.what();
        if (result.diagnostic.find("stale") != std::string::npos ||
            result.diagnostic.find("generation") != std::string::npos) {
            result.stale = true;
            return result;
        }
    }
    std::unordered_map<std::string, const RetrievalCodeNode*> files;
    std::unordered_map<std::string, const RetrievalCodeNode*> exact;
    for (const auto& node : snapshot->nodes()) {
        if (node.kind == RetrievalCodeNodeKind::File) files.emplace(node.path.generic_string(), &node);
        if (node.name == query || node.qualified_name == query || node.path.generic_string() == query)
            exact.emplace(node.id, &node);
    }
    const auto add_if_current = [&](const RetrievalCodeNode& node, double rank) {
        if (result.hits.size() >= limit) return;
        if (std::any_of(result.hits.begin(), result.hits.end(),
                       [&](const auto& hit) { return hit.node_id == node.id; })) return;
        try {
            if (retrieval_source_fingerprint(snapshot->root() / node.path) != node.source_fingerprint) return;
        } catch (...) { return; }
        result.hits.push_back(search_hit_for_node(node, rank, generation));
    };
    std::vector<std::string> exact_ids;
    for (const auto& item : exact) exact_ids.push_back(item.first);
    std::sort(exact_ids.begin(), exact_ids.end());
    for (const auto& id : exact_ids) add_if_current(*exact.at(id), -1.0);
    if (payload.is_object() && payload.contains("matches")) {
        for (const auto& match : payload.at("matches")) {
            const auto path = match.value("path", std::string{});
            const auto found = files.find(path);
            if (found == files.end()) continue;
            add_if_current(*found->second, match.value("rank", 0.0));
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.generation != generation || retrieval_snapshot_ != snapshot ||
            !retrieval_snapshot_ || retrieval_snapshot_->generation() != generation) {
            result.hits.clear();
            result.stale = true;
            result.diagnostic = "retrieval snapshot changed during search";
        }
    }
    return result;
}

std::filesystem::path BackgroundService::project_root() const {
    return options_.project_root;
}

void BackgroundService::worker_loop() {
    if (options_.defer_snapshot_load) {
        load_persisted_snapshot();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        if (!scan_requested_) {
            wake_.wait_for(lock, options_.scan_interval);
        }
        if (stop_requested_.load(std::memory_order_relaxed)) {
            break;
        }
        scan_requested_ = false;
        lock.unlock();
        run_once_impl(options_.low_priority_background_scans);
        lock.lock();
    }
}

void BackgroundService::update_status(ServiceStatus status) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool running = status_.running;
    status_ = std::move(status);
    status_.running = running && !stop_requested_.load(std::memory_order_relaxed);
}

std::string service_status_json(const ServiceStatus& status) {
    nlohmann::json value{
        {"running", status.running},
        {"scan_active", status.scan_active},
        {"scan_progress_percent", status.scan_progress_percent},
        {"scan_phase", status.scan_phase},
        {"scan_progress",
         {{"percent", status.scan_progress_percent},
          {"phase", status.scan_phase}}},
        {"generation", status.generation},
        {"files", status.files},
        {"compile_commands", status.compile_commands},
        {"build_metadata", status.build_metadata_summary},
        {"last_error", status.last_error},
        {"snapshot_loaded", status.snapshot_loaded},
        {"index_stale", status.index_stale},
        {"sqlite_fts_available", status.sqlite_fts_available},
        {"sqlite_database", status.sqlite_database.string()},
        {"last_scan_started", time_to_string(status.last_scan_started)},
        {"last_scan_finished", time_to_string(status.last_scan_finished)},
        {"tree_sitter",
         {{"attempted", status.tree_sitter.attempted},
          {"available", status.tree_sitter.available},
          {"generated", status.tree_sitter.generated},
          {"files_considered", status.tree_sitter.files_considered},
          {"files_parsed", status.tree_sitter.files_parsed},
          {"files_updated", status.tree_sitter.files_updated},
          {"files_unchanged", status.tree_sitter.files_unchanged},
          {"files_added", status.tree_sitter.files_added},
          {"files_changed", status.tree_sitter.files_changed},
          {"files_deleted", status.tree_sitter.files_deleted},
          {"files_failed", status.tree_sitter.files_failed},
          {"files_skipped", status.tree_sitter.files_skipped},
          {"output_directory", status.tree_sitter.output_directory.string()},
          {"manifest_path", status.tree_sitter.manifest_path.string()},
          {"failed_files", status.tree_sitter.failed_files},
          {"skipped_files", status.tree_sitter.skipped_files},
          {"mode", status.tree_sitter.mode},
          {"message", status.tree_sitter.message}}},
    };
    return value.dump(2);
}

int run_service(const ServiceOptions& options, std::istream& in, std::ostream& out, std::ostream& err) {
    (void)options;
    (void)in;
    (void)out;
    err << "index MCP transports are not linked into qt; use native index tools instead\n";
    return 2;
#if 0
    BackgroundService service(options);
    service.start();

    if (options.mcp_stdio) {
        const int code = run_mcp_stdio_server(service, in, out);
        service.stop();
        return code;
    }

    const int code = run_mcp_http_server(service, options.http_port, err);
    service.stop();
    return code;
#endif
}

}  // namespace qt::index
