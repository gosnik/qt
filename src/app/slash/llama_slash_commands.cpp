#include "qt/app/slash/llama_slash_commands.hpp"

#include <cctype>
#include <optional>
#include <string>
#include <utility>

namespace qt::app {
namespace {

bool is_space(char value) {
  return std::isspace(static_cast<unsigned char>(value)) != 0;
}

std::string trim_copy(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() && is_space(text[start])) {
    ++start;
  }

  std::size_t end = text.size();
  while (end > start && is_space(text[end - 1])) {
    --end;
  }
  return std::string(text.substr(start, end - start));
}

std::pair<std::string, std::string> split_first_argument(
    std::string_view arguments) {
  std::string_view text = arguments;
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return {"", ""};
  }

  std::size_t first_end = 0;
  while (first_end < text.size() && !is_space(text[first_end])) {
    ++first_end;
  }

  std::string_view rest = text.substr(first_end);
  while (!rest.empty() && is_space(rest.front())) {
    rest.remove_prefix(1);
  }
  return {std::string(text.substr(0, first_end)), trim_copy(rest)};
}

std::optional<std::string> parse_optional_path_argument(
    std::string_view arguments) {
  const std::string trimmed = trim_copy(arguments);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  if (trimmed.front() != '"' && trimmed.front() != '\'') {
    const auto split = split_first_argument(trimmed);
    return split.first;
  }

  const char quote = trimmed.front();
  std::string path;
  for (std::size_t index = 1; index < trimmed.size(); ++index) {
    const char value = trimmed[index];
    if (value == quote) {
      return path;
    }
    path.push_back(value);
  }

  return path;
}

std::string normalize_subcommand(std::string subcommand) {
  for (char& value : subcommand) {
    value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  }
  return subcommand;
}

void append_common_runtime_step(SlashCommandRuntimeOperation& operation) {
  operation.steps.push_back(
      "Runtime should route the request through the local llama provider state.");
}

}  // namespace

std::vector<const char*> llama_slash_subcommands() {
  std::vector<const char*> names;
  for (const auto& info : llama_slash_subcommand_infos()) {
    names.push_back(info.name);
  }
  return names;
}

std::vector<LlamaSlashSubcommand> llama_slash_subcommand_infos() {
  return {
      {"status", "Show llama runtime, model, KV cache, GPU, grammar, and embedding status.", ""},
      {"models", "List configured local llama models and mark the selected model.", ""},
      {"select", "Select the local llama model used by llama commands.", "<model-id>"},
      {"load", "Load the selected or requested llama model now.", "[model-id]"},
      {"unload", "Release llama model/context resources.", ""},
      {"reload", "Reload llama settings, catalog entries, and runtime state.", ""},
      {"policy", "Show or update llama generation policy overrides.", "[temperature=<n>] [topP=<n>]"},
      {"grammar", "Show, disable, or set grammar-constrained generation.", "<show|off|inline|file> ..."},
      {"kv", "Inspect or mutate the llama KV cache.", "<show|clear|keep|drop|shift> ..."},
      {"gpu", "Show or update pending llama GPU/backend offload settings.", "[gpuLayers=<n>] [mainGpu=<n>] [splitMode=<mode>]"},
      {"embeddings", "Generate a llama embedding for text when supported by the model.", "<text>"},
      {"prompt", "Run text through the selected llama model.", "<text>"},
      {"run", "Run text through a tagged llama instance.", "<embed|rank|instruct|chat> <text>"},
  };
}

bool is_llama_slash_command_read_only(std::string_view arguments) {
  const auto parsed = split_first_argument(arguments);
  const std::string subcommand =
      parsed.first.empty() ? "status" : normalize_subcommand(parsed.first);
  return subcommand == "status" || subcommand == "models";
}

SlashCommandRuntimeOperation plan_llama_slash_command(
    std::string_view arguments) {
  const auto parsed = split_first_argument(arguments);
  const std::string subcommand =
      parsed.first.empty() ? "status" : normalize_subcommand(parsed.first);

  SlashCommandRuntimeOperation operation;
  operation.kind = SlashCommandRuntimeOperationKind::kLlamaProviderOperation;
  operation.value = subcommand;
  if (!parsed.second.empty()) {
    operation.custom_instructions = parsed.second;
  }

  if (subcommand == "status") {
    operation.steps.push_back(
        "Show selected llama model, load state, generation policy, KV cache, GPU, grammar, and embedding status.");
    return operation;
  }
  if (subcommand == "models") {
    operation.steps.push_back(
        "List configured llama models and mark the selected model.");
    return operation;
  }
  if (subcommand == "select") {
    operation.steps.push_back(
        "Select llama/<model-id> through the same runtime path as /model.");
    append_common_runtime_step(operation);
    return operation;
  }
  if (subcommand == "load") {
    operation.steps.push_back(
        "Load the selected or requested llama model immediately.");
    append_common_runtime_step(operation);
    return operation;
  }
  if (subcommand == "unload") {
    operation.steps.push_back(
        "Release llama context/model resources and clear provider-local runtime state.");
    append_common_runtime_step(operation);
    return operation;
  }
  if (subcommand == "reload") {
    operation.steps.push_back(
        "Re-read llama settings, rebuild catalog entries, and reload if needed.");
    append_common_runtime_step(operation);
    return operation;
  }
  if (subcommand == "policy") {
    operation.steps.push_back(
        "Show or update llama generation policy overrides.");
    append_common_runtime_step(operation);
    return operation;
  }
  if (subcommand == "grammar") {
    const auto grammar = split_first_argument(parsed.second);
    if (grammar.first == "file") {
      operation.target_path = parse_optional_path_argument(grammar.second);
    }
    operation.steps.push_back(
        "Show, disable, or update llama grammar-constrained generation.");
    append_common_runtime_step(operation);
    return operation;
  }
  if (subcommand == "kv") {
    operation.steps.push_back("Apply a llama KV cache inspection or mutation.");
    append_common_runtime_step(operation);
    return operation;
  }
  if (subcommand == "gpu") {
    operation.steps.push_back(
        "Show or update pending llama GPU/backend offload settings.");
    append_common_runtime_step(operation);
    return operation;
  }
  if (subcommand == "embeddings") {
    operation.steps.push_back(
        "Generate a llama embedding for quick inspection when supported.");
    append_common_runtime_step(operation);
    return operation;
  }
  if (subcommand == "prompt") {
    operation.steps.push_back(
        "Run the remaining command arguments through the selected llama model and display the generated text.");
    append_common_runtime_step(operation);
    return operation;
  }
  if (subcommand == "run") {
    operation.steps.push_back(
        "Run text through a tagged llama instance: embed, rank, instruct, or chat.");
    operation.steps.push_back(
        "The instruct mode should include active framework tool definitions.");
    append_common_runtime_step(operation);
    return operation;
  }

  operation.steps.push_back(
      "Report supported llama subcommands and preserve the raw arguments for diagnostics.");
  return operation;
}

}  // namespace qt::app
