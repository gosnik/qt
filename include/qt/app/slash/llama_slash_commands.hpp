#pragma once

#include <string_view>
#include <vector>

#include "qt/app/slash/slash_commands.hpp"

namespace qt::app {

struct LlamaSlashSubcommand {
  const char* name;
  const char* description;
  const char* argument_hint;
};

std::vector<LlamaSlashSubcommand> llama_slash_subcommand_infos();
std::vector<const char*> llama_slash_subcommands();
SlashCommandRuntimeOperation plan_llama_slash_command(std::string_view arguments);
bool is_llama_slash_command_read_only(std::string_view arguments);

}  // namespace qt::app
