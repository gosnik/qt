#pragma once

#include "qt/tools/tool.hpp"

#include <map>
#include <memory>
#include <vector>

namespace qt::tools {

class ToolRegistry {
  public:
    bool register_tool(std::unique_ptr<Tool> tool);
    [[nodiscard]] const Tool* find(const std::string& name) const;
    [[nodiscard]] ToolRequest prepare_request(ToolRequest request) const;
    std::vector<ToolDescriptor> descriptors() const;
    std::size_t size() const noexcept;

  private:
    std::map<std::string, std::unique_ptr<Tool>> tools_;
};

}  // namespace qt::tools
