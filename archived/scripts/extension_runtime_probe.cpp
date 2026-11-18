#include "qt/extensions/angelscript_host.hpp"

#include <iostream>
#include <functional>
#include <fstream>
#include <iterator>
#include <stdexcept>

using qt::extensions::AngelScriptCompileRequest;
using qt::extensions::AngelScriptRuntime;
using qt::extensions::Json;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

AngelScriptCompileRequest request(std::string source) {
  AngelScriptCompileRequest result;
  result.module_name = "runtime_probe";
  result.script_name = "runtime_probe.as";
  result.source = std::move(source);
  result.sandbox.max_activation_steps = 1000;
  return result;
}

int main() {
  auto created = AngelScriptRuntime::create(request(
      "void activate() { qt_register_command(\"state\"); "
      "qt_register_message_renderer(\"custom.note\", \"render_note\"); }\n"
      "string command_state(const string &in args) { "
      "return qt_host_call(\"echo\", args); }\n"
      "void command_void(const string &in args) { "
      "qt_host_call(\"remember\", args); }\n"
      "string command_nested(const string &in args) { "
      "return qt_host_call(\"nested\", args); }\n"
      "string command_recurse(const string &in args) { "
      "return qt_host_call(\"recurse\", args); }\n"
      "string render_note(const string &in args) { return args; }\n"));
  require(created.ok(), "runtime creation failed");
  auto runtime = std::move(created.value());
  require(runtime.registration().commands.size() == 1,
          "activation registration was not retained");
  require(runtime.registration().message_renderers.size() == 1 &&
              runtime.registration().message_renderers[0].custom_type ==
                  "custom.note" &&
              runtime.registration().message_renderers[0].handler ==
                  "render_note",
          "message renderer registration was not retained");

  int calls = 0;
  const auto host = [&](std::string_view method,
                        const Json& arguments) -> qt::foundation::Result<Json> {
    ++calls;
    if (method == "fail") {
      return qt::foundation::Result<Json>::failure(
          qt::foundation::ErrorCode::invalid_argument, "host failure");
    }
    return Json{{"method", method}, {"arguments", arguments}, {"calls", calls}};
  };
  auto first = runtime.invoke("command_state", Json{{"value", 1}}, host);
  auto second = runtime.invoke("command_state", Json{{"value", 2}}, host);
  require(first.ok() && second.ok() && first.value().at("calls") == 1 &&
              second.value().at("calls") == 2,
          "runtime state was not retained across calls");
  require(runtime.invoke("command_void", Json{{"value", 3}}, host).ok(),
          "void handler failed");
  require(!runtime.invoke("missing", Json::object(), host).ok(),
          "missing handler was accepted");

  auto failing = runtime.invoke("command_state", Json::object(),
                               [&](std::string_view, const Json&) {
                                 return qt::foundation::Result<Json>::failure(
                                     qt::foundation::ErrorCode::io_error,
                                     "expected host failure");
                               });
  require(!failing.ok(), "host callback failure was swallowed");

  AngelScriptRuntime* runtime_ptr = &runtime;
  std::function<qt::foundation::Result<Json>(std::string_view, const Json&)>
      nested_host;
  nested_host = [&](std::string_view method, const Json& arguments)
      -> qt::foundation::Result<Json> {
    if (method == "nested") {
      return runtime_ptr->invoke("command_state", arguments, nested_host);
    }
    return host(method, arguments);
  };
  const auto nested = runtime.invoke("command_nested", Json{{"value", 4}},
                                     nested_host);
  require(nested.ok() && nested.value().at("calls") == 4,
          "nested runtime callback failed");

  std::function<qt::foundation::Result<Json>(std::string_view, const Json&)>
      recursive_host;
  recursive_host = [&](std::string_view method, const Json& arguments)
      -> qt::foundation::Result<Json> {
    if (method == "recurse") {
      return runtime_ptr->invoke("command_recurse", arguments, recursive_host);
    }
    return host(method, arguments);
  };
  require(!runtime.invoke("command_recurse", Json::object(), recursive_host).ok(),
          "recursive invocation depth was not bounded");
  require(runtime.invoke("command_state", Json::object(), host).ok(),
          "runtime remained unusable after recursive failure");

  auto bounded = AngelScriptRuntime::create(request(
      "void activate() {}\n"
      "void command_loop(const string &in args) { while (true) {} }\n"));
  require(bounded.ok(), "bounded runtime creation failed");
  auto loop = std::move(bounded.value());
  require(!loop.invoke("command_loop", Json::object(), host).ok(),
          "invocation step budget was not enforced");
  std::ifstream plugin("examples/extensions/astra-auto-compact.as");
  require(plugin.good(), "Astra auto-compaction plugin is missing");
  auto astra_created = AngelScriptRuntime::create(request(std::string(
      std::istreambuf_iterator<char>(plugin), std::istreambuf_iterator<char>())));
  if (!astra_created) throw std::runtime_error(astra_created.error().message);
  auto astra = std::move(astra_created.value());
  const auto& hooks = astra.registration().lifecycle_handlers;
  require(hooks.size() == 1 && hooks[0].event == "input",
          "Astra plugin must register an idle-input hook");

  bool idle = true;
  bool fail_compaction = false;
  int compactions = 0;
  int warnings = 0;
  int model_reads = 0;
  Json model = {{"id", "gpt-6-astra"}, {"name", "GPT 6"}};
  Json usage = {{"tokens", 200000}};
  const auto astra_host = [&](std::string_view method, const Json& arguments)
      -> qt::foundation::Result<Json> {
    if (method == "ctx.isIdle") return Json(idle);
    if (method == "ctx.getModel") { ++model_reads; return model; }
    if (method == "ctx.getContextUsage") return usage;
    if (method == "ctx.compact") {
      require(idle && arguments == Json::object(), "invalid compaction call");
      ++compactions;
      if (fail_compaction) return qt::foundation::Result<Json>::failure(
          qt::foundation::ErrorCode::io_error, "expected compaction failure");
      usage = Json{{"tokens", nullptr}, {"contextWindow", 1050000}};
      return Json{{"summary", "compacted"}};
    }
    if (method == "ctx.ui.notify") {
      require(arguments.at("notifyType") == "warning", "expected warning");
      ++warnings;
      return Json(nullptr);
    }
    return qt::foundation::Result<Json>::failure(
        qt::foundation::ErrorCode::invalid_argument, "unexpected host call");
  };
  const auto check_astra = [&](int expected_compactions) {
    const auto result = astra.invoke(hooks[0].handler,
        Json{{"text", "original prompt"}, {"images", Json::array()}}, astra_host);
    if (!result) throw std::runtime_error(result.error().message);
    require(result.value() == Json{{"action", "continue"}},
            "Astra plugin must preserve input text and images");
    require(compactions == expected_compactions, "unexpected compaction count");
  };
  usage = nullptr;
  check_astra(0);
  usage = Json::object();
  check_astra(0);
  usage = Json{{"tokens", nullptr}};
  check_astra(0);
  usage = Json{{"tokens", 199999}};
  check_astra(0);
  usage = Json{{"tokens", 200000}};
  check_astra(0);
  usage = Json{{"tokens", 200001}};
  check_astra(1);
  check_astra(1);  // Unknown post-compaction usage must not trigger again.
  usage = Json{{"tokens", 300000}};
  model = Json{{"id", "gpt-6"}, {"name", "GPT 6"}, {"provider", "astra"}};
  check_astra(1);
  model = nullptr;
  check_astra(1);
  model = Json::object();
  check_astra(1);
  model = Json{{"id", "other-model"}, {"name", "GPT 6 AsTrA"}};
  check_astra(2);
  model = Json{{"id", "vendor/ASTRA-preview"}, {"name", "Preview"}};
  usage = Json{{"tokens", 300000}};
  idle = false;
  const int reads_before = model_reads;
  check_astra(2);
  require(model_reads == reads_before, "busy hook must not inspect context");
  idle = true;
  fail_compaction = true;
  check_astra(3);
  require(warnings == 1, "compaction failure should warn and preserve input");
  fail_compaction = false;
  check_astra(4);  // A failure must not permanently disable later attempts.
  check_astra(4);
  std::cout << "extension runtime probe passed (including Astra plugin)\n";
}
