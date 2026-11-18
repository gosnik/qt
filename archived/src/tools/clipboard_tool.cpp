#include "qt/tools/clipboard_tool.hpp"

#include "qt/ai/images.hpp"
#include "qt/foundation/filesystem.hpp"
#include "qt/foundation/process.hpp"
#include "qt/tools/tool_validation.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <unistd.h>

namespace qt::tools {

namespace {

constexpr std::size_t kMaxClipboardTextBytes = 1024U * 1024U;
constexpr std::size_t kMaxClipboardImageBytes = 16U * 1024U * 1024U;
constexpr std::int64_t kClipboardListTimeoutMs = 1000;
constexpr std::int64_t kClipboardReadTimeoutMs = 3000;
constexpr std::int64_t kClipboardPowerShellTimeoutMs = 5000;

std::string path_separator() {
#ifdef _WIN32
    return ";";
#else
    return ":";
#endif
}

std::vector<ClipboardCommandPlan> preferred_clipboard_commands() {
    std::vector<ClipboardCommandPlan> plans;

    auto add = [&plans](std::string name, std::string executable,
                        std::vector<std::string> arguments) {
        ClipboardCommandPlan plan;
        plan.name = std::move(name);
        plan.executable = std::move(executable);
        plan.arguments = std::move(arguments);
        plans.push_back(std::move(plan));
    };

#ifdef __APPLE__
    add("pbcopy", "pbcopy", {});
    add("wl-copy", "wl-copy", {});
    add("xclip", "xclip", {"-selection", "clipboard"});
    add("clip.exe", "clip.exe", {});
#elif defined(_WIN32)
    add("clip.exe", "clip.exe", {});
    add("pbcopy", "pbcopy", {});
    add("wl-copy", "wl-copy", {});
    add("xclip", "xclip", {"-selection", "clipboard"});
#else
    add("wl-copy", "wl-copy", {});
    add("xclip", "xclip", {"-selection", "clipboard"});
    add("pbcopy", "pbcopy", {});
    add("clip.exe", "clip.exe", {});
#endif

    return plans;
}

bool command_is_available(const std::set<std::string>& available,
                          const std::string& executable) {
    if (available.count(executable) != 0U) {
        return true;
    }
#ifdef _WIN32
    const auto dot = executable.find('.');
    if (dot != std::string::npos && available.count(executable.substr(0U, dot)) != 0U) {
        return true;
    }
#endif
    return false;
}

std::string join_command(const ClipboardCommandPlan& plan) {
    std::ostringstream stream;
    stream << plan.executable;
    for (const auto& argument : plan.arguments) {
        stream << ' ' << argument;
    }
    return stream.str();
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string base_mime_type(std::string mime_type) {
    const auto semicolon = mime_type.find(';');
    if (semicolon != std::string::npos) {
        mime_type.erase(semicolon);
    }
    auto begin = mime_type.begin();
    while (begin != mime_type.end() &&
           std::isspace(static_cast<unsigned char>(*begin)) != 0) {
        begin = mime_type.erase(begin);
    }
    while (!mime_type.empty() &&
           std::isspace(static_cast<unsigned char>(mime_type.back())) != 0) {
        mime_type.pop_back();
    }
    return lowercase_copy(std::move(mime_type));
}

std::string option_environment_value(const ClipboardImageReadOptions& options,
                                     const std::string& name) {
    const auto override = options.environment.find(name);
    if (override != options.environment.end()) {
        return override->second;
    }
    const char* value = std::getenv(name.c_str());
    return value == nullptr ? std::string() : std::string(value);
}

std::string current_platform(const ClipboardImageReadOptions& options) {
    if (options.platform) {
        return lowercase_copy(*options.platform);
    }
#ifdef __APPLE__
    return "darwin";
#elif defined(_WIN32)
    return "win32";
#else
    return "linux";
#endif
}

std::vector<qt::foundation::ProcessEnvironmentVariable> process_environment(
    const ClipboardImageReadOptions& options) {
    std::vector<qt::foundation::ProcessEnvironmentVariable> environment;
    environment.reserve(options.environment.size());
    for (const auto& entry : options.environment) {
        environment.push_back(
            qt::foundation::ProcessEnvironmentVariable{entry.first, entry.second});
    }
    return environment;
}

std::optional<std::string> run_clipboard_command(
    const std::vector<std::string>& argv, std::int64_t timeout_ms,
    const ClipboardImageReadOptions& options) {
    qt::foundation::ProcessRequest request;
    request.mode = qt::foundation::ProcessLaunchMode::argv;
    request.argv = argv;
    request.timeout_ms = timeout_ms;
    request.environment = process_environment(options);
    const auto result = qt::foundation::run_process(request);
    if (!result || result.value().timed_out || result.value().exit_code != 0) {
        return std::nullopt;
    }
    return result.value().stdout_text;
}

std::vector<std::string> split_clipboard_types(const std::string& text) {
    std::vector<std::string> types;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto base = base_mime_type(line);
        if (!base.empty()) {
            types.push_back(std::move(line));
        }
    }
    return types;
}

bool is_wayland_session(const ClipboardImageReadOptions& options) {
    return !option_environment_value(options, "WAYLAND_DISPLAY").empty() ||
           option_environment_value(options, "XDG_SESSION_TYPE") == "wayland";
}

bool is_wsl_session(const ClipboardImageReadOptions& options) {
    if (!option_environment_value(options, "WSL_DISTRO_NAME").empty() ||
        !option_environment_value(options, "WSLENV").empty()) {
        return true;
    }
    const auto version = qt::foundation::read_text_file("/proc/version");
    if (!version) {
        return false;
    }
    const auto lower = lowercase_copy(version.value());
    return lower.find("microsoft") != std::string::npos ||
           lower.find("wsl") != std::string::npos;
}

ClipboardImageReadResult make_clipboard_image_result(std::string bytes,
                                                     std::string mime_type,
                                                     std::string source) {
    ClipboardImageReadResult result;
    result.ok = true;
    result.bytes = std::move(bytes);
    result.mime_type = base_mime_type(std::move(mime_type));
    result.source = std::move(source);
    return result;
}

std::optional<ClipboardImageReadResult> read_clipboard_image_via_wl_paste(
    const ClipboardImageReadOptions& options) {
    const auto list = run_clipboard_command(
        {"wl-paste", "--list-types"}, kClipboardListTimeoutMs, options);
    if (!list) {
        return std::nullopt;
    }
    const auto selected = preferred_clipboard_image_mime_type(
        split_clipboard_types(*list));
    if (!selected) {
        return std::nullopt;
    }
    auto data = run_clipboard_command(
        {"wl-paste", "--type", *selected, "--no-newline"},
        kClipboardReadTimeoutMs, options);
    if (!data || data->empty()) {
        return std::nullopt;
    }
    return make_clipboard_image_result(std::move(*data), *selected, "wl-paste");
}

std::optional<ClipboardImageReadResult> read_clipboard_image_via_xclip(
    const ClipboardImageReadOptions& options) {
    std::vector<std::string> candidate_types;
    const auto targets = run_clipboard_command(
        {"xclip", "-selection", "clipboard", "-t", "TARGETS", "-o"},
        kClipboardListTimeoutMs, options);
    if (targets) {
        candidate_types = split_clipboard_types(*targets);
    }

    std::vector<std::string> try_types;
    const auto preferred =
        candidate_types.empty()
            ? std::optional<std::string>()
            : preferred_clipboard_image_mime_type(candidate_types);
    if (preferred) {
        try_types.push_back(*preferred);
    }
    for (const auto& mime_type :
         {"image/png", "image/jpeg", "image/webp", "image/gif"}) {
        if (std::find(try_types.begin(), try_types.end(), mime_type) ==
            try_types.end()) {
            try_types.push_back(mime_type);
        }
    }

    for (const auto& mime_type : try_types) {
        auto data = run_clipboard_command(
            {"xclip", "-selection", "clipboard", "-t", mime_type, "-o"},
            kClipboardReadTimeoutMs, options);
        if (data && !data->empty()) {
            return make_clipboard_image_result(std::move(*data), mime_type, "xclip");
        }
    }
    return std::nullopt;
}

std::optional<ClipboardImageReadResult> read_clipboard_image_via_powershell(
    const ClipboardImageReadOptions& options) {
    const auto temp_path =
        std::filesystem::temp_directory_path() /
        ("qt-wsl-clip-" + std::to_string(static_cast<long long>(::getpid())) +
         ".png");
    std::error_code ignored;
    std::filesystem::remove(temp_path, ignored);

    const auto win_path_result = run_clipboard_command(
        {"wslpath", "-w", temp_path.generic_string()}, kClipboardListTimeoutMs,
        options);
    if (!win_path_result) {
        return std::nullopt;
    }
    std::string win_path = *win_path_result;
    while (!win_path.empty() &&
           (win_path.back() == '\n' || win_path.back() == '\r')) {
        win_path.pop_back();
    }
    if (win_path.empty()) {
        return std::nullopt;
    }
    std::string quoted_path;
    quoted_path.reserve(win_path.size());
    for (const char ch : win_path) {
        quoted_path.push_back(ch);
        if (ch == '\'') {
            quoted_path.push_back('\'');
        }
    }

    const std::string script =
        "Add-Type -AssemblyName System.Windows.Forms; "
        "Add-Type -AssemblyName System.Drawing; "
        "$path = '" +
        quoted_path +
        "'; $img = [System.Windows.Forms.Clipboard]::GetImage(); "
        "if ($img) { $img.Save($path, "
        "[System.Drawing.Imaging.ImageFormat]::Png); Write-Output 'ok' } "
        "else { Write-Output 'empty' }";
    const auto result = run_clipboard_command(
        {"powershell.exe", "-NoProfile", "-Command", script},
        kClipboardPowerShellTimeoutMs, options);
    if (!result || base_mime_type(*result) != "ok") {
        std::filesystem::remove(temp_path, ignored);
        return std::nullopt;
    }

    const auto bytes = qt::foundation::read_text_file(temp_path);
    std::filesystem::remove(temp_path, ignored);
    if (!bytes || bytes.value().empty()) {
        return std::nullopt;
    }
    return make_clipboard_image_result(bytes.value(), "image/png", "powershell");
}

ClipboardImageReadResult convert_unsupported_clipboard_image(
    ClipboardImageReadResult result) {
    if (!result.ok || is_supported_clipboard_image_mime_type(result.mime_type)) {
        return result;
    }
    if (!clipboard_image_requires_png_conversion(result.mime_type)) {
        result.ok = false;
        result.error = "clipboard data is not an image";
        return result;
    }

    const std::vector<std::uint8_t> bytes(result.bytes.begin(), result.bytes.end());
    qt::ai::ImageContent image{result.mime_type, qt::ai::encode_image_base64(bytes)};
    qt::ai::ImageResizePlanOptions options;
    options.target_media_type = "image/png";
    const auto converted = qt::ai::unsupported_image_converter().convert(image, options);
    if (!converted.image) {
        result.ok = false;
        result.error = "clipboard image could not be converted to image/png";
        return result;
    }
    const auto converted_bytes =
        qt::ai::decode_image_base64(converted.image->data);
    if (!converted_bytes) {
        result.ok = false;
        result.error = "converted clipboard image is not valid base64";
        return result;
    }

    result.bytes.assign(reinterpret_cast<const char*>(converted_bytes->data()),
                        converted_bytes->size());
    result.mime_type = "image/png";
    result.converted_to_png = true;
    return result;
}

std::optional<ClipboardPayloadPlan> make_text_payload(
    const ToolRequest& request, ToolResult& error_result) {
    const auto text = require_string_argument(request, "text", "clipboard");
    if (!text) {
        error_result = make_invalid_request(text.error().summary(), request);
        return std::nullopt;
    }
    if (!is_valid_clipboard_text(text.value())) {
        error_result = make_invalid_request(
            "clipboard text exceeds the 1048576 byte limit", request);
        return std::nullopt;
    }

    ClipboardPayloadPlan payload;
    payload.kind = ClipboardPayloadKind::text;
    payload.source = "argument:text";
    payload.mime_type = "text/plain";
    payload.stdin_bytes = text.value().size();
    return payload;
}

std::optional<ClipboardPayloadPlan> make_image_payload(
    const ToolRequest& request, const ToolContext& context,
    ToolResult& error_result) {
    const auto image_path = require_string_argument(request, "image_path", "clipboard");
    if (!image_path) {
        error_result = make_invalid_request(image_path.error().summary(), request);
        return std::nullopt;
    }

    qt::foundation::Path path = image_path.value();
    if (path.is_relative()) {
        const auto base = context.current_directory.value_or(
            context.workspace_root.value_or(std::filesystem::current_path()));
        path = base / path;
    }
    const auto resolved_path = qt::foundation::canonicalize_path(path);
    if (!resolved_path) {
        error_result = make_invalid_request(resolved_path.error().summary(), request);
        return std::nullopt;
    }
    if (context.workspace_root) {
        const auto root =
            qt::foundation::canonicalize_path(*context.workspace_root);
        if (!root) {
            error_result = make_execution_error(
                ToolResultStatus::execution_error, root.error().summary(), request);
            return std::nullopt;
        }
        if (!is_workspace_path_allowed(resolved_path.value(), root.value(),
                                       context)) {
            error_result = make_execution_error(
                ToolResultStatus::permission_denied,
                "clipboard image path escapes workspace_root", request);
            return std::nullopt;
        }
    }

    const auto mime_type =
        infer_clipboard_image_mime_type(resolved_path.value().generic_string());
    if (!mime_type) {
        error_result = make_invalid_request(
            "clipboard image_path must end with .png, .jpg, .jpeg, .gif, or .webp",
            request);
        return std::nullopt;
    }

    std::error_code error;
    if (!std::filesystem::is_regular_file(resolved_path.value(), error) ||
        error) {
        error_result = make_invalid_request(
            "clipboard image_path must point to a readable regular file", request);
        return std::nullopt;
    }

    const auto bytes = std::filesystem::file_size(resolved_path.value(), error);
    if (error) {
        error_result = make_invalid_request(
            "clipboard image_path size could not be read", request);
        return std::nullopt;
    }
    if (bytes > kMaxClipboardImageBytes) {
        error_result = make_invalid_request(
            "clipboard image_path exceeds the 16777216 byte limit", request);
        return std::nullopt;
    }

    ClipboardPayloadPlan payload;
    payload.kind = ClipboardPayloadKind::image;
    payload.source = resolved_path.value().generic_string();
    payload.mime_type = *mime_type;
    payload.stdin_bytes = static_cast<std::size_t>(bytes);
    return payload;
}

std::optional<std::vector<std::string>> available_commands_from_metadata(
    const ToolContext& context) {
    std::vector<std::string> result;
    if (!context.metadata.is_object() || !context.metadata.contains("clipboard") ||
        !context.metadata.at("clipboard").is_object()) {
        return std::nullopt;
    }

    const auto& clipboard = context.metadata.at("clipboard");
    if (!clipboard.contains("available_commands") ||
        !clipboard.at("available_commands").is_array()) {
        return std::nullopt;
    }

    for (const auto& item : clipboard.at("available_commands")) {
        if (item.is_string()) {
            result.push_back(item.get<std::string>());
        }
    }
    return result;
}

std::vector<std::string> available_commands_for_context(const ToolContext& context) {
    auto overridden = available_commands_from_metadata(context);
    if (overridden) {
        return *overridden;
    }
    return detect_available_clipboard_commands();
}

}  // namespace

std::vector<std::string> detect_available_clipboard_commands() {
    const char* path = std::getenv("PATH");
    if (path == nullptr) {
        return {};
    }

    std::set<std::string> found;
    const auto separator = path_separator();
    std::string text = path;
    std::size_t offset = 0U;
    while (offset <= text.size()) {
        const auto next = text.find(separator, offset);
        const auto segment = text.substr(
            offset, next == std::string::npos ? std::string::npos : next - offset);
        offset = next == std::string::npos ? text.size() + 1U : next + separator.size();

        if (segment.empty()) {
            continue;
        }

        std::error_code error;
        const std::filesystem::path directory(segment);
        if (!std::filesystem::is_directory(directory, error) || error) {
            continue;
        }

        for (const auto& candidate : preferred_clipboard_commands()) {
            const auto path_candidate = directory / candidate.executable;
            if (std::filesystem::exists(path_candidate, error) && !error) {
                found.insert(candidate.executable);
            }
        }
    }

    return {found.begin(), found.end()};
}

std::vector<ClipboardCommandPlan> plan_clipboard_copy_commands(
    const std::vector<std::string>& available_commands) {
    const std::set<std::string> available(available_commands.begin(),
                                          available_commands.end());
    auto plans = preferred_clipboard_commands();
    for (auto& plan : plans) {
        plan.available = command_is_available(available, plan.executable);
        plan.reason = plan.available ? "available" : "executable not found";
    }
    return plans;
}

std::optional<ClipboardCommandPlan> select_clipboard_copy_command(
    const std::vector<std::string>& available_commands) {
    for (const auto& plan : plan_clipboard_copy_commands(available_commands)) {
        if (plan.available) {
            return plan;
        }
    }
    return std::nullopt;
}

bool is_valid_clipboard_text(const std::string& text) noexcept {
    return text.size() <= kMaxClipboardTextBytes;
}

std::optional<std::string> extension_for_clipboard_image_mime_type(
    const std::string& mime_type) {
    const auto base = base_mime_type(mime_type);
    if (base == "image/png") {
        return "png";
    }
    if (base == "image/jpeg") {
        return "jpg";
    }
    if (base == "image/webp") {
        return "webp";
    }
    if (base == "image/gif") {
        return "gif";
    }
    return std::nullopt;
}

bool is_supported_clipboard_image_mime_type(const std::string& mime_type) noexcept {
    return extension_for_clipboard_image_mime_type(mime_type).has_value();
}

std::optional<std::string> preferred_clipboard_image_mime_type(
    const std::vector<std::string>& mime_types) {
    std::vector<std::pair<std::string, std::string>> normalized;
    normalized.reserve(mime_types.size());
    for (const auto& mime_type : mime_types) {
        const auto base = base_mime_type(mime_type);
        if (!base.empty()) {
            normalized.push_back({mime_type, base});
        }
    }

    for (const auto& preferred :
         {"image/png", "image/jpeg", "image/webp", "image/gif"}) {
        const auto match =
            std::find_if(normalized.begin(), normalized.end(),
                         [preferred](const auto& item) {
                             return item.second == preferred;
                         });
        if (match != normalized.end()) {
            return match->first;
        }
    }

    const auto any_image =
        std::find_if(normalized.begin(), normalized.end(), [](const auto& item) {
            return item.second.rfind("image/", 0U) == 0U;
        });
    if (any_image != normalized.end()) {
        return any_image->first;
    }
    return std::nullopt;
}

bool clipboard_image_requires_png_conversion(const std::string& mime_type) noexcept {
    const auto base = base_mime_type(mime_type);
    return base.rfind("image/", 0U) == 0U &&
           !is_supported_clipboard_image_mime_type(base);
}

std::optional<std::string> infer_clipboard_image_mime_type(
    const std::string& path) {
    const std::string extension =
        lowercase_copy(std::filesystem::path(path).extension().generic_string());
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    if (extension == ".gif") {
        return "image/gif";
    }
    if (extension == ".webp") {
        return "image/webp";
    }
    return std::nullopt;
}

bool is_supported_clipboard_image_path(const std::string& path) noexcept {
    return infer_clipboard_image_mime_type(path).has_value();
}

ClipboardImageWriteResult write_clipboard_image_to_temp_path(
    const std::string& bytes, const std::string& mime_type,
    const std::filesystem::path& temp_dir, const std::string& unique_id) {
    ClipboardImageWriteResult result;
    result.mime_type = base_mime_type(mime_type);
    result.bytes = bytes.size();

    if (bytes.empty()) {
        result.error = "clipboard image bytes are empty";
        return result;
    }
    const auto extension = extension_for_clipboard_image_mime_type(mime_type);
    if (!extension) {
        result.error = "clipboard image mime type is not supported";
        return result;
    }
    if (unique_id.empty()) {
        result.error = "clipboard image unique id is empty";
        return result;
    }

    std::error_code error;
    if (!std::filesystem::exists(temp_dir, error) || error ||
        !std::filesystem::is_directory(temp_dir, error) || error) {
        result.error = "clipboard image temp directory is not available";
        return result;
    }

    const auto path =
        temp_dir / ("qt-clipboard-" + unique_id + "." + *extension);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        result.error = "clipboard image temp file could not be opened";
        return result;
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        result.error = "clipboard image temp file could not be written";
        return result;
    }

