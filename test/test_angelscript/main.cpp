#include <angelscript.h>
#include <ggml.h>
#include <llama.h>
#include <scriptstdstring.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <utility>
#include <vector>

namespace {

struct ScriptDiagnostic {
  std::string section;
  int row = 0;
  int column = 0;
  std::string severity;
  std::string message;
};

struct TestCase {
  std::filesystem::path script_path;
  std::string module_name;
  std::string name;
  std::string function_name;
};

struct TestFailure {
  std::string message;
};

struct TestRunState {
  std::vector<TestCase> tests;
  std::vector<TestFailure> current_failures;
  std::shared_ptr<spdlog::logger> log;
  std::filesystem::path project_root;
};

TestRunState* active_state = nullptr;

std::string env_or_empty(const char* name) {
  if (const char* value = std::getenv(name)) {
    return std::string(value);
  }
  return {};
}

std::string shell_quote(std::string_view value) {
  std::string quoted = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

std::string truncate_for_failure(std::string text, std::size_t max_size) {
  if (text.size() <= max_size) {
    return text;
  }
  return text.substr(text.size() - max_size);
}

struct CommandResult {
  int exit_code = -1;
  std::string output;
};

CommandResult run_shell_command(const std::string& command) {
  CommandResult result;
  std::array<char, 4096> buffer{};
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    result.output = "failed to start command: " + command;
    return result;
  }
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) !=
         nullptr) {
    result.output += buffer.data();
  }
  const int status = pclose(pipe);
  if (status == -1) {
    result.exit_code = -1;
  } else if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }
  return result;
}

std::string escape_xml(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      case '\'':
        escaped += "&apos;";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::optional<std::filesystem::path> find_project_root() {
  std::filesystem::path current = std::filesystem::current_path();
  while (true) {
    if (std::filesystem::exists(current / "platformio.ini") &&
        std::filesystem::is_directory(current / "test")) {
      return current;
    }
    if (!current.has_parent_path() || current == current.parent_path()) {
      return std::nullopt;
    }
    current = current.parent_path();
  }
}

std::vector<std::filesystem::path> discover_scripts(
    const std::filesystem::path& root) {
  std::vector<std::filesystem::path> scripts;
  const auto scripts_root = root / "test";
  if (!std::filesystem::is_directory(scripts_root)) {
    return scripts;
  }
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(scripts_root)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".as") {
      continue;
    }
    scripts.push_back(entry.path());
  }
  std::sort(scripts.begin(), scripts.end());
  return scripts;
}

std::shared_ptr<spdlog::logger> make_logger(
    const std::filesystem::path& project_root) {
  std::vector<spdlog::sink_ptr> sinks;
  std::filesystem::path log_path = env_or_empty("QT_TEST_LOG_FILE");
  if (log_path.empty()) {
    log_path = project_root / ".pio" / "test" / "angelscript-tests.log";
  }
  if (!log_path.parent_path().empty()) {
    std::filesystem::create_directories(log_path.parent_path());
  }
  sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
      log_path.string(), true));
  if (env_or_empty("QT_TEST_LOG_STDERR") == "1") {
    sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
  }

  auto logger =
      std::make_shared<spdlog::logger>("qt-tests", sinks.begin(), sinks.end());
  logger->set_pattern("[%Y-%m-%dT%H:%M:%S.%e%z] [%l] %v");
  logger->set_level(spdlog::level::debug);
  logger->flush_on(spdlog::level::warn);
  spdlog::set_default_logger(logger);
  return logger;
}

void message_callback(const asSMessageInfo* message, void* parameter) {
  auto* diagnostics = static_cast<std::vector<ScriptDiagnostic>*>(parameter);
  if (message == nullptr || diagnostics == nullptr) {
    return;
  }
  ScriptDiagnostic diagnostic;
  diagnostic.section = message->section != nullptr ? message->section : "";
  diagnostic.row = message->row;
  diagnostic.column = message->col;
  switch (message->type) {
    case asMSGTYPE_WARNING:
      diagnostic.severity = "warning";
      break;
    case asMSGTYPE_INFORMATION:
      diagnostic.severity = "info";
      break;
    case asMSGTYPE_ERROR:
    default:
      diagnostic.severity = "error";
      break;
  }
  diagnostic.message = message->message != nullptr ? message->message : "";
  diagnostics->push_back(std::move(diagnostic));
}

