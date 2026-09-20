#pragma once

#include <ostream>
#include <string>
#include <vector>

#include "qt/app/runtime/initial_message.hpp"

namespace qt::app {

struct StreamOutputPlan {
  int exit_code = 0;
  std::vector<std::string> stdout_chunks;
  std::vector<std::string> stderr_chunks;
};

bool append_initial_message_diagnostics(
    StreamOutputPlan& plan,
    const std::vector<InitialMessageDiagnostic>& diagnostics);

void emit_stream_output(const StreamOutputPlan& plan, std::ostream& stdout_stream,
                        std::ostream& stderr_stream);

}  // namespace qt::app