    result.ok = true;
    result.path = path;
    result.mime_type = "image/" + (*extension == "jpg" ? std::string("jpeg") : *extension);
    return result;
}

ClipboardImageReadResult read_clipboard_image(
    const ClipboardImageReadOptions& options) {
    ClipboardImageReadResult missing;
    missing.error = "clipboard image is not available";

    if (!option_environment_value(options, "TERMUX_VERSION").empty()) {
        return missing;
    }

    std::optional<ClipboardImageReadResult> image;
    const auto platform = current_platform(options);
    if (platform == "linux") {
        const bool wayland = is_wayland_session(options);
        const bool wsl = is_wsl_session(options);
        if (wayland || wsl) {
            image = read_clipboard_image_via_wl_paste(options);
            if (!image) {
                image = read_clipboard_image_via_xclip(options);
            }
        }
        if (!image && wsl) {
            image = read_clipboard_image_via_powershell(options);
        }
        if (!image && !wayland) {
            image = read_clipboard_image_via_xclip(options);
        }
    }

    if (!image) {
        return missing;
    }
    return convert_unsupported_clipboard_image(std::move(*image));
}

ClipboardTool::ClipboardTool() {
    descriptor_.name = "clipboard";
    descriptor_.description =
        "Copy text or image bytes to the platform clipboard, or return the command plan.";
    descriptor_.deterministic = false;
    descriptor_.destructive = false;
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {
             {"text",
              {
                  {"type", "string"},
                  {"description", "Text that would be piped to the clipboard command."},
              }},
             {"image_path",
              {
                  {"type", "string"},
                  {"description",
                   "Image file that would be piped to the clipboard command."},
              }},
             {"execute",
              {
                  {"type", "boolean"},
                  {"description",
                   "When true, execute the selected clipboard command. Defaults to false for planning."},
              }},
         }},
        {"additionalProperties", false},
    };
}

