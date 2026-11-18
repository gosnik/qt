//! Memory dashboard data (archived `memory_dashboard_server.cpp` operations
//! behind the ImGui/WASM dashboard): overview metrics, the memory explorer,
//! memory detail and history, the relationship graph and its layout,
//! sources, sessions and agents, the context-pack builder, capture,
//! lifecycle actions, governed Dream review, operations and logs.

use std::collections::{BTreeMap, BTreeSet, HashMap};

use crate::error::{MemoryError, Result};
use crate::ingestion::{IngestionRequest, IngestionResult};
use crate::model::*;
use crate::recall::RecallRequest;
use crate::review::*;
use crate::runtime::MemoryRuntime;
use crate::service::LifecycleRequest;

/// Actor recorded for dashboard mutations (archived).
pub const DASHBOARD_ACTOR: &str = "dashboard:user";

/// One "needs attention" signal on the overview.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UrgentSignal {
    pub kind: &'static str,
    pub title: &'static str,
    pub count: u64,
}

impl UrgentSignal {
    pub fn message(&self) -> String {
        format!("{} item(s) need attention", self.count)
    }
}

/// Overview metrics (archived `dashboard_summary_json`).
#[derive(Debug, Clone, PartialEq)]
pub struct DashboardSummary {
    pub project: String,
    pub health_score: i64,
    pub memory_entries: u64,
    pub conflict_count: u64,
    /// Pending staleness reviews (the archived summary reported none).
    pub stale_count: u64,
    pub memory_counts: BTreeMap<String, u64>,
    pub trust_states: BTreeMap<String, u64>,
    pub urgent: Vec<UrgentSignal>,
    pub statistics: MemoryStatistics,
}

/// Health score: 100 minus weighted penalties, never below zero.
pub fn health_score(stats: &MemoryStatistics) -> i64 {
    let penalty = stats.hypotheses as i64 * 2
        + stats.low_confidence as i64 * 3
        + stats.unprovenanced as i64 * 2
        + stats.deprecated as i64 * 2
        + stats.unresolved_conflicts as i64 * 10;
    (100 - penalty).max(0)
}

pub fn summary(runtime: &MemoryRuntime) -> Result<DashboardSummary> {
    let stats = runtime.statistics()?;
    let mut urgent = Vec::new();
    for (kind, title, count) in [
        (
            "conflict",
            "Conflicting memories need resolution",
            stats.unresolved_conflicts,
        ),
        (
            "review",
            "Governed reviews are pending",
            stats.pending_reviews,
        ),
        (
            "provenance",
            "Memories are missing provenance",
            stats.unprovenanced,
        ),
    ] {
        if count != 0 {
            urgent.push(UrgentSignal { kind, title, count });
        }
    }
    let stale_count = runtime
        .reviews()
        .review_statistics()
        .map(|reviews| {
            reviews
                .pending_by_kind
                .get("staleness")
                .copied()
                .unwrap_or(0)
        })
        .unwrap_or(0);
    Ok(DashboardSummary {
        project: runtime.scope().project,
        health_score: health_score(&stats),
        memory_entries: stats.memory_entries,
        conflict_count: stats.conflict_count,
        stale_count,
        memory_counts: stats.memory_counts.clone(),
        trust_states: stats.trust_states.clone(),
        urgent,
        statistics: stats,
    })
}

/// Memory types offered by the explorer and capture filters (archived).
pub const EXPLORER_TYPES: [MemoryType; 8] = [
    MemoryType::Decision,
    MemoryType::Constraint,
    MemoryType::Semantic,
    MemoryType::Episodic,
    MemoryType::Procedural,
    MemoryType::Bug,
    MemoryType::Observation,
    MemoryType::Conversation,
];

/// Statuses offered by the explorer filter (archived).
pub const EXPLORER_STATUSES: [MemoryStatus; 7] = [
    MemoryStatus::Validated,
    MemoryStatus::Active,
    MemoryStatus::Hypothesis,
    MemoryStatus::Deprecated,
    MemoryStatus::Superseded,
    MemoryStatus::Rejected,
    MemoryStatus::Expired,
];

/// Explorer search.
#[derive(Debug, Clone, PartialEq)]
pub struct MemoryFilter {
    pub query: String,
    pub memory_type: Option<MemoryType>,
    pub status: Option<MemoryStatus>,
    pub limit: usize,
    pub include_superseded: bool,
}

impl Default for MemoryFilter {
    fn default() -> Self {
        Self {
            query: String::new(),
            memory_type: None,
            status: None,
            limit: 200,
            include_superseded: true,
        }
    }
}

/// Memories of the runtime scope under the broad policy (archived
/// `dashboard_memories`; the limit is clamped to 1..=500).
pub fn memories(runtime: &MemoryRuntime, filter: &MemoryFilter) -> Result<Vec<MemoryEntry>> {
    let query = MemoryQuery {
        text: filter.query.trim().to_string(),
        policy: RecallPolicy::Broad,
        limit: filter.limit.clamp(1, 500),
        include_superseded: filter.include_superseded,
        statuses: match filter.status {
            Some(status) => vec![status],
            None => MemoryStatus::ALL.to_vec(),
        },
        memory_type: filter.memory_type,
        ..Default::default()
    };
    runtime.memories().query(&runtime.scope(), &query)
}

