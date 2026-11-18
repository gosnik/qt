//! Restricted web tools for the Dreaming research child (archived
//! `QT_MEMORY_DREAM_RESEARCH` mode of `web_tool.cpp` and
//! `runtime_services.cpp`).
//!
//! The three tools share one [`ResearchBudget`] of three calls. Fetches are
//! capped at 512 KiB with a 20 s timeout; searches accept at most six results
//! and 256-character queries and cannot override the search engine; `web_run`
//! cannot open direct URLs. `PI_OFFLINE` / `--offline` still disable every
//! tool, before any budget is spent.

use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Duration;

use futures::future::BoxFuture;
use qt_core::tools::{
    FetchOptions, Tool, ToolContext, ToolOutput, ToolRegistry, WebFetchTool, WebRunLimits,
    WebRunTool, fetch_url, offline_mode_enabled, parse_duckduckgo_results,
};
use serde_json::{Value, json};

/// Tool calls the research child may make in total.
pub const RESEARCH_MAX_TOOL_CALLS: usize = 3;
/// Download cap for every research fetch.
pub const RESEARCH_MAX_RESPONSE_BYTES: usize = 512 * 1024;
/// Fixed fetch timeout.
pub const RESEARCH_TIMEOUT_SECONDS: u64 = 20;
/// Search results per query.
pub const RESEARCH_MAX_SEARCH_RESULTS: usize = 6;
/// Query length cap (bytes).
pub const RESEARCH_MAX_QUERY_CHARACTERS: usize = 256;
/// Returned `web_fetch` content cap.
const RESEARCH_MAX_FETCH_LENGTH: usize = 128 * 1024;
const DEFAULT_SEARCH_LIMIT: usize = 5;

const OFFLINE_MESSAGE: &str = "web access is disabled in offline mode";
const BUDGET_MESSAGE: &str = "Dreaming web research tool-call budget exhausted";

/// Whether this process is a research child (`QT_MEMORY_DREAM_RESEARCH`).
pub fn dream_research_requested() -> bool {
    std::env::var("QT_MEMORY_DREAM_RESEARCH").is_ok_and(|value| {
        let value = value.trim().to_ascii_lowercase();
        !value.is_empty() && !matches!(value.as_str(), "0" | "false" | "no" | "off")
    })
}

/// A shared call budget (archived `WebInvocationBudget`); `0` is unlimited.
#[derive(Debug)]
pub struct ResearchBudget {
    maximum: usize,
    used: AtomicUsize,
}

impl ResearchBudget {
    pub fn new(maximum: usize) -> Self {
        Self {
            maximum,
            used: AtomicUsize::new(0),
        }
    }

    /// Claims one call; false once the budget is spent.
    pub fn try_acquire(&self) -> bool {
        if self.maximum == 0 {
            return true;
        }
        self.used
            .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |used| {
                (used < self.maximum).then_some(used + 1)
            })
            .is_ok()
    }

    pub fn used(&self) -> usize {
        self.used.load(Ordering::SeqCst)
    }
}

/// Offline and budget gates shared by the three tools.
fn gate(ctx: &ToolContext, budget: &ResearchBudget) -> Option<ToolOutput> {
    if offline_mode_enabled(&ctx.web) {
        return Some(ToolOutput::error(OFFLINE_MESSAGE));
    }
    if !budget.try_acquire() {
        return Some(ToolOutput::error(BUDGET_MESSAGE));
    }
    None
}

fn only_arguments(args: &Value, allowed: &[&str]) -> bool {
    match args {
        Value::Object(map) => map.keys().all(|key| allowed.contains(&key.as_str())),
        Value::Null => true,
        _ => false,
    }
}

/// `web_run` limits of the research child.
pub fn research_web_run_limits() -> WebRunLimits {
    WebRunLimits {
        max_queries: 3,
        max_operations: 6,
        max_search_results: RESEARCH_MAX_SEARCH_RESULTS,
        max_response_bytes: RESEARCH_MAX_RESPONSE_BYTES,
        max_query_characters: RESEARCH_MAX_QUERY_CHARACTERS,
        max_invocations: RESEARCH_MAX_TOOL_CALLS,
        allow_direct_urls: false,
        include_evidence_content: true,
    }
}

/// Restricted `web_fetch`. Successful results also carry the returned text
/// as `details.content`, which the parent keeps as citation evidence.
pub struct RestrictedWebFetch {
    inner: WebFetchTool,
    budget: Arc<ResearchBudget>,
}

impl RestrictedWebFetch {
    pub fn new(budget: Arc<ResearchBudget>) -> Self {
        Self {
            inner: WebFetchTool,
            budget,
        }
    }
}

