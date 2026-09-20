#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <map>
#include <vector>

#include <qt/ai/llama_config.hpp>
#include <qt/ai/llama_model_discovery.hpp>
#include <qt/ai/ollama.hpp>
#include <qt/app/config/prompt_history.hpp>
#include <qt/foundation/error.hpp>
#include <nlohmann/json.hpp>

namespace qt::harness {
class RepositoryView;
struct RepoContextDiscovery;
}  // namespace qt::harness

namespace qt::app {

struct FeatureSettings {
  bool load_prompt_templates = true;
  bool load_skills = true;
  bool load_themes = true;
  bool persist_sessions = true;
};

struct FeatureSettingsPatch {
  std::optional<bool> load_prompt_templates;
  std::optional<bool> load_skills;
  std::optional<bool> load_themes;
  std::optional<bool> persist_sessions;

  bool empty() const noexcept;
};

struct ResourceFilterSettings {
  std::vector<std::string> prompt_templates;
  std::vector<std::string> skills;
  std::vector<std::string> themes;
};

struct ProviderRetrySettings {
  std::optional<std::int64_t> timeout_ms;
  std::optional<int> max_retries;
  std::optional<std::int64_t> max_retry_delay_ms;

  bool empty() const noexcept;
};

struct MemoryPrehandlerSettings {
  std::optional<std::string> model;
  std::string thinking = "off";
  std::int64_t timeout_ms = 30000;
  bool fail_open = true;
  std::size_t max_model_input_tokens = 12000;
};

struct MemoryCaptureSettings {
  std::string post_process = "turn_end";
  std::string default_status = "hypothesis";
  std::size_t max_evidence_chars = 32000;
};

struct MemoryDreamingSettings {
  bool enabled = false;
  std::string trigger = "post_turn";
  std::uint64_t interval_seconds = 3600;
  std::uint64_t idle_seconds = 300;
  std::size_t candidate_threshold = 1;
  std::optional<std::string> model;
  std::optional<std::string> verifier_model;
  std::int64_t timeout_ms = 600000;
  std::size_t max_model_input_tokens = 6000;
  std::size_t max_candidates = 8;
  std::string research_policy = "disabled";
  std::string promotion_policy = "review_only";
};

struct MemoryRetrievalSettings {
  bool graph_enabled = false;
  bool dense_enabled = false;
  bool code_enabled = false;
  std::optional<std::string> embedding_model;
  std::string query_prefix;
  std::string document_prefix;
  std::size_t max_depth = 2;
  std::size_t max_seeds = 16;
  std::size_t max_edges = 256;
  std::size_t max_graph_results = 64;
  std::size_t max_fanout = 32;
  std::size_t query_cache_entries = 64;
};

struct MemorySettings {
  bool enabled = false;
  bool automatic_recall = true;
  bool automatic_capture = true;
  std::string recall_policy = "balanced";
  std::size_t context_token_budget = 900;
  std::size_t candidate_limit = 50;
  MemoryPrehandlerSettings prehandler;
  MemoryCaptureSettings capture;
  MemoryDreamingSettings dreaming;
  MemoryRetrievalSettings retrieval;
};

struct MemoryPrehandlerSettingsPatch {
  std::optional<std::optional<std::string>> model;
  std::optional<std::string> thinking;
  std::optional<std::int64_t> timeout_ms;
  std::optional<bool> fail_open;
  std::optional<std::size_t> max_model_input_tokens;

  bool empty() const noexcept;
};

struct MemoryCaptureSettingsPatch {
  std::optional<std::string> post_process;
  std::optional<std::string> default_status;
  std::optional<std::size_t> max_evidence_chars;

  bool empty() const noexcept;
};

struct MemoryDreamingSettingsPatch {
  std::optional<bool> enabled;
  std::optional<std::string> trigger;
  std::optional<std::uint64_t> interval_seconds;
  std::optional<std::uint64_t> idle_seconds;
  std::optional<std::size_t> candidate_threshold;
  std::optional<std::optional<std::string>> model;
  std::optional<std::optional<std::string>> verifier_model;
  std::optional<std::int64_t> timeout_ms;
  std::optional<std::size_t> max_model_input_tokens;
  std::optional<std::size_t> max_candidates;
  std::optional<std::string> research_policy;
  std::optional<std::string> promotion_policy;

  bool empty() const noexcept;
};

struct MemoryRetrievalSettingsPatch {
  std::optional<bool> graph_enabled;
  std::optional<bool> dense_enabled;
  std::optional<bool> code_enabled;
  std::optional<std::optional<std::string>> embedding_model;
  std::optional<std::string> query_prefix;
  std::optional<std::string> document_prefix;
  std::optional<std::size_t> max_depth;
  std::optional<std::size_t> max_seeds;
  std::optional<std::size_t> max_edges;
  std::optional<std::size_t> max_graph_results;
  std::optional<std::size_t> max_fanout;
  std::optional<std::size_t> query_cache_entries;

  bool empty() const noexcept;
};

struct MemorySettingsPatch {
  std::optional<bool> enabled;
  std::optional<bool> automatic_recall;
  std::optional<bool> automatic_capture;
  std::optional<std::string> recall_policy;
  std::optional<std::size_t> context_token_budget;
  std::optional<std::size_t> candidate_limit;
  MemoryPrehandlerSettingsPatch prehandler;
  MemoryCaptureSettingsPatch capture;
  MemoryDreamingSettingsPatch dreaming;
  MemoryRetrievalSettingsPatch retrieval;

