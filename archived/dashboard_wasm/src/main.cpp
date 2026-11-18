#include <SDL.h>
#include <SDL_opengles2.h>
#include <emscripten.h>
#include <emscripten/fetch.h>
#include <emscripten/html5.h>

#include <imgui.h>
#include <imgui_internal.h>
#include "imgui_spectrum.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "imgui_stdlib.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;

constexpr char kAppTitle[] = "qt Memory Dashboard";
constexpr char kGlslVersion[] = "#version 300 es";
constexpr int kInitialWidth = 1540;
constexpr int kInitialHeight = 960;
constexpr int kDockLayoutVersion = 5;
constexpr int kPanelVisibilityVersion = 3;
constexpr float kDefaultUiScale = 1.05f;
constexpr float kUiScaleMin = 0.8f;
constexpr float kUiScaleMax = 1.6f;
constexpr float kUiScaleStep = 0.05f;
constexpr float kBaseFontSize = 18.0f;
constexpr float kMonospaceFontSize = 19.0f;
constexpr float kCompactLayoutWidth = 1000.0f;
constexpr float kDesktopNavigationWidth = 190.0f;
constexpr float kPi = 3.14159265358979323846f;
constexpr char kThemeKey[] = "qtmem.dashboard.theme.v2";
constexpr char kUiScaleKey[] = "qtmem.dashboard.ui_scale";
constexpr char kDockLayoutDesktopKey[] = "qtmem.dashboard.docking_ini.desktop";
constexpr char kDockLayoutCompactKey[] = "qtmem.dashboard.docking_ini.compact";
constexpr char kDockLayoutVersionKey[] = "qtmem.dashboard.docking_version";
constexpr char kDockLayoutModeKey[] = "qtmem.dashboard.docking_compact";
constexpr char kPanelVisibilityVersionKey[] = "qtmem.dashboard.panel_visibility_version";
constexpr char kPanelVisibilityKey[] = "qtmem.dashboard.panel_visibility";
constexpr char kWorkspaceGroupKey[] = "qtmem.dashboard.workspace_group";

enum class ThemePreset {
    dark = 0,
    light = 1,
    classic = 2,
    count = 3,
};

const std::array<const char*, 3> kThemeLabels{
    "Dark",
    "Light",
    "Classic",
};

enum class WorkspaceGroup {
    overview = 0,
    explore = 1,
    capture = 2,
    review = 3,
    operations = 4,
    count = 5,
};

const std::array<const char*, 5> kWorkspaceGroupLabels{
    "Overview",
    "Explore",
    "Capture",
    "Review",
    "Operations",
};

const std::array<const char*, 5> kWorkspaceGroupDescriptions{
    "Health, graph, and recall signals",
    "Search memories and inspect graph links",
    "Write memory, build context, and review sources",
    "Resolve conflicts, stale evidence, and selected details",
    "Agents, sessions, scheduler, logs, and runtime config",
};

constexpr char kStatusWindowTitle[] = "Scope";
constexpr char kOverviewWindowTitle[] = "Overview";
constexpr char kEditorWindowTitle[] = "Capture";
constexpr char kExplorerWindowTitle[] = "Memories";
constexpr char kDetailWindowTitle[] = "Details";
constexpr char kGraphWindowTitle[] = "Graph";
constexpr char kSourcesWindowTitle[] = "Sources";
constexpr char kAgentsWindowTitle[] = "Agents";
constexpr char kSessionsWindowTitle[] = "Sessions";
constexpr char kContextWindowTitle[] = "Context";
constexpr char kSystemWindowTitle[] = "System";
constexpr char kLogsWindowTitle[] = "Logs";
constexpr char kWorkspaceNavTitle[] = "Workspace";

const std::array<const char*, 9> kMemoryTypes{
    "",
    "decision",
    "constraint",
    "semantic",
    "episodic",
    "procedural",
    "bug",
    "observation",
    "conversation",
};

const std::array<const char*, 8> kStatuses{
    "",
    "validated",
    "active",
    "hypothesis",
    "deprecated",
    "superseded",
    "rejected",
    "expired",
};

const std::array<const char*, 19> kEdgeTypes{
    "",
    "mentions",
    "derived_from",
    "validated_by",
    "conflicts_with",
    "supersedes",
    "deprecates",
    "applies_to",
    "owned_by",
    "observed_in",
    "fixed_by",
    "caused_by",
    "uses",
    "depends_on",
    "part_of",
    "related",
    "supports",
    "follows",
    "fixes",
};

const std::array<const char*, 5> kRecallPolicies{
    "balanced",
    "strict",
    "exploratory",
    "debugging",
    "architecture",
};

EM_JS(void, js_set_status, (const char* text), {
    if (typeof Module !== "undefined" && Module.setStatus) {
        Module.setStatus(UTF8ToString(text));
    }
});

EM_JS(int, js_local_storage_get, (const char* key, char* out, int capacity), {
    try {
        const value = window.localStorage.getItem(UTF8ToString(key)) || "";
        const bytes = stringToUTF8(value, out, capacity);
        return bytes;
    } catch (_error) {
        if (capacity > 0) {
            out[0] = 0;
        }
        return 0;
    }
});

EM_JS(void, js_local_storage_set, (const char* key, const char* value), {
    try {
        window.localStorage.setItem(UTF8ToString(key), UTF8ToString(value));
    } catch (_error) {
    }
});

EM_JS(int, js_query_parameter_get, (const char* key, char* out, int capacity), {
    const params = new URLSearchParams(window.location.search);
    const value = params.get(UTF8ToString(key)) || "";
    const bytes = stringToUTF8(value, out, capacity);
    return bytes;
});

EM_JS(int, js_bootstrap_auth_required, (), {
    return window.qtmemDashboardBootstrap && window.qtmemDashboardBootstrap.authRequired ? 1 : 0;
});

std::string js_string_result(std::function<int(char*, int)> fill) {
    std::array<char, 4096> buffer{};
    fill(buffer.data(), static_cast<int>(buffer.size()));
    return std::string(buffer.data());
}

std::string local_storage_get(const char* key) {
    return js_string_result([key](char* out, int capacity) { return js_local_storage_get(key, out, capacity); });
}

void local_storage_set(const char* key, const std::string& value) {
    js_local_storage_set(key, value.c_str());
}

int local_storage_get_int(const char* key, int fallback) {
    const auto stored = local_storage_get(key);
    if (stored.empty()) {
        return fallback;
    }
    try {
        return std::stoi(stored);
    } catch (...) {
        return fallback;
    }
}

float local_storage_get_float(const char* key, float fallback) {
    const auto stored = local_storage_get(key);
    if (stored.empty()) {
        return fallback;
    }
    try {
        return std::stof(stored);
    } catch (...) {
        return fallback;
    }
}

std::string query_parameter_get(const char* key) {
    return js_string_result([key](char* out, int capacity) { return js_query_parameter_get(key, out, capacity); });
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string excerpt(const std::string& value, std::size_t limit) {
    if (value.size() <= limit) {
        return value;
    }
    if (limit <= 3) {
        return value.substr(0, limit);
    }
    return value.substr(0, limit - 3) + "...";
}

std::string json_string(const json& value, const char* key, const std::string& fallback = {}) {
    if (value.contains(key) && value.at(key).is_string()) {
        return value.at(key).get<std::string>();
    }
    return fallback;
}

double json_number(const json& value, const char* key, double fallback = 0.0) {
    if (value.contains(key) && value.at(key).is_number()) {
        return value.at(key).get<double>();
    }
    return fallback;
}

int json_int(const json& value, const char* key, int fallback = 0) {
    if (value.contains(key) && value.at(key).is_number_integer()) {
        return value.at(key).get<int>();
    }
    if (value.contains(key) && value.at(key).is_number()) {
        return static_cast<int>(value.at(key).get<double>());
    }
    return fallback;
}

std::string pretty_json(const json& value) {
    if (value.is_null()) {
        return {};
    }
    return value.dump(2);
}

std::string json_compact_text(const json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned long long>());
    }
    if (value.is_number_float()) {
        std::ostringstream stream;
        stream << value.get<double>();
        return stream.str();
    }
    if (value.is_array()) {
        std::vector<std::string> parts;
        for (const auto& item : value) {
            parts.push_back(json_compact_text(item));
        }
        std::ostringstream stream;
        for (std::size_t index = 0; index < parts.size(); ++index) {
            if (index != 0) {
                stream << ", ";
            }
            stream << parts[index];
        }
        return stream.str();
    }
    if (value.is_null()) {
        return {};
    }
    return value.dump();
}

std::string join_strings(const std::vector<std::string>& values, const char* separator = ", ") {
    std::ostringstream stream;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            stream << separator;
        }
        stream << values[index];
    }
    return stream.str();
}

std::vector<std::string> json_strings(const json& value) {
    std::vector<std::string> values;
    if (!value.is_array()) {
        return values;
    }
    values.reserve(value.size());
    for (const auto& item : value) {
        if (item.is_string()) {
            values.push_back(item.get<std::string>());
        } else if (!item.is_null()) {
            values.push_back(item.dump());
        }
    }
    return values;
}

std::string joined_json_strings(const json& value, const char* separator = ", ") {
    return join_strings(json_strings(value), separator);
}

bool json_bool(const json& value, const char* key, bool fallback = false) {
    if (value.contains(key) && value.at(key).is_boolean()) {
        return value.at(key).get<bool>();
    }
    return fallback;
}

void add_table_row(std::vector<std::pair<std::string, std::string>>& rows, const std::string& label, const std::string& value, bool allow_empty = false) {
    if (!allow_empty && value.empty()) {
        return;
    }
    rows.push_back({label, value});
}

bool parse_integer_string(const std::string& value, long long& parsed) {
    if (value.empty()) {
        return false;
    }
    if (!std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        return false;
    }
    try {
        parsed = std::stoll(value);
        return true;
    } catch (...) {
        return false;
    }
}

std::string human_time_from_epoch(long long epoch_value, bool milliseconds = false) {
    const std::time_t raw_time = milliseconds ? static_cast<std::time_t>(epoch_value / 1000LL) : static_cast<std::time_t>(epoch_value);
    std::tm tm_value{};
#if defined(_WIN32)
    localtime_s(&tm_value, &raw_time);
#else
    localtime_r(&raw_time, &tm_value);
#endif
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm_value) == 0) {
        return std::to_string(epoch_value);
    }
    return std::string(buffer);
}

std::string display_time_text(const std::string& value) {
    long long parsed = 0;
    if (!parse_integer_string(value, parsed)) {
        return value;
    }
    if (value.size() >= 13) {
        return human_time_from_epoch(parsed, true);
    }
    if (value.size() >= 10) {
        return human_time_from_epoch(parsed, false);
    }
    return value;
}

std::string display_time_text(const json& value) {
    if (value.is_string()) {
        return display_time_text(value.get<std::string>());
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        const auto number = value.get<long long>();
        const std::string raw = std::to_string(number);
        return display_time_text(raw);
    }
    return json_compact_text(value);
}

ImVec4 alpha(const ImVec4& color, float a) {
    return ImVec4(color.x, color.y, color.z, a);
}

ImU32 color_u32(const ImVec4& color) {
    return ImGui::ColorConvertFloat4ToU32(color);
}

ImVec4 mix_color(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t);
}

ImVec4 dashboard_accent() {
    return ImVec4(0.18f, 0.78f, 0.88f, 1.0f);
}

ImVec4 dashboard_green() {
    return ImVec4(0.35f, 0.89f, 0.67f, 1.0f);
}

ImVec4 dashboard_orange() {
    return ImVec4(0.96f, 0.63f, 0.24f, 1.0f);
}

ImVec4 dashboard_red() {
    return ImVec4(0.95f, 0.34f, 0.38f, 1.0f);
}

ImVec4 dashboard_violet() {
    return ImVec4(0.63f, 0.42f, 1.0f, 1.0f);
}

void draw_rounded_panel_background(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max, const ImVec4& tint, float rounding) {
    draw_list->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_WindowBg), rounding);
    draw_list->AddRectFilled(
        ImVec2(min.x + 1.0f, min.y + 1.0f),
        ImVec2(max.x - 1.0f, max.y - 1.0f),
        color_u32(alpha(mix_color(tint, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), 0.72f), 0.18f)),
        std::max(0.0f, rounding - 1.0f));
    draw_list->AddRect(min, max, color_u32(alpha(ImVec4(0.68f, 0.82f, 1.0f, 1.0f), 0.14f)), rounding, 0, 1.0f);
}

void draw_dashboard_background(const ImVec2& min, const ImVec2& max) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilledMultiColor(
        min,
        max,
        IM_COL32(7, 12, 24, 255),
        IM_COL32(10, 18, 35, 255),
        IM_COL32(5, 8, 16, 255),
        IM_COL32(9, 20, 32, 255));

    // clean gradient background — no accent circles, no visual noise
}

