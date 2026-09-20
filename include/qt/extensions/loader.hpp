#pragma once

#include <string>
#include <memory>
#include <vector>

#include <qt/extensions/manifest.hpp>
#include <qt/extensions/angelscript_host.hpp>
#include <qt/harness/repository.hpp>

namespace qt::extensions {

class AngelScriptRuntime;

inline constexpr const char* kExtensionManifestFileName = "extension.json";

struct LoadedExtensionRecord {
  std::string root;
  std::string directory;
  std::string path;
  ExtensionManifest manifest;
  std::shared_ptr<AngelScriptRuntime> script_runtime;
  std::shared_ptr<ExtensionHostCall> host_call =
      std::make_shared<ExtensionHostCall>();
};

struct ExtensionLoadResult {
  std::vector<std::string> roots;
  std::vector<LoadedExtensionRecord> loaded_extensions;
  std::vector<std::string> diagnostics;

  bool has_loaded_extensions() const noexcept {
    return !loaded_extensions.empty();
  }

  void add_diagnostic(std::string message) {
    if (!message.empty()) {
      diagnostics.push_back(std::move(message));
    }
  }
};

struct ExtensionLoadOptions {
  std::vector<std::string> filter_patterns;
};

ExtensionLoadResult load_extension_manifests(
    const qt::harness::RepositoryView& repository,
    const std::vector<std::string>& extension_roots);

ExtensionLoadResult load_extension_manifests(
    const qt::harness::RepositoryView& repository,
    const std::vector<std::string>& extension_roots,
    const ExtensionLoadOptions& options);

}  // namespace qt::extensions
