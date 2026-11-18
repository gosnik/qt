//! Bounded typed-graph traversal across memory relations, structured
//! assertions, code-index edges and cross-store links (archived
//! `retrieval_graph.cpp`).

use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::Arc;

use super::{MemoryAssertion, RetrievalOwnerKind, RetrievalReference};
use crate::code::{CodeEdge, CodeNode, CodeNodeKind, CodeSnapshot, source_fingerprint};
use crate::error::Result;
use crate::model::{GraphQuery, MemoryScope, MemoryStatus, Millis, RecallPolicy, now_ms};
use crate::scope::scope_can_access;
use crate::store::MemoryStore;

/// Repository and worktree whose code nodes the traversal may use.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct RetrievalGraphIdentity {
    pub repository: String,
    pub worktree: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum RetrievalGraphDirection {
    Outgoing,
    Incoming,
    #[default]
    Both,
}

impl RetrievalGraphDirection {
    fn allows(self, outgoing: bool) -> bool {
        match self {
            Self::Both => true,
            Self::Outgoing => outgoing,
            Self::Incoming => !outgoing,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct RetrievalGraphSeed {
    pub reference: RetrievalReference,
    /// Depth already consumed by a validated prefix path.
    pub depth: usize,
}

#[derive(Debug, Clone, PartialEq)]
pub struct RetrievalGraphQuery {
    pub seeds: Vec<RetrievalGraphSeed>,
    pub policy: RecallPolicy,
    pub include_global_user: bool,
    pub max_depth: usize,
    pub max_fanout: usize,
    pub max_edges: usize,
    pub max_results: usize,
    pub direction: RetrievalGraphDirection,
    pub allowed_relations: Vec<String>,
    pub as_of: Option<Millis>,
}

impl Default for RetrievalGraphQuery {
    fn default() -> Self {
        Self {
            seeds: Vec::new(),
            policy: RecallPolicy::Balanced,
            include_global_user: false,
            max_depth: 2,
            max_fanout: 16,
            max_edges: 256,
            max_results: 64,
            direction: RetrievalGraphDirection::Both,
            allowed_relations: Vec::new(),
            as_of: None,
        }
    }
}

/// One traversed relation. The persisted relation stays oriented from its
/// original source; `incoming` records that traversal reached it backwards.
#[derive(Debug, Clone, PartialEq)]
pub struct RetrievalGraphEdge {
    pub from: RetrievalReference,
    pub candidates: Vec<RetrievalReference>,
    pub relation: String,
    pub evidence_id: String,
    pub resolution: String,
    pub incoming: bool,
    pub assertion_positive: bool,
    pub valid_from: Option<Millis>,
    pub valid_to: Option<Millis>,
    pub assertion_evidence: Vec<String>,
    pub assertion: Option<MemoryAssertion>,
    pub heuristic: bool,
}

impl RetrievalGraphEdge {
    fn new(
        from: RetrievalReference,
        candidates: Vec<RetrievalReference>,
        relation: String,
        evidence_id: String,
        incoming: bool,
    ) -> Self {
        Self {
            from,
            candidates,
            relation,
            evidence_id,
            resolution: "resolved".into(),
            incoming,
            assertion_positive: true,
            valid_from: None,
            valid_to: None,
            assertion_evidence: Vec::new(),
            assertion: None,
            heuristic: false,
        }
    }

    fn with_assertion(mut self, assertion: &MemoryAssertion) -> Self {
        self.assertion_positive = assertion.positive;
        self.valid_from = assertion.valid_from;
        self.valid_to = assertion.valid_to;
        self.assertion_evidence = assertion
            .evidence
            .iter()
            .map(|item| {
                if item.source_id.is_empty() {
                    item.source_uri.clone()
                } else {
                    item.source_id.clone()
                }
            })
            .filter(|id| !id.is_empty())
            .collect();
        self.assertion = Some(assertion.clone());
        self
    }
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct RetrievalGraphResult {
    pub nodes: Vec<RetrievalReference>,
    pub edges: Vec<RetrievalGraphEdge>,
    pub omissions: Vec<String>,
}

/// Authorises an endpoint beyond the built-in visibility rules.
pub type EndpointAuthorizer<'a> = &'a dyn Fn(&MemoryScope, &RetrievalReference) -> bool;

/// Traverses relations visible from one caller scope.
pub struct RetrievalGraphAdapter<'a> {
    store: &'a MemoryStore,
    snapshot: Option<Arc<CodeSnapshot>>,
    identity: RetrievalGraphIdentity,
    use_links: bool,
    use_structured: bool,
    authorizer: Option<EndpointAuthorizer<'a>>,
}

fn allowed(query: &RetrievalGraphQuery, relation: &str) -> bool {
    query.allowed_relations.is_empty()
        || query.allowed_relations.iter().any(|item| item == relation)
}

impl<'a> RetrievalGraphAdapter<'a> {
    pub fn new(
        store: &'a MemoryStore,
        snapshot: Option<Arc<CodeSnapshot>>,
        identity: RetrievalGraphIdentity,
    ) -> Self {
        Self {
            store,
            snapshot,
            identity,
            use_links: true,
            use_structured: true,
            authorizer: None,
        }
    }

    /// Additional endpoint authorisation (for example revision pins).
    pub fn with_authorizer(mut self, authorizer: EndpointAuthorizer<'a>) -> Self {
        self.authorizer = Some(authorizer);
        self
    }

    /// Disable cross-store links or structured assertions.
    pub fn with_sources(mut self, links: bool, structured: bool) -> Self {
        self.use_links = links;
        self.use_structured = structured;
        self
    }

    fn authorized(&self, scope: &MemoryScope, reference: &RetrievalReference) -> bool {
        self.authorizer
            .is_none_or(|authorize| authorize(scope, reference))
    }

    pub fn traverse(
        &self,
        scope: &MemoryScope,
        query: &RetrievalGraphQuery,
    ) -> Result<RetrievalGraphResult> {
        let mut result = RetrievalGraphResult::default();
        let max_depth = query.max_depth.min(8);
        let max_fanout = query.max_fanout.min(64);
        let max_edges = query.max_edges.min(256);
        let max_results = query.max_results.min(64);
        if max_results == 0 || max_edges == 0 || max_fanout == 0 {
            return Ok(result);
        }
        let snapshot = self.snapshot.as_deref();
        if snapshot.is_some_and(|snapshot| snapshot.stale) {
            result.omissions.push("code snapshot is stale".into());
        }
        let mut code_by_id: HashMap<&str, &CodeNode> = HashMap::new();
        let mut code_out: HashMap<&str, Vec<&CodeEdge>> = HashMap::new();
        let mut code_in: HashMap<&str, Vec<&CodeEdge>> = HashMap::new();
        if let Some(snapshot) = snapshot {
            for node in &snapshot.nodes {
                if node.repository == self.identity.repository
                    && node.worktree == self.identity.worktree
                {
                    code_by_id.insert(node.id.as_str(), node);
                }
            }
            for edge in &snapshot.edges {
                code_out
                    .entry(edge.caller_id.as_str())
                    .or_default()
                    .push(edge);
                for callee in &edge.callee_ids {
                    code_in.entry(callee.as_str()).or_default().push(edge);
                }
            }
        }
        let code_node = |reference: &RetrievalReference| -> Option<&CodeNode> {
            let snapshot = snapshot?;
            if snapshot.stale || reference.kind != RetrievalOwnerKind::Code {
                return None;
            }
            let node = *code_by_id.get(reference.id.as_str())?;
            if !node.path.is_empty()
                && source_fingerprint(&snapshot.root.join(&node.path)) != node.source_fingerprint
            {
                return None;
            }
            let pinned = !reference.repository.is_empty()
                || !reference.worktree.is_empty()
                || !reference.path.is_empty()
                || !reference.source_fingerprint.is_empty()
                || reference.snapshot_generation != 0;
            if pinned
                && (reference.repository.is_empty()
                    || reference.worktree.is_empty()
                    || reference.snapshot_generation == 0
                    || (node.kind != CodeNodeKind::Project
                        && (reference.path.is_empty() || reference.source_fingerprint.is_empty())))
            {
                return None;
            }
            if (!reference.repository.is_empty() && reference.repository != node.repository)
                || (!reference.worktree.is_empty() && reference.worktree != node.worktree)
                || (!reference.path.is_empty() && reference.path != node.path)
                || (!reference.source_fingerprint.is_empty()
                    && reference.source_fingerprint != node.source_fingerprint)
                || (reference.snapshot_generation != 0
                    && reference.snapshot_generation != node.generation)
            {
                return None;
            }
            Some(node)
        };
        let single_entity = |id: &str| -> Result<Option<String>> {
            let matches =
                self.store
                    .find_entities(scope, id, query.policy, query.include_global_user, 2)?;
            Ok((matches.len() == 1).then(|| matches[0].id.clone()))
        };
        let normalize = |reference: &RetrievalReference| -> Result<Option<RetrievalReference>> {
            match reference.kind {
                RetrievalOwnerKind::Code => Ok(code_node(reference).map(CodeSnapshot::reference)),
                RetrievalOwnerKind::Entity => {
                    if !self.use_structured || reference.id.is_empty() {
                        return Ok(None);
                    }
                    Ok(single_entity(&reference.id)?.map(|id| RetrievalReference {
                        id,
                        ..reference.clone()
                    }))
                }
                RetrievalOwnerKind::Memory => Ok(Some(reference.clone())),
            }
        };
        let visible = |reference: &RetrievalReference| -> Result<bool> {
            Ok(match reference.kind {
                RetrievalOwnerKind::Code => {
                    code_node(reference).is_some() && self.authorized(scope, reference)
                }
                RetrievalOwnerKind::Entity => {
                    self.use_structured
                        && self
                            .store
                            .find_entities(
                                scope,
                                &reference.id,
                                query.policy,
                                query.include_global_user,
                                2,
                            )?
                            .len()
                            == 1
                        && self.authorized(scope, reference)
                }
                RetrievalOwnerKind::Memory => {
                    let memory = self.store.find_for_recall(
                        scope,
                        &reference.id,
                        query.policy,
                        query.include_global_user,
                    )?;
                    memory.is_some_and(|memory| {
                        scope_can_access(
                            scope,
                            &memory.scope,
                            query.policy,
                            query.include_global_user,
                        ) && matches!(
                            memory.status,
                            MemoryStatus::Active | MemoryStatus::Validated
                        )
                    }) && self.authorized(scope, reference)
                }
            })
        };

        let mut frontier: VecDeque<(RetrievalReference, usize)> = VecDeque::new();
        let mut visited: HashSet<String> = HashSet::new();
        for (examined_seeds, seed) in query.seeds.iter().enumerate() {
            if result.nodes.len() >= max_results || examined_seeds >= 128 {
                result.omissions.push("seed budget exhausted".into());
                break;
            }
            let Some(normalized) = normalize(&seed.reference)? else {
                result.omissions.push("seed endpoint omitted".into());
                continue;
            };
            if !visible(&normalized)? {
                result.omissions.push("seed endpoint omitted".into());
                continue;
            }
            if seed.depth > max_depth {
                result
                    .omissions
                    .push("seed depth exceeds traversal budget".into());
                continue;
            }
            if visited.insert(normalized.key()) {
                result.nodes.push(normalized.clone());
                frontier.push_back((normalized, seed.depth));
            }
        }

        let mut examined = 0usize;
        while examined < max_edges && result.nodes.len() < max_results {
            let Some((current, depth)) = frontier.pop_front() else {
                break;
            };
            if depth >= max_depth {
                continue;
            }
            let mut outgoing: Vec<RetrievalGraphEdge> = Vec::new();
            match current.kind {
                RetrievalOwnerKind::Memory => {
                    let graph_query = GraphQuery {
                        memory_id: current.id.clone(),
                        limit: max_fanout.min(max_edges - examined),
                        ..Default::default()
                    };
                    for edge in self.store.query_edges(scope, &graph_query)? {
                        if outgoing.len() >= max_fanout || examined >= max_edges {
                            break;
                        }
                        examined += 1;
                        let is_outgoing = edge.from_id == current.id;
                        if !query.direction.allows(is_outgoing) || !allowed(query, &edge.edge_type)
                        {
                            continue;
                        }
                        let id = if is_outgoing {
                            &edge.to_id
                        } else {
                            &edge.from_id
                        };
                        if id.is_empty() {
                            continue;
                        }
                        let evidence = if edge.id.is_empty() {
                            edge.source.clone()
                        } else {
                            format!("graph_edge:{}", edge.id)
                        };
                        outgoing.push(RetrievalGraphEdge::new(
                            current.clone(),
                            vec![RetrievalReference::new(
                                RetrievalOwnerKind::Memory,
                                id.clone(),
                            )],
                            edge.edge_type.clone(),
                            evidence,
                            !is_outgoing,
                        ));
                    }
                    if self.use_structured
                        && examined < max_edges
                        && outgoing.len() < max_fanout
                        && query.direction != RetrievalGraphDirection::Incoming
                    {
                        let owner = self.store.find_for_recall(
                            scope,
                            &current.id,
                            query.policy,
                            query.include_global_user,
                        )?;
                        let assertion = match &owner {
                            Some(owner) => {
                                self.store
                                    .assertion(&owner.scope, &current.id, query.as_of)?
                            }
                            None => None,
                        };
                        let at = query.as_of.unwrap_or_else(now_ms);
                        if let Some(assertion) = assertion.filter(|assertion| {
                            !assertion.evidence.is_empty()
                                && (current.revision == 0 || current.revision == assertion.revision)
                                && assertion.valid_at(at)
                                && self.authorized(
                                    scope,
                                    &RetrievalReference::memory(
                                        &assertion.memory_id,
                                        assertion.revision,
                                    ),
                                )
                        }) {
                            let evidence = assertion.evidence_id();
                            let mut add_membership = |relation: &str, entity_id: &str| {
                                if entity_id.is_empty()
                                    || examined >= max_edges
                                    || outgoing.len() >= max_fanout
                                    || !allowed(query, relation)
                                {
                                    return;
                                }
                                outgoing.push(
                                    RetrievalGraphEdge::new(
                                        current.clone(),
                                        vec![RetrievalReference::new(
                                            RetrievalOwnerKind::Entity,
                                            entity_id,
                                        )],
                                        relation.into(),
                                        evidence.clone(),
                                        false,
                                    )
                                    .with_assertion(&assertion),
                                );
                                examined += 1;
                            };
                            add_membership("assertion_subject", &assertion.subject_id);
                            if assertion.positive {
                                add_membership("assertion_object", &assertion.object_id);
                            }
                        }
                    }
                }
                RetrievalOwnerKind::Entity if self.use_structured => {
                    if query.allowed_relations.is_empty() {
                        continue;
                    }
                    let mut assertions = Vec::new();
                    if query.direction != RetrievalGraphDirection::Incoming {
                        assertions = self.store.query_assertions(
                            scope,
                            &current.id,
                            "",
                            "",
                            query.as_of,
                            query.policy,
                            query.include_global_user,
                            max_fanout,
                        )?;
                    }
                    if query.direction != RetrievalGraphDirection::Outgoing {
                        assertions.extend(self.store.query_assertions(
                            scope,
                            "",
                            "",
                            &current.id,
                            query.as_of,
                            query.policy,
                            query.include_global_user,
                            max_fanout,
                        )?);
                    }
                    for assertion in &assertions {
                        if outgoing.len() >= max_fanout || examined >= max_edges {
                            break;
                        }
                        let owner = self.store.find_for_recall(
                            scope,
                            &assertion.memory_id,
                            query.policy,
                            query.include_global_user,
                        )?;
                        if !owner.is_some_and(|owner| {
                            matches!(owner.status, MemoryStatus::Active | MemoryStatus::Validated)
                        }) {
                            continue;
                        }
                        let at = query.as_of.unwrap_or_else(now_ms);
                        if assertion.evidence.is_empty() || !assertion.valid_at(at) {
                            continue;
                        }
                        let assertion_owner =
                            RetrievalReference::memory(&assertion.memory_id, assertion.revision);
                        if !self.authorized(scope, &assertion_owner) {
                            continue;
                        }
                        let outgoing_assertion = assertion.subject_id == current.id;
                        if !allowed(query, &assertion.predicate) {
                            continue;
                        }
                        let target_id = if outgoing_assertion {
                            &assertion.object_id
                        } else {
                            &assertion.subject_id
                        };
                        let target = if target_id.is_empty() {
                            // A literal is evidence on its governed owner, not
                            // an invented entity.
                            if !outgoing_assertion {
                                continue;
                            }
                            assertion_owner
                        } else {
                            RetrievalReference::new(RetrievalOwnerKind::Entity, target_id.clone())
                        };
                        outgoing.push(
                            RetrievalGraphEdge::new(
                                current.clone(),
                                vec![target],
                                assertion.predicate.clone(),
                                assertion.evidence_id(),
                                !outgoing_assertion,
                            )
                            .with_assertion(assertion),
                        );
                        examined += 1;
                    }
                }
                _ => {
                    if code_node(&current).is_some()
                        && let Some(snapshot) = snapshot
                    {
                        let budget = max_fanout.min(max_edges - examined);
                        let mut code_edges: Vec<&CodeEdge> = Vec::new();
                        if query.direction != RetrievalGraphDirection::Incoming {
                            for edge in code_out.get(current.id.as_str()).into_iter().flatten() {
                                if code_edges.len() >= budget {
                                    break;
                                }
                                code_edges.push(edge);
                            }
                        }
                        if query.direction != RetrievalGraphDirection::Outgoing {
                            for edge in code_in.get(current.id.as_str()).into_iter().flatten() {
                                if code_edges.len() >= budget {
                                    break;
                                }
                                code_edges.push(edge);
                            }
                        }
                        for edge in code_edges {
                            if outgoing.len() >= max_fanout || examined >= max_edges {
                                break;
                            }
                            examined += 1;
                            let outgoing_edge = edge.caller_id == current.id;
                            let incoming_edge = !outgoing_edge
                                && edge.callee_ids.iter().any(|id| id == &current.id);
                            if (!outgoing_edge && !incoming_edge)
                                || (!query.direction.allows(outgoing_edge)
                                    && !(incoming_edge && query.direction.allows(false)))
                                || !allowed(query, &edge.relation)
                            {
                                continue;
                            }
                            let mut projected = RetrievalGraphEdge::new(
                                current.clone(),
                                Vec::new(),
                                edge.relation.clone(),
                                format!(
                                    "code-snapshot:{}:{}:{}",
                                    snapshot.generation, edge.caller_id, edge.relation
                                ),
                                incoming_edge,
                            );
                            projected.heuristic = edge.heuristic;
                            projected.resolution = if edge.ambiguous {
                                "ambiguous".into()
                            } else if edge.callee_ids.is_empty() {
                                "unresolved".into()
                            } else {
                                "resolved".into()
                            };
                            if outgoing_edge {
                                for id in &edge.callee_ids {
                                    if projected.candidates.len() >= max_fanout {
                                        break;
                                    }
                                    if let Some(node) = code_by_id.get(id.as_str()) {
                                        projected.candidates.push(CodeSnapshot::reference(node));
                                    }
                                }
                            } else if let Some(node) = code_by_id.get(edge.caller_id.as_str()) {
                                projected.candidates.push(CodeSnapshot::reference(node));
                            }
                            if projected.candidates.is_empty() && !edge.unresolved_name.is_empty() {
                                projected.resolution = "unresolved".into();
                            }
                            outgoing.push(projected);
                        }
                    }
                }
            }
            if self.use_links && examined < max_edges && outgoing.len() < max_fanout {
                let budget = (max_fanout - outgoing.len()).min(max_edges - examined);
                for link in self.store.links(
                    scope,
                    &current,
                    query.policy,
                    query.include_global_user,
                    budget,
                )? {
                    if examined >= max_edges {
                        break;
                    }
                    examined += 1;
                    if !visible(&link.from)? || !visible(&link.to)? {
                        result
                            .omissions
                            .push("cross-store link endpoint omitted by visibility policy".into());
                        continue;
                    }
                    if !allowed(query, &link.relation) {
                        continue;
                    }
                    let outgoing_link = link.from.key() == current.key();
                    let incoming_link = link.to.key() == current.key();
                    if (!outgoing_link && !incoming_link)
                        || (!query.direction.allows(outgoing_link)
                            && !(incoming_link && query.direction.allows(false)))
                    {
                        continue;
                    }
                    let target = if outgoing_link {
                        link.to.clone()
                    } else {
                        link.from.clone()
                    };
                    outgoing.push(RetrievalGraphEdge::new(
                        current.clone(),
                        vec![target],
                        link.relation.clone(),
                        link.evidence_id.clone(),
                        incoming_link,
                    ));
                }
            }
            for edge in outgoing.into_iter().take(max_fanout) {
                let mut visible_edge = edge.clone();
                visible_edge.candidates.clear();
                let mut remaining = max_results - result.nodes.len();
                for candidate in &edge.candidates {
                    if !visible(candidate)? {
                        result
                            .omissions
                            .push("edge endpoint omitted by visibility policy".into());
                        continue;
                    }
                    if !visited.contains(&candidate.key()) {
                        if remaining == 0 {
                            continue;
                        }
                        remaining -= 1;
                    }
                    visible_edge.candidates.push(candidate.clone());
                }
                if visible_edge.candidates.is_empty() && !edge.candidates.is_empty() {
                    continue;
                }
                for candidate in &visible_edge.candidates {
                    if result.nodes.len() >= max_results {
                        break;
                    }
                    if !visited.insert(candidate.key()) {
                        continue;
                    }
                    result.nodes.push(candidate.clone());
                    if result.nodes.len() < max_results && edge.assertion_positive {
                        frontier.push_back((candidate.clone(), depth + 1));
                    }
                }
                result.edges.push(visible_edge);
                if result.nodes.len() >= max_results {
                    break;
                }
            }
        }
        if examined >= max_edges {
            result.omissions.push("edge budget exhausted".into());
        }
        Ok(result)
    }
}