const ToolDescriptor& ClipboardTool::descriptor() const { return descriptor_; }

ToolResult ClipboardTool::invoke(const ToolRequest& request,
                                 const ToolContext& context) const {
    if (!is_valid(request)) {
        return make_invalid_request("invalid tool request", request);
    }
    if (request.tool_name != descriptor_.name) {
        return make_invalid_request("tool request name does not match clipboard",
                                    request);
    }
    if (!has_only_arguments(request, {"text", "image_path", "execute"})) {
        return make_invalid_request(
            "clipboard accepts only text, image_path, and execute arguments", request);
    }
    bool execute = false;
    if (request.arguments.contains("execute")) {
        if (!request.arguments.at("execute").is_boolean()) {
            return make_invalid_request("clipboard requires a boolean execute argument",
                                        request);
        }
        execute = request.arguments.at("execute").get<bool>();
    }

    if (request.arguments.contains("text") &&
        !request.arguments.at("text").is_string()) {
        return make_invalid_request(
            "clipboard requires a string text argument", request);
    }
    if (request.arguments.contains("image_path") &&
        !request.arguments.at("image_path").is_string()) {
        return make_invalid_request(
            "clipboard requires a string image_path argument", request);
    }

    const bool has_text_argument = request.arguments.contains("text");
    const bool has_image_path_argument = request.arguments.contains("image_path");
    const bool has_non_empty_text =
        has_text_argument && !request.arguments.at("text").get<std::string>().empty();
    const bool has_non_empty_image_path =
        has_image_path_argument &&
        !request.arguments.at("image_path").get<std::string>().empty();
    const bool use_text =
        has_text_argument &&
        (!has_image_path_argument || has_non_empty_text || !has_non_empty_image_path);
    const bool use_image_path =
        has_image_path_argument &&
        (!has_text_argument || has_non_empty_image_path || !has_non_empty_text);
    if (use_text == use_image_path) {
        return make_invalid_request(
            "clipboard requires exactly one of text or image_path", request);
    }

    ToolResult error_result;
    const auto payload = use_text
                             ? make_text_payload(request, error_result)
                             : make_image_payload(request, context, error_result);
    if (!payload) {
        return error_result;
    }

    const auto available_commands = available_commands_for_context(context);
    const auto plans = plan_clipboard_copy_commands(available_commands);
    const auto selected = select_clipboard_copy_command(available_commands);

    ToolResult result;
    result.call_id = request.call_id;
    result.status = selected ? ToolResultStatus::success : ToolResultStatus::not_found;
    if (!selected) {
        result.error_message = "no supported clipboard command is available";
    }
    result.output = Json{
        {"operation", "copy"},
        {"valid", static_cast<bool>(selected)},
        {"payload", *payload},
        {"stdin_bytes", payload->stdin_bytes},
        {"available_commands", available_commands},
        {"candidates", plans},
    };
    if (selected) {
        result.output["plan"] = Json(*selected);
        result.output["command"] = join_command(*selected);
        if (execute) {
            std::string stdin_text;
            if (use_text) {
                stdin_text = request.arguments.at("text").get<std::string>();
            } else {
                const auto bytes =
                    qt::foundation::read_text_file(payload->source);
                if (!bytes) {
                    return make_execution_error(ToolResultStatus::execution_error,
                                                bytes.error().summary(), request);
                }
                stdin_text = bytes.value();
            }
            qt::foundation::ProcessRequest process;
            process.mode = qt::foundation::ProcessLaunchMode::argv;
            process.argv.push_back(selected->executable);
            process.argv.insert(process.argv.end(), selected->arguments.begin(),
                                selected->arguments.end());
            process.stdin_text = std::move(stdin_text);
            process.timeout_ms = 3000;
            const auto copied = qt::foundation::run_process(process);
            if (!copied) {
                return make_execution_error(ToolResultStatus::execution_error,
                                            copied.error().summary(), request);
            }
            if (copied.value().timed_out || copied.value().exit_code != 0) {
                return make_execution_error(
                    ToolResultStatus::execution_error,
                    copied.value().timed_out
                        ? "clipboard copy timed out"
                        : copied.value().stderr_text.empty()
                              ? "clipboard copy failed"
                              : copied.value().stderr_text,
                    request);
            }
            result.output["copied"] = true;
        }
    }
    return result;
}

void to_json(Json& json, ClipboardCommandPlan const& plan) {
    Json command = Json::array();
    command.push_back(plan.executable);
    for (const auto& argument : plan.arguments) {
        command.push_back(argument);
    }

    json = Json{
        {"name", plan.name},
        {"executable", plan.executable},
        {"arguments", plan.arguments},
        {"command", std::move(command)},
        {"available", plan.available},
        {"reason", plan.reason},
    };
}

void to_json(Json& json, ClipboardPayloadPlan const& plan) {
    json = Json{
        {"kind", plan.kind == ClipboardPayloadKind::text ? "text" : "image"},
        {"source", plan.source},
        {"stdin_bytes", plan.stdin_bytes},
    };
    if (plan.mime_type) {
        json["mime_type"] = *plan.mime_type;
    }
}

}  // namespace qt::tools
