#pragma once

#include "qt/app/cli/auxiliary_mode.hpp"

#include <string>
#include <vector>

namespace qt::app {

struct ReferenceAuxiliaryCliParseResult {
  bool ok = false;
  ReferenceAuxiliaryModeArgs args;
  std::string error_text;
};

ReferenceAuxiliaryCliParseResult parse_auxiliary_cli_args(
    const std::vector<std::string>& argv_like);

ReferenceAuxiliaryCliParseResult parse_auxiliary_cli_args(
    int argc, const char* const argv[]);

}  // namespace qt::app