/// The page text without the pagination notices `web_fetch` appends.
fn fetched_text(output: &ToolOutput) -> String {
    let text = output.text_content();
    let mut end = text.len();
    for marker in ["\n\n[Content truncated:", "\n\n[Response exceeded"] {
        if let Some(position) = text.find(marker) {
            end = end.min(position);
        }
    }
    text[..end].to_string()
}

impl Tool for RestrictedWebFetch {
    fn name(&self) -> &'static str {
        "web_fetch"
    }
    fn description(&self) -> String {
        self.inner.description()
    }
    fn snippet(&self) -> &'static str {
        self.inner.snippet()
    }
    fn parameters(&self) -> Value {
        self.inner.parameters()
    }
    fn parallel_safe(&self) -> bool {
        true
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        if let Some(refused) = gate(&ctx, &self.budget) {
            return Box::pin(async move { refused });
        }
        if !only_arguments(
            &args,
            &["url", "max_length", "start_index", "raw", "timeout_seconds"],
        ) {
            return Box::pin(async {
                ToolOutput::error(
                    "web_fetch accepts only url, max_length, start_index, raw, and timeout_seconds arguments",
                )
            });
        }
        let mut args = match args {
            Value::Object(map) => map,
            _ => Default::default(),
        };
        let requested = args
            .get("max_length")
            .and_then(Value::as_f64)
            .filter(|value| *value > 0.0)
            .map(|value| value as usize);
        let max_length = requested.map_or(RESEARCH_MAX_FETCH_LENGTH, |value| {
            value.min(RESEARCH_MAX_FETCH_LENGTH)
        });
        args.insert("max_length".into(), json!(max_length));
        args.insert(
            "max_response_bytes".into(),
            json!(RESEARCH_MAX_RESPONSE_BYTES),
        );
        args.insert("timeout_seconds".into(), json!(RESEARCH_TIMEOUT_SECONDS));
        let future = self.inner.execute(Value::Object(args), ctx);
        Box::pin(async move {
            let mut output = future.await;
            if !output.is_error
                && let Some(Value::Object(details)) = output.details.as_mut()
            {
                let content = fetched_text(&ToolOutput {
                    content: output.content.clone(),
                    is_error: false,
                    details: None,
                });
                details.insert("content".into(), json!(content));
            }
            output
        })
    }
}

/// Restricted `web_search` on the configured engine.
pub struct RestrictedWebSearch {
    budget: Arc<ResearchBudget>,
}

impl RestrictedWebSearch {
    pub fn new(budget: Arc<ResearchBudget>) -> Self {
        Self { budget }
    }
}

fn percent_encode(text: &str) -> String {
    let mut out = String::with_capacity(text.len() * 3);
    for byte in text.bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.' | b'~') {
            out.push(byte as char);
        } else {
            out.push_str(&format!("%{byte:02X}"));
        }
    }
    out
}

fn search_limit(args: &Value) -> Result<usize, &'static str> {
    match args.get("limit") {
        None | Some(Value::Null) => Ok(DEFAULT_SEARCH_LIMIT),
        Some(value) => {
            let Some(number) = value.as_i64() else {
                return Err("web_search requires numeric limit");
            };
            if number <= 0 {
                return Err("web_search requires positive limit");
            }
            if number as usize > RESEARCH_MAX_SEARCH_RESULTS {
                return Err("Dreaming web research is limited to six search results");
            }
            Ok(number as usize)
        }
    }
}

