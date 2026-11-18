#pragma once

#include <cstdint>
#include <optional>
#include <qt/ai/model_catalog.hpp>
#include <qt/ai/models.hpp>
#include <qt/harness/session_manager.hpp>
#include <string>
#include <vector>

namespace qt::app {

struct ReferenceRpcForkMessage {
  std::string entry_id;
  std::string text;
};

std::optional<std::int64_t> rpc_context_window(
    const ai::ModelRef& model);
std::optional<std::int64_t> rpc_context_window(
    const ai::ModelCatalog& catalog, const ai::ModelRef& model);
std::vector<ReferenceRpcForkMessage> session_fork_messages(
    const harness::SessionManager& session_manager);

}  // namespace qt::app