/// Whether a memory may only change through governed Dream review.
pub fn requires_dream_review(memory: &MemoryEntry) -> bool {
    memory.status == MemoryStatus::DreamArtifact
        || matches!(memory.memory_type, MemoryType::Dream | MemoryType::Research)
}

fn time_text(millis: Millis) -> String {
    if millis == 0 {
        return "never".into();
    }
    let seconds = millis.div_euclid(1000);
    let (days, rest) = (seconds.div_euclid(86_400), seconds.rem_euclid(86_400));
    // Howard Hinnant's civil-from-days.
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!(
        "{year:04}-{month:02}-{day:02} {:02}:{:02}:{:02} UTC",
        rest / 3600,
        rest % 3600 / 60,
        rest % 60
    )
}

/// `(label, value)` rows of the memory detail (archived `compat_memory_json`
/// fields).
pub fn memory_rows(memory: &MemoryEntry) -> Vec<(&'static str, String)> {
    let scope = &memory.scope;
    vec![
        ("ID", memory.id.clone()),
        ("Type", memory.memory_type.to_string()),
        ("Status", memory.status.to_string()),
        (
            "Requires Dream review",
            requires_dream_review(memory).to_string(),
        ),
        ("Project", scope.project.clone()),
        ("Repository", scope.repository_display.clone()),
        ("Branch", scope.branch.clone()),
        ("Session", scope.session.clone()),
        ("Agent", scope.agent.clone()),
        ("Confidence", format!("{:.2}", memory.confidence)),
        ("Quality", format!("{:.2}", memory.quality_score)),
        (
            "Token cost",
            format!(
                "{} bytes, ~{} input tokens (utf8_bytes_div_4)",
                memory.content.len(),
                memory.content.len().div_ceil(4)
            ),
        ),
        ("Source", memory.source.clone()),
        ("Source type", memory.provenance.source_type.to_string()),
        ("Source URI", memory.provenance.source_uri.clone()),
        ("Source hash", memory.provenance.source_hash.clone()),
        ("Source span", memory.provenance.source_span.clone()),
        ("Created by", memory.provenance.created_by.clone()),
        ("Tags", memory.tags.join(", ")),
        ("Accesses", memory.access_count.to_string()),
        ("Successes", memory.success_count.to_string()),
        ("Failures", memory.failure_count.to_string()),
        ("Replacement", memory.replacement_id.clone()),
        ("Superseded by", memory.superseded_by.clone()),
        ("Created", time_text(memory.created_at)),
        ("Updated", time_text(memory.updated_at)),
        ("Last accessed", time_text(memory.last_accessed_at)),
    ]
}

/// A memory and its revision history.
#[derive(Debug, Clone, PartialEq)]
pub struct MemoryDetail {
    pub entry: MemoryEntry,
    pub revisions: Vec<MemoryRevision>,
}

pub fn memory_detail(runtime: &MemoryRuntime, id: &str) -> Result<MemoryDetail> {
    let scope = runtime.scope();
    Ok(MemoryDetail {
        entry: runtime.memories().get(&scope, id)?,
        revisions: runtime.memories().revisions(&scope, id)?,
    })
}

/// One revision line: `<kind>  <time>  <status> <excerpt>`.
pub fn revision_line(revision: &MemoryRevision) -> String {
    let entry: serde_json::Value =
        serde_json::from_str(&revision.entry_json).unwrap_or(serde_json::Value::Null);
    let status = entry
        .get("status")
        .and_then(serde_json::Value::as_str)
        .unwrap_or("");
    let content = entry
        .get("content")
        .and_then(serde_json::Value::as_str)
        .unwrap_or("");
    format!(
        "{}  {}  {}  {}",
        revision.kind,
        time_text(revision.created_at),
        status,
        super::review::text_excerpt(content, 100)
    )
}

/// A graph node.
#[derive(Debug, Clone, PartialEq)]
pub struct GraphNode {
    pub id: String,
    pub label: String,
    pub memory_type: MemoryType,
    pub status: MemoryStatus,
    pub degree: usize,
    /// Connected component, largest first.
    pub group: usize,
}

/// Graph data for the explorer filter (archived `/dashboard/graph`).
#[derive(Debug, Clone, PartialEq, Default)]
pub struct GraphData {
    pub nodes: Vec<GraphNode>,
    /// Edges between shown nodes.
    pub edges: Vec<GraphEdge>,
    /// Edges returned by the store, including those to hidden memories.
    pub total_edges: usize,
}

fn prefix(text: &str, limit: usize) -> String {
    let mut end = limit.min(text.len());
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    text[..end].to_string()
}