impl Tool for RestrictedWebSearch {
    fn name(&self) -> &'static str {
        "web_search"
    }
    fn description(&self) -> String {
        "Search the web using a public HTML search endpoint and return result links.".into()
    }
    fn snippet(&self) -> &'static str {
        "Search the web (DuckDuckGo) for result links"
    }
    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "query": { "type": "string", "description": "Search query (at most 256 characters)." },
                "limit": { "type": "number", "description": "Maximum results (default 5, at most 6)." },
                "timeout_seconds": { "type": "number", "description": "Ignored; research fetches use a fixed timeout." }
            },
            "required": ["query"],
            "additionalProperties": false
        })
    }
    fn parallel_safe(&self) -> bool {
        true
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        let refused = gate(&ctx, &self.budget);
        Box::pin(async move {
            if let Some(refused) = refused {
                return refused;
            }
            if !only_arguments(&args, &["query", "limit", "engine_url", "timeout_seconds"]) {
                return ToolOutput::error(
                    "web_search accepts only query, limit, engine_url, and timeout_seconds arguments",
                );
            }
            let Some(query) = args.get("query").and_then(Value::as_str) else {
                return ToolOutput::error("web_search requires string query");
            };
            if query.is_empty() {
                return ToolOutput::error("web_search requires a non-empty query");
            }
            if query.len() > RESEARCH_MAX_QUERY_CHARACTERS {
                return ToolOutput::error(
                    "Dreaming web research queries must not exceed 256 characters",
                );
            }
            if args.get("engine_url").is_some() {
                return ToolOutput::error(
                    "Dreaming web research cannot override the search engine",
                );
            }
            let limit = match search_limit(&args) {
                Ok(limit) => limit,
                Err(message) => return ToolOutput::error(message),
            };
            let template = ctx.web.search_engine_url.clone();
            if !template.contains("{query}") {
                return ToolOutput::error("web_search engine_url must contain {query}");
            }
            let search_url = template.replacen("{query}", &percent_encode(query), 1);
            let options = FetchOptions {
                max_response_bytes: RESEARCH_MAX_RESPONSE_BYTES,
                timeout: Duration::from_secs(RESEARCH_TIMEOUT_SECONDS),
                public_only: !ctx.web.allow_private_network,
                user_agent: ctx.web.user_agent.clone(),
            };
            let response = match fetch_url(&search_url, &options, &ctx.cancel).await {
                Ok(response) => response,
                Err(error) => return ToolOutput::error(error.message),
            };
            if !(200..300).contains(&response.status) {
                return ToolOutput::error(format!(
                    "search endpoint returned HTTP {}",
                    response.status
                ));
            }
            let html = String::from_utf8_lossy(&response.body);
            let results = parse_duckduckgo_results(&html, limit);
            let text = if results.is_empty() {
                "No web search results found".to_string()
            } else {
                results
                    .iter()
                    .enumerate()
                    .map(|(index, result)| {
                        format!("{}. {}\n   {}", index + 1, result.title, result.url)
                    })
                    .collect::<Vec<_>>()
                    .join("\n")
            };
            ToolOutput::text(text).with_details(json!({
                "query": query,
                "url": search_url,
                "results": results,
                "count": results.len(),
            }))
        })
    }
}

/// Restricted `web_run` (no direct URLs, evidence content included).
pub struct RestrictedWebRun {
    inner: WebRunTool,
    budget: Arc<ResearchBudget>,
}

impl RestrictedWebRun {
    pub fn new(budget: Arc<ResearchBudget>) -> Self {
        Self {
            inner: WebRunTool::with_limits(research_web_run_limits()),
            budget,
        }
    }
}

impl Tool for RestrictedWebRun {
    fn name(&self) -> &'static str {
        "web_run"
    }
    fn description(&self) -> String {
        self.inner.description()
    }
    fn snippet(&self) -> &'static str {
        self.inner.snippet()
    }
    fn parameters(&self) -> Value {
        self.inner.parameters()
    }
    fn parallel_safe(&self) -> bool {
        self.inner.parallel_safe()
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        if let Some(refused) = gate(&ctx, &self.budget) {
            return Box::pin(async move { refused });
        }
        self.inner.execute(args, ctx)
    }
}

