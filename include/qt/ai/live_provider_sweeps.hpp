#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "qt/ai/auth_storage.hpp"
#include "qt/ai/model_catalog.hpp"

namespace qt::ai {

struct LiveProviderSweepOptions {
  std::string enable_env_var = "QT_ENABLE_LIVE_PROVIDER_SWEEPS";
  std::optional<std::string> provider_filter;
  std::size_t max_cases_per_provider = 1;
};

struct LiveProviderSweepCase {
  std::string provider;
  std::string model;
  std::string api;
  std::string credential_source;
  Json metadata = Json::object();
};

struct LiveProviderSweepPlan {
  bool enabled = false;
  std::string enable_env_var;
  std::vector<LiveProviderSweepCase> cases;
  std::vector<std::string> skipped;
};

bool live_provider_sweeps_enabled(const std::string& env_var);
LiveProviderSweepPlan plan_live_provider_sweeps(
    const ModelCatalog& catalog, const AuthStorage& storage = {},
    const LiveProviderSweepOptions& options = {});

}  // namespace qt::ai