/// Builds graph data from memories and edges: degrees, and groups as
/// connected components ordered by size then smallest member ID.
pub fn build_graph(memories: &[MemoryEntry], edges: Vec<GraphEdge>) -> GraphData {
    let ids: BTreeSet<&str> = memories.iter().map(|m| m.id.as_str()).collect();
    let total_edges = edges.len();
    let edges: Vec<GraphEdge> = edges
        .into_iter()
        .filter(|edge| ids.contains(edge.from_id.as_str()) && ids.contains(edge.to_id.as_str()))
        .collect();
    let mut degree: HashMap<&str, usize> = HashMap::new();
    let mut parent: HashMap<&str, &str> = ids.iter().map(|id| (*id, *id)).collect();
    fn find<'a>(parent: &mut HashMap<&'a str, &'a str>, id: &'a str) -> &'a str {
        let mut root = id;
        while parent[root] != root {
            root = parent[root];
        }
        let mut current = id;
        while parent[current] != root {
            let next = parent[current];
            parent.insert(current, root);
            current = next;
        }
        root
    }
    for edge in &edges {
        *degree.entry(edge.from_id.as_str()).or_insert(0) += 1;
        *degree.entry(edge.to_id.as_str()).or_insert(0) += 1;
        let (a, b) = (
            find(&mut parent, edge.from_id.as_str()),
            find(&mut parent, edge.to_id.as_str()),
        );
        if a != b {
            let (low, high) = if a < b { (a, b) } else { (b, a) };
            parent.insert(high, low);
        }
    }
    let mut components: BTreeMap<&str, Vec<&str>> = BTreeMap::new();
    for id in &ids {
        let root = find(&mut parent, id);
        components.entry(root).or_default().push(id);
    }
    let mut ordered: Vec<Vec<&str>> = components.into_values().collect();
    ordered.sort_by(|a, b| b.len().cmp(&a.len()).then_with(|| a[0].cmp(b[0])));
    let group_of: HashMap<&str, usize> = ordered
        .iter()
        .enumerate()
        .flat_map(|(index, members)| members.iter().map(move |id| (*id, index)))
        .collect();
    let nodes = memories
        .iter()
        .map(|memory| GraphNode {
            id: memory.id.clone(),
            label: super::review::text_excerpt(&prefix(&memory.content, 56), 56),
            memory_type: memory.memory_type,
            status: memory.status,
            degree: degree.get(memory.id.as_str()).copied().unwrap_or(0),
            group: group_of.get(memory.id.as_str()).copied().unwrap_or(0),
        })
        .collect();
    GraphData {
        nodes,
        edges,
        total_edges,
    }
}

pub fn graph(
    runtime: &MemoryRuntime,
    filter: &MemoryFilter,
    edge_type: &str,
    edge_limit: usize,
) -> Result<GraphData> {
    let memories = memories(runtime, filter)?;
    let edges = runtime.store().query_edges(
        &runtime.scope(),
        &GraphQuery {
            edge_type: edge_type.trim().to_string(),
            limit: edge_limit.clamp(1, 500),
            ..Default::default()
        },
    )?;
    Ok(build_graph(&memories, edges))
}

/// A node position in world coordinates and its radius.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct NodeLayout {
    pub x: f32,
    pub y: f32,
    pub radius: f32,
}

/// Archived `rebuild_graph_layout`: groups on a circle (radius
/// `max(260, 150·√groups)`), each group's nodes on rings of 6, 12, 18 …
/// (radius `70 + 52·ring`) ordered by degree then ID; a single-node group sits
/// at its centre with radius `20 + 1.2·min(12, degree)`.
pub fn graph_layout(graph: &GraphData) -> HashMap<String, NodeLayout> {
    use std::f32::consts::PI;
    let mut groups: BTreeMap<usize, Vec<&GraphNode>> = BTreeMap::new();
    for node in &graph.nodes {
        groups.entry(node.group).or_default().push(node);
    }
    let mut groups: Vec<(usize, Vec<&GraphNode>)> = groups.into_iter().collect();
    for (_, nodes) in &mut groups {
        nodes.sort_by(|a, b| b.degree.cmp(&a.degree).then_with(|| a.id.cmp(&b.id)));
    }
    groups.sort_by(|a, b| b.1.len().cmp(&a.1.len()).then_with(|| a.0.cmp(&b.0)));
    let count = groups.len();
    let cluster_radius = if count <= 1 {
        0.0
    } else {
        (150.0 * (count as f32).sqrt()).max(260.0)
    };
    let mut layout = HashMap::new();
    for (index, (_, nodes)) in groups.iter().enumerate() {
        let angle = if count <= 1 {
            0.0
        } else {
            2.0 * PI * index as f32 / count as f32
        };
        let (cx, cy) = (cluster_radius * angle.cos(), cluster_radius * angle.sin());
        let node_radius = |node: &GraphNode| 20.0 + node.degree.min(12) as f32 * 1.2;
        if nodes.len() == 1 {
            layout.insert(
                nodes[0].id.clone(),
                NodeLayout {
                    x: cx,
                    y: cy,
                    radius: node_radius(nodes[0]),
                },
            );
            continue;
        }
        let mut consumed = 0;
        let mut ring = 0usize;
        while consumed < nodes.len() {
            let capacity = if ring == 0 { 6 } else { 6 + ring * 6 };
            let ring_count = capacity.min(nodes.len() - consumed);
            let ring_radius = 70.0 + ring as f32 * 52.0;
            for slot in 0..ring_count {
                let node = nodes[consumed + slot];
                let ring_angle = 2.0 * PI * slot as f32 / ring_count as f32 + angle * 0.35;
                layout.insert(
                    node.id.clone(),
                    NodeLayout {
                        x: cx + ring_radius * ring_angle.cos(),
                        y: cy + ring_radius * ring_angle.sin(),
                        radius: node_radius(node),
                    },
                );
            }
            consumed += ring_count;
            ring += 1;
        }
    }
    layout
}

