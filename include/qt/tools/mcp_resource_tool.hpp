#pragma once

#include "qt/tools/tool.hpp"

namespace qt::tools {

class ListMcpResourcesTool final : public Tool {
  public:
    ListMcpResourcesTool();

    const ToolDescriptor& descriptor() const override;
    ToolResult invoke(const ToolRequest& request,
                      const ToolContext& context) const override;

  private:
    ToolDescriptor descriptor_;
};

class ReadMcpResourceTool final : public Tool {
  public:
    ReadMcpResourceTool();

    const ToolDescriptor& descriptor() const override;
    ToolResult invoke(const ToolRequest& request,
                      const ToolContext& context) const override;

  private:
    ToolDescriptor descriptor_;
};

}  // namespace qt::tools
