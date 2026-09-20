#include "qt/foundation/pi_user_agent.hpp"

#include <sstream>
#include <string>

namespace qt::foundation {

std::string pi_user_agent(std::string_view version) {
    const char* platform =
#if defined(_WIN32)
        "win32";
#elif defined(__APPLE__)
        "darwin";
#else
        "linux";
#endif

    const char* runtime = "native/c++17";

    const char* arch =
#if defined(__x86_64__) || defined(_M_X64)
        "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
        "arm64";
#elif defined(__i386__)
        "ia32";
#else
        "unknown";
#endif

    std::ostringstream agent;
    agent << "pi/" << version << " (" << platform << "; " << runtime << "; " << arch << ")";
    return agent.str();
}

}  // namespace qt::foundation