void qt_test(const std::string& name, const std::string& function_name) {
  if (active_state == nullptr) {
    return;
  }
  TestCase test;
  test.name = name;
  test.function_name = function_name;
  active_state->tests.push_back(std::move(test));
}

void qt_log(const std::string& message) {
  if (active_state != nullptr && active_state->log) {
    active_state->log->info("script: {}", message);
  }
}

void record_failure(std::string message) {
  if (active_state == nullptr) {
    return;
  }
  active_state->current_failures.push_back(TestFailure{std::move(message)});
}

void qt_assert(bool condition, const std::string& message) {
  if (!condition) {
    record_failure(message.empty() ? "assertion failed" : message);
  }
}

void qt_assert_eq(const std::string& actual, const std::string& expected,
                  const std::string& message) {
  if (actual != expected) {
    record_failure(message + " expected=\"" + expected + "\" actual=\"" +
                   actual + "\"");
  }
}

void qt_assert_eq_i(asINT64 actual, asINT64 expected,
                    const std::string& message) {
  if (actual != expected) {
    record_failure(message + " expected=" + std::to_string(expected) +
                   " actual=" + std::to_string(actual));
  }
}

bool qt_run_tui_pty_probe() {
  if (active_state == nullptr) {
    return false;
  }
  if (env_or_empty("QT_TEST_SKIP_TUI_PTY_PROBE") == "1") {
    if (active_state->log) {
      active_state->log->warn("skipping TUI PTY probe by request");
    }
    return true;
  }
  const std::filesystem::path root = active_state->project_root;
  const std::string probe_args = env_or_empty("QT_TUI_PTY_PROBE_ARGS");
  const std::filesystem::path dump_path =
      root / ".pio" / "test" / "tui-pty-probe.bin";
  const std::string command =
      "cd " + shell_quote(root.string()) +
      " && pio run -e native" +
      " && python3 scripts/tui_pty_probe.py --binary .pio/build/native/qt --dump " +
      shell_quote(dump_path.string()) +
      (probe_args.empty() ? std::string{} : " " + probe_args) + " 2>&1";
  if (active_state->log) {
    active_state->log->info("running TUI PTY probe: {}", command);
  }
  const auto result = run_shell_command(command);
  if (active_state->log) {
    active_state->log->info("TUI PTY probe exited with {}", result.exit_code);
    active_state->log->debug("TUI PTY probe output:\n{}", result.output);
  }
  if (result.exit_code == 0) {
    return true;
  }
  record_failure("TUI PTY/tmux probe failed with exit code " +
                 std::to_string(result.exit_code) + "\n" +
                 truncate_for_failure(result.output, 6000U));
  return false;
}

bool qt_run_mcp_client_probe() {
  if (active_state == nullptr) {
    return false;
  }
  const std::filesystem::path root = active_state->project_root;
  const std::string command =
      "cd " + shell_quote(root.string()) +
      " && python3 scripts/mcp_client_probe.py 2>&1";
  if (active_state->log) {
    active_state->log->info("running MCP client probe: {}", command);
  }
  const auto result = run_shell_command(command);
  if (active_state->log) {
    active_state->log->info("MCP client probe exited with {}",
                            result.exit_code);
    active_state->log->debug("MCP client probe output:\n{}", result.output);
  }
  if (result.exit_code == 0) {
    return true;
  }
  record_failure("MCP client probe failed with exit code " +
                 std::to_string(result.exit_code) + "\n" +
                 truncate_for_failure(result.output, 6000U));
  return false;
}

