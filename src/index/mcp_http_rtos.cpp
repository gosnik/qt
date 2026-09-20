#include "index/mcp_http_rtos.hpp"

#include "index/string_util.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace qt::index {
namespace {

using json = nlohmann::json;

std::size_t query_size(const HttpRequest& request, const std::string& key, std::size_t fallback) {
    const auto found = request.query.find(key);
    if (found == request.query.end() || found->second.empty()) {
        return fallback;
    }
    return static_cast<std::size_t>(std::strtoull(found->second.c_str(), nullptr, 10));
}

std::string query_value(const HttpRequest& request, const std::string& key, const std::string& fallback = {}) {
    const auto found = request.query.find(key);
    return found == request.query.end() || found->second.empty() ? fallback : found->second;
}

struct RtosObservation {
    std::string name;
    std::string label;
    std::string api;
    std::string file;
    std::size_t line = 0;
    std::string detail;
};

std::string first_identifier_argument(const std::string& args) {
    std::smatch match;
    if (std::regex_search(args, match, std::regex(R"(\s*&?\s*([A-Za-z_][A-Za-z0-9_]*|\"[^\"]+\"))"))) {
        auto value = match[1].str();
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return {};
}

std::string nth_identifier_argument(const std::string& args, std::size_t target_index) {
    std::regex identifier(R"((&?\s*[A-Za-z_][A-Za-z0-9_]*|\"[^\"]+\"))");
    std::size_t index = 0;
    for (std::sregex_iterator it(args.begin(), args.end(), identifier), end; it != end; ++it, ++index) {
        if (index != target_index) {
            continue;
        }
        auto value = trim_ascii((*it)[1].str());
        if (!value.empty() && value.front() == '&') {
            value = trim_ascii(value.substr(1));
        }
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return {};
}

void add_rtos_observation(json& nodes,
                          std::set<std::string>& seen,
                          const RtosObservation& observation) {
    if (observation.name.empty()) {
        return;
    }
    const auto id = observation.label + ":" + observation.name;
    if (!seen.insert(id).second) {
        return;
    }
    nodes.push_back({{"id", id},
                     {"label", observation.label},
                     {"name", observation.name},
                     {"api", observation.api},
                     {"file", observation.file},
                     {"line", observation.line},
                     {"detail", observation.detail}});
}

}  // namespace

json rtos_task_graph_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto limit = query_size(request, "limit", 500);
    const auto file_filter = query_value(request, "file");
    json nodes = json::array();
    json edges = json::array();
    std::set<std::string> seen_nodes;
    add_rtos_observation(nodes, seen_nodes, {"scheduler", "Scheduler", "rtos", "", 0, "implicit root"});
    const std::regex call_regex(R"(\b([A-Za-z_][A-Za-z0-9_]*)\s*\(([^;]*)\))");
    const std::regex decl_regex(R"(\b(TaskHandle_t|QueueHandle_t|SemaphoreHandle_t|TimerHandle_t|osThreadId_t|osMessageQueueId_t|osSemaphoreId_t|osMutexId_t|osTimerId_t)\s+([A-Za-z_][A-Za-z0-9_]*))");
    const std::regex assignment_regex(R"(([A-Za-z_][A-Za-z0-9_]*)\s*=\s*[A-Za-z_][A-Za-z0-9_]*\s*\()");

    const auto add_edge = [&](const std::string& from,
                              const std::string& to,
                              const std::string& type,
                              const std::string& api,
                              const std::string& file,
                              std::size_t line) {
        if (from.empty() || to.empty()) {
            return;
        }
        if (limit != 0 && edges.size() >= limit) {
            return;
        }
        edges.push_back({{"from", from}, {"to", to}, {"type", type}, {"api", api}, {"file", file}, {"line", line}});
    };

    for (const auto& file : service.project_scan().files) {
        if (file.kind != ProjectFileKind::CSource && file.kind != ProjectFileKind::CxxSource &&
            file.kind != ProjectFileKind::Header) {
            continue;
        }
        const auto relative = file.path.generic_string();
        if (!file_filter.empty() && relative.find(file_filter) == std::string::npos) {
            continue;
        }
        std::ifstream input(service.project_root() / file.path);
        if (!input) {
            continue;
        }
        std::string line;
        std::size_t line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            for (std::sregex_iterator it(line.begin(), line.end(), decl_regex), end; it != end; ++it) {
                const auto type = (*it)[1].str();
                const auto name = (*it)[2].str();
                std::string label = "RTOSResource";
                if (type.find("Task") != std::string::npos || type.find("Thread") != std::string::npos) {
                    label = "Task";
                } else if (type.find("Queue") != std::string::npos) {
                    label = "Queue";
                } else if (type.find("Timer") != std::string::npos) {
                    label = "Timer";
                } else if (type.find("Mutex") != std::string::npos) {
                    label = "Mutex";
                } else if (type.find("Semaphore") != std::string::npos) {
                    label = "Semaphore";
                }
                add_rtos_observation(nodes, seen_nodes, {name, label, type, relative, line_number, "declaration"});
            }
            for (std::sregex_iterator it(line.begin(), line.end(), call_regex), end; it != end; ++it) {
                const auto api = (*it)[1].str();
                const auto args = (*it)[2].str();
                std::string assigned_to;
                std::smatch assignment_match;
                if (std::regex_search(line, assignment_match, assignment_regex)) {
                    assigned_to = assignment_match[1].str();
                }
                if (api == "xTaskCreate" || api == "xTaskCreateStatic" || api == "xTaskCreatePinnedToCore" ||
                    api == "osThreadNew" || api == "k_thread_create") {
                    const auto task = api == "osThreadNew" ? nth_identifier_argument(args, 0)
                                      : api == "k_thread_create" ? nth_identifier_argument(args, 0)
                                                                 : nth_identifier_argument(args, 1);
                    add_rtos_observation(nodes, seen_nodes, {task, "Task", api, relative, line_number, args});
                    add_edge("scheduler", "Task:" + task, "CREATES_TASK", api, relative, line_number);
                } else if (api == "xQueueCreate" || api == "xQueueCreateStatic" || api == "osMessageQueueNew" ||
                           api == "k_msgq_init" || api == "K_MSGQ_DEFINE") {
                    const auto queue = api == "K_MSGQ_DEFINE" || api == "k_msgq_init" ? nth_identifier_argument(args, 0)
                                                                                       : (!assigned_to.empty() ? assigned_to : first_identifier_argument(args));
                    add_rtos_observation(nodes, seen_nodes, {queue, "Queue", api, relative, line_number, args});
                } else if (api == "xQueueSend" || api == "xQueueSendToBack" || api == "xQueueSendToFront" ||
                           api == "xQueueReceive" || api == "osMessageQueuePut" || api == "osMessageQueueGet" ||
                           api == "k_msgq_put" || api == "k_msgq_get") {
                    const auto queue = first_identifier_argument(args);
                    add_rtos_observation(nodes, seen_nodes, {queue, "Queue", api, relative, line_number, args});
                    add_edge(relative, "Queue:" + queue, api.find("Receive") != std::string::npos || api.find("Get") != std::string::npos || api.find("_get") != std::string::npos ? "RECEIVES_FROM" : "SENDS_TO", api, relative, line_number);
                } else if (api == "xSemaphoreCreateBinary" || api == "xSemaphoreCreateCounting" ||
                           api == "xSemaphoreGive" || api == "xSemaphoreTake" || api == "osSemaphoreNew" ||
                           api == "osSemaphoreAcquire" || api == "osSemaphoreRelease" || api == "k_sem_init" ||
                           api == "k_sem_take" || api == "k_sem_give") {
                    const auto semaphore = !assigned_to.empty() ? assigned_to : first_identifier_argument(args);
                    add_rtos_observation(nodes, seen_nodes, {semaphore, "Semaphore", api, relative, line_number, args});
                    if (api.find("Give") != std::string::npos || api.find("Release") != std::string::npos || api.find("_give") != std::string::npos) {
                        add_edge(relative, "Semaphore:" + semaphore, "GIVES", api, relative, line_number);
                    } else if (api.find("Take") != std::string::npos || api.find("Acquire") != std::string::npos || api.find("_take") != std::string::npos) {
                        add_edge(relative, "Semaphore:" + semaphore, "TAKES", api, relative, line_number);
                    }
                } else if (api == "xSemaphoreCreateMutex" || api == "xSemaphoreCreateRecursiveMutex" ||
                           api == "osMutexNew" || api == "osMutexAcquire" || api == "osMutexRelease" ||
                           api == "k_mutex_init" || api == "k_mutex_lock" || api == "k_mutex_unlock") {
                    const auto mutex = !assigned_to.empty() ? assigned_to : first_identifier_argument(args);
                    add_rtos_observation(nodes, seen_nodes, {mutex, "Mutex", api, relative, line_number, args});
                    if (api.find("Acquire") != std::string::npos || api.find("lock") != std::string::npos) {
                        add_edge(relative, "Mutex:" + mutex, "LOCKS", api, relative, line_number);
                    } else if (api.find("Release") != std::string::npos || api.find("unlock") != std::string::npos) {
                        add_edge(relative, "Mutex:" + mutex, "UNLOCKS", api, relative, line_number);
                    }
                } else if (api == "xTimerCreate" || api == "xTimerStart" || api == "xTimerStop" ||
                           api == "osTimerNew" || api == "osTimerStart" || api == "osTimerStop" ||
                           api == "k_timer_init" || api == "k_timer_start" || api == "k_timer_stop") {
                    const auto timer = !assigned_to.empty() ? assigned_to : first_identifier_argument(args);
                    add_rtos_observation(nodes, seen_nodes, {timer, "Timer", api, relative, line_number, args});
                    if (api.find("Start") != std::string::npos || api.find("_start") != std::string::npos) {
                        add_edge(relative, "Timer:" + timer, "STARTS_TIMER", api, relative, line_number);
                    } else if (api.find("Stop") != std::string::npos || api.find("_stop") != std::string::npos) {
                        add_edge(relative, "Timer:" + timer, "STOPS_TIMER", api, relative, line_number);
                    }
                } else if (api == "xTaskNotify" || api == "xTaskNotifyGive" || api == "vTaskNotifyGiveFromISR" ||
                           api == "ulTaskNotifyTake" || api == "xTaskNotifyWait") {
                    const auto task = first_identifier_argument(args);
                    add_rtos_observation(nodes, seen_nodes, {task, "Notification", api, relative, line_number, args});
                    add_edge(relative, "Notification:" + task, api.find("Take") != std::string::npos || api.find("Wait") != std::string::npos ? "WAITS_NOTIFICATION" : "SENDS_NOTIFICATION", api, relative, line_number);
                }
            }
        }
    }
    return {{"nodes", nodes}, {"edges", edges}, {"query", request.query}};
}


}  // namespace qt::index