/// The research child's whole tool set: restricted `web_run`, `web_search`
/// and `web_fetch` sharing one three-call budget. The composition root
/// registers exactly this registry when [`dream_research_requested`] holds.
pub fn research_registry() -> ToolRegistry {
    let budget = Arc::new(ResearchBudget::new(RESEARCH_MAX_TOOL_CALLS));
    ToolRegistry::empty()
        .with(Arc::new(RestrictedWebRun::new(budget.clone())))
        .with(Arc::new(RestrictedWebSearch::new(budget.clone())))
        .with(Arc::new(RestrictedWebFetch::new(budget)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use qt_core::tools::WebOptions;
    use std::io::{BufRead, BufReader, Write};
    use std::net::TcpListener;

    /// Serves `pages` (path → HTML) on 127.0.0.1 until the test ends.
    fn serve(pages: Vec<(&'static str, String)>) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let address = listener.local_addr().unwrap();
        std::thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { continue };
                let mut reader = BufReader::new(stream.try_clone().unwrap());
                let mut request_line = String::new();
                let _ = reader.read_line(&mut request_line);
                loop {
                    let mut line = String::new();
                    if reader.read_line(&mut line).unwrap_or(0) == 0 || line == "\r\n" {
                        break;
                    }
                }
                let path = request_line
                    .split_whitespace()
                    .nth(1)
                    .unwrap_or("/")
                    .to_string();
                let path = path.split('?').next().unwrap_or("/").to_string();
                let body = pages
                    .iter()
                    .find(|(page, _)| *page == path)
                    .map(|(_, body)| body.clone());
                let (status, body) = match body {
                    Some(body) => ("200 OK", body),
                    None => ("404 Not Found", "missing".to_string()),
                };
                let _ = write!(
                    stream,
                    "HTTP/1.1 {status}\r\nContent-Type: text/html\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
                    body.len()
                );
            }
        });
        format!("http://{address}")
    }

    fn context(base: &str) -> ToolContext {
        let mut ctx = ToolContext::new(std::env::temp_dir());
        ctx.web = WebOptions {
            search_engine_url: format!("{base}/search?q={{query}}"),
            allow_private_network: true,
            ..Default::default()
        };
        ctx
    }

    fn runtime() -> tokio::runtime::Runtime {
        tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap()
    }

    #[test]
    fn budget_is_shared_and_bounded() {
        let budget = ResearchBudget::new(3);
        assert!((0..3).all(|_| budget.try_acquire()));
        assert!(!budget.try_acquire());
        assert_eq!(budget.used(), 3);
        let unlimited = ResearchBudget::new(0);
        assert!((0..10).all(|_| unlimited.try_acquire()));
        let limits = research_web_run_limits();
        assert!(!limits.allow_direct_urls);
        assert!(limits.include_evidence_content);
        assert_eq!(limits.max_invocations, 3);
        let registry = research_registry();
        let mut names = registry.names();
        names.sort();
        assert_eq!(names, ["web_fetch", "web_run", "web_search"]);
    }

    #[test]
    fn restricted_tools_enforce_limits_and_attach_content() {
        let base = serve(vec![
            (
                "/search",
                "<a class=\"result__a\" href=\"http://127.0.0.1/doc\">Doc</a>".to_string(),
            ),
            (
                "/doc",
                "<html><body><p>Retry with backoff is standard.</p></body></html>".into(),
            ),
        ]);
        let runtime = runtime();
        let budget = Arc::new(ResearchBudget::new(3));
        let search = RestrictedWebSearch::new(budget.clone());
        let fetch = RestrictedWebFetch::new(budget.clone());
        let ctx = context(&base);

        // Argument errors still consume the shared budget, as in the archive.
        let too_long =
            runtime.block_on(search.execute(json!({"query": "x".repeat(257)}), ctx.clone()));
        assert!(too_long.is_error);
        assert_eq!(
            too_long.text_content(),
            "Dreaming web research queries must not exceed 256 characters"
        );
        let engine = runtime.block_on(search.execute(
            json!({"query": "retry", "engine_url": "http://evil/{query}"}),
            ctx.clone(),
        ));
        assert_eq!(
            engine.text_content(),
            "Dreaming web research cannot override the search engine"
        );
        assert_eq!(budget.used(), 2);
        let found =
            runtime.block_on(search.execute(json!({"query": "retry backoff"}), ctx.clone()));
        assert!(!found.is_error, "{}", found.text_content());
        assert_eq!(found.details.as_ref().unwrap()["count"], 1);
        let exhausted =
            runtime.block_on(fetch.execute(json!({"url": format!("{base}/doc")}), ctx.clone()));
        assert_eq!(exhausted.text_content(), BUDGET_MESSAGE);

        let budget = Arc::new(ResearchBudget::new(3));
        let search = RestrictedWebSearch::new(budget.clone());
        let fetch = RestrictedWebFetch::new(budget.clone());
        let limit =
            runtime.block_on(search.execute(json!({"query": "q", "limit": 7}), ctx.clone()));
        assert_eq!(
            limit.text_content(),
            "Dreaming web research is limited to six search results"
        );
        let extra =
            runtime.block_on(fetch.execute(json!({"url": "x", "headers": {}}), ctx.clone()));
        assert_eq!(
            extra.text_content(),
            "web_fetch accepts only url, max_length, start_index, raw, and timeout_seconds arguments"
        );
        let page = runtime.block_on(fetch.execute(
            json!({"url": format!("{base}/doc"), "max_length": 999999}),
            ctx.clone(),
        ));
        assert!(!page.is_error, "{}", page.text_content());
        let details = page.details.unwrap();
        assert_eq!(details["max_response_bytes"], RESEARCH_MAX_RESPONSE_BYTES);
        assert_eq!(details["max_length"], RESEARCH_MAX_FETCH_LENGTH);
        assert_eq!(details["status"], 200);
        assert!(
            details["content"]
                .as_str()
                .unwrap()
                .contains("Retry with backoff is standard.")
        );
    }

    #[test]
    fn offline_mode_refuses_before_spending_budget() {
        let runtime = runtime();
        let budget = Arc::new(ResearchBudget::new(3));
        let run = RestrictedWebRun::new(budget.clone());
        let mut ctx = context("http://127.0.0.1:9");
        ctx.web.offline = true;
        let output = runtime.block_on(run.execute(json!({"search_query": [{"q": "x"}]}), ctx));
        assert_eq!(output.text_content(), OFFLINE_MESSAGE);
        assert_eq!(budget.used(), 0);
        assert_eq!(percent_encode("a b/é"), "a%20b%2F%C3%A9");
    }
}