bool qt_run_prompt_history_probe() {
  if (active_state == nullptr) {
    return false;
  }
  const std::filesystem::path root = active_state->project_root;
  const std::string command =
      "cd " + shell_quote(root.string()) +
      " && pio run -e native" +
      " && scripts/prompt_history_probe.py --binary .pio/build/native/qt" +
      " && scripts/memory_settings_probe.py --binary .pio/build/native/qt" +
      " && scripts/memory_dashboard_probe.py --binary .pio/build/native/qt" +
      " && scripts/tool_call_pipeline_probe.py --binary .pio/build/native/qt" +
      " && scripts/filesystem_access_probe.py --binary .pio/build/native/qt 2>&1";
  if (active_state->log) {
    active_state->log->info("running prompt history probe: {}", command);
  }
  const auto result = run_shell_command(command);
  if (active_state->log) {
    active_state->log->info("prompt history probe exited with {}",
                            result.exit_code);
    active_state->log->debug("prompt history probe output:\n{}",
                             result.output);
  }
  if (result.exit_code == 0) {
    return true;
  }
  record_failure("prompt history probe failed with exit code " +
                 std::to_string(result.exit_code) + "\n" +
                 truncate_for_failure(result.output, 6000U));
  return false;
}

bool qt_run_subagent_probe() {
  if (active_state == nullptr) {
    return false;
  }
  const auto root = find_project_root();
  if (!root) {
    if (active_state->log) {
      active_state->log->error("unable to find project root for subagent probe");
    }
    return false;
  }
  const std::string command =
      "cd " + shell_quote(root->string()) + " && scripts/subagent_probe.py 2>&1";
  if (active_state->log) {
    active_state->log->info("running subagent probe: {}", command);
  }
  const auto result = run_shell_command(command);
  if (active_state->log && !result.output.empty()) {
    active_state->log->info("subagent probe output:\n{}", result.output);
  }
  if (active_state->log && result.exit_code != 0) {
    active_state->log->error("subagent probe exited with {}", result.exit_code);
  }
  return result.exit_code == 0;
}

bool qt_run_web_run_probe() {
  if (active_state == nullptr) {
    return false;
  }
  const auto root = find_project_root();
  if (!root) {
    if (active_state->log) {
      active_state->log->error("unable to find project root for web_run probe");
    }
    return false;
  }
  const std::string command =
      "cd " + shell_quote(root->string()) + " && scripts/web_run_probe.py 2>&1";
  if (active_state->log) {
    active_state->log->info("running web_run probe: {}", command);
  }
  const auto result = run_shell_command(command);
  if (active_state->log && !result.output.empty()) {
    active_state->log->info("web_run probe output:\n{}", result.output);
  }
  if (active_state->log && result.exit_code != 0) {
    active_state->log->error("web_run probe exited with {}", result.exit_code);
  }
  return result.exit_code == 0;
}

bool qt_run_memory_probe() {
  if (active_state == nullptr) {
    return false;
  }
  const auto root = find_project_root();
  if (!root) {
    if (active_state->log) {
      active_state->log->error("unable to find project root for memory probe");
    }
    return false;
  }
  const std::string command =
      "cd " + shell_quote(root->string()) +
      " && pio run -e native" +
      " && scripts/openrouter_limits_probe.py --binary .pio/build/native/qt" +
      " && scripts/model_resolution_probe.py" +
      " && scripts/llama_offload_probe.py" +
      " && scripts/memory_model_routing_probe.py" +
      " && scripts/dreaming_probe.py" +
      " && scripts/memory_probe.py 2>&1";
  if (active_state->log) {
    active_state->log->info("running built-in memory probe: {}", command);
  }
  const auto result = run_shell_command(command);
  if (active_state->log && !result.output.empty()) {
    active_state->log->info("memory probe output:\n{}", result.output);
  }
  if (active_state->log && result.exit_code != 0) {
    active_state->log->error("memory probe exited with {}", result.exit_code);
  }
  return result.exit_code == 0;
}