void draw_property_table(const char* id, const std::vector<std::pair<std::string, std::string>>& rows) {
    if (rows.empty()) {
        ImGui::TextUnformatted("No data.");
        return;
    }
    if (ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        for (const auto& row : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextWrapped("%s", row.first.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", row.second.c_str());
        }
        ImGui::EndTable();
    }
}

struct WindowVisibility {
    bool workspace_nav{true};
    bool status{true};
    bool overview{true};
    bool editor{true};
    bool explorer{true};
    bool detail{true};
    bool graph{true};
    bool sources{true};
    bool agents{true};
    bool sessions{true};
    bool context{true};
    bool system{true};
    bool logs{true};
};

WindowVisibility default_window_visibility() {
    WindowVisibility visibility;
    visibility.workspace_nav = true;
    visibility.status = true;
    visibility.overview = true;
    visibility.editor = false;
    visibility.explorer = true;
    visibility.detail = false;
    visibility.graph = true;
    visibility.sources = false;
    visibility.agents = false;
    visibility.sessions = false;
    visibility.context = false;
    visibility.system = false;
    visibility.logs = false;
    return visibility;
}

WindowVisibility window_visibility_from_json(const std::string& payload) {
    WindowVisibility visibility = default_window_visibility();
    if (payload.empty()) {
        return visibility;
    }
    try {
        const auto value = json::parse(payload);
        if (!value.is_object()) {
            return visibility;
        }
        visibility.status = value.value("status", visibility.status);
        visibility.workspace_nav = value.value("workspace_nav", visibility.workspace_nav);
        visibility.overview = value.value("overview", visibility.overview);
        visibility.editor = value.value("editor", visibility.editor);
        visibility.explorer = value.value("explorer", visibility.explorer);
        visibility.detail = value.value("detail", visibility.detail);
        visibility.graph = value.value("graph", visibility.graph);
        visibility.sources = value.value("sources", visibility.sources);
        visibility.agents = value.value("agents", visibility.agents);
        visibility.sessions = value.value("sessions", visibility.sessions);
        visibility.context = value.value("context", visibility.context);
        visibility.system = value.value("system", visibility.system);
        visibility.logs = value.value("logs", visibility.logs);
    } catch (...) {
    }
    return visibility;
}

std::string window_visibility_json(const WindowVisibility& visibility) {
    return json{
        {"workspace_nav", visibility.workspace_nav},
        {"status", visibility.status},
        {"overview", visibility.overview},
        {"editor", visibility.editor},
        {"explorer", visibility.explorer},
        {"detail", visibility.detail},
        {"graph", visibility.graph},
        {"sources", visibility.sources},
        {"agents", visibility.agents},
        {"sessions", visibility.sessions},
        {"context", visibility.context},
        {"system", visibility.system},
        {"logs", visibility.logs},
    }
        .dump();
}

enum class BannerTone {
    info,
    success,
    warning,
    error,
};

struct HttpResponse {
    bool ok{false};
    bool unauthorized{false};
    int status{0};
    std::string text;
    json payload{json::object()};
    std::string error_message;
};

struct ScopeFields {
    std::string project;
    std::string session;
    std::string agent_id;
    std::string conversation_id;
    std::string task;
};

struct EditorState {
    std::string memory_type{"observation"};
    std::string status{"active"};
    std::string source{"dashboard:manual"};
    std::string source_type{"manual"};
    std::string source_uri;
    std::string tags_csv;
    std::string content;
    float confidence{0.75f};
    std::string raw_text;
    bool stage_for_review{true};
    bool dry_run{false};
};

struct ExplorerState {
    std::string query;
    std::string memory_type;
    std::string status;
    int limit{50};
    bool include_superseded{false};
};

struct ContextState {
    std::string task;
    std::string query;
    std::string recall_policy{"balanced"};
    int budget_tokens{1400};
    bool include_linked_tools{false};
    json result{json::object()};
};

struct GraphFilters {
    std::string query;
    std::string memory_type;
    std::string status;
    std::string edge_type;
    int limit{250};
    bool include_isolates{false};
    std::string search;
};

struct GraphNodeLayout {
    ImVec2 world{0.0f, 0.0f};
    float radius{18.0f};
    std::string group_id;
    int group_index{0};
};

struct GraphViewport {
    float zoom{1.0f};
    ImVec2 pan{0.0f, 0.0f};
    bool fit_on_next_draw{true};
};

struct SystemState {
    std::string ai_config_draft;
    int scheduler_interval_seconds{3600};
    int selected_ai_service_index{0};
    int selected_ai_model_index{0};
};

struct AppState {
    SDL_Window* window{nullptr};
    SDL_GLContext gl_context{nullptr};
    ImFont* monospace_font{nullptr};
    bool running{true};
    bool started_initial_refresh{false};
    bool auth_required{false};
    bool dockspace_configured{false};
    bool dock_layout_save_pending{false};
    bool compact_layout{false};
    bool metrics_window_open{false};
    bool style_editor_open{false};
    int pending_requests{0};
    float ui_scale{kDefaultUiScale};
    int theme_index{static_cast<int>(ThemePreset::dark)};
    int active_group{static_cast<int>(WorkspaceGroup::overview)};
    std::string pending_focus_window;
    int canvas_css_width{0};
    int canvas_css_height{0};
    BannerTone banner_tone{BannerTone::info};
    std::string banner_text{"Booting qtmem dashboard..."};
    std::string last_saved_dock_layout;
    std::string auth_token;
    std::string project;
    ScopeFields scope{};
    EditorState editor{};
    ExplorerState explorer{};
    ContextState context{};
    GraphFilters graph_filters{};
    GraphViewport graph_view{};
    SystemState system{};
    json service_status{json::object()};
    json metrics{json::object()};
    json summary{json::object()};
    std::vector<json> explorer_entries;
    std::string selected_memory_id;
    json selected_memory_detail{json::object()};
    json selected_memory_history{json::object()};
    json dream_review_preview{json::object()};
    std::string dream_review_action;
    std::string dream_review_subject;
    unsigned dream_review_generation{0};
    bool dream_review_pending{false};
    json graph{json::object()};
    std::unordered_map<std::string, GraphNodeLayout> graph_layout;
    std::set<std::string> hidden_graph_groups;
    std::string selected_graph_node_id;
    std::vector<json> sources;
    std::string selected_source_id;
    json selected_source_detail{json::object()};
    std::vector<json> agents;
    std::string selected_agent_id;
    json selected_agent_detail{json::object()};
    std::vector<json> sessions;
    std::string selected_session_id;
    json selected_session_detail{json::object()};
    json logs{json::object()};
    json scheduler{json::object()};
    json ai_config{json::object()};
    json ai_metadata{json::object()};
    WindowVisibility windows{};
};

ImVec4 tone_color(BannerTone tone) {
    switch (tone) {
    case BannerTone::success:
        return dashboard_green();
    case BannerTone::warning:
        return dashboard_orange();
    case BannerTone::error:
        return dashboard_red();
    case BannerTone::info:
    default:
        return dashboard_accent();
    }
}

void set_banner(AppState& app, const std::string& text, BannerTone tone) {
    app.banner_text = text;
    app.banner_tone = tone;
    js_set_status(text.c_str());
}

json scoped_body(const AppState& app, json body = json::object()) {
    if (!app.project.empty()) {
        body["project"] = app.project;
    }
    return body;
}

std::vector<std::string> split_csv_tags(const std::string& csv) {
    std::vector<std::string> tags;
    std::stringstream stream(csv);
    std::string item;
    while (std::getline(stream, item, ',')) {
        const auto start = item.find_first_not_of(" \t\r\n");
        const auto end = item.find_last_not_of(" \t\r\n");
        if (start == std::string::npos || end == std::string::npos) {
            continue;
        }
        tags.push_back(item.substr(start, end - start + 1));
    }
    return tags;
}

struct RequestContext {
    AppState* app{nullptr};
    std::string method;
    std::string path;
    std::string body;
    std::vector<std::string> headers_storage;
    std::vector<const char*> headers;
    std::function<void(const HttpResponse&)> callback;
};

void request_complete(emscripten_fetch_t* fetch, bool success) {
    std::unique_ptr<RequestContext> context(static_cast<RequestContext*>(fetch->userData));
    auto& app = *context->app;
    HttpResponse response;
    response.status = static_cast<int>(fetch->status);
    response.ok = success && fetch->status >= 200 && fetch->status < 300;
    response.unauthorized = fetch->status == 401;
    if (fetch->data != nullptr && fetch->numBytes > 0) {
        response.text.assign(fetch->data, fetch->numBytes);
        try {
            response.payload = json::parse(response.text);
        } catch (...) {
            response.payload = json::object();
        }
    }
    if (!response.ok && response.error_message.empty()) {
        if (response.unauthorized) {
            response.error_message = "unauthorized";
        } else if (!response.text.empty()) {
            response.error_message = excerpt(response.text, 200);
        } else {
            response.error_message = "request failed";
        }
    }
    if (response.unauthorized) {
        app.auth_required = true;
    }
    app.pending_requests = std::max(0, app.pending_requests - 1);
    if (app.pending_requests == 0 && success) {
        js_set_status("Ready");
    }
    if (context->callback) {
        context->callback(response);
    }
    emscripten_fetch_close(fetch);
}

void on_fetch_success(emscripten_fetch_t* fetch) {
    request_complete(fetch, true);
}

void on_fetch_error(emscripten_fetch_t* fetch) {
    request_complete(fetch, false);
}

void begin_request(
    AppState& app,
    const std::string& method,
    const std::string& path,
    const json* body,
    std::function<void(const HttpResponse&)> callback) {
    auto context = std::make_unique<RequestContext>();
    context->app = &app;
    context->method = method;
    context->path = path;
    context->callback = std::move(callback);
    if (body != nullptr) {
        context->body = body->dump();
    }

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::snprintf(attr.requestMethod, sizeof(attr.requestMethod), "%s", method.c_str());
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.onsuccess = on_fetch_success;
    attr.onerror = on_fetch_error;
    context->headers_storage = {
        "Accept",
        "application/json",
    };
    if (body != nullptr) {
        context->headers_storage.push_back("Content-Type");
        context->headers_storage.push_back("application/json");
    }
    if (!app.auth_token.empty()) {
        context->headers_storage.push_back("Authorization");
        context->headers_storage.push_back("Bearer " + app.auth_token);
    }
    context->headers.reserve(context->headers_storage.size() + 1);
    for (const auto& header : context->headers_storage) {
        context->headers.push_back(header.c_str());
    }
    context->headers.push_back(nullptr);
    attr.requestHeaders = context->headers.data();
    if (!context->body.empty()) {
        attr.requestData = context->body.c_str();
        attr.requestDataSize = context->body.size();
    }
    attr.userData = context.get();

    app.pending_requests += 1;
    js_set_status(("Loading " + path + "...").c_str());
    emscripten_fetch(&attr, path.c_str());
    context.release();
}

void get_json(AppState& app, const std::string& path, std::function<void(const HttpResponse&)> callback) {
    begin_request(app, "GET", path, nullptr, std::move(callback));
}

void post_json(AppState& app, const std::string& path, const json& body, std::function<void(const HttpResponse&)> callback) {
    begin_request(app, "POST", path, &body, std::move(callback));
}

void persist_settings(const AppState& app) {
    local_storage_set("qtmem.dashboard.project", app.project);
    local_storage_set("qtmem.dashboard.auth_token", app.auth_token);
    local_storage_set(kThemeKey, std::to_string(app.theme_index));
    local_storage_set(kUiScaleKey, std::to_string(app.ui_scale));
    local_storage_set(kWorkspaceGroupKey, std::to_string(app.active_group));
    local_storage_set(kPanelVisibilityVersionKey, std::to_string(kPanelVisibilityVersion));
    local_storage_set(kPanelVisibilityKey, window_visibility_json(app.windows));
}

const char* docking_layout_key(bool compact_layout) {
    return compact_layout ? kDockLayoutCompactKey : kDockLayoutDesktopKey;
}

std::string load_docking_layout(bool compact_layout) {
    const auto stored_version = local_storage_get_int(kDockLayoutVersionKey, -1);
    if (stored_version != kDockLayoutVersion) {
        return {};
    }
    return local_storage_get(docking_layout_key(compact_layout));
}

void save_docking_layout(const char* data, std::size_t size, bool compact_layout) {
    local_storage_set(kDockLayoutVersionKey, std::to_string(kDockLayoutVersion));
    local_storage_set(kDockLayoutModeKey, compact_layout ? "1" : "0");
    if (data == nullptr || size == 0) {
        local_storage_set(docking_layout_key(compact_layout), "");
        return;
    }
    local_storage_set(docking_layout_key(compact_layout), std::string(data, size));
}

void persist_docking_layout(AppState& app) {
    ImGuiIO& io = ImGui::GetIO();
    const bool should_save = io.WantSaveIniSettings || app.dock_layout_save_pending || app.last_saved_dock_layout.empty();
    if (!should_save) {
        return;
    }

    std::size_t size = 0;
    const char* data = ImGui::SaveIniSettingsToMemory(&size);
    if (data != nullptr && size > 0) {
        const std::string serialized(data, size);
        if (serialized != app.last_saved_dock_layout || app.dock_layout_save_pending) {
            save_docking_layout(data, size, app.compact_layout);
            app.last_saved_dock_layout = serialized;
        }
    }
    io.WantSaveIniSettings = false;
    app.dock_layout_save_pending = false;
}

ImFont* load_monospace_font() {
    ImGuiIO& io = ImGui::GetIO();
    static const ImWchar ranges[] = {
        0x0020, 0x00FF,
        0x2500, 0x257F,
        0x2580, 0x259F,
        0x25A0, 0x25FF,
        0x2600, 0x26FF,
        0x2700, 0x27BF,
        0x2900, 0x297F,
        0x2B00, 0x2BFF,
        0x3000, 0x303F,
        0xE000, 0xE0FF,
        0,
    };

    ImFontConfig config{};
    config.MergeMode = false;
    config.GlyphRanges = ranges;
    ImFont* font = io.Fonts->AddFontFromFileTTF("static/fonts/ned/JetBrainsMonoNL-Regular.ttf", kMonospaceFontSize, &config, ranges);
    if (font == nullptr) {
        return nullptr;
    }

    ImFontConfig fallback_config{};
    fallback_config.MergeMode = true;
    static const ImWchar braille_ranges[] = {0x2800, 0x28FF, 0};
    io.Fonts->AddFontFromFileTTF("static/fonts/ned/DejaVuSans.ttf", kMonospaceFontSize, &fallback_config, braille_ranges);
    return font;
}

void apply_theme(ThemePreset preset, float ui_scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle{};
    switch (preset) {
    case ThemePreset::dark:
        ImGui::StyleColorsDark(&style);
        break;
    case ThemePreset::classic:
        ImGui::StyleColorsClassic(&style);
        break;
    case ThemePreset::light:
    case ThemePreset::count:
    default:
        ImGui::StyleColorsLight(&style);
        break;
    }

    style.FrameRounding = 11.0f;
    style.ChildRounding = 18.0f;
    style.PopupRounding = 14.0f;
    style.ScrollbarRounding = 10.0f;
    style.GrabRounding = 999.0f;
    style.TabRounding = 12.0f;
    style.WindowRounding = 20.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.WindowPadding = ImVec2(16.0f, 14.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.CellPadding = ImVec2(8.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 9.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.SeparatorTextPadding = ImVec2(8.0f, 6.0f);
    style.SeparatorTextBorderSize = 1.0f;
    style.IndentSpacing = 20.0f;
    style.DockingSeparatorSize = 2.0f;
    style.ScaleAllSizes(ui_scale);
    style.FontScaleMain = ui_scale;

    auto& colors = style.Colors;
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.18f, 0.29f, 0.41f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.16f, 0.70f, 0.84f, 0.54f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.20f, 0.76f, 0.92f, 0.82f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.35f, 0.45f, 0.55f, 0.28f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.35f, 0.45f, 0.55f, 0.12f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.16f, 0.70f, 0.84f, 0.24f);
    if (preset == ThemePreset::light) {
        colors[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.92f, 0.88f, 1.0f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.98f, 0.97f, 0.95f, 0.94f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.96f, 0.95f, 0.92f, 0.98f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.89f, 0.91f, 0.94f, 0.92f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.77f, 0.86f, 0.93f, 0.96f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.67f, 0.80f, 0.91f, 0.98f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.85f, 0.88f, 0.91f, 1.0f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.76f, 0.84f, 0.91f, 1.0f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.93f, 0.94f, 0.96f, 1.0f);
        colors[ImGuiCol_Header] = ImVec4(0.75f, 0.84f, 0.92f, 0.82f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.68f, 0.81f, 0.91f, 0.94f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.59f, 0.75f, 0.89f, 0.97f);
        colors[ImGuiCol_Button] = ImVec4(0.72f, 0.83f, 0.91f, 1.0f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.61f, 0.77f, 0.88f, 1.0f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.52f, 0.71f, 0.86f, 1.0f);
        colors[ImGuiCol_Tab] = ImVec4(0.84f, 0.88f, 0.92f, 1.0f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.68f, 0.80f, 0.89f, 1.0f);
        colors[ImGuiCol_TabSelected] = ImVec4(0.66f, 0.79f, 0.90f, 1.0f);
        colors[ImGuiCol_TabDimmed] = ImVec4(0.87f, 0.90f, 0.93f, 1.0f);
        colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.75f, 0.84f, 0.91f, 1.0f);
        colors[ImGuiCol_DockingPreview] = ImVec4(0.29f, 0.56f, 0.79f, 0.68f);
        colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.91f, 0.89f, 0.85f, 1.0f);
        colors[ImGuiCol_Border] = ImVec4(0.20f, 0.27f, 0.34f, 0.24f);
        colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.27f, 0.34f, 0.18f);
    } else if (preset == ThemePreset::dark) {
        colors[ImGuiCol_Text] = ImVec4(0.93f, 0.96f, 1.00f, 0.96f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.62f, 0.71f, 0.88f);
        colors[ImGuiCol_WindowBg] = ImVec4(0.04f, 0.06f, 0.10f, 0.94f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.07f, 0.09f, 0.14f, 0.88f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.08f, 0.12f, 0.98f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.15f, 0.22f, 0.95f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.23f, 0.33f, 0.98f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.12f, 0.34f, 0.46f, 1.0f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.08f, 0.12f, 0.96f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.12f, 0.18f, 0.98f);
        colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.05f, 0.07f, 0.11f, 0.76f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.04f, 0.06f, 0.10f, 0.90f);
        colors[ImGuiCol_Header] = ImVec4(0.10f, 0.25f, 0.34f, 0.82f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.12f, 0.42f, 0.54f, 0.92f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.13f, 0.56f, 0.68f, 1.0f);
        colors[ImGuiCol_Button] = ImVec4(0.10f, 0.45f, 0.58f, 0.90f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.13f, 0.60f, 0.75f, 0.96f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.08f, 0.36f, 0.49f, 1.0f);
        colors[ImGuiCol_Tab] = ImVec4(0.08f, 0.13f, 0.20f, 0.92f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.12f, 0.35f, 0.47f, 0.98f);
        colors[ImGuiCol_TabSelected] = ImVec4(0.10f, 0.29f, 0.39f, 1.0f);
        colors[ImGuiCol_TabDimmed] = ImVec4(0.06f, 0.09f, 0.14f, 0.72f);
        colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.08f, 0.19f, 0.28f, 0.92f);
        colors[ImGuiCol_DockingPreview] = ImVec4(0.14f, 0.72f, 0.88f, 0.70f);
        colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.02f, 0.03f, 0.06f, 0.0f);
        colors[ImGuiCol_Border] = ImVec4(0.47f, 0.66f, 0.82f, 0.16f);
        colors[ImGuiCol_Separator] = ImVec4(0.47f, 0.66f, 0.82f, 0.14f);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
        colors[ImGuiCol_CheckMark] = ImVec4(0.32f, 0.86f, 0.73f, 1.0f);
        colors[ImGuiCol_SliderGrab] = ImVec4(0.24f, 0.76f, 0.90f, 0.95f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.36f, 0.90f, 0.74f, 1.0f);
    }
}

void set_ui_scale(AppState& app, float scale) {
    app.ui_scale = std::clamp(scale, kUiScaleMin, kUiScaleMax);
    apply_theme(static_cast<ThemePreset>(app.theme_index), app.ui_scale);
}

void apply_workspace_group(AppState& app, WorkspaceGroup group);

void load_persistent_ui_state(AppState& app) {
    app.theme_index = std::clamp(local_storage_get_int(kThemeKey, static_cast<int>(ThemePreset::dark)), 0, static_cast<int>(ThemePreset::count) - 1);
    app.active_group = std::clamp(local_storage_get_int(kWorkspaceGroupKey, static_cast<int>(WorkspaceGroup::overview)), 0, static_cast<int>(WorkspaceGroup::count) - 1);
    app.ui_scale = local_storage_get_float(kUiScaleKey, kDefaultUiScale);
    apply_theme(static_cast<ThemePreset>(app.theme_index), app.ui_scale);

    app.compact_layout = local_storage_get_int(kDockLayoutModeKey, 0) != 0;
    const auto stored_layout = load_docking_layout(app.compact_layout);
    if (!stored_layout.empty()) {
        ImGui::LoadIniSettingsFromMemory(stored_layout.c_str(), stored_layout.size());
        app.last_saved_dock_layout = stored_layout;
        app.dockspace_configured = true;
    } else {
        app.dock_layout_save_pending = true;
    }

    if (local_storage_get_int(kPanelVisibilityVersionKey, -1) == kPanelVisibilityVersion) {
        app.windows = window_visibility_from_json(local_storage_get(kPanelVisibilityKey));
    } else {
        app.windows = default_window_visibility();
        apply_workspace_group(app, static_cast<WorkspaceGroup>(app.active_group));
    }
}

void refresh_overview(AppState& app);
void refresh_explorer(AppState& app, bool refresh_selection);
void refresh_sources(AppState& app);
void refresh_agents(AppState& app);
void refresh_sessions(AppState& app);
void refresh_graph(AppState& app);
void refresh_system(AppState& app);

void sync_project_scope(AppState& app) {
    app.scope.project = app.project;
}

void refresh_all(AppState& app) {
    sync_project_scope(app);
    persist_settings(app);
    refresh_overview(app);
    refresh_explorer(app, true);
    refresh_graph(app);
    refresh_sources(app);
    refresh_agents(app);
    refresh_sessions(app);
    refresh_system(app);
}

void apply_workspace_group(AppState& app, WorkspaceGroup group) {
    app.active_group = static_cast<int>(group);
    WindowVisibility visibility{};
    visibility.workspace_nav = true;
    visibility.status = true;
    visibility.overview = false;
    visibility.editor = false;
    visibility.explorer = false;
    visibility.detail = false;
    visibility.graph = false;
    visibility.sources = false;
    visibility.agents = false;
    visibility.sessions = false;
    visibility.context = false;
    visibility.system = false;
    visibility.logs = false;

    switch (group) {
    case WorkspaceGroup::overview:
        visibility.overview = true;
        visibility.graph = true;
        visibility.explorer = true;
        break;
    case WorkspaceGroup::explore:
        visibility.graph = true;
        visibility.explorer = true;
        visibility.detail = !app.selected_memory_id.empty();
        break;
    case WorkspaceGroup::capture:
        visibility.editor = true;
        visibility.context = true;
        visibility.sources = true;
        break;
    case WorkspaceGroup::review:
        visibility.overview = true;
        visibility.explorer = true;
        visibility.detail = true;
        visibility.sources = true;
        break;
    case WorkspaceGroup::operations:
        visibility.agents = true;
        visibility.sessions = true;
        visibility.system = true;
        visibility.logs = true;
        break;
    case WorkspaceGroup::count:
    default:
        visibility = default_window_visibility();
        break;
    }

    app.windows = visibility;
    switch (group) {
    case WorkspaceGroup::overview:
    case WorkspaceGroup::review:
        app.pending_focus_window = kOverviewWindowTitle;
        break;
    case WorkspaceGroup::explore:
        app.pending_focus_window = kExplorerWindowTitle;
        break;
    case WorkspaceGroup::capture:
        app.pending_focus_window = kEditorWindowTitle;
        break;
    case WorkspaceGroup::operations:
        app.pending_focus_window = kAgentsWindowTitle;
        break;
    case WorkspaceGroup::count:
    default:
        app.pending_focus_window.clear();
        break;
    }
    app.dock_layout_save_pending = true;
    persist_settings(app);
}

