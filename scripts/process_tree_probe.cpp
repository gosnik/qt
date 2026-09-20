#include "qt/foundation/process.hpp"

#include <chrono>
#include <csignal>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
  try {
    require(argc == 2, "usage: process_tree_probe <pid-file>");
    const std::string pid_file = argv[1];
    qt::foundation::ProcessCancellationToken token;
    qt::foundation::ProcessRequest request;
    request.mode = qt::foundation::ProcessLaunchMode::shell;
    request.command = "python3 -c 'import os,time; p=os.fork(); "
                      "(os.setsid(),open(\"" + pid_file +
                      "\",\"w\").write(str(os.getpid())),time.sleep(30)) if p==0 else time.sleep(30)'";
    request.cancellation_token = std::make_shared<qt::foundation::ProcessCancellationToken>();
    auto cancellation = request.cancellation_token;
    qt::foundation::Result<qt::foundation::ProcessResult> result(
        qt::foundation::Error{qt::foundation::ErrorCode::internal_error, "not started"});
    std::thread runner([&] { result = qt::foundation::run_process(request); });
    for (int attempt = 0; attempt < 100; ++attempt) {
      std::ifstream input(pid_file);
      if (input.good()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    cancellation->cancel();
    runner.join();
    require(result.ok() && result.value().cancelled, "process cancellation failed");
    std::ifstream input(pid_file);
    pid_t descendant = 0;
    input >> descendant;
    require(descendant > 0, "descendant pid was not recorded");
    for (int attempt = 0; attempt < 100; ++attempt) {
      if (::kill(descendant, 0) != 0) return 0;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("recursive descendant survived cancellation");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "process tree probe failed: %s\n", error.what());
    return 1;
  }
}