bool qt_run_memory_review_probe() {
  if (active_state == nullptr) {
    return false;
  }
  const auto root = find_project_root();
  if (!root) {
    if (active_state->log) {
      active_state->log->error(
          "unable to find project root for memory review probe");
    }
    return false;
  }
  const std::string command =
      "cd " + shell_quote(root->string()) +
      " && scripts/memory_review_probe.py --require-target-contract 2>&1";
  if (active_state->log) {
    active_state->log->info("running governed memory review probe: {}", command);
  }
  const auto result = run_shell_command(command);
  if (active_state->log && !result.output.empty()) {
    active_state->log->info("memory review probe output:\n{}", result.output);
  }
  if (active_state->log && result.exit_code != 0) {
    active_state->log->error("memory review probe exited with {}",
                             result.exit_code);
  }
  return result.exit_code == 0;
}

bool qt_prism_ternary_format_supported() {
  return GGML_TYPE_Q2_0 == 42 && LLAMA_FTYPE_MOSTLY_Q2_0 == 41 &&
         ggml_blck_size(GGML_TYPE_Q2_0) == 128 &&
         ggml_type_size(GGML_TYPE_Q2_0) == 34 &&
         GGML_TYPE_Q2_0 != GGML_TYPE_TQ2_0;
}

bool register_test_api(asIScriptEngine* engine) {
  RegisterStdString(engine);
  return engine->RegisterGlobalFunction(
             "void qt_test(const string &in, const string &in)",
             asFUNCTION(qt_test), asCALL_CDECL) >= 0 &&
         engine->RegisterGlobalFunction("void qt_log(const string &in)",
                                        asFUNCTION(qt_log), asCALL_CDECL) >= 0 &&
         engine->RegisterGlobalFunction(
             "void qt_assert(bool, const string &in)", asFUNCTION(qt_assert),
             asCALL_CDECL) >= 0 &&
         engine->RegisterGlobalFunction(
             "void qt_assert_eq(const string &in, const string &in, const string &in)",
             asFUNCTION(qt_assert_eq), asCALL_CDECL) >= 0 &&
         engine->RegisterGlobalFunction(
             "void qt_assert_eq_i(int64, int64, const string &in)",
             asFUNCTION(qt_assert_eq_i), asCALL_CDECL) >= 0 &&
         engine->RegisterGlobalFunction("bool qt_run_tui_pty_probe()",
                                        asFUNCTION(qt_run_tui_pty_probe),
                                        asCALL_CDECL) >= 0 &&
         engine->RegisterGlobalFunction("bool qt_run_mcp_client_probe()",
                                        asFUNCTION(qt_run_mcp_client_probe),
                                        asCALL_CDECL) >= 0 &&
         engine->RegisterGlobalFunction("bool qt_run_prompt_history_probe()",
                                        asFUNCTION(qt_run_prompt_history_probe),
                                        asCALL_CDECL) >= 0 &&
         engine->RegisterGlobalFunction("bool qt_run_subagent_probe()",
                                        asFUNCTION(qt_run_subagent_probe),
                                        asCALL_CDECL) >= 0 &&
         engine->RegisterGlobalFunction("bool qt_run_web_run_probe()",
                                        asFUNCTION(qt_run_web_run_probe),
                                        asCALL_CDECL) >= 0 &&
         engine->RegisterGlobalFunction("bool qt_run_memory_probe()",
                                        asFUNCTION(qt_run_memory_probe),
                                        asCALL_CDECL) >= 0 &&
         engine->RegisterGlobalFunction("bool qt_run_memory_review_probe()",
                                        asFUNCTION(qt_run_memory_review_probe),
                                        asCALL_CDECL) >= 0 &&
         engine->RegisterGlobalFunction(
             "bool qt_prism_ternary_format_supported()",
             asFUNCTION(qt_prism_ternary_format_supported),
             asCALL_CDECL) >= 0;
}