void rebuild_graph_layout(AppState& app) {
    app.graph_layout.clear();
    if (!app.graph.contains("nodes") || !app.graph.at("nodes").is_array()) {
        return;
    }

    std::map<std::string, std::vector<json>> grouped_nodes;
    if (app.graph.at("nodes").is_array()) {
        for (const auto& node : app.graph.at("nodes")) {
            grouped_nodes[json_string(node, "group_id", "cluster-1")].push_back(node);
        }
    }

    std::vector<std::pair<std::string, std::vector<json>>> groups;
    for (auto& [group_id, nodes] : grouped_nodes) {
        std::sort(nodes.begin(), nodes.end(), [](const json& lhs, const json& rhs) {
            const auto lhs_degree = json_int(lhs, "degree", 0);
            const auto rhs_degree = json_int(rhs, "degree", 0);
            if (lhs_degree != rhs_degree) {
                return lhs_degree > rhs_degree;
            }
            return json_string(lhs, "id") < json_string(rhs, "id");
        });
        groups.push_back({group_id, nodes});
    }
    std::sort(groups.begin(), groups.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second.size() != rhs.second.size()) {
            return lhs.second.size() > rhs.second.size();
        }
        return lhs.first < rhs.first;
    });

    const float cluster_radius = groups.size() <= 1 ? 0.0f : std::max(260.0f, 150.0f * std::sqrt(static_cast<float>(groups.size())));
    for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
        const float angle = groups.size() <= 1 ? 0.0f : (2.0f * kPi * static_cast<float>(group_index) / static_cast<float>(groups.size()));
        const ImVec2 cluster_center{
            cluster_radius * std::cos(angle),
            cluster_radius * std::sin(angle),
        };
        const auto& nodes = groups[group_index].second;
        if (nodes.size() == 1) {
            const auto node_id = json_string(nodes.front(), "id");
            app.graph_layout[node_id] = GraphNodeLayout{
                cluster_center,
                20.0f + std::min(12, json_int(nodes.front(), "degree", 0)) * 1.2f,
                groups[group_index].first,
                json_int(nodes.front(), "group_index", static_cast<int>(group_index)),
            };
            continue;
        }
        std::size_t consumed = 0;
        std::size_t ring = 0;
        while (consumed < nodes.size()) {
            const std::size_t ring_capacity = ring == 0 ? 6 : 6 + ring * 6;
            const std::size_t ring_count = std::min(ring_capacity, nodes.size() - consumed);
            const float ring_radius = 70.0f + static_cast<float>(ring) * 52.0f;
            for (std::size_t index = 0; index < ring_count; ++index) {
                const auto& node = nodes[consumed + index];
                const float ring_angle = (2.0f * kPi * static_cast<float>(index) / static_cast<float>(ring_count)) + angle * 0.35f;
                const ImVec2 world{
                    cluster_center.x + ring_radius * std::cos(ring_angle),
                    cluster_center.y + ring_radius * std::sin(ring_angle),
                };
                const auto node_id = json_string(node, "id");
                app.graph_layout[node_id] = GraphNodeLayout{
                    world,
                    17.0f + std::min(12, json_int(node, "degree", 0)) * 1.15f,
                    groups[group_index].first,
                    json_int(node, "group_index", static_cast<int>(group_index)),
                };
            }
            consumed += ring_count;
            ring += 1;
        }
    }
    app.graph_view.fit_on_next_draw = true;
}

bool graph_layout_bounds(const AppState& app, ImVec2& min_world, ImVec2& max_world) {
    if (app.graph_layout.empty()) {
        return false;
    }

    bool first = true;
    for (const auto& [node_id, layout] : app.graph_layout) {
        (void)node_id;
        const ImVec2 local_min{layout.world.x - layout.radius - 28.0f, layout.world.y - layout.radius - 24.0f};
        const ImVec2 local_max{layout.world.x + layout.radius + 140.0f, layout.world.y + layout.radius + 24.0f};
        if (first) {
            min_world = local_min;
            max_world = local_max;
            first = false;
            continue;
        }
        min_world.x = std::min(min_world.x, local_min.x);
        min_world.y = std::min(min_world.y, local_min.y);
        max_world.x = std::max(max_world.x, local_max.x);
        max_world.y = std::max(max_world.y, local_max.y);
    }
    return !first;
}

void load_memory_detail(AppState& app, const std::string& memory_id, bool focus = true);

void refresh_overview(AppState& app) {
    get_json(app, "/mcp/status", [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Failed to read /mcp/status: " + response.error_message, BannerTone::error);
            return;
        }
        app.service_status = response.payload;
        app.auth_required = response.payload.value("auth_required", app.auth_required);
    });

    get_json(app, "/mcp/metrics", [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Failed to read /mcp/metrics: " + response.error_message, BannerTone::error);
            return;
        }
        app.metrics = response.payload;
    });

    post_json(app, "/dashboard/summary", scoped_body(app), [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Failed to load dashboard summary: " + response.error_message, BannerTone::error);
            return;
        }
        app.summary = response.payload.value("dashboard_summary", json::object());
        set_banner(
            app,
            "Loaded dashboard summary for " + (app.project.empty() ? std::string("all projects") : app.project) + ".",
            BannerTone::success);
    });
}

void refresh_explorer(AppState& app, bool refresh_selection) {
    json body = scoped_body(app);
    if (!app.explorer.query.empty()) {
        body["query"] = app.explorer.query;
    }
    if (!app.explorer.memory_type.empty()) {
        body["type"] = app.explorer.memory_type;
    }
    if (!app.explorer.status.empty()) {
        body["status"] = app.explorer.status;
    }
    body["limit"] = app.explorer.limit;
    body["include_superseded"] = app.explorer.include_superseded;
    post_json(app, "/mcp/memory/query", body, [&app, refresh_selection](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Memory query failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.explorer_entries.clear();
        if (response.payload.contains("entries") && response.payload.at("entries").is_array()) {
            for (const auto& entry : response.payload.at("entries")) {
                app.explorer_entries.push_back(entry);
            }
        }
        if (refresh_selection) {
            if (app.selected_memory_id.empty() && !app.explorer_entries.empty()) {
                app.selected_memory_id = json_string(app.explorer_entries.front(), "id");
            }
            if (!app.selected_memory_id.empty()) {
                load_memory_detail(app, app.selected_memory_id, false);
            }
        }
        set_banner(app, "Loaded " + std::to_string(app.explorer_entries.size()) + " memory entries.", BannerTone::success);
    });
}

void load_memory_detail(AppState& app, const std::string& memory_id, bool focus) {
    if (memory_id.empty()) {
        return;
    }
    if (focus) {
        app.windows.detail = true;
        app.pending_focus_window = kDetailWindowTitle;
    }
    app.selected_memory_id = memory_id;
    app.selected_memory_detail = json::object();
    app.dream_review_preview = json::object();
    ++app.dream_review_generation;
    app.dream_review_pending = false;
    post_json(app, "/dashboard/memory", scoped_body(app, {{"id", memory_id}}), [&app, memory_id](const HttpResponse& response) {
        if (app.selected_memory_id != memory_id) return;
        if (!response.ok) {
            set_banner(app, "Memory detail failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.selected_memory_detail = response.payload.value("memory", json::object());
    });
    post_json(app, "/dashboard/memory/history", scoped_body(app, {{"id", memory_id}}), [&app, memory_id](const HttpResponse& response) {
        if (app.selected_memory_id != memory_id) return;
        if (!response.ok) {
            set_banner(app, "Memory history failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.selected_memory_history = response.payload;
    });
}

void refresh_sources(AppState& app) {
    post_json(app, "/dashboard/sources", scoped_body(app), [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Source list failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.sources.clear();
        if (response.payload.contains("sources") && response.payload.at("sources").is_array()) {
            for (const auto& source : response.payload.at("sources")) {
                app.sources.push_back(source);
            }
        }
        if (app.selected_source_id.empty() && !app.sources.empty()) {
            app.selected_source_id = json_string(app.sources.front(), "source_id");
        }
        if (!app.selected_source_id.empty()) {
            post_json(app, "/dashboard/source", scoped_body(app, {{"source_id", app.selected_source_id}}), [&app](const HttpResponse& detail_response) {
                if (detail_response.ok) {
                    app.selected_source_detail = detail_response.payload;
                }
            });
        }
    });
}

void load_source_detail(AppState& app, const std::string& source_id) {
    if (source_id.empty()) {
        return;
    }
    app.selected_source_id = source_id;
    post_json(app, "/dashboard/source", scoped_body(app, {{"source_id", source_id}}), [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Source detail failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.selected_source_detail = response.payload;
    });
}

void refresh_agents(AppState& app) {
    post_json(app, "/dashboard/agents", scoped_body(app), [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Agent summary failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.agents.clear();
        if (response.payload.contains("agents") && response.payload.at("agents").is_array()) {
            for (const auto& agent : response.payload.at("agents")) {
                app.agents.push_back(agent);
            }
        }
        if (app.selected_agent_id.empty() && !app.agents.empty()) {
            app.selected_agent_id = json_string(app.agents.front(), "agent_id");
        }
        if (!app.selected_agent_id.empty()) {
            post_json(app, "/dashboard/agent", scoped_body(app, {{"agent_id", app.selected_agent_id}}), [&app](const HttpResponse& detail_response) {
                if (detail_response.ok) {
                    app.selected_agent_detail = detail_response.payload;
                }
            });
        }
    });
}

void load_agent_detail(AppState& app, const std::string& agent_id) {
    if (agent_id.empty()) {
        return;
    }
    app.selected_agent_id = agent_id;
    post_json(app, "/dashboard/agent", scoped_body(app, {{"agent_id", agent_id}}), [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Agent detail failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.selected_agent_detail = response.payload;
    });
}

void refresh_sessions(AppState& app) {
    post_json(app, "/dashboard/sessions", scoped_body(app), [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Session summary failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.sessions.clear();
        if (response.payload.contains("sessions") && response.payload.at("sessions").is_array()) {
            for (const auto& session : response.payload.at("sessions")) {
                app.sessions.push_back(session);
            }
        }
        if (app.selected_session_id.empty() && !app.sessions.empty()) {
            app.selected_session_id = json_string(app.sessions.front(), "session");
        }
        if (!app.selected_session_id.empty()) {
            post_json(app, "/dashboard/session", scoped_body(app, {{"session", app.selected_session_id}}), [&app](const HttpResponse& detail_response) {
                if (detail_response.ok) {
                    app.selected_session_detail = detail_response.payload;
                }
            });
        }
    });
}

void load_session_detail(AppState& app, const std::string& session_id) {
    if (session_id.empty()) {
        return;
    }
    app.selected_session_id = session_id;
    post_json(app, "/dashboard/session", scoped_body(app, {{"session", session_id}}), [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Session detail failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.selected_session_detail = response.payload;
    });
}

void refresh_graph(AppState& app) {
    json body = scoped_body(app);
    if (!app.graph_filters.query.empty()) {
        body["query"] = app.graph_filters.query;
    }
    if (!app.graph_filters.memory_type.empty()) {
        body["memory_type"] = app.graph_filters.memory_type;
    }
    if (!app.graph_filters.status.empty()) {
        body["status"] = app.graph_filters.status;
    }
    if (!app.graph_filters.edge_type.empty()) {
        body["edge_type"] = app.graph_filters.edge_type;
    }
    body["limit"] = app.graph_filters.limit;
    body["include_isolates"] = app.graph_filters.include_isolates;
    post_json(app, "/dashboard/graph", body, [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Graph load failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.graph = response.payload.value("graph", json::object());
        app.hidden_graph_groups.clear();
        if (app.graph.contains("nodes") && app.graph.at("nodes").is_array() && !app.graph.at("nodes").empty()) {
            app.selected_graph_node_id = json_string(app.graph.at("nodes").front(), "id");
        } else {
            app.selected_graph_node_id.clear();
        }
        rebuild_graph_layout(app);
        set_banner(app, "Loaded graph with " + std::to_string(json_int(app.graph.value("stats", json::object()), "nodes", 0)) + " nodes.", BannerTone::success);
    });
}

void refresh_system(AppState& app) {
    get_json(app, "/mcp/logs", [&app](const HttpResponse& response) {
        if (response.ok) {
            app.logs = response.payload;
        }
    });
    get_json(app, "/mcp/ai/config", [&app](const HttpResponse& response) {
        if (response.ok) {
            app.ai_config = response.payload;
            app.system.ai_config_draft = pretty_json(response.payload);
        }
    });
    get_json(app, "/mcp/ai/metadata", [&app](const HttpResponse& response) {
        if (response.ok) {
            app.ai_metadata = response.payload;
        }
    });
    post_json(app, "/mcp/postprocess/scheduler/status", json::object(), [&app](const HttpResponse& response) {
        if (response.ok) {
            app.scheduler = response.payload;
        }
    });
}

void write_memory(AppState& app) {
    json body = scoped_body(app);
    if (!app.scope.session.empty()) {
        body["session"] = app.scope.session;
    }
    if (!app.scope.agent_id.empty()) {
        body["agent_id"] = app.scope.agent_id;
    }
    if (!app.scope.conversation_id.empty()) {
        body["conversation_id"] = app.scope.conversation_id;
    }
    if (!app.scope.task.empty()) {
        body["task"] = app.scope.task;
    }
    body["type"] = app.editor.memory_type;
    body["status"] = app.editor.status;
    body["confidence"] = app.editor.confidence;
    body["source"] = app.editor.source;
    body["source_type"] = app.editor.source_type;
    body["source_uri"] = app.editor.source_uri;
    body["content"] = app.editor.content;
    body["tags"] = split_csv_tags(app.editor.tags_csv);
    post_json(app, "/mcp/memory/write", body, [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Memory write failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.editor.content.clear();
        set_banner(app, "Stored memory " + response.payload.value("id", std::string{}) + ".", BannerTone::success);
        refresh_overview(app);
        refresh_explorer(app, true);
        refresh_graph(app);
        refresh_sources(app);
    });
}

void process_raw_text(AppState& app) {
    json body = scoped_body(app);
    if (!app.scope.session.empty()) {
        body["session"] = app.scope.session;
    }
    if (!app.scope.agent_id.empty()) {
        body["agent_id"] = app.scope.agent_id;
    }
    if (!app.scope.conversation_id.empty()) {
        body["conversation_id"] = app.scope.conversation_id;
    }
    body["type"] = app.editor.memory_type;
    body["status"] = app.editor.status;
    body["source"] = "process:text-block";
    body["source_type"] = "processed_text";
    body["content"] = app.editor.raw_text;
    body["confidence"] = app.editor.confidence;
    body["tags"] = split_csv_tags(app.editor.tags_csv);
    body["dry_run"] = app.editor.dry_run;
    body["stage_for_review"] = app.editor.stage_for_review;
    post_json(app, "/mcp/memory/process", body, [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "memory.process failed: " + response.error_message, BannerTone::error);
            return;
        }
        set_banner(
            app,
            "Processed raw text into " + std::to_string(response.payload.value("count", 0)) + " candidate memories.",
            BannerTone::success);
        if (!app.editor.dry_run) {
            refresh_overview(app);
            refresh_explorer(app, true);
            refresh_graph(app);
            refresh_sources(app);
        }
    });
}

void memory_action(AppState& app, const std::string& path, const std::string& success_text, json body) {
    if (app.selected_memory_id.empty()) {
        return;
    }
    body["id"] = app.selected_memory_id;
    post_json(app, path, body, [&app, success_text](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Memory action failed: " + response.error_message, BannerTone::error);
            return;
        }
        set_banner(app, success_text, BannerTone::success);
        refresh_overview(app);
        refresh_explorer(app, true);
        load_memory_detail(app, app.selected_memory_id);
        refresh_graph(app);
        refresh_sources(app);
    });
}

void preview_dream_review(AppState& app, const std::string& action) {
    const auto subject = app.selected_memory_id;
    const auto generation = ++app.dream_review_generation;
    app.dream_review_pending = true;
    app.dream_review_preview = json::object();
    post_json(app, "/dashboard/review/preview", {{"id", subject}, {"action", action}},
        [&app, subject, action, generation](const HttpResponse& response) {
            if (generation != app.dream_review_generation || app.selected_memory_id != subject) return;
            app.dream_review_pending = false;
            if (!response.ok) {
                set_banner(app, "Dream review preview failed: " + response.error_message, BannerTone::error);
                return;
            }
            const auto& preview = response.payload;
            if (!preview.is_object() || !preview.contains("review_id") ||
                !preview.at("review_id").is_string() || preview.at("review_id").get_ref<const std::string&>().empty() ||
                !preview.contains("review_revision") || !preview.at("review_revision").is_number_unsigned() ||
                !preview.contains("idempotency_token") || !preview.at("idempotency_token").is_string() ||
                preview.at("idempotency_token").get_ref<const std::string&>().empty() ||
                !preview.contains("applicable") || !preview.at("applicable").is_boolean()) {
                set_banner(app, "Dream review preview was incomplete. Refresh and try again.", BannerTone::error);
                return;
            }
            app.dream_review_subject = subject;
            app.dream_review_action = action;
            app.dream_review_preview = response.payload;
        });
}

void apply_dream_review(AppState& app) {
    const auto preview = app.dream_review_preview;
    const auto subject = app.dream_review_subject;
    const auto generation = app.dream_review_generation;
    app.dream_review_pending = true;
    post_json(app, "/dashboard/review/apply",
        {{"review_id", preview.at("review_id")}, {"action", app.dream_review_action},
         {"expected_revision", preview.at("review_revision")},
         {"idempotency_key", preview.at("idempotency_token")}, {"confirm", true}},
        [&app, subject, generation](const HttpResponse& response) {
            if (generation != app.dream_review_generation || app.selected_memory_id != subject) return;
            app.dream_review_pending = false;
            app.dream_review_preview = json::object();
            if (!response.ok) {
                set_banner(app, "Dream review failed: " + response.error_message + ". Preview again before retrying.", BannerTone::error);
                return;
            }
            set_banner(app, "Dream review applied.", BannerTone::success);
            refresh_overview(app);
            refresh_explorer(app, true);
            if (app.selected_memory_id == subject) load_memory_detail(app, subject, false);
            refresh_graph(app);
            refresh_sources(app);
        });
}

void build_context(AppState& app) {
    json body = scoped_body(app);
    body["task"] = app.context.task;
    body["query"] = app.context.query;
    body["budget_tokens"] = app.context.budget_tokens;
    body["recall_policy"] = app.context.recall_policy;
    body["include_linked_tools"] = app.context.include_linked_tools;
    post_json(app, "/context/build", body, [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Context build failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.context.result = response.payload;
        set_banner(app, "Built context pack.", BannerTone::success);
    });
}

void recheck_source(AppState& app) {
    if (app.selected_source_id.empty()) {
        return;
    }
    post_json(app, "/sources/recheck", scoped_body(app, {{"source_id", app.selected_source_id}}), [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Source recheck failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.selected_source_detail = response.payload;
        refresh_sources(app);
        set_banner(app, "Rechecked source " + app.selected_source_id + ".", BannerTone::success);
    });
}

void start_scheduler(AppState& app) {
    json body = scoped_body(app);
    body["interval_seconds"] = app.system.scheduler_interval_seconds;
    post_json(app, "/mcp/postprocess/scheduler/start", body, [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Scheduler start failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.scheduler = response.payload;
        set_banner(app, "Started background scheduler.", BannerTone::success);
    });
}

void stop_scheduler(AppState& app) {
    post_json(app, "/mcp/postprocess/scheduler/stop", json::object(), [&app](const HttpResponse& response) {
        if (!response.ok) {
            set_banner(app, "Scheduler stop failed: " + response.error_message, BannerTone::error);
            return;
        }
        app.scheduler = response.payload;
        set_banner(app, "Stopped background scheduler.", BannerTone::success);
    });
}

void save_ai_config(AppState& app) {
    auto save_payload = [&app](const json& payload) {
        post_json(app, "/mcp/ai/config", payload, [&app](const HttpResponse& response) {
            if (!response.ok) {
                set_banner(app, "AI config update failed: " + response.error_message, BannerTone::error);
                return;
            }
            app.ai_config = response.payload;
            app.system.ai_config_draft = pretty_json(response.payload);
            set_banner(app, "Saved AI server configuration.", BannerTone::success);
        });
    };

    try {
        const auto payload = json::parse(app.system.ai_config_draft);
        save_payload(payload);
    } catch (const std::exception& ex) {
        set_banner(app, std::string("AI config is not valid JSON: ") + ex.what(), BannerTone::error);
    }
}

int responsive_columns(int maximum, float minimum_width = 210.0f) {
    return std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x /
        (minimum_width * ImGui::GetStyle().FontScaleMain)), 1, maximum);
}