/// World-space bounds of a layout including node radii.
pub fn layout_bounds(layout: &HashMap<String, NodeLayout>) -> Option<(f32, f32, f32, f32)> {
    let mut iter = layout.values();
    let first = iter.next()?;
    let mut bounds = (
        first.x - first.radius,
        first.y - first.radius,
        first.x + first.radius,
        first.y + first.radius,
    );
    for node in iter {
        bounds.0 = bounds.0.min(node.x - node.radius);
        bounds.1 = bounds.1.min(node.y - node.radius);
        bounds.2 = bounds.2.max(node.x + node.radius);
        bounds.3 = bounds.3.max(node.y + node.radius);
    }
    Some(bounds)
}

/// Archived fit: zoom so the layout fits with 72 px padding, clamped to
/// 0.32–1.45, centred.
pub fn fit_view(layout: &HashMap<String, NodeLayout>, width: f32, height: f32) -> (f32, f32, f32) {
    let Some((min_x, min_y, max_x, max_y)) = layout_bounds(layout) else {
        return (1.0, width * 0.5, height * 0.5);
    };
    let padding = 72.0;
    let world_width = (max_x - min_x).max(1.0);
    let world_height = (max_y - min_y).max(1.0);
    let zoom_x = ((width - padding * 2.0) / world_width).max(0.1);
    let zoom_y = ((height - padding * 2.0) / world_height).max(0.1);
    let zoom = zoom_x.min(zoom_y).clamp(0.32, 1.45);
    let (center_x, center_y) = ((min_x + max_x) * 0.5, (min_y + max_y) * 0.5);
    (
        zoom,
        width * 0.5 - center_x * zoom,
        height * 0.5 - center_y * zoom,
    )
}

/// Sources of the runtime scope.
pub fn sources(runtime: &MemoryRuntime) -> Result<Vec<SourceRecord>> {
    let mut result: Vec<SourceRecord> = Vec::new();
    for scope in owner_scopes(runtime)? {
        for source in runtime.store().sources(&scope)? {
            if !result.iter().any(|known| known.id == source.id) {
                result.push(source);
            }
        }
    }
    Ok(result)
}

/// The runtime's exact scope, then its durable scope when that differs
/// (the Review Inbox fallback): records written by capture and imports live
/// in the durable scope.
fn owner_scopes(runtime: &MemoryRuntime) -> Result<Vec<MemoryScope>> {
    let exact = runtime.scope();
    let durable = crate::scope::durable_memory_scope(exact.clone());
    let mut scopes = vec![exact.clone()];
    if crate::scope::scope_key(&durable)? != crate::scope::scope_key(&exact)? {
        scopes.push(durable);
    }
    Ok(scopes)
}

/// Event history of a source (newest first).
pub fn source_events(runtime: &MemoryRuntime, source_id: &str) -> Result<Vec<SourceEvent>> {
    let mut events: Vec<SourceEvent> = Vec::new();
    let mut found = false;
    for scope in owner_scopes(runtime)? {
        if runtime
            .store()
            .sources(&scope)?
            .iter()
            .any(|source| source.id == source_id)
        {
            found = true;
            for event in runtime.store().source_events(&scope, source_id)? {
                if !events.iter().any(|known| known.id == event.id) {
                    events.push(event);
                }
            }
        }
    }
    if !found {
        return Err(MemoryError::invalid("source not found"));
    }
    events.sort_by(|a, b| {
        b.created_at
            .cmp(&a.created_at)
            .then_with(|| a.id.cmp(&b.id))
    });
    Ok(events)
}

/// `(label, value)` rows of a source.
pub fn source_rows(source: &SourceRecord) -> Vec<(&'static str, String)> {
    vec![
        ("Source ID", source.id.clone()),
        ("Type", source.source_type.to_string()),
        ("URI", source.uri.clone()),
        ("Display name", source.display_name.clone()),
        ("Trust level", source.trust_level.clone()),
        ("Hash status", source.hash_status.clone()),
        ("Hash", source.hash.clone()),
        ("Ingest count", source.ingest_count.to_string()),
        ("Imported", time_text(source.imported_at)),
        ("Last ingested", time_text(source.last_ingested_at)),
        ("Last checked", time_text(source.last_checked_at)),
    ]
}

pub fn source_event_line(event: &SourceEvent) -> String {
    format!(
        "{}  {}  {}  {}",
        time_text(event.created_at),
        event.action,
        event.status,
        event.note
    )
}

/// Memories grouped by session or agent.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GroupRow {
    pub id: String,
    pub memories_written: usize,
    pub last_active: Millis,
}

