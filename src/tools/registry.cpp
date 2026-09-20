#include "qt/tools/registry.hpp"

#include <algorithm>

namespace qt::tools {
namespace {

const char* implementation_name_for(const std::string& name) {
    if (name == "read") {
        return "read_text";
    }
    if (name == "write") {
        return "write_text";
    }
    if (name == "grep") {
        return "search_text";
    }
    if (name == "ls") {
        return "list_directory";
    }
    return nullptr;
}

ToolDescriptor model_facing_descriptor(ToolDescriptor descriptor) {
    const auto erase_properties = [&](std::initializer_list<const char*> names) {
        auto properties = descriptor.input_schema.find("properties");
        if (properties == descriptor.input_schema.end() ||
            !properties->is_object()) {
            return;
        }
        for (const char* name : names) properties->erase(name);
    };
    if (descriptor.name == "read_text") {
        descriptor.name = "read";
        descriptor.description =
            "Read the contents of a file. Supports offset and limit for large files.";
        erase_properties({"file_path", "filePath"});
        descriptor.input_schema["required"] = Json::array({"path"});
    } else if (descriptor.name == "write_text") {
        descriptor.name = "write";
        descriptor.description =
            "Write content to a file, creating parent directories when needed.";
        erase_properties({"file_path", "filePath", "text"});
        descriptor.input_schema["required"] = Json::array({"path", "content"});
    } else if (descriptor.name == "edit") {
        descriptor.description =
            "Apply one or more exact text replacements to a file. Each edit "
            "requires oldText and newText.";
        erase_properties({"file_path", "filePath", "oldText", "newText",
                          "old_string", "new_string", "oldString",
                          "newString", "replace_all"});
        auto properties = descriptor.input_schema.find("properties");
        if (properties != descriptor.input_schema.end() &&
            properties->is_object() && properties->contains("edits")) {
            auto& edits = properties->at("edits");
            edits["minItems"] = 1;
            if (edits.contains("items") && edits.at("items").is_object()) {
                auto& item_schema = edits["items"];
                if (item_schema.contains("properties") &&
                    item_schema.at("properties").is_object()) {
                    auto& item_properties = item_schema["properties"];
                    for (const char* alias :
                         {"old_string", "new_string", "oldString",
                          "newString"}) {
                        item_properties.erase(alias);
                    }
                }
                item_schema["required"] =
                    Json::array({"oldText", "newText"});
            }
        }
        descriptor.input_schema["required"] = Json::array({"path", "edits"});
    } else if (descriptor.name == "search_text") {
        descriptor.name = "grep";
        descriptor.description =
            "Search for literal text or a regular expression. For C/C++ "
            "definitions and identifier lookup, prefer the symbol tool.";
        erase_properties({"query"});
        descriptor.input_schema["required"] = Json::array({"pattern"});
    } else if (descriptor.name == "list_directory") {
        descriptor.name = "ls";
        descriptor.description =
            "List directory contents, including dotfiles, sorted by display name.";
    }
    return descriptor;
}

}  // namespace

bool ToolRegistry::register_tool(std::unique_ptr<Tool> tool) {
    if (!tool) {
        return false;
    }

    const auto& descriptor = tool->descriptor();
    if (!is_valid(descriptor)) {
        return false;
    }

    return tools_.emplace(descriptor.name, std::move(tool)).second;
}

const Tool* ToolRegistry::find(const std::string& name) const {
    const auto iterator = tools_.find(name);
    if (iterator != tools_.end()) {
        return iterator->second.get();
    }

    const auto alias = implementation_name_for(name);
    if (alias == nullptr) {
        return nullptr;
    }

    const auto alias_iterator = tools_.find(alias);
    if (alias_iterator == tools_.end()) {
        return nullptr;
    }
    return alias_iterator->second.get();
}

ToolRequest ToolRegistry::prepare_request(ToolRequest request) const {
    if (const auto* implementation_name =
            implementation_name_for(request.tool_name)) {
        request.tool_name = implementation_name;
    }
    if (request.tool_name == "search_text" &&
        !request.arguments.contains("path")) {
        request.arguments["path"] = ".";
    }
    const auto canonicalize_alias = [&](const char* canonical,
                                        std::initializer_list<const char*> aliases) {
        if (!request.arguments.contains(canonical)) {
            for (const char* alias : aliases) {
                if (request.arguments.contains(alias)) {
                    request.arguments[canonical] = request.arguments.at(alias);
                    break;
                }
            }
        }
        for (const char* alias : aliases) request.arguments.erase(alias);
    };
    if (request.tool_name == "read_text") {
        canonicalize_alias("path", {"file_path", "filePath"});
    } else if (request.tool_name == "write_text") {
        canonicalize_alias("path", {"file_path", "filePath"});
        canonicalize_alias("content", {"text"});
    } else if (request.tool_name == "edit") {
        canonicalize_alias("path", {"file_path", "filePath"});
        if (!request.arguments.contains("edits")) {
            Json edit = Json::object();
            for (const char* name : {"oldText", "old_string", "oldString"}) {
                if (request.arguments.contains(name)) {
                    edit["oldText"] = request.arguments.at(name);
                    break;
                }
            }
            for (const char* name : {"newText", "new_string", "newString"}) {
                if (request.arguments.contains(name)) {
                    edit["newText"] = request.arguments.at(name);
                    break;
                }
            }
            if (request.arguments.contains("replace_all")) {
                edit["replace_all"] = request.arguments.at("replace_all");
            }
            if (!edit.empty()) request.arguments["edits"] = Json::array({edit});
        }
        if (request.arguments.contains("edits") &&
            request.arguments.at("edits").is_array()) {
            for (auto& edit : request.arguments["edits"]) {
                if (!edit.is_object()) continue;
                if (!edit.contains("oldText")) {
                    for (const char* alias : {"old_string", "oldString"}) {
                        if (edit.contains(alias)) {
                            edit["oldText"] = edit.at(alias);
                            break;
                        }
                    }
                }
                if (!edit.contains("newText")) {
                    for (const char* alias : {"new_string", "newString"}) {
                        if (edit.contains(alias)) {
                            edit["newText"] = edit.at(alias);
                            break;
                        }
                    }
                }
                for (const char* alias :
                     {"old_string", "new_string", "oldString", "newString"}) {
                    edit.erase(alias);
                }
            }
        }
        for (const char* legacy :
             {"oldText", "newText", "old_string", "new_string", "oldString",
              "newString", "replace_all"}) {
            request.arguments.erase(legacy);
        }
    } else if (request.tool_name == "search_text") {
        canonicalize_alias("pattern", {"query"});
    }
    if (request.tool_name == "list_directory") {
        const auto path = request.arguments.find("path");
        if (path != request.arguments.end() && path->is_string()) {
            const std::string value = path->get<std::string>();
            if (value == "list_files" || value == "list_directory" ||
                value == "files") {
                request.arguments["path"] = ".";
            }
        }
    }
    if (request.tool_name == "index" && request.arguments.is_object() &&
        request.arguments.contains("arguments") &&
        request.arguments.at("arguments").is_object()) {
        auto& forwarded = request.arguments["arguments"];
        for (const char* field : {"category", "operation", "include_schemas"}) {
            if (!request.arguments.contains(field) && forwarded.contains(field)) {
                request.arguments[field] = forwarded.at(field);
                forwarded.erase(field);
            }
        }
        if (request.arguments.contains("operation") &&
            request.arguments.at("operation").is_string()) {
            const std::string operation =
                request.arguments.at("operation").get<std::string>();
            if (operation == "symbol_lookup" || operation == "find_symbol" ||
                operation == "lookup_symbol") {
                request.arguments["operation"] = "code.symbol";
                request.arguments.erase("category");
            }
            if (request.arguments.at("operation") == "code.symbol" &&
                forwarded.contains("pattern") &&
                !forwarded.contains("name")) {
                forwarded["name"] = forwarded.at("pattern");
                forwarded.erase("pattern");
            }
        }
    }
    return request;
}

std::vector<ToolDescriptor> ToolRegistry::descriptors() const {
    std::vector<ToolDescriptor> result;
    result.reserve(tools_.size());

    for (const auto& entry : tools_) {
        result.push_back(model_facing_descriptor(entry.second->descriptor()));
    }
    std::sort(result.begin(), result.end(),
              [](const ToolDescriptor& lhs, const ToolDescriptor& rhs) {
                  return lhs.name < rhs.name;
              });

    return result;
}

std::size_t ToolRegistry::size() const noexcept { return tools_.size(); }

}  // namespace qt::tools
