#pragma once

#include "qt/tools/tool.hpp"

namespace qt::tools {

class BashTool final : public Tool {
  public:
    BashTool();

    const ToolDescriptor& descriptor() const override;
    ToolResult invoke(const ToolRequest& request,
                      const ToolContext& context) const override;

  private:
    ToolDescriptor descriptor_;
};

}  // namespace qt::tools
