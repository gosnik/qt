#include "qt/foundation/process.hpp"
#include "qt/tools/web_tool.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using qt::tools::Json;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

namespace qt::foundation {

void ProcessCancellationToken::cancel() noexcept {
    cancelled_.store(true, std::memory_order_release);
}

bool ProcessCancellationToken::is_cancelled() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
}

}  // namespace qt::foundation

int main() {
    try {
        std::vector<std::string> urls;
        qt::tools::WebRunOptions options;
        options.fetch = [&urls](const std::string& url, std::size_t max_bytes,
                                bool raw, int timeout_seconds) {
            urls.push_back(url);
            require(max_bytes <= 2U * 1024U * 1024U,
                    "web_run must bound provider response bytes");
            require(raw, "web_run navigation must preserve links before extraction");
            require(timeout_seconds == 20, "web_run must use the bounded default timeout");
            if (url.find("duckduckgo.com") != std::string::npos) {
                return Json{{"status", 200},
                            {"content",
                             "<a class=\"result__a\" "
                             "href=\"https://example.com/a\">Alpha</a>"}};
            }
            if (url == "https://example.com/a") {
                return Json{{"status", 200},
                            {"content",
                             "<html><body><h1>Alpha heading</h1><p>Needle text "
                             "for retrieval.</p><a href=\"/b\">Beta link</a></body></html>"}};
            }
            if (url == "https://example.com/b") {
                return Json{{"status", 200},
                            {"content", "<article>Beta destination</article>"}};
            }
            return Json{{"status", 404}, {"content", "missing"}};
        };

        qt::tools::WebRunTool tool(options);
        const auto& descriptor = tool.descriptor();
        require(descriptor.name == "web_run", "web_run descriptor name mismatch");
        require(!descriptor.deterministic && !descriptor.local_only,
                "network tools must be marked non-deterministic and non-local");
        const auto& properties = descriptor.input_schema.at("properties");
        require(properties.contains("search_query") && properties.contains("open") &&
                    properties.contains("click") && properties.contains("find") &&
                    properties.contains("response_length"),
                "optimized command schema is incomplete");
        require(!properties.contains("finance") && !properties.contains("screenshot"),
                "unsupported Codex verticals must not be advertised");

        qt::tools::ToolContext context;
        qt::tools::ToolRequest search;
        search.tool_name = "web_run";
        search.arguments =
            Json{{"search_query",
                  Json::array({Json{{"q", "alpha"},
                                    {"recency", 7},
                                    {"domains", Json::array({"example.com"})}},
                               Json{{"q", "alpha second angle"}}})},
                 {"response_length", "medium"}};
        const auto search_result = tool.invoke(search, context);
        require(search_result.status == qt::tools::ToolResultStatus::success,
                "batched search failed");
        require(search_result.output.value("external_context", false),
                "web output must be marked external context");
        require(search_result.output.at("references").size() == 2U,
                "each batched query must preserve its source reference");
        require(urls.front().find("site%3Aexample.com") != std::string::npos &&
                    urls.front().find("df=w") != std::string::npos,
                "domain and recency filters were not forwarded to search");
        const auto ref_id =
            search_result.output.at("references").front().at("ref_id").get<std::string>();
        require(ref_id == "turn0search0", "stable Codex-style search ref id mismatch");

        qt::tools::ToolRequest open;
        open.tool_name = "web_run";
        open.arguments = Json{{"open", Json::array({Json{{"ref_id", ref_id}}})},
                              {"response_length", "short"}};
        const auto open_result = tool.invoke(open, context);
        const auto open_text = open_result.output.at("text").get<std::string>();
        require(open_result.status == qt::tools::ToolResultStatus::success &&
                    open_text.find("Alpha heading") != std::string::npos &&
                    open_text.find("[0] [Beta link]") != std::string::npos,
                "deferred open did not return readable line-numbered content and links");
        require(!open_result.output.at("references").front().contains("content"),
                "ordinary web responses unexpectedly duplicated source evidence");
        auto evidence_options = options;
        evidence_options.include_evidence_content = true;
        qt::tools::WebRunTool evidence_tool(evidence_options);
        const auto evidence_search = evidence_tool.invoke(search, context);
        const auto& search_reference = evidence_search.output.at("references").front();
        require(!search_reference.contains("content"),
                "search snippets must not masquerade as fetched evidence");
        auto evidence_open = open;
        evidence_open.arguments["open"][0]["ref_id"] = search_reference.at("ref_id");
        const auto evidence_result = evidence_tool.invoke(evidence_open, context);
        const auto& evidence_reference = evidence_result.output.at("references").front();
        require(evidence_reference.at("url") == search_reference.at("url") &&
                    evidence_reference.at("content").get<std::string>().find("Alpha heading") != std::string::npos &&
                    evidence_reference.at("content").get<std::string>().size() <= 4096U,
                "opened verifier evidence must be bounded and bound to its actual URL");

        qt::tools::ToolRequest click;
        click.tool_name = "web_run";
        click.arguments =
            Json{{"click", Json::array({Json{{"ref_id", ref_id}, {"id", 0}}})}};
        const auto click_result = tool.invoke(click, context);
        require(click_result.status == qt::tools::ToolResultStatus::success &&
                    click_result.output.at("text").get<std::string>().find(
                        "Beta destination") != std::string::npos,
                "numbered-link navigation failed");

        qt::tools::ToolRequest find;
        find.tool_name = "web_run";
        find.arguments = Json{{"find",
                               Json::array({Json{{"ref_id", ref_id},
                                                 {"pattern", "needle"}}})}};
        const auto find_result = tool.invoke(find, context);
        require(find_result.status == qt::tools::ToolResultStatus::success &&
                    find_result.output.at("text").get<std::string>().find(
                        "Needle text") != std::string::npos,
                "case-insensitive cached-page find failed");

        qt::tools::ToolRequest private_open;
        private_open.tool_name = "web_run";
        private_open.arguments = Json{{"open",
                                       Json::array({Json{{"ref_id",
                                                         "http://127.0.0.1/secret"}}})}};
        const auto before_private = urls.size();
        const auto private_result = tool.invoke(private_open, context);
        require(private_result.status == qt::tools::ToolResultStatus::execution_error &&
                    urls.size() == before_private,
                "private addresses must be rejected before network access");

        qt::tools::ToolRequest oversized;
        oversized.tool_name = "web_run";
        oversized.arguments =
            Json{{"search_query",
                  Json::array({Json{{"q", "1"}}, Json{{"q", "2"}},
                               Json{{"q", "3"}}, Json{{"q", "4"}},
                               Json{{"q", "5"}}})}};
        require(tool.invoke(oversized, context).status ==
                    qt::tools::ToolResultStatus::invalid_request,
                "query batch maximum must be enforced");

        qt::tools::WebRunOptions restricted_options = options;
        restricted_options.allow_direct_urls = false;
        restricted_options.max_query_characters = 8U;
        restricted_options.max_invocations = 2U;
        qt::tools::WebRunTool restricted(restricted_options);
        require(restricted.invoke(private_open, context).status ==
                    qt::tools::ToolResultStatus::execution_error,
                "research web access must reject direct URLs");
        qt::tools::ToolRequest long_query;
        long_query.tool_name = "web_run";
        long_query.arguments = Json{{"search_query", Json::array(
            {Json{{"q", "query text is too long"}}})}};
        require(restricted.invoke(long_query, context).status ==
                    qt::tools::ToolResultStatus::execution_error,
                "research web access must bound query characters");
        require(restricted.invoke(search, context).status ==
                    qt::tools::ToolResultStatus::permission_denied,
                "research web access must enforce its invocation budget");

        auto shared_budget =
            std::make_shared<qt::tools::WebInvocationBudget>(2U);
        qt::tools::WebRunOptions shared_options = options;
        shared_options.invocation_budget = shared_budget;
        qt::tools::WebRunTool shared_run(shared_options);
        qt::tools::WebSearchTool shared_search(shared_budget, true);
        qt::tools::WebFetchTool shared_fetch(shared_budget, true);
        qt::tools::ToolRequest custom_search;
        custom_search.tool_name = "web_search";
        custom_search.arguments =
            Json{{"query", "bounded research"},
                 {"engine_url", "https://example.com/?q={query}"}};
        require(shared_search.invoke(custom_search, context).status ==
                    qt::tools::ToolResultStatus::invalid_request,
                "research web_search must reject custom search engines");
        require(shared_run.invoke(search, context).status ==
                    qt::tools::ToolResultStatus::success,
                "web_run must share the research budget with standalone web tools");
        qt::tools::ToolRequest fetch;
        fetch.tool_name = "web_fetch";
        fetch.arguments = Json{{"url", "https://example.com/a"}};
        require(shared_fetch.invoke(fetch, context).status ==
                    qt::tools::ToolResultStatus::permission_denied,
                "web_fetch must honor the shared research tool-call budget");

        context.cancellation_token =
            std::make_shared<qt::foundation::ProcessCancellationToken>();
        context.cancellation_token->cancel();
        require(tool.invoke(search, context).status ==
                    qt::tools::ToolResultStatus::execution_error,
                "pre-cancelled web access must stop before fetching");

        ::setenv("PI_OFFLINE", "1", 1);
        context.cancellation_token.reset();
        require(tool.invoke(search, context).status ==
                    qt::tools::ToolResultStatus::permission_denied,
                "offline mode must deny direct web access");
        ::unsetenv("PI_OFFLINE");

        std::cout << "web_run_probe_ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "web_run_probe_failed: " << error.what() << '\n';
        return 1;
    }
}
