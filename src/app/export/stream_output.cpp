#include "qt/app/export/stream_output.hpp"

namespace qt::app {

bool append_initial_message_diagnostics(
    StreamOutputPlan& plan,
    const std::vector<InitialMessageDiagnostic>& diagnostics) {
  bool has_errors = false;
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.severity == InitialMessageDiagnosticSeverity::error) {
      plan.stderr_chunks.push_back("Error: " + diagnostic.message + "\n");
      has_errors = true;
    } else if (diagnostic.severity ==
               InitialMessageDiagnosticSeverity::warning) {
      plan.stderr_chunks.push_back("Warning: " + diagnostic.message + "\n");
    } else {
      plan.stderr_chunks.push_back(diagnostic.message + "\n");
    }
  }
  return has_errors;
}

void emit_stream_output(const StreamOutputPlan& plan, std::ostream& stdout_stream,
                        std::ostream& stderr_stream) {
  for (const auto& chunk : plan.stdout_chunks) {
    stdout_stream << chunk;
  }
  if (!plan.stdout_chunks.empty()) {
    stdout_stream.flush();
  }

  for (const auto& chunk : plan.stderr_chunks) {
    stderr_stream << chunk;
  }
  if (!plan.stderr_chunks.empty()) {
    stderr_stream.flush();
  }
}

}  // namespace qt::app
