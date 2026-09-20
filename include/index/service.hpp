#pragma once

#include "index/code_index.hpp"
#include "index/project_scan.hpp"
#include "index/retrieval_snapshot.hpp"
#include "index/retrieval_search.hpp"
#include "index/tree_sitter_metadata.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace qt::index {

struct ServiceOptions {
    std::filesystem::path project_root = ".";
    std::filesystem::path state_directory;
    std::filesystem::path compile_commands_output;
    std::chrono::seconds scan_interval{30};
    bool mcp_stdio = false;
    int http_port = 8765;
    bool low_priority_background_scans = true;
    bool defer_snapshot_load = false;
    TreeSitterOptions tree_sitter;
    bool retrieval_enabled = false;
};

struct ServiceStatus {
    bool running = false;
    bool scan_active = false;
    unsigned scan_progress_percent = 0;
    std::string scan_phase;
    std::size_t generation = 0;
    std::size_t files = 0;
    std::size_t compile_commands = 0;
    std::string build_metadata_summary;
    std::string last_error;
    bool snapshot_loaded = false;
    bool index_stale = false;
    bool sqlite_fts_available = false;
    std::filesystem::path sqlite_database;
    std::chrono::system_clock::time_point last_scan_started;
    std::chrono::system_clock::time_point last_scan_finished;
    TreeSitterResult tree_sitter;
};

class BackgroundService {
public:
    explicit BackgroundService(ServiceOptions options);
    ~BackgroundService();

    BackgroundService(const BackgroundService&) = delete;
    BackgroundService& operator=(const BackgroundService&) = delete;

    void start();
    void stop();
    void request_scan();
    void run_once();
    ServiceStatus status() const;
    CodeIndex code_index() const;
    ProjectScanResult project_scan() const;
    std::shared_ptr<const RetrievalSnapshot> retrieval_snapshot() const;
    RetrievalSearchResult retrieval_search(const std::string& query,
                                           std::size_t limit = 1000) const;
    std::filesystem::path project_root() const;
    const ServiceOptions& options() const;

private:
    void worker_loop();
    void load_persisted_snapshot();
    void run_once_impl(bool force_low_priority);
    void update_status(ServiceStatus status);

    ServiceOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::atomic_bool stop_requested_{false};
    bool scan_requested_ = false;
    ServiceStatus status_;
    CodeIndex code_index_;
    ProjectScanResult scan_;
    std::unordered_map<std::string, std::string> scan_fingerprints_;
    std::shared_ptr<const RetrievalSnapshot> retrieval_snapshot_;
};

std::string service_status_json(const ServiceStatus& status);
int run_service(const ServiceOptions& options, std::istream& in, std::ostream& out, std::ostream& err);

}  // namespace qt::index
