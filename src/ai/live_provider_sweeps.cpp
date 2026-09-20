#include "qt/ai/live_provider_sweeps.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

namespace qt::ai {
namespace {

std::string trim_copy(const std::string& value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  });
  const auto last =
      std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
      }).base();
  if (first >= last) {
    return "";
  }
  return std::string(first, last);
}

std::string lowercase_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string provider_key(const std::string& provider) {
  return lowercase_copy(trim_copy(provider));
}

std::optional<std::string> getenv_nonempty(const std::string& name) {
  const char* value = std::getenv(name.c_str());
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
}

bool env_truthy(const std::string& value) {
  const std::string key = lowercase_copy(trim_copy(value));
  return key == "1" || key == "true" || key == "yes" || key == "on";
}

std::optional<std::string> credential_source_for_provider(
    const AuthStorage& storage, const std::string& provider) {
  if (const auto* record = find_provider_auth_record(storage, provider);
      record != nullptr) {
    if (record->api_key) {
      return "auth.json api key";
    }
    if (record->token) {
      return "auth.json token";
    }
  }

  for (const auto& name : auth_env_var_names_for_provider(provider)) {
    if (getenv_nonempty(name)) {
      return name;
    }
  }
  return std::nullopt;
}

}  // namespace

bool live_provider_sweeps_enabled(const std::string& env_var) {
  if (trim_copy(env_var).empty()) {
    return false;
  }
  const auto value = getenv_nonempty(env_var);
  return value && env_truthy(*value);
}

LiveProviderSweepPlan plan_live_provider_sweeps(
    const ModelCatalog& catalog, const AuthStorage& storage,
    const LiveProviderSweepOptions& options) {
  LiveProviderSweepPlan plan;
  plan.enable_env_var = options.enable_env_var;
  plan.enabled = live_provider_sweeps_enabled(options.enable_env_var);
  if (!plan.enabled) {
    plan.skipped.push_back("live provider sweeps disabled; set " +
                           options.enable_env_var + "=1 to enable");
    return plan;
  }

  const std::string filter_text = options.provider_filter.value_or("");
  const std::string filter = provider_key(filter_text);
  std::map<std::string, std::size_t> cases_by_provider;
  for (const auto& model : catalog.models) {
    const std::string provider = trim_copy(model.ref.provider);
    const std::string key = provider_key(provider);
    if (key.empty()) {
      continue;
    }
    if (!filter.empty() && key != filter) {
      continue;
    }
    if (cases_by_provider[key] >= options.max_cases_per_provider) {
      continue;
    }

    const auto credential_source =
        credential_source_for_provider(storage, provider);
    if (!credential_source) {
      if (cases_by_provider[key] == 0U) {
        plan.skipped.push_back("missing credentials for provider " + provider);
      }
      continue;
    }

    LiveProviderSweepCase test_case;
    test_case.provider = provider;
    test_case.model = model.ref.name;
    test_case.api = model.ref.api;
    test_case.credential_source = *credential_source;
    test_case.metadata = model.metadata;
    plan.cases.push_back(std::move(test_case));
    ++cases_by_provider[key];
  }

  if (!filter.empty() && plan.cases.empty()) {
    plan.skipped.push_back("no enabled live provider sweep cases for " +
                           filter_text);
  }
  return plan;
}

}  // namespace qt::ai