// Continue an action row only when the next control fits inside this panel.
void same_line_if_fits(float width) {
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right) {
        ImGui::SameLine();
    }
}

void next_action(const char* label) {
    same_line_if_fits(ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f);
}

void draw_status_badge(const char* label, const std::string& value, const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, alpha(color, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_Border, alpha(color, 0.20f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::BeginChild(label, ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::TextDisabled("%s", label);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", value.c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

ImVec4 status_chip_color(const std::string& value) {
    const auto lowered = lower_copy(value);
    if (lowered.find("validated") != std::string::npos || lowered == "ok" || lowered == "active") {
        return dashboard_green();
    }
    if (lowered.find("warning") != std::string::npos || lowered.find("hypothesis") != std::string::npos || lowered.find("changed") != std::string::npos) {
        return dashboard_orange();
    }
    if (lowered.find("rejected") != std::string::npos || lowered.find("deprecated") != std::string::npos || lowered.find("conflict") != std::string::npos || lowered.find("missing") != std::string::npos) {
        return dashboard_red();
    }
    if (lowered.find("dream") != std::string::npos) {
        return dashboard_violet();
    }
    return dashboard_accent();
}

void draw_chip(const std::string& text, const ImVec4& color) {
    if (text.empty()) {
        return;
    }
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float text_width = std::max(1.0f, width - 16.0f);
    const ImVec2 text_size = ImGui::CalcTextSize(text.c_str(), nullptr, false, text_width);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::min(width, text_size.x + 16.0f), text_size.y + 6.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
        color_u32(alpha(color, 0.16f)), 6.0f);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(origin.x + 8.0f, origin.y + 3.0f),
        color_u32(color), text.c_str(), nullptr, text_width);
    ImGui::Dummy(size);
}

void draw_tag_cloud(const std::vector<std::string>& tags) {
    if (tags.empty()) {
        ImGui::TextDisabled("No tags.");
        return;
    }
    for (std::size_t index = 0; index < tags.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        if (index != 0) {
            same_line_if_fits(ImGui::CalcTextSize(tags[index].c_str()).x + 16.0f);
        }
        draw_chip(tags[index], status_chip_color(tags[index]));
        ImGui::PopID();
    }
}

void draw_metric_card(const char* label, const std::string& value, const ImVec4& color, const char* detail = "Current scope") {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, alpha(color, 0.22f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::BeginChild(label, ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    const ImVec2 card_max(window_pos.x + window_size.x, window_pos.y + window_size.y);
    draw_rounded_panel_background(draw_list, window_pos, card_max, color, 18.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, alpha(ImGui::GetStyle().Colors[ImGuiCol_Text], 0.64f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    const ImVec2 value_pos = ImGui::GetCursorScreenPos();
    draw_list->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize() * 1.55f,
        value_pos,
        ImGui::GetColorU32(ImGuiCol_Text),
        value.c_str());
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 1.7f));

    ImGui::PushStyleColor(ImGuiCol_Text, alpha(color, 0.92f));
    ImGui::TextWrapped("%s", detail);
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void draw_count_distribution_card(const char* title, const json& counts, const ImVec4& accent) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, alpha(accent, 0.18f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::BeginChild(title, ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
    draw_rounded_panel_background(ImGui::GetWindowDrawList(), min, max, accent, 18.0f);

    ImGui::TextUnformatted(title);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    if (!counts.is_object() || counts.empty()) {
        ImGui::TextWrapped("No memories in this scope yet.");
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        return;
    }

    double max_value = 1.0;
    for (const auto& [_key, value] : counts.items()) {
        if (value.is_number()) {
            max_value = std::max(max_value, value.get<double>());
        }
    }

    int rows = 0;
    for (const auto& [key, value] : counts.items()) {
        if (rows >= 6) {
            break;
        }
        const double raw = value.is_number() ? value.get<double>() : 0.0;
        const float fraction = static_cast<float>(std::clamp(raw / max_value, 0.0, 1.0));
        ImGui::TextWrapped("%s  ·  %.0f", key.c_str(), raw);
        const ImVec2 bar_min = ImGui::GetCursorScreenPos();
        const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        const float height = 7.0f;
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(bar_min, ImVec2(bar_min.x + width, bar_min.y + height), color_u32(ImVec4(1.0f, 1.0f, 1.0f, 0.055f)), 999.0f);
        draw_list->AddRectFilled(bar_min, ImVec2(bar_min.x + width * fraction, bar_min.y + height), color_u32(alpha(accent, 0.88f)), 999.0f);
        ImGui::Dummy(ImVec2(0.0f, height + 8.0f));
        ++rows;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

bool draw_labeled_input(const char* label, const char* id, std::string& value, ImGuiInputTextFlags flags = 0) {
    ImGui::TextDisabled("%s", label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    std::string item_id = "##";
    item_id += id;
    return ImGui::InputText(item_id.c_str(), &value, flags);
}

void draw_empty_notice(const char* title, const char* detail) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
    ImGui::BeginChild(title, ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::TextDisabled("%s", title);
    ImGui::TextWrapped("%s", detail);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void draw_key_value_grid(const json& object) {
    if (!object.is_object()) {
        ImGui::TextUnformatted("No data.");
        return;
    }
    if (ImGui::BeginTable("kv", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& [key, value] : object.items()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextWrapped("%s", key.c_str());
            ImGui::TableSetColumnIndex(1);
            if (value.is_string()) {
                ImGui::TextWrapped("%s", value.get_ref<const std::string&>().c_str());
            } else {
                ImGui::TextWrapped("%s", value.dump().c_str());
            }
        }
        ImGui::EndTable();
    }
}

void draw_json_block(const char* id, const json& value, float height = 180.0f, ImFont* font = nullptr) {
    const auto child_height = height < 0.0f ? 0.0f : height;
    ImGui::BeginChild(id, ImVec2(0.0f, child_height), true, ImGuiWindowFlags_HorizontalScrollbar);
    if (font != nullptr) {
        ImGui::PushFont(font, 0.0f);
    }
    const auto text = pretty_json(value);
    if (text.empty()) {
        ImGui::TextUnformatted("No data.");
    } else {
        ImGui::TextUnformatted(text.c_str());
    }
    if (font != nullptr) {
        ImGui::PopFont();
    }
    ImGui::EndChild();
}

template <std::size_t N>
std::vector<std::string> array_to_vector(const std::array<const char*, N>& values) {
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const char* value : values) {
        result.push_back(value);
    }
    return result;
}

std::vector<std::string> json_string_options(const json& root, const char* key, const std::string& current = {}, bool include_blank = false) {
    std::vector<std::string> values;
    if (include_blank) {
        values.push_back("");
    }
    if (root.contains(key) && root.at(key).is_array()) {
        for (const auto& item : root.at(key)) {
            if (!item.is_string()) {
                continue;
            }
            const auto candidate = item.get<std::string>();
            if (std::find(values.begin(), values.end(), candidate) == values.end()) {
                values.push_back(candidate);
            }
        }
    }
    if (!current.empty() && std::find(values.begin(), values.end(), current) == values.end()) {
        values.push_back(current);
    }
    return values;
}

bool combo_string_vector(const char* label, const std::vector<std::string>& options, std::string& value, const char* empty_label = "all") {
    if (options.empty()) {
        ImGui::TextDisabled("%s: no options", label);
        return false;
    }

    std::size_t current = 0;
    for (std::size_t index = 0; index < options.size(); ++index) {
        if (value == options[index]) {
            current = index;
            break;
        }
    }
    const char* preview = options[current].empty() ? empty_label : options[current].c_str();
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (std::size_t index = 0; index < options.size(); ++index) {
            const bool selected = index == current;
            const char* text = options[index].empty() ? empty_label : options[index].c_str();
            if (ImGui::Selectable(text, selected)) {
                value = options[index];
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void ensure_ai_config_shape(AppState& app) {
    if (!app.ai_config.is_object()) {
        app.ai_config = json::object();
    }
    if (!app.ai_config.contains("services") || !app.ai_config.at("services").is_array()) {
        app.ai_config["services"] = json::array();
    }
    if (!app.ai_config.contains("default_service") || !app.ai_config.at("default_service").is_string()) {
        app.ai_config["default_service"] = "";
    }
    app.ai_config["count"] = app.ai_config.at("services").size();
    if (app.system.selected_ai_service_index < 0) {
        app.system.selected_ai_service_index = 0;
    }
    if (app.system.selected_ai_service_index >= static_cast<int>(app.ai_config.at("services").size())) {
        app.system.selected_ai_service_index = std::max(0, static_cast<int>(app.ai_config.at("services").size()) - 1);
    }
}

void sync_ai_config_draft(AppState& app) {
    ensure_ai_config_shape(app);
    app.system.ai_config_draft = pretty_json(app.ai_config);
}

json* selected_ai_service(AppState& app) {
    ensure_ai_config_shape(app);
    auto& services = app.ai_config.at("services");
    if (services.empty()) {
        return nullptr;
    }
    return &services.at(static_cast<std::size_t>(app.system.selected_ai_service_index));
}

std::vector<std::string> ai_provider_options(const AppState& app, const std::string& current = {}) {
    std::vector<std::string> values = json_string_options(app.ai_metadata, "providers", current, false);
    if (app.ai_metadata.contains("provider_presets") && app.ai_metadata.at("provider_presets").is_object()) {
        for (const auto& [key, _value] : app.ai_metadata.at("provider_presets").items()) {
            if (std::find(values.begin(), values.end(), key) == values.end()) {
                values.push_back(key);
            }
        }
    }
    if (!current.empty() && std::find(values.begin(), values.end(), current) == values.end()) {
        values.push_back(current);
    }
    return values;
}

void apply_service_provider_preset(const AppState& app, json& service) {
    const auto provider = json_string(service, "provider");
    if (provider.empty()) {
        return;
    }
    if (!app.ai_metadata.contains("provider_presets") || !app.ai_metadata.at("provider_presets").is_object()) {
        return;
    }
    if (!app.ai_metadata.at("provider_presets").contains(provider) || !app.ai_metadata.at("provider_presets").at(provider).is_object()) {
        return;
    }
    const auto& preset = app.ai_metadata.at("provider_presets").at(provider);
    if (preset.contains("api")) {
        service["api"] = preset.at("api");
    }
    if (preset.contains("base_url")) {
        service["base_url"] = preset.at("base_url");
    }
    if (preset.contains("api_key_env")) {
        service["api_key_env"] = preset.at("api_key_env");
    }
    if (preset.contains("auth_header")) {
        service["auth_header"] = preset.at("auth_header");
    }
}

void ensure_ai_service_defaults(json& service) {
    if (!service.is_object()) {
        service = json::object();
    }
    if (!service.contains("name") || !service.at("name").is_string()) {
        service["name"] = "";
    }
    if (!service.contains("base_url") || !service.at("base_url").is_string()) {
        service["base_url"] = "";
    }
    if (!service.contains("api_style") || !service.at("api_style").is_string()) {
        service["api_style"] = "";
    }
    if (!service.contains("provider") || !service.at("provider").is_string()) {
        service["provider"] = "";
    }
    if (!service.contains("api") || !service.at("api").is_string()) {
        service["api"] = "";
    }
    if (!service.contains("package") || !service.at("package").is_string()) {
        service["package"] = "";
    }
    if (!service.contains("api_key_env") || !service.at("api_key_env").is_string()) {
        service["api_key_env"] = "";
    }
    if (!service.contains("headers") || !service.at("headers").is_object()) {
        service["headers"] = json::object();
    }
    if (!service.contains("compat") || !service.at("compat").is_object()) {
        service["compat"] = json::object();
    }
    if (!service.contains("model_overrides") || !service.at("model_overrides").is_object()) {
        service["model_overrides"] = json::object();
    }
    if (!service.contains("models") || !service.at("models").is_array()) {
        service["models"] = json::array();
    }
    if (!service.contains("options") || !service.at("options").is_object()) {
        service["options"] = json::object();
    }
    if (!service.contains("enabled") || !service.at("enabled").is_boolean()) {
        service["enabled"] = true;
    }
    if (!service.contains("auth_header") || !service.at("auth_header").is_boolean()) {
        service["auth_header"] = false;
    }
    if (!service.contains("timeout_ms") || !service.at("timeout_ms").is_number_integer()) {
        service["timeout_ms"] = 120000;
    }
}

void ensure_ai_model_defaults(json& model, const json& service) {
    if (!model.is_object()) {
        model = json::object();
    }
    if (!model.contains("id") || !model.at("id").is_string()) {
        model["id"] = "";
    }
    if (!model.contains("name") || !model.at("name").is_string()) {
        model["name"] = "";
    }
    if (!model.contains("api") || !model.at("api").is_string()) {
        model["api"] = json_string(service, "api");
    }
    if (!model.contains("provider") || !model.at("provider").is_string()) {
        model["provider"] = json_string(service, "provider");
    }
    if (!model.contains("input") || !model.at("input").is_array()) {
        model["input"] = json::array({"text"});
    }
    if (!model.contains("reasoning") || !model.at("reasoning").is_boolean()) {
        model["reasoning"] = false;
    }
}

void draw_memory_entries_table(
    const char* id,
    const json& entries,
    const std::string& selected_id,
    const std::function<void(const std::string&)>& on_select) {
    if (!entries.is_array() || entries.empty()) {
        ImGui::TextUnformatted("No memories.");
        return;
    }
    // Content-first rows stay readable in both a full workspace and a narrow
    // inspector. A bounded list leaves following history sections reachable.
    const float height = std::min(420.0f, std::max(150.0f, static_cast<float>(entries.size()) * 145.0f)) * ImGui::GetStyle().FontScaleMain;
    ImGui::BeginChild(id, ImVec2(0.0f, height), ImGuiChildFlags_Borders);
    for (const auto& entry : entries) {
        const auto entry_id = json_string(entry, "id");
        ImGui::PushID(entry_id.c_str());
        const auto type = json_string(entry, "type", json_string(entry, "memory_type", "Memory"));
        const auto status = json_string(entry, "status");
        ImGui::BeginGroup();
        const bool selected = ImGui::Selectable("Open memory", selected_id == entry_id);
        ImGui::TextWrapped("%s · %s", type.c_str(), status.c_str());
        ImGui::TextWrapped("%s", excerpt(json_string(entry, "content"), 280).c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Select this memory for the full content and history.");
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("ID: %s", entry_id.c_str());
        const auto source = json_string(entry, "source", json_string(entry, "source_type"));
        if (!source.empty()) ImGui::TextWrapped("Source: %s", source.c_str());
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        if (selected || ImGui::IsItemClicked()) on_select(entry_id);
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void draw_event_table(const char* id, const json& activity) {
    if (!activity.is_array() || activity.empty()) {
        ImGui::TextUnformatted("No activity.");
        return;
    }
    if (ImGui::BeginTable(id, 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
            ImVec2(0.0f, 220.0f), 800.0f * ImGui::GetStyle().FontScaleMain)) {
        ImGui::TableSetupColumn("Timestamp", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Session", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Conversation", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Summary");
        ImGui::TableHeadersRow();
        for (const auto& event : activity) {
            const auto payload = event.value("payload", json::object());
            std::vector<std::string> summary;
            if (!json_string(payload, "task").empty()) {
                summary.push_back("task: " + json_string(payload, "task"));
            }
            if (payload.contains("used_tokens")) {
                summary.push_back("tokens: " + std::to_string(json_int(payload, "used_tokens", 0)));
            }
            if (!json_string(payload, "query").empty()) {
                summary.push_back("query: " + excerpt(json_string(payload, "query"), 60));
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%lld", event.value("timestamp", 0LL));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(event.value("type", "").c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", payload.value("session", "").c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextWrapped("%s", payload.value("conversation_id", "").c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextWrapped("%s", join_strings(summary).c_str());
        }
        ImGui::EndTable();
    }
}

bool combo_string(const char* label, const std::array<const char*, 9>& options, std::string& value) {
    int current = 0;
    for (std::size_t index = 0; index < options.size(); ++index) {
        if (value == options[index]) {
            current = static_cast<int>(index);
            break;
        }
    }
    bool changed = false;
    if (ImGui::BeginCombo(label, options[current][0] == '\0' ? "all" : options[current])) {
        for (std::size_t index = 0; index < options.size(); ++index) {
            const bool selected = current == static_cast<int>(index);
            const char* text = options[index][0] == '\0' ? "all" : options[index];
            if (ImGui::Selectable(text, selected)) {
                value = options[index];
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

template <std::size_t N>
bool combo_string(const char* label, const std::array<const char*, N>& options, std::string& value) {
    int current = 0;
    for (std::size_t index = 0; index < options.size(); ++index) {
        if (value == options[index]) {
            current = static_cast<int>(index);
            break;
        }
    }
    bool changed = false;
    if (ImGui::BeginCombo(label, options[current][0] == '\0' ? "all" : options[current])) {
        for (std::size_t index = 0; index < options.size(); ++index) {
            const bool selected = current == static_cast<int>(index);
            const char* text = options[index][0] == '\0' ? "all" : options[index];
            if (ImGui::Selectable(text, selected)) {
                value = options[index];
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

template <std::size_t N>
bool draw_labeled_combo_string(const char* label, const char* id, const std::array<const char*, N>& options, std::string& value) {
    ImGui::TextDisabled("%s", label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    std::string combo_id = "##";
    combo_id += id;
    return combo_string(combo_id.c_str(), options, value);
}

ImU32 graph_palette(int index) {
    static const std::array<ImVec4, 8> colors{
        ImVec4(0.18f, 0.78f, 0.88f, 1.0f),
        ImVec4(0.96f, 0.63f, 0.24f, 1.0f),
        ImVec4(0.35f, 0.89f, 0.67f, 1.0f),
        ImVec4(0.63f, 0.42f, 1.00f, 1.0f),
        ImVec4(0.94f, 0.82f, 0.29f, 1.0f),
        ImVec4(0.30f, 0.58f, 0.95f, 1.0f),
        ImVec4(0.95f, 0.34f, 0.38f, 1.0f),
        ImVec4(0.44f, 0.93f, 0.88f, 1.0f),
    };
    return ImGui::ColorConvertFloat4ToU32(colors[static_cast<std::size_t>(index) % colors.size()]);
}

const json* graph_node_by_id(const AppState& app, const std::string& node_id) {
    if (!app.graph.contains("nodes") || !app.graph.at("nodes").is_array()) {
        return nullptr;
    }
    for (const auto& node : app.graph.at("nodes")) {
        if (json_string(node, "id") == node_id) {
            return &node;
        }
    }
    return nullptr;
}

bool graph_group_hidden(const AppState& app, const std::string& group_id) {
    return app.hidden_graph_groups.find(group_id) != app.hidden_graph_groups.end();
}

void draw_graph_canvas(AppState& app, const ImVec2& size) {
    ImGui::BeginChild("graph-canvas", size, true, ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 canvas_max{canvas_min.x + canvas_size.x, canvas_min.y + canvas_size.y};
    draw_list->AddRectFilledMultiColor(
        canvas_min,
        canvas_max,
        IM_COL32(11, 18, 32, 255),
        IM_COL32(13, 31, 44, 255),
        IM_COL32(7, 10, 20, 255),
        IM_COL32(12, 20, 34, 255));
    draw_list->AddRect(canvas_min, canvas_max, IM_COL32(148, 183, 215, 44), 18.0f, 0, 1.0f);

    if (!app.graph.contains("nodes") || !app.graph.at("nodes").is_array() || app.graph.at("nodes").empty()) {
        draw_list->AddText(ImVec2(canvas_min.x + 18.0f, canvas_min.y + 20.0f), IM_COL32(178, 196, 214, 230), "Load a graph to inspect typed memory relationships.");
        ImGui::EndChild();
        return;
    }

    if (app.graph_view.fit_on_next_draw) {
        ImVec2 min_world{};
        ImVec2 max_world{};
        if (graph_layout_bounds(app, min_world, max_world)) {
            const float world_width = std::max(1.0f, max_world.x - min_world.x);
            const float world_height = std::max(1.0f, max_world.y - min_world.y);
            const float padding = 72.0f;
            const float zoom_x = std::max(0.1f, (canvas_size.x - padding * 2.0f) / world_width);
            const float zoom_y = std::max(0.1f, (canvas_size.y - padding * 2.0f) / world_height);
            app.graph_view.zoom = std::clamp(std::min(zoom_x, zoom_y), 0.32f, 1.45f);
            const ImVec2 center_world{(min_world.x + max_world.x) * 0.5f, (min_world.y + max_world.y) * 0.5f};
            app.graph_view.pan = ImVec2(
                canvas_size.x * 0.5f - center_world.x * app.graph_view.zoom,
                canvas_size.y * 0.5f - center_world.y * app.graph_view.zoom);
        } else {
            app.graph_view.pan = ImVec2(canvas_size.x * 0.5f, canvas_size.y * 0.5f);
            app.graph_view.zoom = 1.0f;
        }
        app.graph_view.fit_on_next_draw = false;
    }

    const bool hovered = ImGui::IsWindowHovered();
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        const float next_zoom = std::clamp(app.graph_view.zoom + ImGui::GetIO().MouseWheel * 0.08f, 0.25f, 2.5f);
        app.graph_view.zoom = next_zoom;
    }
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        app.graph_view.pan.x += delta.x;
        app.graph_view.pan.y += delta.y;
    }

    auto world_to_screen = [&](const ImVec2& world) {
        return ImVec2(
            canvas_min.x + app.graph_view.pan.x + world.x * app.graph_view.zoom,
            canvas_min.y + app.graph_view.pan.y + world.y * app.graph_view.zoom);
    };

    auto screen_to_world = [&](const ImVec2& screen) {
        return ImVec2(
            (screen.x - canvas_min.x - app.graph_view.pan.x) / app.graph_view.zoom,
            (screen.y - canvas_min.y - app.graph_view.pan.y) / app.graph_view.zoom);
    };

    const ImVec2 mouse_screen = ImGui::GetIO().MousePos;
    const ImVec2 mouse_world = screen_to_world(mouse_screen);
    std::string hovered_node_id;

    const float grid_step = std::max(54.0f, 88.0f * app.graph_view.zoom);
    const float grid_offset_x = std::fmod(app.graph_view.pan.x, grid_step);
    const float grid_offset_y = std::fmod(app.graph_view.pan.y, grid_step);
    for (float x = canvas_min.x + grid_offset_x; x < canvas_max.x; x += grid_step) {
        draw_list->AddLine(ImVec2(x, canvas_min.y), ImVec2(x, canvas_max.y), IM_COL32(156, 194, 220, 18), 1.0f);
    }
    for (float y = canvas_min.y + grid_offset_y; y < canvas_max.y; y += grid_step) {
        draw_list->AddLine(ImVec2(canvas_min.x, y), ImVec2(canvas_max.x, y), IM_COL32(156, 194, 220, 16), 1.0f);
    }

    if (app.graph.contains("edges") && app.graph.at("edges").is_array()) {
        for (const auto& edge : app.graph.at("edges")) {
            const auto from_id = json_string(edge, "from_id");
            const auto to_id = json_string(edge, "to_id");
            const auto from_it = app.graph_layout.find(from_id);
            const auto to_it = app.graph_layout.find(to_id);
            if (from_it == app.graph_layout.end() || to_it == app.graph_layout.end()) {
                continue;
            }
            if (graph_group_hidden(app, from_it->second.group_id) || graph_group_hidden(app, to_it->second.group_id)) {
                continue;
            }
            const auto p1 = world_to_screen(from_it->second.world);
            const auto p2 = world_to_screen(to_it->second.world);
            const auto color = json_string(edge, "type") == "conflicts_with" ? IM_COL32(245, 84, 96, 178) : IM_COL32(96, 184, 214, 88);
            draw_list->AddLine(p1, p2, color, json_string(edge, "type") == "conflicts_with" ? 2.0f : 1.2f);
        }
    }

    if (app.graph.contains("nodes") && app.graph.at("nodes").is_array()) {
        const int total_nodes = static_cast<int>(app.graph.at("nodes").size());
        for (const auto& node : app.graph.at("nodes")) {
            const auto node_id = json_string(node, "id");
            const auto it = app.graph_layout.find(node_id);
            if (it == app.graph_layout.end()) {
                continue;
            }
            if (graph_group_hidden(app, it->second.group_id)) {
                continue;
            }
            const float density_scale = total_nodes > 90 ? 0.68f : (total_nodes > 56 ? 0.80f : 1.0f);
            const float radius = it->second.radius * app.graph_view.zoom * density_scale;
            const auto center = world_to_screen(it->second.world);
            const float dx = mouse_world.x - it->second.world.x;
            const float dy = mouse_world.y - it->second.world.y;
            const bool is_hovered = hovered && ((dx * dx) + (dy * dy)) <= (it->second.radius * it->second.radius);
            if (is_hovered) {
                hovered_node_id = node_id;
            }
            const ImU32 fill = graph_palette(it->second.group_index);
            const ImU32 outline = node_id == app.selected_graph_node_id ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 246, 255, 180);
            draw_list->AddCircleFilled(center, radius + 7.0f, IM_COL32(53, 215, 236, 28), 28);
            draw_list->AddCircleFilled(center, radius, fill, 28);
            draw_list->AddCircle(center, radius + 2.0f, outline, 28, node_id == app.selected_graph_node_id ? 3.0f : 1.5f);
            const int degree = json_int(node, "degree", 0);
            const bool selected = node_id == app.selected_graph_node_id;
            const bool show_label =
                selected ||
                is_hovered ||
                total_nodes <= 32 ||
                (degree >= 8 && app.graph_view.zoom >= 0.55f) ||
                app.graph_view.zoom >= 1.35f;
            if (show_label) {
                const auto label = excerpt(json_string(node, "label", node_id), selected || is_hovered ? 34 : 22);
                draw_list->AddText(ImVec2(center.x + radius + 8.0f, center.y - 8.0f), IM_COL32(222, 236, 247, selected || is_hovered ? 250 : 198), label.c_str());
            }
        }
    }

    if (!hovered_node_id.empty() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        app.selected_graph_node_id = hovered_node_id;
        load_memory_detail(app, hovered_node_id);
    }
    if (!hovered_node_id.empty()) {
        if (const auto* node = graph_node_by_id(app, hovered_node_id)) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(json_string(*node, "label", hovered_node_id).c_str());
            ImGui::Separator();
            ImGui::TextWrapped("%s", json_string(*node, "summary").c_str());
            ImGui::Text("type: %s", json_string(*node, "memory_type").c_str());
            ImGui::Text("status: %s", json_string(*node, "status").c_str());
            ImGui::EndTooltip();
        }
    }

    draw_list->AddText(ImVec2(canvas_min.x + 18.0f, canvas_min.y + 18.0f), IM_COL32(166, 189, 207, 220), "Left click selects. Hover or zoom to reveal labels. Right drag pans. Mouse wheel zooms.");
    ImGui::EndChild();
}

void draw_overview_tab(AppState& app) {
    const auto stats = app.summary.value("health", json::object());
    const auto recent = app.summary.value("recent_activity", json::array());
    const int health_score = app.summary.value("health_score", 0);
    const int memory_entries = app.summary.value("memory_entries", 0);
    const int conflict_count = app.summary.value("conflict_count", 0);
    const int stale_count = app.summary.value("stale_count", 0);

    ImGui::SeparatorText("Your memory at a glance");
    ImGui::TextWrapped("Scope: %s", app.project.empty() ? "Current runtime" : app.project.c_str());
    ImGui::TextWrapped("See what is stored, what needs attention, and where it came from.");
    if (ImGui::Button("Refresh overview")) {
        refresh_overview(app);
    }
    next_action("Browse memories");
    if (ImGui::Button("Browse memories")) {
        apply_workspace_group(app, WorkspaceGroup::explore);
    }
    next_action("Add memory");
    if (ImGui::Button("Add memory")) {
        apply_workspace_group(app, WorkspaceGroup::capture);
    }
    ImGui::Spacing();

    if (ImGui::BeginTable("overview-cards", responsive_columns(4, 155.0f), ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        draw_metric_card("Health", std::to_string(health_score), tone_color(BannerTone::info), "Trust weighted");
        ImGui::TableNextColumn();
        draw_metric_card("Memories", std::to_string(memory_entries), tone_color(BannerTone::success), "Indexed entries");
        ImGui::TableNextColumn();
        draw_metric_card("Conflicts", std::to_string(conflict_count), tone_color(conflict_count > 0 ? BannerTone::warning : BannerTone::success), "Needs review");
        ImGui::TableNextColumn();
        draw_metric_card("Stale", std::to_string(stale_count), tone_color(stale_count > 0 ? BannerTone::warning : BannerTone::success), "Evidence drift");
        ImGui::EndTable();
    }

    if (ImGui::BeginTable("overview-main-grid", responsive_columns(2, 275.0f), ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        draw_count_distribution_card("Memory Mix", app.summary.value("memory_counts", json::object()), dashboard_accent());
        ImGui::TableNextColumn();
        draw_count_distribution_card("Trust States", app.summary.value("trust_states", json::object()), dashboard_green());
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Review Queue");
    const auto urgent = app.summary.value("urgent_reviews", json::array());
    if (urgent.empty()) {
        ImGui::TextWrapped("No urgent review signals in this scope.");
    } else {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, alpha(dashboard_orange(), 0.08f));
        ImGui::PushStyleColor(ImGuiCol_Border, alpha(dashboard_orange(), 0.18f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
        ImGui::BeginChild("urgent-review-card", ImVec2(0.0f, 120.0f), true);
        for (const auto& item : urgent) {
            const auto message = json_string(item, "message", json_string(item, "title"));
            const auto type = json_string(item, "type");
            if (!type.empty()) {
                draw_chip(type, status_chip_color(type));
            }
            ImGui::TextWrapped("%s", message.c_str());
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }

    ImGui::SeparatorText("Recent Activity");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, alpha(dashboard_accent(), 0.06f));
    ImGui::PushStyleColor(ImGuiCol_Border, alpha(dashboard_accent(), 0.14f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::BeginChild("recent-activity-card", ImVec2(0.0f, 140.0f), true);
    if (recent.empty()) {
        ImGui::TextWrapped("No activity yet. Add a memory from Capture to get started.");
    } else {
        for (const auto& item : recent) {
            std::string line;
            if (item.is_object()) {
                line = json_string(item, "message", json_string(item, "title", json_string(item, "type", "activity")));
                const auto actor = json_string(item, "actor");
                if (!actor.empty()) {
                    line += " / " + actor;
                }
            } else {
                line = item.dump();
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::BulletText("%s", line.c_str());
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    if (ImGui::CollapsingHeader("Service Status")) {
        draw_json_block("overview-status", app.service_status, 160.0f, app.monospace_font);
    }
    if (ImGui::CollapsingHeader("Metrics")) {
        draw_json_block("overview-metrics", app.metrics, 180.0f, app.monospace_font);
    }
    if (ImGui::CollapsingHeader("Health Detail")) {
        draw_json_block("overview-health", stats, 160.0f, app.monospace_font);
    }
}

void draw_editor_tab(AppState& app) {
    ImGui::TextWrapped("Save a decision, constraint, or observation with its source. New memories follow the current scope and review policy.");
    ImGui::SeparatorText("Scope");
    draw_labeled_input("Project", "editor-project", app.project);
    if (ImGui::BeginTable("editor-scope-grid", responsive_columns(2), ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        draw_labeled_input("Session", "editor-session", app.scope.session);
        ImGui::TableNextColumn();
        draw_labeled_input("Agent", "editor-agent", app.scope.agent_id);
        ImGui::TableNextColumn();
        draw_labeled_input("Conversation", "editor-conversation", app.scope.conversation_id);
        ImGui::TableNextColumn();
        draw_labeled_input("Task", "editor-task", app.scope.task);
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Structured Memory");
    if (ImGui::BeginTable("editor-classification-grid", responsive_columns(3), ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        draw_labeled_combo_string("Type", "editor-type", kMemoryTypes, app.editor.memory_type);
        ImGui::TableNextColumn();
        draw_labeled_combo_string("Status", "editor-status", kStatuses, app.editor.status);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Confidence");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##editor-confidence", &app.editor.confidence, 0.0f, 1.0f, "%.2f");
        ImGui::EndTable();
    }
    draw_labeled_input("Source", "editor-source", app.editor.source);
    if (ImGui::BeginTable("editor-source-grid", responsive_columns(2), ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        draw_labeled_input("Source Type", "editor-source-type", app.editor.source_type);
        ImGui::TableNextColumn();
        draw_labeled_input("Source URI", "editor-source-uri", app.editor.source_uri);
        ImGui::EndTable();
    }
    draw_labeled_input("Tags", "editor-tags", app.editor.tags_csv);
    ImGui::TextDisabled("Content");
    ImGui::InputTextMultiline("##editor-content", &app.editor.content, ImVec2(-FLT_MIN, 170.0f));
    if (ImGui::Button("Write Memory")) {
        write_memory(app);
    }
    next_action("Clear Structured");
    if (ImGui::Button("Clear Structured")) {
        app.editor.content.clear();
    }

    ImGui::SeparatorText("Raw Text Processing");
    ImGui::Checkbox("Stage for review", &app.editor.stage_for_review);
    same_line_if_fits(ImGui::CalcTextSize("Dry run").x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::Checkbox("Dry run", &app.editor.dry_run);
    ImGui::TextDisabled("Raw Session Text");
    ImGui::InputTextMultiline("##editor-raw-session-text", &app.editor.raw_text, ImVec2(-FLT_MIN, 220.0f));
    if (ImGui::Button("Process Raw Text")) {
        process_raw_text(app);
    }
}

void draw_explorer_tab(AppState& app) {
    ImGui::TextWrapped("Search stored memories. Select a result to read its content, source, and history.");
    if (draw_labeled_input("Search memories", "explorer-query", app.explorer.query, ImGuiInputTextFlags_EnterReturnsTrue)) {
        refresh_explorer(app, true);
    }
    if (ImGui::BeginTable("explorer-filter-grid", responsive_columns(2), ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        draw_labeled_combo_string("Type Filter", "explorer-type-filter", kMemoryTypes, app.explorer.memory_type);
        ImGui::TableNextColumn();
        draw_labeled_combo_string("Status Filter", "explorer-status-filter", kStatuses, app.explorer.status);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Limit");
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputInt("##explorer-limit", &app.explorer.limit);
    app.explorer.limit = std::clamp(app.explorer.limit, 1, 1000);
    ImGui::Checkbox("Include superseded", &app.explorer.include_superseded);
    if (ImGui::Button("Search Memories")) {
        refresh_explorer(app, true);
    }
    next_action("Refresh Detail");
    ImGui::BeginDisabled(app.selected_memory_id.empty());
    if (ImGui::Button("Refresh Detail") && !app.selected_memory_id.empty()) {
        load_memory_detail(app, app.selected_memory_id);
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::Text("%zu results", app.explorer_entries.size());
    if (app.explorer_entries.empty()) {
        draw_empty_notice("No memories found", "Try a different search or clear the filters. Use Capture to save your first memory.");
    } else {
        draw_memory_entries_table("memory-table", app.explorer_entries, app.selected_memory_id,
            [&app](const std::string& id) { load_memory_detail(app, id); });
    }
}

void draw_memory_detail_window(AppState& app) {
    if (app.selected_memory_id.empty()) {
        draw_empty_notice("Choose a memory", "Open a result in Memory Explorer or select a graph node to read its content and history.");
        return;
    }

    ImGui::TextWrapped("Memory: %s", app.selected_memory_id.c_str());
    const auto entry = app.selected_memory_detail.value("entry", json::object());
    if (entry.empty()) {
        ImGui::TextWrapped("Loading memory details...");
        return;
    }
    const auto memory_type = json_string(entry, "type", json_string(entry, "memory_type"));
    const bool dream_review = memory_type == "dream" || memory_type == "research" ||
                              json_string(entry, "status") == "dream_artifact";
    ImGui::BeginDisabled(app.dream_review_pending);
    if (ImGui::Button(dream_review ? "Review approval" : "Promote")) {
        if (dream_review) preview_dream_review(app, "approve");
        else memory_action(app, "/mcp/memory/promote", "Promoted memory.", json::object());
    }
    next_action("Validate");
    ImGui::BeginDisabled(dream_review);
    if (ImGui::Button("Validate")) {
        memory_action(app, "/mcp/memory/validate", "Validated memory.", json::object());
    }
    next_action("Deprecate");
    if (ImGui::Button("Deprecate")) {
        memory_action(app, "/mcp/memory/deprecate", "Deprecated memory.", json::object());
    }
    ImGui::EndDisabled();
    next_action(dream_review ? "Review rejection" : "Reject");
    if (ImGui::Button(dream_review ? "Review rejection" : "Reject")) {
        if (dream_review) preview_dream_review(app, "reject");
        else memory_action(app, "/mcp/memory/reject", "Rejected memory.", json::object());
    }
    next_action("Forget");
    if (ImGui::Button("Forget")) {
        post_json(app, "/mcp/memory/forget", scoped_body(app, {{"id", app.selected_memory_id}}), [&app](const HttpResponse& response) {
            if (!response.ok) {
                set_banner(app, "Forget failed: " + response.error_message, BannerTone::error);
                return;
            }
            app.selected_memory_id.clear();
            app.selected_memory_detail = json::object();
            app.selected_memory_history = json::object();
            set_banner(app, "Forgotten memory.", BannerTone::success);
            refresh_overview(app);
            refresh_explorer(app, true);
            refresh_graph(app);
            refresh_sources(app);
        });
    }
    ImGui::EndDisabled();
    if (app.dream_review_pending) ImGui::TextWrapped("Processing Dream review...");
    if (!app.dream_review_preview.empty() && app.dream_review_subject == app.selected_memory_id) {
        ImGui::SeparatorText("Confirm Dream review");
        ImGui::TextWrapped("Decision: %s", app.dream_review_action.c_str());
        for (const auto* section : {"will_do", "will_not_do", "warnings"}) {
            const auto lines = app.dream_review_preview.value(section, json::array());
            if (lines.empty()) continue;
            ImGui::TextUnformatted(std::string(section) == "will_do" ? "Will do:" :
                                   std::string(section) == "will_not_do" ? "Will not do:" : "Warnings:");
            for (const auto& line : lines) {
                if (line.is_string()) ImGui::TextWrapped("- %s", line.get_ref<const std::string&>().c_str());
            }
        }
        ImGui::BeginDisabled(app.dream_review_pending || !app.dream_review_preview.value("applicable", false));
        if (ImGui::Button("Confirm review")) apply_dream_review(app);
        ImGui::EndDisabled();
        next_action("Cancel review");
        ImGui::BeginDisabled(app.dream_review_pending);
        if (ImGui::Button("Cancel review")) {
            app.dream_review_preview = json::object();
            ++app.dream_review_generation;
        }
        ImGui::EndDisabled();
    }
    ImGui::SeparatorText("Detail");
    if (entry.empty()) {
        draw_empty_notice("Memory Detail", "Select a node or explorer row to inspect a memory entry. The detail panel opens automatically when a memory is loaded.");
        return;
    }
    std::vector<std::pair<std::string, std::string>> rows;
    add_table_row(rows, "ID", json_string(entry, "id"));
    add_table_row(rows, "Confidence", json_compact_text(entry.value("confidence", json())));
    add_table_row(rows, "Quality Score", json_compact_text(entry.value("quality_score", json())));
    add_table_row(rows, "Project", json_string(entry, "project"));
    add_table_row(rows, "Session", json_string(entry, "session"));
    add_table_row(rows, "Agent", json_string(entry, "agent_id"));
    add_table_row(rows, "Conversation", json_string(entry, "conversation_id"));
    add_table_row(rows, "Created", display_time_text(json_string(entry, "created_at")));
    add_table_row(rows, "Updated", display_time_text(json_string(entry, "updated_at")));
    add_table_row(rows, "Last Accessed", display_time_text(json_string(entry, "last_accessed_at")));
    add_table_row(rows, "Replacement", json_string(entry, "replacement_id", json_string(entry, "superseded_by")));
    std::vector<std::pair<std::string, std::string>> provenance_rows;
    add_table_row(provenance_rows, "Source", json_string(entry, "source"));
    add_table_row(provenance_rows, "Source Type", json_string(entry, "source_type"));
    add_table_row(provenance_rows, "Source URI", json_string(entry, "source_uri"));
    if (ImGui::BeginTable("memory-detail-layout", responsive_columns(2, 340.0f), ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        draw_property_table("memory-detail-table", rows);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Classification");
        const auto type = json_string(entry, "type", json_string(entry, "memory_type"));
        draw_chip(type, status_chip_color(type));
        same_line_if_fits(ImGui::CalcTextSize(json_string(entry, "status").c_str()).x + 16.0f);
        draw_chip(json_string(entry, "status"), status_chip_color(json_string(entry, "status")));
        ImGui::SeparatorText("Provenance");
        draw_property_table("memory-detail-provenance", provenance_rows);
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Tags");
    draw_tag_cloud(json_strings(entry.value("tags", json::array())));
    ImGui::SeparatorText("Content");
    ImGui::BeginChild("memory-detail-content", ImVec2(0.0f, 140.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
    if (app.monospace_font != nullptr) {
        ImGui::PushFont(app.monospace_font, 0.0f);
    }
    ImGui::TextWrapped("%s", json_string(entry, "content").c_str());
    if (app.monospace_font != nullptr) {
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::SeparatorText("Supersession Chain");
    draw_memory_entries_table(
        "memory-supersession-chain",
        app.selected_memory_detail.value("supersession_chain", json::array()),
        app.selected_memory_id,
        [&app](const std::string& memory_id) { load_memory_detail(app, memory_id); });
    ImGui::SeparatorText("History");
    const auto revisions = app.selected_memory_history.value("revisions", json::array());
    if (ImGui::BeginTable(
            "memory-history-table",
            5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
            ImVec2(0.0f, 240.0f), 850.0f * ImGui::GetStyle().FontScaleMain)) {
        ImGui::TableSetupColumn("Created", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Content");
        ImGui::TableHeadersRow();
        for (const auto& revision : revisions) {
            const auto revision_entry = revision.value("entry", json::object());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextWrapped("%s", json_string(revision, "created_at").c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(json_string(revision, "revision_kind").c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(json_string(revision_entry, "status").c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(json_string(revision_entry, "type").c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextWrapped("%s", excerpt(json_string(revision_entry, "content"), 120).c_str());
        }
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Raw Memory JSON")) {
        draw_json_block("memory-detail-json", app.selected_memory_detail, 220.0f, app.monospace_font);
        draw_json_block("memory-history-json", app.selected_memory_history, 220.0f, app.monospace_font);
    }
}

void draw_graph_sidebar(AppState& app) {
    draw_labeled_input("Focus Query", "graph-focus-query", app.graph_filters.query);
    draw_labeled_combo_string("Memory Type", "graph-memory-type", kMemoryTypes, app.graph_filters.memory_type);
    draw_labeled_combo_string("Trust State", "graph-trust-state", kStatuses, app.graph_filters.status);
    draw_labeled_combo_string("Relationship", "graph-edge-type", kEdgeTypes, app.graph_filters.edge_type);
    ImGui::TextDisabled("Node Limit");
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputInt("##graph-node-limit", &app.graph_filters.limit);
    app.graph_filters.limit = std::clamp(app.graph_filters.limit, 1, 1000);
    ImGui::Checkbox("Include isolated memories", &app.graph_filters.include_isolates);
    if (ImGui::Button("Load Graph")) {
        refresh_graph(app);
    }
    next_action("Reset");
    if (ImGui::Button("Reset")) {
        app.graph_filters = GraphFilters{};
        refresh_graph(app);
    }
    ImGui::SeparatorText("Local Search");
    draw_labeled_input("Search Loaded Nodes", "graph-local-search", app.graph_filters.search);
    const auto lowered = lower_copy(app.graph_filters.search);
    if (!lowered.empty() && app.graph.contains("nodes") && app.graph.at("nodes").is_array()) {
        for (const auto& node : app.graph.at("nodes")) {
            const auto haystack = lower_copy(
                json_string(node, "label") + " " + json_string(node, "summary") + " " + json_string(node, "search_text"));
            if (haystack.find(lowered) == std::string::npos) {
                continue;
            }
            const auto id = json_string(node, "id");
            if (ImGui::Selectable((json_string(node, "label", id) + "##" + id).c_str(), app.selected_graph_node_id == id)) {
                app.selected_graph_node_id = id;
                load_memory_detail(app, id);
            }
        }
    }

    ImGui::SeparatorText("Clusters");
    if (app.graph.contains("groups") && app.graph.at("groups").is_array()) {
        for (const auto& group : app.graph.at("groups")) {
            const auto id = json_string(group, "id");
            bool visible = !graph_group_hidden(app, id);
            if (ImGui::Checkbox((json_string(group, "label", id) + "##" + id).c_str(), &visible)) {
                if (visible) {
                    app.hidden_graph_groups.erase(id);
                } else {
                    app.hidden_graph_groups.insert(id);
                }
            }
        }
    }

    ImGui::SeparatorText("Selected Node");
    if (const auto* node = graph_node_by_id(app, app.selected_graph_node_id)) {
        ImGui::TextWrapped("%s", json_string(*node, "label").c_str());
        ImGui::Text("type: %s", json_string(*node, "memory_type").c_str());
        ImGui::Text("status: %s", json_string(*node, "status").c_str());
        ImGui::Text("cluster: %s", json_string(*node, "group_label").c_str());
        ImGui::Separator();
        ImGui::TextWrapped("%s", json_string(*node, "content", json_string(*node, "summary")).c_str());
        if (ImGui::Button("Open Memory Detail")) {
            load_memory_detail(app, json_string(*node, "id"));
        }
    } else {
        ImGui::TextUnformatted("Select a graph node.");
    }

    if (app.graph.contains("stats")) {
        ImGui::SeparatorText("Graph Stats");
        draw_key_value_grid(app.graph.at("stats"));
    }
    if (app.graph.contains("warnings") && app.graph.at("warnings").is_array()) {
        ImGui::SeparatorText("Warnings");
        for (const auto& warning : app.graph.at("warnings")) {
            ImGui::BulletText("%s", json_string(warning, "message").c_str());
        }
    }
}

void draw_graph_tab(AppState& app) {
    ImGui::TextWrapped("Explore how memories connect. Select a node to inspect it; drag to pan and scroll to zoom.");
    if (responsive_columns(2, 350.0f) == 1) {
        if (ImGui::CollapsingHeader("Graph filters")) draw_graph_sidebar(app);
        draw_graph_canvas(app, ImVec2(0.0f, std::max(260.0f, ImGui::GetContentRegionAvail().y)));
        return;
    }
    if (ImGui::BeginTable("graph-layout", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("sidebar", ImGuiTableColumnFlags_WidthStretch, 0.70f);
        ImGui::TableSetupColumn("viewer", ImGuiTableColumnFlags_WidthStretch, 1.70f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        draw_graph_sidebar(app);
        ImGui::TableSetColumnIndex(1);
        draw_graph_canvas(app, ImVec2(0.0f, ImGui::GetContentRegionAvail().y));
        ImGui::EndTable();
    }
}

void draw_sources_tab(AppState& app) {
    if (ImGui::Button("Refresh Sources")) {
        refresh_sources(app);
    }
    next_action("Recheck Selected");
    if (ImGui::Button("Recheck Selected") && !app.selected_source_id.empty()) {
        recheck_source(app);
    }
    const int columns = responsive_columns(2, 420.0f);
    if (app.sources.empty()) {
        draw_empty_notice("No sources yet", "Sources will appear here when memories with provenance are saved.");
        return;
    }
    if (ImGui::BeginTable("sources-layout", columns, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        ImGui::BeginChild("sources-list", ImVec2(0.0f, columns == 1 ? 200.0f : std::max(200.0f, ImGui::GetContentRegionAvail().y)), true);
        for (const auto& source : app.sources) {
            const auto id = json_string(source, "source_id");
            const auto label = json_string(source, "display_name", id);
            if (ImGui::Selectable((label + "##" + id).c_str(), app.selected_source_id == id)) {
                load_source_detail(app, id);
            }
            draw_chip(json_string(source, "hash_status", "unknown"), status_chip_color(json_string(source, "hash_status", "unknown")));
            next_action("000 memories");
            ImGui::TextDisabled("%d memories", source.value("memory_count", 0));
            const auto uri = json_string(source, "source_uri");
            if (!uri.empty()) {
                ImGui::TextWrapped("%s", uri.c_str());
            }
            ImGui::Spacing();
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("source-detail", ImVec2(0.0f, std::max(350.0f, ImGui::GetContentRegionAvail().y)), true);
        const auto detail = app.selected_source_detail.value("source", json::object());
        std::vector<std::pair<std::string, std::string>> rows;
        add_table_row(rows, "Source ID", json_string(detail, "source_id"));
        add_table_row(rows, "Display Name", json_string(detail, "display_name"));
        add_table_row(rows, "Type", json_string(detail, "source_type"));
        add_table_row(rows, "URI", json_string(detail, "source_uri"));
        add_table_row(rows, "Memory Count", std::to_string(detail.value("memory_count", 0)), true);
        add_table_row(rows, "Stale Memories", std::to_string(detail.value("stale_memories", 0)), true);
        add_table_row(rows, "Conflicts", std::to_string(detail.value("conflicting_memories", 0)), true);
        add_table_row(rows, "Imported At", display_time_text(detail.value("imported_at", json())));
        add_table_row(rows, "Last Checked", display_time_text(detail.value("last_checked_at", json())));
        draw_property_table("source-detail-table", rows);
        ImGui::TextDisabled("Source Status");
        draw_chip(json_string(detail, "hash_status", "unknown"), status_chip_color(json_string(detail, "hash_status", "unknown")));
        if (detail.contains("status_counts")) {
            ImGui::SeparatorText("Status Counts");
            draw_key_value_grid(detail.at("status_counts"));
        }
        ImGui::SeparatorText("Memories");
        draw_memory_entries_table(
            "source-memories-table",
            detail.value("memories", json::array()),
            app.selected_memory_id,
            [&app](const std::string& memory_id) { load_memory_detail(app, memory_id); });
        ImGui::SeparatorText("History");
        const auto history = detail.value("history", json::array());
        if (ImGui::BeginTable(
                "source-history-table",
                5,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
                ImVec2(0.0f, 180.0f), 850.0f * ImGui::GetStyle().FontScaleMain)) {
            ImGui::TableSetupColumn("Created", ImGuiTableColumnFlags_WidthFixed, 170.0f);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Note");
            ImGui::TableHeadersRow();
            for (const auto& event : history) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextWrapped("%s", json_string(event, "created_at").c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(json_string(event, "action").c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(json_string(event, "status").c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextWrapped("%s", excerpt(json_string(event, "source_hash"), 24).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextWrapped("%s", json_string(event, "note").c_str());
            }
            ImGui::EndTable();
        }
        if (ImGui::CollapsingHeader("Raw Source JSON")) {
            draw_json_block("source-detail-json", app.selected_source_detail, 220.0f, app.monospace_font);
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

void draw_agents_window(AppState& app) {
    if (ImGui::Button("Refresh Agents")) {
        refresh_agents(app);
    }
    const int columns = responsive_columns(2, 480.0f);
    if (app.agents.empty()) {
        draw_empty_notice("No agent activity", "Agent activity appears here after scoped memory operations.");
        return;
    }
    if (ImGui::BeginTable("agents-layout", columns, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        ImGui::BeginChild("agents-list", ImVec2(0.0f, columns == 1 ? 200.0f : std::max(200.0f, ImGui::GetContentRegionAvail().y)), true);
        if (ImGui::BeginTable("agents-table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
                ImVec2(0.0f, 0.0f), 550.0f * ImGui::GetStyle().FontScaleMain)) {
            ImGui::TableSetupColumn("Agent");
            ImGui::TableSetupColumn("Sessions", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Memories", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Recalls", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();
            for (const auto& agent : app.agents) {
                const auto id = json_string(agent, "agent_id");
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(id.c_str(), app.selected_agent_id == id, ImGuiSelectableFlags_SpanAllColumns)) {
                    load_agent_detail(app, id);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", agent.value("session_count", 0));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d", agent.value("memories_written", 0));
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%d", agent.value("memory_recall_events", 0));
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("agent-detail", ImVec2(0.0f, std::max(350.0f, ImGui::GetContentRegionAvail().y)), true);
        const auto agent = app.selected_agent_detail.value("agent", json::object());
        std::vector<std::pair<std::string, std::string>> rows;
        add_table_row(rows, "Agent", json_string(agent, "agent_id"));
        add_table_row(rows, "Sessions", joined_json_strings(agent.value("sessions", json::array())));
        add_table_row(rows, "Memories Written", std::to_string(agent.value("memories_written", 0)), true);
        add_table_row(rows, "Recall Events", std::to_string(agent.value("memory_recall_events", 0)), true);
        add_table_row(rows, "Context Builds", std::to_string(agent.value("context_builds", 0)), true);
        add_table_row(rows, "Avg Context Tokens", std::to_string(agent.value("context_budget_avg", 0)), true);
        add_table_row(rows, "Last Active", json_compact_text(agent.value("last_active", json())), true);
        draw_property_table("agent-detail-table", rows);
        ImGui::SeparatorText("Memories");
        draw_memory_entries_table(
            "agent-memories-table",
            app.selected_agent_detail.value("memories", json::array()),
            app.selected_memory_id,
            [&app](const std::string& memory_id) { load_memory_detail(app, memory_id); });
        ImGui::SeparatorText("Activity");
        draw_event_table("agent-activity-table", app.selected_agent_detail.value("activity", json::array()));
        if (ImGui::CollapsingHeader("Raw Agent JSON")) {
            draw_json_block("agent-detail-json", app.selected_agent_detail, 220.0f, app.monospace_font);
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

void draw_sessions_window(AppState& app) {
    if (ImGui::Button("Refresh Sessions")) {
        refresh_sessions(app);
    }
    const int columns = responsive_columns(2, 480.0f);
    if (app.sessions.empty()) {
        draw_empty_notice("No session activity", "Sessions appear here after memories are saved or recalled.");
        return;
    }
    if (ImGui::BeginTable("sessions-layout", columns, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        ImGui::BeginChild("sessions-list", ImVec2(0.0f, columns == 1 ? 200.0f : std::max(200.0f, ImGui::GetContentRegionAvail().y)), true);
        if (ImGui::BeginTable("sessions-table", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
                ImVec2(0.0f, 0.0f), 650.0f * ImGui::GetStyle().FontScaleMain)) {
            ImGui::TableSetupColumn("Session");
            ImGui::TableSetupColumn("Agents", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Convs", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Contexts", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Recalls", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();
            for (const auto& session : app.sessions) {
                const auto id = json_string(session, "session");
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(id.c_str(), app.selected_session_id == id, ImGuiSelectableFlags_SpanAllColumns)) {
                    load_session_detail(app, id);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", session.value("agent_count", 0));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d", session.value("conversation_count", 0));
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%d", session.value("context_builds", 0));
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%d", session.value("memory_recall_events", 0));
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("session-detail", ImVec2(0.0f, std::max(350.0f, ImGui::GetContentRegionAvail().y)), true);
        const auto session = app.selected_session_detail.value("session", json::object());
        std::vector<std::pair<std::string, std::string>> rows;
        add_table_row(rows, "Session", json_string(session, "session"));
        add_table_row(rows, "Agents", joined_json_strings(session.value("agents", json::array())));
        add_table_row(rows, "Conversations", joined_json_strings(session.value("conversations", json::array())));
        add_table_row(rows, "Memories Written", std::to_string(session.value("memories_written", 0)), true);
        add_table_row(rows, "Recall Events", std::to_string(session.value("memory_recall_events", 0)), true);
        add_table_row(rows, "Context Builds", std::to_string(session.value("context_builds", 0)), true);
        add_table_row(rows, "Avg Context Tokens", std::to_string(session.value("context_budget_avg", 0)), true);
        add_table_row(rows, "Last Active", json_compact_text(session.value("last_active", json())), true);
        draw_property_table("session-detail-table", rows);
        ImGui::SeparatorText("Memories");
        draw_memory_entries_table(
            "session-memories-table",
            app.selected_session_detail.value("memories", json::array()),
            app.selected_memory_id,
            [&app](const std::string& memory_id) { load_memory_detail(app, memory_id); });
        ImGui::SeparatorText("Activity");
        draw_event_table("session-activity-table", app.selected_session_detail.value("activity", json::array()));
        if (ImGui::CollapsingHeader("Raw Session JSON")) {
            draw_json_block("session-detail-json", app.selected_session_detail, 220.0f, app.monospace_font);
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

void draw_context_tab(AppState& app) {
    draw_labeled_input("Task", "context-task", app.context.task);
    draw_labeled_input("Query", "context-query", app.context.query);
    ImGui::TextDisabled("%s", "Context Settings");
    draw_labeled_combo_string("Recall policy", "context-policy", kRecallPolicies, app.context.recall_policy);
    ImGui::TextDisabled("Token budget");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputInt("##context-budget", &app.context.budget_tokens);
    app.context.budget_tokens = std::max(0, app.context.budget_tokens);
    ImGui::Checkbox("Include linked tools", &app.context.include_linked_tools);
    if (ImGui::Button("Build Context Pack")) {
        build_context(app);
    }
    const auto& pack = app.context.result.value("context_pack", json::object());
    ImGui::SeparatorText("Summary");
    if (pack.is_object()) {
        draw_key_value_grid({
            {"memory_count", pack.value("memory_count", 0)},
            {"warnings", pack.value("warning_count", 0)},
            {"budget_tokens", pack.value("budget_tokens", 0)},
            {"included_tokens", pack.value("used_tokens", 0)},
        });
    } else {
        ImGui::TextUnformatted("No context pack built yet.");
    }
    if (pack.contains("included_counts")) {
        ImGui::SeparatorText("Included Counts");
        draw_key_value_grid(pack.at("included_counts"));
    }
    if (pack.contains("warnings") && pack.at("warnings").is_array()) {
        ImGui::SeparatorText("Warnings");
        if (ImGui::BeginTable("context-warnings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
                ImVec2(0.0f, 180.0f), 580.0f * ImGui::GetStyle().FontScaleMain)) {
            ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Message");
            ImGui::TableHeadersRow();
            for (const auto& warning : pack.at("warnings")) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(json_string(warning, "code").c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(json_string(warning, "severity").c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped("%s", json_string(warning, "message").c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::SeparatorText("Context Markdown");
    ImGui::BeginChild("context-markdown", ImVec2(0.0f, 260.0f), true);
    if (app.monospace_font != nullptr) {
        ImGui::PushFont(app.monospace_font, 0.0f);
    }
    ImGui::TextWrapped("%s", json_string(pack, "context_markdown").c_str());
    if (app.monospace_font != nullptr) {
        ImGui::PopFont();
    }
    ImGui::EndChild();
    if (ImGui::CollapsingHeader("Context JSON")) {
        draw_json_block("context-json", app.context.result, 220.0f, app.monospace_font);
    }
}

void draw_system_tab(AppState& app) {
    ensure_ai_config_shape(app);
    if (ImGui::Button("Refresh System")) {
        refresh_system(app);
    }
    next_action("Start Scheduler");
    if (ImGui::Button("Start Scheduler")) {
        start_scheduler(app);
    }
    next_action("Stop Scheduler");
    if (ImGui::Button("Stop Scheduler")) {
        stop_scheduler(app);
    }
    ImGui::TextDisabled("Interval (seconds)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputInt("##scheduler-interval", &app.system.scheduler_interval_seconds);
    app.system.scheduler_interval_seconds = std::max(60, app.system.scheduler_interval_seconds);

    ImGui::SeparatorText("Scheduler");
    draw_property_table(
        "scheduler-table",
        {
            {"Running", json_compact_text(app.scheduler.value("running", json(false)))},
            {"Interval (s)", json_compact_text(app.scheduler.value("interval_seconds", json(0)))},
            {"Project Scope", json_string(app.scheduler, "project_scope")},
            {"Last Run", json_compact_text(app.scheduler.value("last_run_at", json()))},
            {"Queue Depth", json_compact_text(app.scheduler.value("queue_depth", json()))},
        });
    if (ImGui::CollapsingHeader("Scheduler Debug")) {
        draw_json_block("scheduler-json", app.scheduler, 160.0f, app.monospace_font);
    }
    ImGui::SeparatorText("AI Server Config");
    auto& services = app.ai_config.at("services");
    if (ImGui::Button("Add Service")) {
        const std::string next_name = "service-" + std::to_string(services.size() + 1);
        services.push_back({
            {"name", next_name},
            {"base_url", ""},
            {"api_style", ""},
            {"provider", ""},
            {"api", ""},
            {"package", ""},
            {"api_key_env", ""},
            {"headers", json::object()},
            {"auth_header", false},
            {"compat", json::object()},
            {"model_overrides", json::object()},
            {"models", json::array()},
            {"options", json::object()},
            {"enabled", true},
            {"timeout_ms", 120000},
        });
        app.system.selected_ai_service_index = static_cast<int>(services.size()) - 1;
        sync_ai_config_draft(app);
    }
    next_action("Duplicate Service");
    if (ImGui::Button("Duplicate Service")) {
        if (json* service = selected_ai_service(app)) {
            json copy = *service;
            copy["name"] = json_string(copy, "name", "service") + "-copy";
            services.push_back(copy);
            app.system.selected_ai_service_index = static_cast<int>(services.size()) - 1;
            sync_ai_config_draft(app);
        }
    }
    next_action("Remove Service");
    if (ImGui::Button("Remove Service")) {
        if (!services.empty()) {
            services.erase(services.begin() + app.system.selected_ai_service_index);
            if (app.system.selected_ai_service_index >= static_cast<int>(services.size())) {
                app.system.selected_ai_service_index = std::max(0, static_cast<int>(services.size()) - 1);
            }
            sync_ai_config_draft(app);
        }
    }
    next_action("Save Form Config");
    if (ImGui::Button("Save Form Config")) {
        app.ai_config["count"] = services.size();
        app.system.ai_config_draft = pretty_json(app.ai_config);
        save_ai_config(app);
    }

    if (services.empty()) {
        ImGui::TextUnformatted("No AI servers configured.");
    } else {
        const int columns = responsive_columns(2, 440.0f);
        if (ImGui::BeginTable("ai-config-layout", columns, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableNextColumn();
            ImGui::BeginChild("ai-service-list", ImVec2(0.0f, columns == 1 ? 200.0f : 480.0f), true);
            if (ImGui::BeginTable("ai-service-table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
                    ImVec2(0.0f, 0.0f), 550.0f * ImGui::GetStyle().FontScaleMain)) {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Provider", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("API", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 36.0f);
                ImGui::TableHeadersRow();
                for (std::size_t index = 0; index < services.size(); ++index) {
                    const auto& service = services[index];
                    const bool selected = static_cast<int>(index) == app.system.selected_ai_service_index;
                    const auto name = json_string(service, "name", "service");
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        app.system.selected_ai_service_index = static_cast<int>(index);
                        app.system.selected_ai_model_index = 0;
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", json_string(service, "provider").c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextWrapped("%s", json_string(service, "api").c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(json_bool(service, "enabled", true) ? "Y" : "N");
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
            ImGui::TableNextColumn();
            ImGui::BeginChild("ai-service-detail", ImVec2(0.0f, 480.0f), true);
            if (json* service = selected_ai_service(app)) {
                ensure_ai_service_defaults(*service);
                ImGui::InputText("Name", &(*service)["name"].get_ref<std::string&>());
                const auto provider_options = ai_provider_options(app, json_string(*service, "provider"));
                combo_string_vector("Provider", provider_options, (*service)["provider"].get_ref<std::string&>(), "(select)");
                next_action("Apply Provider Preset");
                if (ImGui::Button("Apply Provider Preset")) {
                    apply_service_provider_preset(app, *service);
                }
                const auto api_style_options = json_string_options(app.ai_metadata, "api_styles", json_string(*service, "api_style"), true);
                combo_string_vector("API Style", api_style_options, (*service)["api_style"].get_ref<std::string&>(), "(auto)");
                const auto api_options = json_string_options(app.ai_metadata, "apis", json_string(*service, "api"), false);
                combo_string_vector("API", api_options, (*service)["api"].get_ref<std::string&>(), "(select)");
                ImGui::InputText("Base URL", &(*service)["base_url"].get_ref<std::string&>());
                ImGui::InputText("Package", &(*service)["package"].get_ref<std::string&>());
                ImGui::InputText("API Key Env", &(*service)["api_key_env"].get_ref<std::string&>());
                bool enabled = json_bool(*service, "enabled", true);
                if (ImGui::Checkbox("Enabled", &enabled)) {
                    (*service)["enabled"] = enabled;
                }
                bool auth_header = json_bool(*service, "auth_header", false);
                if (ImGui::Checkbox("Send Auth Header", &auth_header)) {
                    (*service)["auth_header"] = auth_header;
                }
                int timeout_ms = json_int(*service, "timeout_ms", 120000);
                if (ImGui::InputInt("Timeout (ms)", &timeout_ms)) {
                    (*service)["timeout_ms"] = std::max(1000, timeout_ms);
                }
                std::string default_service = app.ai_config.value("default_service", std::string{});
                bool is_default = default_service == json_string(*service, "name");
                if (ImGui::Checkbox("Default Service", &is_default) && is_default) {
                    app.ai_config["default_service"] = json_string(*service, "name");
                }
                ImGui::SeparatorText("Defaults");
                json& options = (*service)["options"];
                if (!options.is_object()) {
                    options = json::object();
                }
                std::string reasoning = json_string(options, "reasoning");
                std::string reasoning_summary = json_string(options, "reasoningSummary");
                std::string cache_retention = json_string(options, "cacheRetention");
                std::string transport = json_string(options, "transport");
                const auto reasoning_options = json_string_options(app.ai_metadata, "thinking_levels", reasoning, true);
                const auto summary_options = json_string_options(app.ai_metadata, "reasoning_summaries", reasoning_summary, true);
                const auto cache_options = json_string_options(app.ai_metadata, "cache_retentions", cache_retention, true);
                const auto transport_options = json_string_options(app.ai_metadata, "transports", transport, true);
                if (combo_string_vector("Reasoning", reasoning_options, reasoning, "(unset)")) {
                    if (reasoning.empty()) {
                        options.erase("reasoning");
                    } else {
                        options["reasoning"] = reasoning;
                    }
                }
                if (combo_string_vector("Reasoning Summary", summary_options, reasoning_summary, "(unset)")) {
                    if (reasoning_summary.empty()) {
                        options.erase("reasoningSummary");
                    } else {
                        options["reasoningSummary"] = reasoning_summary;
                    }
                }
                if (combo_string_vector("Cache Retention", cache_options, cache_retention, "(unset)")) {
                    if (cache_retention.empty()) {
                        options.erase("cacheRetention");
                    } else {
                        options["cacheRetention"] = cache_retention;
                    }
                }
                if (combo_string_vector("Transport", transport_options, transport, "(unset)")) {
                    if (transport.empty()) {
                        options.erase("transport");
                    } else {
                        options["transport"] = transport;
                    }
                }

                ImGui::SeparatorText("Models");
                json& models = (*service)["models"];
                if (!models.is_array()) {
                    models = json::array();
                }
                if (ImGui::Button("Add Model")) {
                    models.push_back({
                        {"id", ""},
                        {"name", ""},
                        {"api", json_string(*service, "api")},
                        {"provider", json_string(*service, "provider")},
                        {"input", json::array({"text"})},
                        {"reasoning", false},
                    });
                    app.system.selected_ai_model_index = static_cast<int>(models.size()) - 1;
                }
                ImGui::SameLine();
                if (ImGui::Button("Remove Model")) {
                    if (!models.empty()) {
                        models.erase(models.begin() + std::clamp(app.system.selected_ai_model_index, 0, static_cast<int>(models.size()) - 1));
                        app.system.selected_ai_model_index = std::max(0, static_cast<int>(models.size()) - 1);
                    }
                }
                if (ImGui::BeginTable("ai-models-table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
                        ImVec2(0.0f, 160.0f), 580.0f * ImGui::GetStyle().FontScaleMain)) {
                    ImGui::TableSetupColumn("ID");
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                    ImGui::TableSetupColumn("Reasoning", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableHeadersRow();
                    for (std::size_t index = 0; index < models.size(); ++index) {
                        const auto& model = models[index];
                        const bool selected = static_cast<int>(index) == app.system.selected_ai_model_index;
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        if (ImGui::Selectable(json_string(model, "id", "(new model)").c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                            app.system.selected_ai_model_index = static_cast<int>(index);
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextWrapped("%s", json_string(model, "name").c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextWrapped("%s", joined_json_strings(model.value("input", json::array())).c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(json_bool(model, "reasoning", false) ? "yes" : "no");
                    }
                    ImGui::EndTable();
                }
                if (!models.empty()) {
                    const int model_index = std::clamp(app.system.selected_ai_model_index, 0, static_cast<int>(models.size()) - 1);
                    json& model = models.at(static_cast<std::size_t>(model_index));
                    ensure_ai_model_defaults(model, *service);
                    ImGui::InputText("Model ID", &model["id"].get_ref<std::string&>());
                    ImGui::InputText("Model Name", &model["name"].get_ref<std::string&>());
                    std::string model_api = json_string(model, "api", json_string(*service, "api"));
                    if (combo_string_vector("Model API", api_options, model_api, "(inherit)")) {
                        model["api"] = model_api;
                    }
                    auto model_provider_options = ai_provider_options(app, json_string(model, "provider", json_string(*service, "provider")));
                    std::string model_provider = json_string(model, "provider", json_string(*service, "provider"));
                    if (combo_string_vector("Model Provider", model_provider_options, model_provider, "(inherit)")) {
                        model["provider"] = model_provider;
                    }
                    std::string model_base_url = json_string(model, "baseUrl");
                    if (ImGui::InputText("Model Base URL", &model_base_url)) {
                        if (model_base_url.empty()) {
                            model.erase("baseUrl");
                        } else {
                            model["baseUrl"] = model_base_url;
                        }
                    }
                    bool model_reasoning = json_bool(model, "reasoning", false);
                    if (ImGui::Checkbox("Model Supports Reasoning", &model_reasoning)) {
                        model["reasoning"] = model_reasoning;
                    }
                    int context_window = json_int(model, "contextWindow", 0);
                    if (ImGui::InputInt("Context Window", &context_window)) {
                        if (context_window <= 0) {
                            model.erase("contextWindow");
                        } else {
                            model["contextWindow"] = context_window;
                        }
                    }
                    int max_tokens = json_int(model, "maxTokens", 0);
                    if (ImGui::InputInt("Max Tokens", &max_tokens)) {
                        if (max_tokens <= 0) {
                            model.erase("maxTokens");
                        } else {
                            model["maxTokens"] = max_tokens;
                        }
                    }
                    json& input = model["input"];
                    if (!input.is_array()) {
                        input = json::array();
                    }
                    ImGui::TextUnformatted("Input Types");
                    for (const auto& input_type : json_string_options(app.ai_metadata, "model_input_types", {}, false)) {
                        auto values = json_strings(input);
                        bool selected = std::find(values.begin(), values.end(), input_type) != values.end();
                        if (ImGui::Checkbox(input_type.c_str(), &selected)) {
                            if (selected) {
                                if (std::find(values.begin(), values.end(), input_type) == values.end()) {
                                    values.push_back(input_type);
                                }
                            } else {
                                values.erase(std::remove(values.begin(), values.end(), input_type), values.end());
                            }
                            input = values;
                        }
                        ImGui::SameLine();
                    }
                    ImGui::NewLine();
                }
            }
            ImGui::EndChild();
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Advanced JSON")) {
        if (ImGui::Button("Sync Raw Draft")) {
            sync_ai_config_draft(app);
        }
        next_action("Save Raw JSON");
        if (ImGui::Button("Save Raw JSON")) {
            save_ai_config(app);
        }
        ImGui::InputTextMultiline("AI Config Draft", &app.system.ai_config_draft, ImVec2(-FLT_MIN, 220.0f));
    }
    if (ImGui::CollapsingHeader("AI Metadata")) {
        std::vector<std::pair<std::string, std::string>> metadata_rows;
        add_table_row(metadata_rows, "Providers", std::to_string(json_strings(app.ai_metadata.value("providers", json::array())).size()), true);
        add_table_row(metadata_rows, "APIs", std::to_string(json_strings(app.ai_metadata.value("apis", json::array())).size()), true);
        add_table_row(metadata_rows, "API Styles", joined_json_strings(app.ai_metadata.value("api_styles", json::array())));
        add_table_row(metadata_rows, "Transports", joined_json_strings(app.ai_metadata.value("transports", json::array())));
        add_table_row(metadata_rows, "Reasoning Levels", joined_json_strings(app.ai_metadata.value("thinking_levels", json::array())));
        draw_property_table("ai-metadata-table", metadata_rows);
        if (ImGui::CollapsingHeader("AI Metadata Debug")) {
            draw_json_block("ai-metadata-json", app.ai_metadata, 200.0f, app.monospace_font);
        }
    }
}
void draw_logs_window(AppState& app) {
    if (ImGui::Button("Refresh Logs")) {
        refresh_system(app);
    }
    ImGui::Separator();
    if (app.logs.contains("logs") && app.logs.at("logs").is_array()) {
        if (ImGui::BeginTable(
                "logs-table",
                4,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
                ImVec2(0.0f, std::max(180.0f, ImGui::GetContentRegionAvail().y)), 750.0f * ImGui::GetStyle().FontScaleMain)) {
            ImGui::TableSetupColumn("Timestamp", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Logger", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Message");
            ImGui::TableHeadersRow();
            for (const auto& entry : app.logs.at("logs")) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextWrapped("%s", display_time_text(entry.value("timestamp_ms", json())).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(json_string(entry, "level").c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped("%s", json_string(entry, "logger").c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextWrapped("%s", json_string(entry, "message", json_string(entry, "formatted")).c_str());
            }
            ImGui::EndTable();
        }
    } else {
        ImGui::TextUnformatted("No logs loaded.");
    }
}

void draw_workspace_nav_button(AppState& app, WorkspaceGroup group) {
    const int index = static_cast<int>(group);
    const bool selected = app.active_group == index;
    const ImVec4 accent = selected ? dashboard_accent() : ImVec4(0.42f, 0.52f, 0.66f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? alpha(accent, 0.30f) : ImVec4(0.08f, 0.11f, 0.17f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, alpha(dashboard_accent(), selected ? 0.36f : 0.22f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, alpha(dashboard_accent(), 0.44f));
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? ImVec4(0.95f, 0.99f, 1.0f, 1.0f) : ImVec4(0.72f, 0.80f, 0.90f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 11.0f));
    if (ImGui::Button(kWorkspaceGroupLabels[static_cast<std::size_t>(index)], ImVec2(-FLT_MIN, 0.0f))) {
        apply_workspace_group(app, group);
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextWrapped("%s", kWorkspaceGroupDescriptions[static_cast<std::size_t>(index)]);
        ImGui::EndTooltip();
    }
}

void draw_workspace_navigation(AppState& app) {
    ImGui::SeparatorText("Workspace");
    draw_workspace_nav_button(app, WorkspaceGroup::overview);
    draw_workspace_nav_button(app, WorkspaceGroup::explore);
    draw_workspace_nav_button(app, WorkspaceGroup::capture);
    draw_workspace_nav_button(app, WorkspaceGroup::review);
    draw_workspace_nav_button(app, WorkspaceGroup::operations);
}

void draw_workspace_panel(AppState& app) {
    if (!app.windows.workspace_nav || app.compact_layout) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.50f, 0.68f, 0.90f, 0.14f));
    if (ImGui::Begin(kWorkspaceNavTitle, &app.windows.workspace_nav,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        draw_workspace_nav_button(app, WorkspaceGroup::overview);
        draw_workspace_nav_button(app, WorkspaceGroup::explore);
        draw_workspace_nav_button(app, WorkspaceGroup::capture);
        draw_workspace_nav_button(app, WorkspaceGroup::review);
        draw_workspace_nav_button(app, WorkspaceGroup::operations);
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void draw_status_window(AppState& app) {
    ImGui::TextWrapped("%s", app.banner_text.c_str());
    ImGui::TextWrapped(
        "%s · %s · %d request%s pending",
        app.auth_required ? "auth required" : "same-origin",
        app.project.empty() ? "all projects" : "project scoped",
        app.pending_requests,
        app.pending_requests == 1 ? "" : "s");
    if (ImGui::Button("Refresh All")) {
        refresh_all(app);
    }
    next_action("Save Settings");
    if (ImGui::Button("Save Settings")) {
        sync_project_scope(app);
        persist_settings(app);
        set_banner(app, "Saved dashboard scope, theme, and auth token locally.", BannerTone::success);
    }

    ImGui::SeparatorText("Scope");
    draw_labeled_input("Project", "status-project", app.project);
    if (ImGui::CollapsingHeader("Advanced scope")) {
        draw_labeled_input("Session", "status-session", app.scope.session);
        draw_labeled_input("Agent", "status-agent", app.scope.agent_id);
        draw_labeled_input("Conversation", "status-conversation", app.scope.conversation_id);
        draw_labeled_input("Task", "status-task", app.scope.task);
    }
    if (ImGui::CollapsingHeader("Authentication")) {
        draw_labeled_input("Bearer Token", "status-auth-token", app.auth_token, ImGuiInputTextFlags_Password);
    }

    if (ImGui::CollapsingHeader("Service snapshot")) {
        draw_key_value_grid({
            {"auth_required", app.auth_required},
            {"pending_requests", app.pending_requests},
            {"memory_entries", app.summary.value("memory_entries", 0)},
            {"health_score", app.summary.value("health_score", 0)},
            {"conflicts", app.summary.value("conflict_count", 0)},
            {"stale", app.summary.value("stale_count", 0)},
        });
    }
}

void build_default_dock_layout(ImGuiID dockspace_id, const ImVec2& size, bool compact_layout, float ui_scale) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, size);

    const std::array<const char*, 12> panels{{
        kStatusWindowTitle, kOverviewWindowTitle, kEditorWindowTitle, kExplorerWindowTitle,
        kDetailWindowTitle, kGraphWindowTitle, kSourcesWindowTitle, kAgentsWindowTitle,
        kSessionsWindowTitle, kContextWindowTitle, kSystemWindowTitle, kLogsWindowTitle,
    }};
    if (compact_layout) {
        for (const char* panel : panels) {
            ImGui::DockBuilderDockWindow(panel, dockspace_id);
        }
    } else {
        ImGuiID primary = dockspace_id;
        const float navigation_fraction = std::clamp(
            (kDesktopNavigationWidth * ui_scale) / std::max(size.x, 1.0f), 0.12f, 0.22f);
        ImGuiID navigation = ImGui::DockBuilderSplitNode(primary, ImGuiDir_Left, navigation_fraction, nullptr, &primary);
        ImGuiID secondary = ImGui::DockBuilderSplitNode(primary, ImGuiDir_Right, 0.30f, nullptr, &primary);
        ImGui::DockBuilderDockWindow(kWorkspaceNavTitle, navigation);
        for (const char* panel : {kOverviewWindowTitle, kEditorWindowTitle, kExplorerWindowTitle, kGraphWindowTitle,
                                  kAgentsWindowTitle, kSessionsWindowTitle, kSystemWindowTitle, kLogsWindowTitle}) {
            ImGui::DockBuilderDockWindow(panel, primary);
        }
        for (const char* panel : {kStatusWindowTitle, kDetailWindowTitle, kSourcesWindowTitle, kContextWindowTitle}) {
            ImGui::DockBuilderDockWindow(panel, secondary);
        }
    }
    ImGui::DockBuilderFinish(dockspace_id);
}

bool begin_dashboard_panel(const char* title, bool* open) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.50f, 0.68f, 0.90f, 0.14f));
    const bool visible = ImGui::Begin(title, open);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    return visible;
}

bool draw_panel_toggle(const char* label, bool& visible) {
    return ImGui::MenuItem(label, nullptr, &visible);
}

bool draw_view_menu(AppState& app) {
    bool settings_dirty = false;
    bool visibility_changed = false;
    const int current_theme = app.theme_index;
    const char* theme_preview = kThemeLabels[static_cast<std::size_t>(std::clamp(current_theme, 0, static_cast<int>(kThemeLabels.size()) - 1))];

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    if (ImGui::BeginCombo("Theme##view", theme_preview)) {
        for (int index = 0; index < static_cast<int>(kThemeLabels.size()); ++index) {
            const bool selected = app.theme_index == index;
            if (ImGui::Selectable(kThemeLabels[static_cast<std::size_t>(index)], selected)) {
                app.theme_index = index;
                apply_theme(static_cast<ThemePreset>(app.theme_index), app.ui_scale);
                settings_dirty = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    float scale = app.ui_scale;
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("UI Scale", &scale, kUiScaleMin, kUiScaleMax, "%.2f")) {
        set_ui_scale(app, scale);
        settings_dirty = true;
    }
    if (ImGui::MenuItem("Reset UI Scale")) {
        set_ui_scale(app, kDefaultUiScale);
        settings_dirty = true;
    }
    ImGui::Separator();

    struct PanelMenuEntry {
        const char* label;
        bool* flag;
    };

    const auto command = std::array<PanelMenuEntry, 1>{{
        {kStatusWindowTitle, &app.windows.status},
    }};
    const auto memory = std::array<PanelMenuEntry, 5>{{
        {kOverviewWindowTitle, &app.windows.overview},
        {kEditorWindowTitle, &app.windows.editor},
        {kExplorerWindowTitle, &app.windows.explorer},
        {kDetailWindowTitle, &app.windows.detail},
        {kGraphWindowTitle, &app.windows.graph},
    }};
    const auto analysis = std::array<PanelMenuEntry, 4>{{
        {kSourcesWindowTitle, &app.windows.sources},
        {kAgentsWindowTitle, &app.windows.agents},
        {kSessionsWindowTitle, &app.windows.sessions},
        {kContextWindowTitle, &app.windows.context},
    }};
    const auto runtime = std::array<PanelMenuEntry, 2>{{
        {kSystemWindowTitle, &app.windows.system},
        {kLogsWindowTitle, &app.windows.logs},
    }};

    const auto for_each_panel = [&](auto&& fn) {
        const auto apply_group = [&](const auto& entries) {
            for (const auto& entry : entries) {
                fn(entry);
            }
        };
        apply_group(command);
        apply_group(memory);
        apply_group(analysis);
        apply_group(runtime);
    };

    const auto set_group_visibility = [&](const auto& entries, bool target_visibility) {
        for (const auto& entry : entries) {
            if (*entry.flag != target_visibility) {
                *entry.flag = target_visibility;
                visibility_changed = true;
            }
        }
    };

    bool all_panels_visible = true;
    for_each_panel([&](const PanelMenuEntry& entry) {
        all_panels_visible = all_panels_visible && *entry.flag;
    });

    if (ImGui::MenuItem(all_panels_visible ? "Hide All Panels" : "Show All Panels")) {
        const bool target_visibility = !all_panels_visible;
        for_each_panel([&](const PanelMenuEntry& entry) {
            if (*entry.flag != target_visibility) {
                *entry.flag = target_visibility;
                visibility_changed = true;
            }
        });
    }

    const auto draw_group_menu = [&](const char* name, const auto& entries, const char* show_all_label, const char* hide_all_label) {
        if (!ImGui::BeginMenu(name)) {
            return;
        }

        bool group_all_visible = true;
        for (const auto& entry : entries) {
            group_all_visible = group_all_visible && *entry.flag;
        }

        if (ImGui::MenuItem(group_all_visible ? hide_all_label : show_all_label, nullptr, group_all_visible)) {
            set_group_visibility(entries, !group_all_visible);
        }

        ImGui::Separator();
        for (const auto& entry : entries) {
            visibility_changed = draw_panel_toggle(entry.label, *entry.flag) || visibility_changed;
        }
        ImGui::EndMenu();
    };

    draw_group_menu("Command", command, "Show Command Panels", "Hide Command Panels");
    draw_group_menu("Memory", memory, "Show All Memory Panels", "Hide All Memory Panels");
    draw_group_menu("Analysis", analysis, "Show All Analysis Panels", "Hide All Analysis Panels");
    draw_group_menu("System", runtime, "Show All System Panels", "Hide All System Panels");

    ImGui::Separator();
    if (ImGui::MenuItem("Reset Dock Layout")) {
        app.active_group = static_cast<int>(WorkspaceGroup::overview);
        app.windows = default_window_visibility();
        app.dockspace_configured = false;
        app.dock_layout_save_pending = true;
        app.last_saved_dock_layout.clear();
        save_docking_layout("", 0, app.compact_layout);
        app.metrics_window_open = false;
        app.style_editor_open = false;
        visibility_changed = true;
    }

    ImGui::Separator();
    ImGui::MenuItem("Metrics / Debugger", nullptr, &app.metrics_window_open);
    ImGui::MenuItem("Style Editor", nullptr, &app.style_editor_open);

    if (visibility_changed) {
        settings_dirty = true;
    }
    return settings_dirty;
}

bool render_dashboard_menu_bar(AppState& app) {
    bool settings_dirty = false;
    if (ImGui::BeginMenu("Workspace")) {
        draw_workspace_navigation(app);
        ImGui::Separator();
        settings_dirty = ImGui::MenuItem(
            "Show docked navigation", nullptr, &app.windows.workspace_nav, !app.compact_layout) || settings_dirty;
        if (app.compact_layout) {
            ImGui::TextDisabled("Navigation is available in this menu at narrow widths.");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Actions")) {
        if (ImGui::MenuItem("Refresh All")) {
            refresh_all(app);
        }
        if (ImGui::MenuItem("Persist Settings")) {
            sync_project_scope(app);
            persist_settings(app);
            set_banner(app, "Persisted dashboard settings.", BannerTone::success);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        settings_dirty = draw_view_menu(app) || settings_dirty;
        ImGui::EndMenu();
    }

    ImGui::Separator();
    ImGui::TextDisabled(app.pending_requests == 0 ? "Ready" : "%d pending", app.pending_requests);
    return settings_dirty;
}

void render_docked_windows(AppState& app) {
    const WindowVisibility previous = app.windows;

    draw_workspace_panel(app);

    if (app.windows.status) {
        if (begin_dashboard_panel(kStatusWindowTitle, &app.windows.status)) {
            draw_status_window(app);
        }
        ImGui::End();
    }

    if (app.windows.overview) {
        if (begin_dashboard_panel(kOverviewWindowTitle, &app.windows.overview)) {
            draw_overview_tab(app);
        }
        ImGui::End();
    }

    if (app.windows.editor) {
        if (begin_dashboard_panel(kEditorWindowTitle, &app.windows.editor)) {
            draw_editor_tab(app);
        }
        ImGui::End();
    }

    if (app.windows.explorer) {
        if (begin_dashboard_panel(kExplorerWindowTitle, &app.windows.explorer)) {
            draw_explorer_tab(app);
        }
        ImGui::End();
    }

    if (app.windows.detail) {
        if (begin_dashboard_panel(kDetailWindowTitle, &app.windows.detail)) {
            draw_memory_detail_window(app);
        }
        ImGui::End();
    }

    if (app.windows.graph) {
        if (begin_dashboard_panel(kGraphWindowTitle, &app.windows.graph)) {
            draw_graph_tab(app);
        }
        ImGui::End();
    }

    if (app.windows.sources) {
        if (begin_dashboard_panel(kSourcesWindowTitle, &app.windows.sources)) {
            draw_sources_tab(app);
        }
        ImGui::End();
    }

    if (app.windows.agents) {
        if (begin_dashboard_panel(kAgentsWindowTitle, &app.windows.agents)) {
            draw_agents_window(app);
        }
        ImGui::End();
    }

    if (app.windows.sessions) {
        if (begin_dashboard_panel(kSessionsWindowTitle, &app.windows.sessions)) {
            draw_sessions_window(app);
        }
        ImGui::End();
    }

    if (app.windows.context) {
        if (begin_dashboard_panel(kContextWindowTitle, &app.windows.context)) {
            draw_context_tab(app);
        }
        ImGui::End();
    }

    if (app.windows.system) {
        if (begin_dashboard_panel(kSystemWindowTitle, &app.windows.system)) {
            draw_system_tab(app);
        }
        ImGui::End();
    }

    if (app.windows.logs) {
        if (begin_dashboard_panel(kLogsWindowTitle, &app.windows.logs)) {
            draw_logs_window(app);
        }
        ImGui::End();
    }

    if (previous.workspace_nav != app.windows.workspace_nav ||
        previous.status != app.windows.status || previous.overview != app.windows.overview ||
        previous.editor != app.windows.editor || previous.explorer != app.windows.explorer ||
        previous.detail != app.windows.detail || previous.graph != app.windows.graph ||
        previous.sources != app.windows.sources || previous.agents != app.windows.agents ||
        previous.sessions != app.windows.sessions || previous.context != app.windows.context ||
        previous.system != app.windows.system || previous.logs != app.windows.logs) {
        persist_settings(app);
    }

    if (app.metrics_window_open) {
        ImGui::ShowMetricsWindow(&app.metrics_window_open);
    }
    if (app.style_editor_open) {
        ImGui::Begin("Style Editor", &app.style_editor_open);
        ImGui::ShowStyleEditor();
        ImGui::End();
    }
}

void render_dashboard(AppState& app) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const ImGuiWindowFlags host_flags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_MenuBar;
    const bool host_open = ImGui::Begin("qtmem-dockspace-root", nullptr, host_flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    if (host_open) {
        draw_dashboard_background(viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y));
        bool settings_dirty = false;
        if (ImGui::BeginMenuBar()) {
            settings_dirty = render_dashboard_menu_bar(app) || settings_dirty;
            ImGui::EndMenuBar();
        }

        const ImGuiID dockspace_id = ImGui::GetID("qtmem-dockspace");
        if (!app.dockspace_configured) {
            build_default_dock_layout(dockspace_id, viewport->Size, app.compact_layout, app.ui_scale);
            app.dockspace_configured = true;
            app.dock_layout_save_pending = true;
        }
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
        if (settings_dirty) {
            persist_settings(app);
        }
    }
    ImGui::End();

    render_docked_windows(app);
    if (!app.pending_focus_window.empty()) {
        ImGui::SetWindowFocus(app.pending_focus_window.c_str());
        app.pending_focus_window.clear();
    }
}

void resize_dashboard_canvas(AppState& app) {
    double css_width = 0.0;
    double css_height = 0.0;
    if (emscripten_get_element_css_size("#canvas", &css_width, &css_height) != EMSCRIPTEN_RESULT_SUCCESS) {
        return;
    }
    const int width = std::max(1, static_cast<int>(std::lround(css_width)));
    const int height = std::max(1, static_cast<int>(std::lround(css_height)));
    if (width != app.canvas_css_width || height != app.canvas_css_height) {
        SDL_SetWindowSize(app.window, width, height);
        app.canvas_css_width = width;
        app.canvas_css_height = height;
    }
}

void update_dashboard_layout_mode(AppState& app) {
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(app.window, &width, &height);
    (void)height;
    const bool compact_layout = static_cast<float>(width) < kCompactLayoutWidth * app.ui_scale;
    if (compact_layout == app.compact_layout) {
        return;
    }

    if (app.dockspace_configured) {
        app.dock_layout_save_pending = true;
        persist_docking_layout(app);
    }
    app.compact_layout = compact_layout;
    const auto incoming_layout = load_docking_layout(app.compact_layout);
    if (!incoming_layout.empty()) {
        ImGui::LoadIniSettingsFromMemory(incoming_layout.c_str(), incoming_layout.size());
        app.last_saved_dock_layout = incoming_layout;
        app.dockspace_configured = true;
        app.dock_layout_save_pending = false;
    } else {
        app.last_saved_dock_layout.clear();
        app.dockspace_configured = false;
        app.dock_layout_save_pending = true;
    }
    constexpr std::array<const char*, static_cast<std::size_t>(WorkspaceGroup::count)> primary_windows{{
        kOverviewWindowTitle, kExplorerWindowTitle, kEditorWindowTitle, kOverviewWindowTitle, kAgentsWindowTitle,
    }};
    app.pending_focus_window = primary_windows[static_cast<std::size_t>(app.active_group)];
}

void main_loop(void* user_data) {
    auto& app = *static_cast<AppState*>(user_data);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT) {
            app.running = false;
        }
    }

    if (!app.started_initial_refresh) {
        app.started_initial_refresh = true;
        refresh_all(app);
    }

    resize_dashboard_canvas(app);
    update_dashboard_layout_mode(app);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    sync_project_scope(app);
    render_dashboard(app);
    persist_docking_layout(app);
    ImGui::Render();

    int drawable_width = 0;
    int drawable_height = 0;
    SDL_GL_GetDrawableSize(app.window, &drawable_width, &drawable_height);
    glViewport(0, 0, drawable_width, drawable_height);
    const ImVec4 clear = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    glClearColor(clear.x, clear.y, clear.z, clear.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(app.window);
}

SDL_GLContext create_gl_context(SDL_Window* window) {
    struct GlContextAttempt {
        int depth_size;
        int stencil_size;
    };

    constexpr std::array<GlContextAttempt, 3> attempts{{
        {24, 8},
        {16, 8},
        {0, 0},
    }};

    for (const auto& attempt : attempts) {
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, attempt.depth_size);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, attempt.stencil_size);

        if (auto* context = SDL_GL_CreateContext(window)) {
            return context;
        }
        SDL_ClearError();
    }

    return nullptr;
}

}  // namespace

int main() {
    AppState app;
    app.project = query_parameter_get("project");
    if (app.project.empty()) {
        app.project = local_storage_get("qtmem.dashboard.project");
    }
    app.scope.project = app.project;
    app.auth_token = local_storage_get("qtmem.dashboard.auth_token");
    app.auth_required = js_bootstrap_auth_required() != 0;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    app.window = SDL_CreateWindow(
        kAppTitle,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        kInitialWidth,
        kInitialHeight,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (app.window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    app.gl_context = create_gl_context(app.window);
    if (app.gl_context == nullptr) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(app.window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_MakeCurrent(app.window, app.gl_context);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;
    ImGui::Spectrum::LoadFont(kBaseFontSize);
    app.monospace_font = load_monospace_font();
    load_persistent_ui_state(app);
    ImGui_ImplSDL2_InitForOpenGL(app.window, app.gl_context);
    ImGui_ImplOpenGL3_Init(kGlslVersion);

    set_banner(
        app,
        app.auth_required
            ? "Service auth is enabled. Enter a bearer token in the shell to load protected dashboard data."
            : "Booting qtmem dashboard...",
        app.auth_required ? BannerTone::warning : BannerTone::info);
    persist_settings(app);

    emscripten_set_main_loop_arg(main_loop, &app, 0, true);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(app.gl_context);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