fn group_key(memory: &MemoryEntry, by_agent: bool) -> String {
    let key = if by_agent {
        &memory.scope.agent
    } else {
        &memory.scope.session
    };
    if key.is_empty() {
        "unknown".into()
    } else {
        key.clone()
    }
}

/// Archived `/dashboard/agents` and `/dashboard/sessions`.
pub fn group_rows(memories: &[MemoryEntry], by_agent: bool) -> Vec<GroupRow> {
    let mut grouped: BTreeMap<String, (usize, Millis)> = BTreeMap::new();
    for memory in memories {
        let entry = grouped.entry(group_key(memory, by_agent)).or_insert((0, 0));
        entry.0 += 1;
        entry.1 = entry.1.max(memory.updated_at);
    }
    grouped
        .into_iter()
        .map(|(id, (memories_written, last_active))| GroupRow {
            id,
            memories_written,
            last_active,
        })
        .collect()
}

/// Memories of one session or agent group.
pub fn group_members(memories: &[MemoryEntry], by_agent: bool, id: &str) -> Vec<MemoryEntry> {
    memories
        .iter()
        .filter(|memory| group_key(memory, by_agent) == id)
        .cloned()
        .collect()
}

pub fn last_active_text(millis: Millis) -> String {
    time_text(millis)
}

/// Context pack builder input.
#[derive(Debug, Clone, PartialEq)]
pub struct ContextBuild {
    pub query: String,
    /// `strict`, `balanced`, `broad`, or empty for the configured policy.
    pub recall_policy: String,
    pub budget_tokens: usize,
    pub candidate_limit: usize,
    pub result_limit: usize,
    pub include_global_user: bool,
}

impl Default for ContextBuild {
    fn default() -> Self {
        Self {
            query: String::new(),
            recall_policy: String::new(),
            budget_tokens: 900,
            candidate_limit: 50,
            result_limit: 20,
            include_global_user: false,
        }
    }
}

/// A built context pack and its explanation.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ContextBuildResult {
    pub markdown: String,
    pub memory_ids: Vec<String>,
    pub block_ids: Vec<String>,
    pub code_ids: Vec<String>,
    pub warnings: Vec<String>,
    pub budget_tokens: usize,
    pub used_tokens: usize,
    pub omitted: usize,
    /// One line per context item (selected or not) with its reason.
    pub explanation: Vec<String>,
    /// One line per recalled memory with its score.
    pub memories: Vec<String>,
    pub code_candidates: usize,
}

/// Archived `/context/build`: validates the request, previews retrieval
/// without recording access and explains the pack.
pub fn build_context(runtime: &MemoryRuntime, input: &ContextBuild) -> Result<ContextBuildResult> {
    let query = input.query.trim();
    if query.is_empty() {
        return Err(MemoryError::invalid("context build requires a query"));
    }
    let policy = match input.recall_policy.as_str() {
        "" => runtime.coordinator().settings().recall_policy,
        "strict" => RecallPolicy::Strict,
        "balanced" => RecallPolicy::Balanced,
        "broad" => RecallPolicy::Broad,
        _ => {
            return Err(MemoryError::invalid(
                "context build recall_policy must be strict, balanced, or broad",
            ));
        }
    };
    if input.budget_tokens == 0 || input.budget_tokens > 16_000 {
        return Err(MemoryError::invalid(
            "context build budget_tokens must be from 1 to 16000",
        ));
    }
    let request = RecallRequest {
        query: query.to_string(),
        policy,
        include_global_user: input.include_global_user,
        candidate_limit: input.candidate_limit,
        result_limit: input.result_limit,
        ..Default::default()
    };
    let preview = runtime.preview_retrieval(request, input.budget_tokens)?;
    let pack = &preview.context;
    let explanation = pack
        .item_explanations
        .iter()
        .map(|item| {
            let mut ids: Vec<String> = item.memory_ids.clone();
            ids.extend(item.code_ids.iter().cloned());
            ids.extend(item.block_ids.iter().cloned());
            let mut line = format!(
                "[{}] {} {}{} — {}",
                if item.selected { "x" } else { " " },
                item.category,
                ids.join(", "),
                match item.rendered_bytes {
                    Some(bytes) => format!(" ({bytes} bytes)"),
                    None => String::new(),
                },
                item.reason
            );
            if item.graph_bundle {
                line += " [graph bundle]";
            }
            if !item.eligible {
                line += " [not eligible]";
            }
            for warning in &item.warnings {
                line += &format!(" warning: {warning}");
            }
            // Categories carry their markdown heading; keep one line per item.
            super::review::text_excerpt(&line, 600)
        })
        .collect();
    let memories = preview
        .memories
        .iter()
        .map(|item| {
            format!(
                "{}  {}/{}  score={:.3} rrf={:.4}{}  {}",
                item.memory.id,
                item.memory.memory_type,
                item.memory.status,
                item.score.total,
                item.score.reciprocal_rank_fusion,
                if pack.memory_ids.contains(&item.memory.id) {
                    " [in pack]"
                } else {
                    ""
                },
                super::review::text_excerpt(&item.memory.content, 90)
            )
        })
        .collect();
    Ok(ContextBuildResult {
        markdown: pack.markdown.clone(),
        memory_ids: pack.memory_ids.clone(),
        block_ids: pack.block_ids.clone(),
        code_ids: pack.code_ids.clone(),
        warnings: pack.warnings.clone(),
        budget_tokens: input.budget_tokens,
        used_tokens: pack.estimated_tokens,
        omitted: pack.omitted,
        explanation,
        memories,
        code_candidates: preview.code.len(),
    })
}

