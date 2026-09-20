#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "qt/ai/model_catalog.hpp"
#include "qt/ai/provider_transport.hpp"
#include "qt/foundation/error.hpp"

namespace qt::ai {

inline constexpr std::int64_t kDefaultOllamaTimeoutMs = 120'000;
inline constexpr std::int64_t kDefaultOllamaContextWindow = 131'072;
inline constexpr std::int64_t kDefaultOllamaMaxTokens = 16'384;

struct OllamaModelInfo {
  std::string name;
  std::string modified_at;
  std::string digest;
  std::uint64_t size = 0;
  std::vector<std::string> capabilities;
  std::int64_t context_window = kDefaultOllamaContextWindow;
  std::int64_t max_tokens = kDefaultOllamaMaxTokens;
  Json details = Json::object();
};

struct OllamaInstanceSettings {
  std::string id;
  std::string name;
  std::string base_url = "http://localhost:11434";
  std::optional<std::string> api_key;
  bool enabled = true;
  std::int64_t timeout_ms = kDefaultOllamaTimeoutMs;
  std::vector<OllamaModelInfo> models;
};

std::string normalize_ollama_base_url(std::string value);
std::string ollama_provider_id(const std::string& instance_id);
bool is_ollama_provider_id(const std::string& provider_id) noexcept;

foundation::Result<Json> ollama_request(
    const OllamaInstanceSettings& instance, const std::string& method,
    const std::string& path, const Json& body = Json(),
    HttpTransport* transport = nullptr);

foundation::Result<std::vector<OllamaModelInfo>> discover_ollama_models(
    const OllamaInstanceSettings& instance, HttpTransport* transport = nullptr);

ModelCatalog model_catalog_from_ollama_settings(
    const std::vector<OllamaInstanceSettings>& instances);

}  // namespace qt::ai
