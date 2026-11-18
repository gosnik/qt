#pragma once

#include <qt/extensions/contributions.hpp>
#include <qt/extensions/angelscript_host.hpp>
#include <qt/foundation/error.hpp>
#include <qt/foundation/process.hpp>

#include <cstdint>
#include <memory>

namespace qt::app {

struct ExtensionContributionRunOptions {
  std::int64_t timeout_ms = 30'000;
  std::shared_ptr<qt::foundation::ProcessCancellationToken> cancellation_token;
  qt::extensions::ExtensionHostCall host_call;
};

qt::foundation::Result<qt::extensions::ExtensionContributionResult>
run_extension_contribution_subprocess(
    const qt::extensions::ExtensionContributionDescriptor& contribution,
    qt::extensions::Json arguments,
    ExtensionContributionRunOptions options = {});

}  // namespace qt::app