/// Capture form input.
#[derive(Debug, Clone, PartialEq)]
pub struct CaptureWrite {
    pub content: String,
    pub memory_type: MemoryType,
    /// Capped at `candidate`/`hypothesis` (anything else is `hypothesis`).
    pub status: MemoryStatus,
    pub confidence: f64,
    pub source: String,
    pub source_type: SourceType,
    pub source_uri: String,
    pub tags: Vec<String>,
}

impl Default for CaptureWrite {
    fn default() -> Self {
        Self {
            content: String::new(),
            memory_type: MemoryType::Observation,
            status: MemoryStatus::Hypothesis,
            confidence: 0.5,
            source: "dashboard".into(),
            source_type: SourceType::Manual,
            source_uri: String::new(),
            tags: Vec::new(),
        }
    }
}

/// Statuses a dashboard write may request.
pub const CAPTURE_STATUSES: [MemoryStatus; 2] = [MemoryStatus::Candidate, MemoryStatus::Hypothesis];

/// Archived `/mcp/memory/write`: stores a memory capped at hypothesis.
pub fn write_memory(runtime: &MemoryRuntime, input: &CaptureWrite) -> Result<MemoryEntry> {
    let write = MemoryWrite {
        content: input.content.clone(),
        memory_type: input.memory_type,
        status: if input.status == MemoryStatus::Candidate {
            MemoryStatus::Candidate
        } else {
            MemoryStatus::Hypothesis
        },
        source: if input.source.is_empty() {
            "dashboard".into()
        } else {
            input.source.clone()
        },
        confidence: input.confidence.clamp(0.0, 1.0),
        provenance: Provenance {
            source_type: input.source_type,
            source_uri: input.source_uri.clone(),
            created_by: "user".into(),
            ..Default::default()
        },
        tags: input.tags.clone(),
        quality_score: 0.0,
    };
    let scope = runtime.scope();
    let id = runtime.memories().remember(&scope, write)?;
    runtime.memories().get(&scope, &id)
}

/// Archived `/mcp/memory/process`: extracts candidates from raw text and
/// stores them as `candidate` memories unless `dry_run`.
pub fn process_text(
    runtime: &MemoryRuntime,
    content: &str,
    source_uri: &str,
    tags: Vec<String>,
    dry_run: bool,
) -> Result<IngestionResult> {
    let request = IngestionRequest {
        content: content.to_string(),
        source: "dashboard:text".into(),
        provenance: Provenance {
            source_type: SourceType::ProcessedText,
            source_uri: source_uri.to_string(),
            created_by: "user".into(),
            ..Default::default()
        },
        tags,
        minimum_confidence: 0.6,
        ..Default::default()
    };
    runtime
        .ingestion()
        .ingest(&runtime.scope(), &request, !dry_run)
}

/// A lifecycle action from the memory detail.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LifecycleAction {
    Promote,
    Deprecate,
    Reject,
}

impl LifecycleAction {
    pub fn status(self) -> MemoryStatus {
        match self {
            Self::Promote => MemoryStatus::Active,
            Self::Deprecate => MemoryStatus::Deprecated,
            Self::Reject => MemoryStatus::Rejected,
        }
    }

    pub fn done_text(self) -> &'static str {
        match self {
            Self::Promote => "Promoted memory.",
            Self::Deprecate => "Deprecated memory.",
            Self::Reject => "Rejected memory.",
        }
    }
}

const DREAM_GUARD: &str = "Dream-derived memories require governed dashboard review preview and apply with explicit confirmation";
const NO_DREAM_REVIEW: &str =
    "no pending governed Dream review exists for this memory in the current runtime scope";

/// The pending Dream review whose subject is `subject_id` (archived
/// `pending_dream_review_for_subject`).
pub fn pending_dream_review(runtime: &MemoryRuntime, subject_id: &str) -> Result<ReviewItem> {
    if subject_id.is_empty() {
        return Err(MemoryError::invalid(
            "dashboard review preview requires string id",
        ));
    }
    let query = ReviewQuery {
        statuses: vec![ReviewStatus::Pending],
        subject_id: Some(subject_id.to_string()),
        kinds: super::review::DREAM_KINDS
            .iter()
            .map(|kind| kind.to_string())
            .collect(),
        limit: 2,
        ..Default::default()
    };
    let mut matches = runtime
        .reviews()
        .review_items(&query)?
        .into_iter()
        .filter(|review| {
            review.subject_id == subject_id
                && super::review::DREAM_KINDS.contains(&review.kind.as_str())
        });
    let first = matches
        .next()
        .ok_or_else(|| MemoryError::invalid(NO_DREAM_REVIEW))?;
    if matches.next().is_some() {
        return Err(MemoryError::invalid(
            "multiple pending governed Dream reviews match this memory",
        ));
    }
    Ok(first)
}

