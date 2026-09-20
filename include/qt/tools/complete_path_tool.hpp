#pragma once

#include "qt/tools/tool.hpp"

namespace qt::tools {

class CompletePathTool final : public Tool {
  public:
    CompletePathTool();

    const ToolDescriptor& descriptor() const override;
    ToolResult invoke(const ToolRequest& request,
                      const ToolContext& context) const override;

  private:
    ToolDescriptor descriptor_;
};

}  // namespace qt::tools