  bool empty() const noexcept;
};

struct McpServerSettings {
  std::string name;
  bool enabled = true;
  std::string transport = "stdio";
  std::optional<std::string> command;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  std::optional<std::string> url;
  std::map<std::string, std::string> headers;
  nlohmann::json sampling = nlohmann::json::object();
  int timeout_seconds = 120;
  int connect_timeout_seconds = 60;
};

struct AppSettings {
  std::string cwd;
  std::string workspace_root;
  std::string session_dir;
  std::string agent_home;
  std::optional<std::string> default_provider;
  std::optional<std::string> default_model;
  std::optional<std::string> default_thinking_level;
  std::string theme = "dark";
  bool quiet_startup = false;
  bool collapse_changelog = false;
  bool hide_thinking_block = false;
  bool enable_skill_commands = true;
  bool allow_external_filesystem_access = false;
  bool image_auto_resize = true;
  bool terminal_show_progress = false;
  std::size_t terminal_prompt_history_limit = kDefaultPromptHistoryLimit;
  std::vector<std::string> terminal_tool_entry_compaction_enabled_tools;
  bool retry_enabled = true;
  ProviderRetrySettings provider_retry;
  std::string steering_mode = "one-at-a-time";
  std::string follow_up_mode = "one-at-a-time";
  std::string transport = "sse";
  std::optional<std::string> shell_path;
  std::optional<std::string> shell_command_prefix;
  std::optional<std::vector<std::string>> npm_command;
  std::vector<std::string> enabled_models;
  std::vector<std::string> prompt_roots;
  std::vector<std::string> skill_roots;
  std::vector<std::string> theme_roots;
  ResourceFilterSettings resource_filters;
  std::vector<McpServerSettings> mcp_servers;
  std::vector<ai::OllamaInstanceSettings> ollama_instances;
  FeatureSettings features;
  MemorySettings memory;
  ai::LlamaSettings llama;
};

struct AppSettingsPatch {
  std::optional<std::string> cwd;
  std::optional<std::string> workspace_root;
  std::optional<std::string> session_dir;
  std::optional<std::string> agent_home;
  std::optional<std::string> default_provider;
  std::optional<std::string> default_model;
  std::optional<std::string> default_thinking_level;
  std::optional<std::string> theme;
  std::optional<bool> quiet_startup;
  std::optional<bool> collapse_changelog;
  std::optional<bool> hide_thinking_block;
  std::optional<bool> enable_skill_commands;
  std::optional<bool> allow_external_filesystem_access;
  std::optional<bool> image_auto_resize;
  std::optional<bool> terminal_show_progress;
  std::optional<std::size_t> terminal_prompt_history_limit;
  std::optional<std::vector<std::string>>
      terminal_tool_entry_compaction_enabled_tools;
  std::optional<bool> retry_enabled;
  ProviderRetrySettings provider_retry;
  std::optional<std::string> steering_mode;
  std::optional<std::string> follow_up_mode;
  std::optional<std::string> transport;
  std::optional<std::string> shell_path;
  std::optional<std::string> shell_command_prefix;
  std::optional<std::vector<std::string>> npm_command;
  std::optional<std::vector<std::string>> enabled_models;
  std::optional<std::vector<std::string>> prompt_roots;
  std::optional<std::vector<std::string>> skill_roots;
  std::optional<std::vector<std::string>> theme_roots;
  std::optional<std::vector<std::string>> prompt_template_filters;
  std::optional<std::vector<std::string>> skill_filters;
  std::optional<std::vector<std::string>> theme_filters;
  std::optional<std::vector<McpServerSettings>> mcp_servers;
  std::optional<std::vector<ai::OllamaInstanceSettings>> ollama_instances;
  FeatureSettingsPatch features;
  MemorySettingsPatch memory;
  std::optional<ai::LlamaSettings> llama;

  bool empty() const noexcept;
};

struct LoadedSettingsDocument {
  std::string path;
  AppSettingsPatch patch;
};

struct PersistedLlamaModelDiscovery {
  std::string settings_path;
  std::size_t added = 0;
  std::size_t skipped = 0;
  std::vector<std::string> diagnostics;
};

struct LoadSettingsRequest {
  std::string cwd;
  std::optional<std::string> agent_home;
  std::optional<std::string> workspace_root;
  std::optional<std::string> project_settings_path;
  std::optional<AppSettingsPatch> override_settings;
};

AppSettingsPatch merge_settings_patches(const AppSettingsPatch& base,
                                        const AppSettingsPatch& overlay);

foundation::Result<LoadedSettingsDocument> load_settings_document(
    const harness::RepositoryView& repository, const std::string& path);

foundation::Result<AppSettings> load_app_settings(
    const harness::RepositoryView& repository, const LoadSettingsRequest& request);

foundation::Result<PersistedLlamaModelDiscovery> persist_discovered_llama_models(
    const std::string& agent_home, const std::string& cwd,
    const ai::LlamaModelDiscoveryResult& discovery);

}  // namespace qt::app
