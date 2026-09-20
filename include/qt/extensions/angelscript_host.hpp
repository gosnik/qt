#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <qt/foundation/error.hpp>

namespace qt::extensions {

using Json = nlohmann::json;

enum class AngelScriptDiagnosticSeverity {
  kInfo,
  kWarning,
  kError,
};

struct AngelScriptDependencyInfo {
  bool available = false;
  bool linked = false;
  std::string name = "AngelScript";
  std::optional<std::string> version;
  std::string source = "not configured";
  std::vector<std::string> diagnostics;
};

struct AngelScriptDiagnostic {
  AngelScriptDiagnosticSeverity severity =
      AngelScriptDiagnosticSeverity::kInfo;
  std::string section;
  std::size_t row = 0;
  std::size_t column = 0;
  std::string message;
};

struct AngelScriptCommandRegistration {
  std::string name;
  std::optional<std::string> title;
  std::optional<std::string> description;
};

struct AngelScriptToolRegistration {
  std::string name;
  std::optional<std::string> title;
  std::optional<std::string> description;
};

struct AngelScriptResourceRegistration {
  std::string type;
  std::string path;
  std::optional<std::string> name;
};

struct AngelScriptLifecycleRegistration {
  std::string event;
  std::string handler;
};

struct AngelScriptProviderRegistration {
  std::string name;
  Json config = Json::object();
};

struct AngelScriptFlagRegistration {
  std::string name;
  std::string type = "boolean";
  Json default_value = Json(nullptr);
  std::optional<std::string> description;
};

struct AngelScriptShortcutRegistration {
  std::string key;
  std::string handler;
  std::optional<std::string> description;
};

struct AngelScriptMessageRendererRegistration {
  std::string custom_type;
  std::string handler;
};

struct AngelScriptPluginRegistration {
  std::string manifest_name;
  std::optional<std::string> display_name;
  std::vector<AngelScriptCommandRegistration> commands;
  std::vector<AngelScriptToolRegistration> tools;
  std::vector<AngelScriptResourceRegistration> resources;
  std::vector<AngelScriptLifecycleRegistration> lifecycle_handlers;
  std::vector<AngelScriptProviderRegistration> providers;
  std::vector<AngelScriptFlagRegistration> flags;
  std::vector<AngelScriptShortcutRegistration> shortcuts;
  std::vector<AngelScriptMessageRendererRegistration> message_renderers;
  Json metadata = Json::object();
};

struct AngelScriptMethodBinding {
  std::string declaration;
  std::string native_owner;
};

struct AngelScriptClassBinding {
  std::string name;
  std::string native_type;
  std::string description;
  std::vector<AngelScriptMethodBinding> methods;
};

struct AngelScriptBindingCatalog {
  std::vector<AngelScriptClassBinding> classes;
};

struct AngelScriptSandboxPolicy {
  std::size_t max_source_bytes = 256U * 1024U;
  std::size_t max_activation_steps = 100000U;
  bool allow_module_imports = false;
};

struct AngelScriptCompileRequest {
  std::string module_name;
  std::string script_name = "<memory>";
  std::string source;
  AngelScriptPluginRegistration seed_registration;
  AngelScriptSandboxPolicy sandbox;
};

struct AngelScriptCompileResult {
  AngelScriptDependencyInfo dependency;
  bool ok = false;
  AngelScriptPluginRegistration registration;
  std::vector<AngelScriptDiagnostic> diagnostics;

  bool has_errors() const noexcept;
};

std::string to_string(AngelScriptDiagnosticSeverity severity);

AngelScriptDependencyInfo detect_angelscript_dependency();

struct ExtensionManifest;

AngelScriptPluginRegistration
adapt_manifest_to_angelscript_registration(const ExtensionManifest& manifest);

const AngelScriptBindingCatalog& angelscript_binding_catalog();

class AngelScriptHost {
 public:
  AngelScriptHost();
  explicit AngelScriptHost(AngelScriptDependencyInfo dependency);

  const AngelScriptDependencyInfo& dependency() const noexcept;

  qt::foundation::Result<AngelScriptCompileResult> compile(
      const AngelScriptCompileRequest& request) const;

 private:
  AngelScriptDependencyInfo dependency_;
};

using ExtensionHostCall = std::function<qt::foundation::Result<Json>(
    std::string_view method, const Json& arguments)>;

class AngelScriptRuntime {
 public:
  AngelScriptRuntime();
  ~AngelScriptRuntime();
  AngelScriptRuntime(AngelScriptRuntime&&) noexcept;
  AngelScriptRuntime& operator=(AngelScriptRuntime&&) noexcept;
  AngelScriptRuntime(const AngelScriptRuntime&) = delete;
  AngelScriptRuntime& operator=(const AngelScriptRuntime&) = delete;

  static qt::foundation::Result<AngelScriptRuntime> create(
      const AngelScriptCompileRequest& request);
  const AngelScriptPluginRegistration& registration() const;
  qt::foundation::Result<Json> invoke(std::string_view handler,
                                      const Json& arguments,
                                      const ExtensionHostCall& host_call);

 private:
  struct Impl;
  explicit AngelScriptRuntime(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace qt::extensions
