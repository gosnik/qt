#pragma once

#include "qt/tools/tool.hpp"

namespace qt::tools {

class ReadTextTool final : public Tool {
  public:
    ReadTextTool();

    const ToolDescriptor& descriptor() const override;
    ToolResult invoke(const ToolRequest& request,
                      const ToolContext& context) const override;

  private:
    ToolDescriptor descriptor_;
};

}  // namespace qt::tools