bool execute_function(asIScriptEngine* engine, asIScriptFunction* function,
                      std::vector<TestFailure>& failures) {
  asIScriptContext* context = engine->CreateContext();
  if (context == nullptr) {
    failures.push_back({"failed to create AngelScript context"});
    return false;
  }
  if (context->Prepare(function) < 0) {
    failures.push_back({"failed to prepare AngelScript function"});
    context->Release();
    return false;
  }
  const int status = context->Execute();
  if (status != asEXECUTION_FINISHED) {
    std::string message = "AngelScript execution failed";
    if (status == asEXECUTION_EXCEPTION) {
      message = context->GetExceptionString();
      if (const auto* exception_function = context->GetExceptionFunction()) {
        message += " in ";
        message += exception_function->GetDeclaration();
      }
      message += " at line ";
      message += std::to_string(context->GetExceptionLineNumber());
    }
    failures.push_back({message});
    context->Release();
    return false;
  }
  context->Release();
  return true;
}

struct ModuleLoadResult {
  std::unique_ptr<asIScriptEngine, void (*)(asIScriptEngine*)> engine{
      nullptr, [](asIScriptEngine* value) {
        if (value != nullptr) {
          value->ShutDownAndRelease();
        }
      }};
  std::vector<ScriptDiagnostic> diagnostics;
  std::vector<TestFailure> failures;
};

ModuleLoadResult load_script_module(const std::filesystem::path& path,
                                    TestRunState& state) {
  ModuleLoadResult result;
  result.engine.reset(asCreateScriptEngine());
  if (!result.engine) {
    result.failures.push_back({"failed to create AngelScript engine"});
    return result;
  }
  result.engine->SetMessageCallback(asFUNCTION(message_callback),
                                    &result.diagnostics, asCALL_CDECL);
  if (!register_test_api(result.engine.get())) {
    result.failures.push_back({"failed to register AngelScript test API"});
    return result;
  }

  const std::string source = read_text_file(path);
  asIScriptModule* module = result.engine->GetModule(
      path.filename().string().c_str(), asGM_ALWAYS_CREATE);
  if (module == nullptr) {
    result.failures.push_back({"failed to create AngelScript module"});
    return result;
  }
  if (module->AddScriptSection(path.string().c_str(), source.c_str(),
                               source.size()) < 0) {
    result.failures.push_back({"failed to add AngelScript source section"});
    return result;
  }
  if (module->Build() < 0) {
    result.failures.push_back({"failed to build AngelScript module"});
    return result;
  }
  auto* register_tests = module->GetFunctionByDecl("void register_tests()");
  if (register_tests == nullptr) {
    result.failures.push_back(
        {"missing required AngelScript function: void register_tests()"});
    return result;
  }

  const std::size_t before = state.tests.size();
  std::vector<TestFailure> registration_failures;
  (void)execute_function(result.engine.get(), register_tests,
                         registration_failures);
  for (auto& failure : registration_failures) {
    result.failures.push_back(std::move(failure));
  }
  for (std::size_t index = before; index < state.tests.size(); ++index) {
    state.tests[index].script_path = path;
    state.tests[index].module_name = path.filename().string();
  }
  return result;
}

std::string test_declaration(const std::string& function_name) {
  return "void " + function_name + "()";
}

bool matches_filter(const TestCase& test, std::string_view filter) {
  if (filter.empty()) {
    return true;
  }
  return test.name.find(filter) != std::string::npos ||
         test.function_name.find(filter) != std::string::npos ||
         test.script_path.generic_string().find(filter) != std::string::npos;
}

}  // namespace