/// The exact authority scope for a dashboard transition: the runtime's own
/// scope, or its durable (repository/branch) scope for durable memories —
/// the same fallback the Review Inbox uses. Anything broader is refused by
/// governance ("outside the exact authority scope").
pub fn authority_scope(runtime_scope: &MemoryScope, subject: &MemoryScope) -> Result<MemoryScope> {
    let durable = crate::scope::durable_memory_scope(runtime_scope.clone());
    if crate::scope::scope_key(subject)? == crate::scope::scope_key(&durable)? {
        Ok(durable)
    } else {
        Ok(runtime_scope.clone())
    }
}

/// Archived `/mcp/memory/promote|deprecate|reject`: an audited transition,
/// refused for Dream-derived memories.
pub fn transition(
    runtime: &MemoryRuntime,
    id: &str,
    action: LifecycleAction,
) -> Result<MemoryEntry> {
    match pending_dream_review(runtime, id) {
        Ok(_) => return Err(MemoryError::invalid(DREAM_GUARD)),
        Err(error) if error.to_string() == NO_DREAM_REVIEW => {}
        Err(error) => return Err(error),
    }
    let runtime_scope = runtime.scope();
    let subject = runtime.memories().get(&runtime_scope, id)?;
    if requires_dream_review(&subject) {
        return Err(MemoryError::invalid(DREAM_GUARD));
    }
    let scope = authority_scope(&runtime_scope, &subject.scope)?;
    runtime.governance().transition(
        &scope,
        &LifecycleRequest::new(
            id,
            action.status(),
            DASHBOARD_ACTOR,
            "explicit memory dashboard action",
        ),
    )
}

/// Message for the dashboard Validate button (archived refusal).
pub const VALIDATE_REFUSAL: &str =
    "validation requires evidence and a governed /memory review action";

/// Previews a governed Dream review decision for a Dream-derived memory.
pub fn preview_dream_review(
    runtime: &MemoryRuntime,
    subject_id: &str,
    approve: bool,
) -> Result<(ReviewItem, ReviewAction, ReviewMutationPreview)> {
    let review = pending_dream_review(runtime, subject_id)?;
    let action = ReviewAction::new(
        if approve {
            ReviewActionKind::Approve
        } else {
            ReviewActionKind::Reject
        },
        DASHBOARD_ACTOR,
    );
    let preview = runtime
        .reviews()
        .preview_review(&review.id, action.clone())?;
    Ok((review, action, preview))
}

/// Applies a previewed Dream review decision with human authority.
pub fn apply_dream_review(
    runtime: &MemoryRuntime,
    review_id: &str,
    mut action: ReviewAction,
    preview: &ReviewMutationPreview,
) -> Result<ReviewApplicationResult> {
    let detail = runtime.reviews().review_detail(review_id)?;
    if !super::review::DREAM_KINDS.contains(&detail.item.kind.as_str()) {
        return Err(MemoryError::invalid(
            "review_id is not a governed Dream review",
        ));
    }
    action.human_authority = true;
    action.expected_revision = preview.review_revision;
    action.idempotency_key = preview.idempotency_token.clone();
    runtime.reviews().apply_review_action(review_id, action)
}

/// Retrieval engine status lines for Operations.
pub fn retrieval_lines(runtime: &MemoryRuntime) -> Vec<String> {
    let status = runtime.retrieval_status();
    let mut lines = vec![
        format!(
            "Retrieval: enabled={} running={} generation={}",
            status.enabled,
            status.running,
            if status.generation.is_empty() {
                "none"
            } else {
                &status.generation
            }
        ),
        format!(
            "Index jobs: pending={} leased={} failed={} complete={} chunks={} completed={}",
            status.jobs.pending,
            status.jobs.leased,
            status.jobs.failed,
            status.jobs.complete,
            status.jobs.chunks,
            status.completed_jobs
        ),
        format!(
            "Dense queries: cache hits={} entries={} bytes={} embedding failures={}",
            status.queries.cache_hits,
            status.queries.cache_entries,
            status.queries.cache_bytes,
            status.queries.embedding_failures
        ),
        format!(
            "Code recall: enabled={} available={} stale={} generation={}{}",
            status.code.enabled,
            status.code.available,
            status.code.stale,
            status.code.generation,
            if status.code.repository.is_empty() {
                String::new()
            } else {
                format!(" repository={}", status.code.repository)
            }
        ),
    ];
    if !status.last_error.is_empty() {
        lines.push(format!("Last retrieval error: {}", status.last_error));
    }
    if !status.queries.last_error.is_empty() {
        lines.push(format!("Last query error: {}", status.queries.last_error));
    }
    lines
}

/// Lines of a retrieval rebuild report.
pub fn rebuild_lines(report: &crate::retrieval::RetrievalMaintenanceReport) -> Vec<String> {
    vec![format!(
        "Retrieval rebuild: owners={} chunks invalidated={} vectors invalidated={} jobs superseded={} jobs enqueued={}",
        report.owners,
        report.chunks_invalidated,
        report.vectors_invalidated,
        report.jobs_superseded,
        report.jobs_enqueued
    )]
}

