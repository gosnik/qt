#pragma once

#include "qt/tools/tool.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace qt::tools {

class WebInvocationBudget {
  public:
    explicit WebInvocationBudget(std::size_t maximum);
    bool try_acquire() noexcept;

  private:
    std::size_t maximum_;
    std::atomic<std::size_t> used_{0U};
};

class WebFetchTool final : public Tool {
  public:
    explicit WebFetchTool(
        std::shared_ptr<WebInvocationBudget> invocation_budget = {},
        bool restricted_research = false);

    const ToolDescriptor& descriptor() const override;
    ToolResult invoke(const ToolRequest& request,
                      const ToolContext& context) const override;

  private:
    std::shared_ptr<WebInvocationBudget> invocation_budget_;
    bool restricted_research_ = false;
    ToolDescriptor descriptor_;
};

class WebSearchTool final : public Tool {
  public:
    explicit WebSearchTool(
        std::shared_ptr<WebInvocationBudget> invocation_budget = {},
        bool restricted_research = false);

    const ToolDescriptor& descriptor() const override;
    ToolResult invoke(const ToolRequest& request,
                      const ToolContext& context) const override;

  private:
    std::shared_ptr<WebInvocationBudget> invocation_budget_;
    bool restricted_research_ = false;
    ToolDescriptor descriptor_;
};

using WebFetchFunction =
    std::function<Json(const std::string&, std::size_t, bool, int)>;

struct WebRunOptions {
    WebFetchFunction fetch;
    std::size_t max_queries = 4U;
    std::size_t max_operations = 8U;
    std::size_t max_search_results = 10U;
    std::size_t max_response_bytes = 2U * 1024U * 1024U;
    std::size_t max_query_characters = 1024U;
    std::size_t max_invocations = 0U;
    bool allow_direct_urls = true;
    bool include_evidence_content = false;
    std::shared_ptr<WebInvocationBudget> invocation_budget;
};

class WebRunState;

class WebRunTool final : public Tool {
  public:
    explicit WebRunTool(WebRunOptions options = {});

    const ToolDescriptor& descriptor() const override;
    ToolResult invoke(const ToolRequest& request,
                      const ToolContext& context) const override;

  private:
    WebRunOptions options_;
    std::shared_ptr<WebRunState> state_;
    ToolDescriptor descriptor_;
};

}  // namespace qt::tools
