#pragma once

#include "qt/tools/tool.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace qt::tools {

struct ClipboardCommandPlan {
    std::string name;
    std::string executable;
    std::vector<std::string> arguments;
    bool available = false;
    std::string reason;
};

enum class ClipboardPayloadKind {
    text,
    image,
};

struct ClipboardPayloadPlan {
    ClipboardPayloadKind kind = ClipboardPayloadKind::text;
    std::string source;
    std::optional<std::string> mime_type;
    std::size_t stdin_bytes = 0U;
};

struct ClipboardImageWriteResult {
    bool ok = false;
    std::filesystem::path path;
    std::string mime_type;
    std::size_t bytes = 0U;
    std::string error;
};

struct ClipboardImageReadOptions {
    std::optional<std::string> platform;
    std::unordered_map<std::string, std::string> environment;
};

struct ClipboardImageReadResult {
    bool ok = false;
    std::string bytes;
    std::string mime_type;
    std::string source;
    bool converted_to_png = false;
    std::string error;
};

std::vector<std::string> detect_available_clipboard_commands();
std::vector<ClipboardCommandPlan> plan_clipboard_copy_commands(
    const std::vector<std::string>& available_commands);
std::optional<ClipboardCommandPlan> select_clipboard_copy_command(
    const std::vector<std::string>& available_commands);
bool is_valid_clipboard_text(const std::string& text) noexcept;
std::optional<std::string> extension_for_clipboard_image_mime_type(
    const std::string& mime_type);
bool is_supported_clipboard_image_mime_type(const std::string& mime_type) noexcept;
std::optional<std::string> preferred_clipboard_image_mime_type(
    const std::vector<std::string>& mime_types);
bool clipboard_image_requires_png_conversion(const std::string& mime_type) noexcept;
std::optional<std::string> infer_clipboard_image_mime_type(
    const std::string& path);
bool is_supported_clipboard_image_path(const std::string& path) noexcept;
ClipboardImageWriteResult write_clipboard_image_to_temp_path(
    const std::string& bytes, const std::string& mime_type,
    const std::filesystem::path& temp_dir, const std::string& unique_id);
ClipboardImageReadResult read_clipboard_image(
    const ClipboardImageReadOptions& options = {});

class ClipboardTool final : public Tool {
  public:
    ClipboardTool();

    const ToolDescriptor& descriptor() const override;
    ToolResult invoke(const ToolRequest& request,
                      const ToolContext& context) const override;

  private:
    ToolDescriptor descriptor_;
};

void to_json(Json& json, ClipboardCommandPlan const& plan);
void to_json(Json& json, ClipboardPayloadPlan const& plan);

}  // namespace qt::tools