/// Audit log lines (newest first) for the Logs view.
pub fn audit_lines(runtime: &MemoryRuntime, limit: usize) -> Result<Vec<String>> {
    let mut events: Vec<AuditEvent> = Vec::new();
    for scope in owner_scopes(runtime)? {
        for event in runtime.store().audit_events(&scope, limit)? {
            if !events.iter().any(|known| known.id == event.id) {
                events.push(event);
            }
        }
    }
    events.sort_by(|a, b| {
        b.created_at
            .cmp(&a.created_at)
            .then_with(|| b.id.cmp(&a.id))
    });
    events.truncate(limit);
    Ok(events
        .iter()
        .map(|event| {
            let mut line = format!(
                "{}  {}  {}",
                time_text(event.created_at),
                event.actor,
                event.action
            );
            if !event.reason.is_empty() {
                line += &format!("  reason={}", event.reason);
            }
            if !event.before_id.is_empty() || !event.after_id.is_empty() {
                line += &format!("  {} -> {}", event.before_id, event.after_id);
            }
            if !event.evidence_uri.is_empty() {
                line += &format!("  evidence={}", event.evidence_uri);
            }
            line
        })
        .collect())
}

/// System lines: schema, database, scope and memory count.
pub fn system_lines(runtime: &MemoryRuntime) -> Vec<String> {
    let scope = runtime.scope();
    let options = runtime.options();
    vec![
        "Service: qt-memory-dashboard (native)".into(),
        format!("Schema version: {}", runtime.schema_version()),
        format!(
            "Database: {}",
            options
                .agent_home
                .join("memory")
                .join("memory.sqlite3")
                .display()
        ),
        format!(
            "Memory entries: {}",
            runtime
                .memory_count()
                .map(|count| count.to_string())
                .unwrap_or_else(|error| format!("unavailable ({error})"))
        ),
        format!("Scope: {}", super::review::exact_scope(&scope)),
        format!("Scope kind: {}", scope.kind),
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    fn memory(id: &str) -> MemoryEntry {
        MemoryEntry {
            id: id.into(),
            content: format!("content of {id}"),
            ..Default::default()
        }
    }

    #[test]
    fn formats_utc_times() {
        assert_eq!(time_text(0), "never");
        assert_eq!(time_text(1_700_000_000_000), "2023-11-14 22:13:20 UTC");
        assert_eq!(time_text(951_782_400_000), "2000-02-29 00:00:00 UTC");
    }

    #[test]
    fn health_score_penalties() {
        let stats = MemoryStatistics {
            hypotheses: 2,
            low_confidence: 1,
            unresolved_conflicts: 1,
            ..Default::default()
        };
        assert_eq!(health_score(&stats), 100 - 4 - 3 - 10);
        let worst = MemoryStatistics {
            unresolved_conflicts: 20,
            ..Default::default()
        };
        assert_eq!(health_score(&worst), 0);
    }

    #[test]
    fn graph_groups_components_and_lays_out_rings() {
        let memories: Vec<MemoryEntry> = ["a", "b", "c", "d", "e"]
            .iter()
            .map(|id| memory(id))
            .collect();
        let edges = vec![
            GraphEdge::new("a", "b", "related"),
            GraphEdge::new("b", "c", "related"),
            GraphEdge::new("d", "zzz", "related"),
        ];
        let graph = build_graph(&memories, edges);
        assert_eq!(graph.total_edges, 3);
        assert_eq!(graph.edges.len(), 2);
        let group = |id: &str| graph.nodes.iter().find(|n| n.id == id).unwrap().group;
        assert_eq!(group("a"), group("c"));
        assert_ne!(group("a"), group("d"));
        assert_eq!(group("a"), 0);
        assert_eq!(graph.nodes.iter().find(|n| n.id == "b").unwrap().degree, 2);
        let layout = graph_layout(&graph);
        assert_eq!(layout.len(), 5);
        // Three groups: cluster radius max(260, 150·√3).
        let d = layout["d"];
        let expected = (150.0f32 * 3f32.sqrt()).max(260.0);
        assert!(((d.x * d.x + d.y * d.y).sqrt() - expected).abs() < 0.5);
        let (zoom, _, _) = fit_view(&layout, 800.0, 600.0);
        assert!((0.32..=1.45).contains(&zoom));
    }

    #[test]
    fn groups_by_session_and_agent() {
        let mut a = memory("a");
        a.scope.session = "s1".into();
        a.updated_at = 5;
        let mut b = memory("b");
        b.scope.session = "s1".into();
        b.updated_at = 9;
        let c = memory("c");
        let rows = group_rows(&[a.clone(), b, c.clone()], false);
        assert_eq!(
            rows,
            [
                GroupRow {
                    id: "s1".into(),
                    memories_written: 2,
                    last_active: 9
                },
                GroupRow {
                    id: "unknown".into(),
                    memories_written: 1,
                    last_active: 0
                }
            ]
        );
        assert_eq!(group_members(&[a, c], true, "unknown").len(), 2);
    }
}