int main() {
  const auto project_root = find_project_root();
  if (!project_root) {
    std::cerr << "qt AngelScript tests: could not locate project root\n";
    return 1;
  }

  TestRunState state;
  state.project_root = *project_root;
  state.log = make_logger(*project_root);
  active_state = &state;
  state.log->info("AngelScript test run started");

  const auto scripts = discover_scripts(*project_root);
  if (scripts.empty()) {
    std::cerr << "qt AngelScript tests: no .as scripts found under test/\n";
    return 1;
  }

  struct LoadedModule {
    std::filesystem::path script_path;
    ModuleLoadResult module;
  };
  std::vector<LoadedModule> modules;
  std::size_t load_failures = 0;
  for (const auto& script : scripts) {
    state.log->info("loading {}", script.generic_string());
    auto loaded = load_script_module(script, state);
    for (const auto& diagnostic : loaded.diagnostics) {
      state.log->warn("{}:{}:{} {}: {}", diagnostic.section, diagnostic.row,
                      diagnostic.column, diagnostic.severity,
                      diagnostic.message);
    }
    for (const auto& failure : loaded.failures) {
      ++load_failures;
      std::cerr << script.generic_string() << ": " << failure.message << '\n';
      state.log->error("{}: {}", script.generic_string(), failure.message);
    }
    modules.push_back(LoadedModule{script, std::move(loaded)});
  }

  const std::string filter = env_or_empty("QT_AS_TEST_FILTER");
  std::size_t passed = 0;
  std::size_t failed = load_failures;
  std::size_t skipped = 0;
  std::vector<std::pair<TestCase, std::vector<TestFailure>>> failures;

  for (const auto& test : state.tests) {
    if (!matches_filter(test, filter)) {
      ++skipped;
      continue;
    }

    auto module_it = std::find_if(
        modules.begin(), modules.end(), [&test](const LoadedModule& module) {
          return module.script_path == test.script_path;
        });
    if (module_it == modules.end() || !module_it->module.engine) {
      failures.push_back({test, {{"test module was not loaded"}}});
      ++failed;
      continue;
    }
    asIScriptModule* module =
        module_it->module.engine->GetModule(test.module_name.c_str());
    asIScriptFunction* function =
        module != nullptr
            ? module->GetFunctionByDecl(test_declaration(test.function_name)
                                            .c_str())
            : nullptr;
    state.current_failures.clear();
    if (function == nullptr) {
      state.current_failures.push_back(
          {"registered test function was not found: " + test.function_name});
    } else {
      state.log->info("running {}", test.name);
      (void)execute_function(module_it->module.engine.get(), function,
                             state.current_failures);
    }

    if (state.current_failures.empty()) {
      ++passed;
      std::cout << "[PASS] " << test.name << '\n';
      state.log->info("passed {}", test.name);
    } else {
      ++failed;
      std::cout << "[FAIL] " << test.name << '\n';
      for (const auto& failure : state.current_failures) {
        std::cout << "  " << failure.message << '\n';
        state.log->error("{}: {}", test.name, failure.message);
      }
      failures.push_back({test, state.current_failures});
    }
  }

  const auto report_dir = *project_root / ".pio" / "test";
  std::filesystem::create_directories(report_dir);
  std::ofstream report(report_dir / "angelscript-results.xml");
  report << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  report << "<testsuite name=\"angelscript\" tests=\""
         << (passed + failed + skipped) << "\" failures=\"" << failed
         << "\" skipped=\"" << skipped << "\">\n";
  for (const auto& test : state.tests) {
    report << "  <testcase classname=\""
           << escape_xml(test.script_path.generic_string()) << "\" name=\""
           << escape_xml(test.name) << "\">";
    const auto failure_it = std::find_if(
        failures.begin(), failures.end(), [&test](const auto& entry) {
          return entry.first.script_path == test.script_path &&
                 entry.first.function_name == test.function_name &&
                 entry.first.name == test.name;
        });
    if (failure_it != failures.end()) {
      report << "\n";
      for (const auto& failure : failure_it->second) {
        report << "    <failure message=\""
               << escape_xml(failure.message) << "\" />\n";
      }
      report << "  </testcase>\n";
    } else {
      report << "</testcase>\n";
    }
  }
  report << "</testsuite>\n";

  std::cout << "AngelScript tests: passed=" << passed << " failed=" << failed
            << " skipped=" << skipped << '\n';
  state.log->info("AngelScript test run finished: passed={} failed={} skipped={}",
                  passed, failed, skipped);
  spdlog::shutdown();
  return failed == 0 ? 0 : 1;
}
