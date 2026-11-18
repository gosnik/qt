//! Governed Dreaming (archived `MemoryRuntime` Dreaming queue, worker and
//! status).
//!
//! Dreaming is an explicitly enabled background pipeline distinct from
//! consolidation: an isolated tool-free child sees a randomised, bounded
//! sample of governed memories and may propose one hypothesis or
//! association. The trusted parent validates it and stores a Dream artifact
//! that stays outside trusted recall behind a pending review.
//!
//! Triggers (`memory.dreaming.trigger`): `post_turn` (after capture, but only
//! once the foreground has been idle for `idleSeconds`), `interval` (cycles at
//! least `intervalSeconds` apart, also idle-gated), `candidate_threshold`
//! (after `candidateThreshold` newly captured candidates) and explicit
//! `manual` runs (`/memory dream run`), which jump the queue.
//!
//! [`DreamingService`] owns the queue. A composition root starts its worker
//! thread with [`DreamingService::start_worker`] and feeds it activity
//! ([`DreamingService::note_activity`],
//! [`DreamingService::set_foreground_active`]) and capture results
//! ([`DreamingService::note_capture`]).

pub mod child;
pub mod commands;
pub mod cycle;
pub mod protocol;

use std::collections::VecDeque;
use std::path::PathBuf;
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use serde_json::{Value, json};

use crate::error::{Result, invalid};
use crate::model::*;
use crate::post_processing::{DreamArtifactResult, DreamResearchEvidence, PostProcessingService};
use crate::review::ReviewGovernancePolicy;
use crate::scope::durable_memory_scope;
use crate::settings::MemoryDreamingSettings;
use crate::store::MemoryStore;
use crate::verifier::VerifierConfig;

pub use child::{
    CancelFlag, ChildEvent, ChildExit, ChildOutcome, ChildRequest, ChildTask, IsolatedChild,
    ProcessChild, parse_json_mode_output, run_with_cpu_fallback,
};
pub use commands::{dream_command, dream_rpc};
pub use cycle::DreamCycleReport;
pub use protocol::{
    DREAM_ARTIFACT_KINDS, DREAMING_PROMPT, DREAMING_PROTOCOL_VERSION, DREAMING_SELECTION_ALGORITHM,
    ValidatedDream, validate_dream_output,
};

/// Queued cycles beyond which new non-manual requests are refused.
pub const MAX_PENDING_DREAMS: usize = 8;

/// Why a cycle was queued.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum DreamTrigger {
    PostTurn,
    Interval,
    CandidateThreshold,
    Manual,
}

impl DreamTrigger {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::PostTurn => "post_turn",
            Self::Interval => "interval",
            Self::CandidateThreshold => "candidate_threshold",
            Self::Manual => "manual",
        }
    }

    pub fn parse(value: &str) -> Option<Self> {
        [
            Self::PostTurn,
            Self::Interval,
            Self::CandidateThreshold,
            Self::Manual,
        ]
        .into_iter()
        .find(|trigger| trigger.as_str() == value)
    }
}

/// `memory.dreaming.researchPolicy`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ResearchPolicy {
    Disabled,
    /// Only caller-supplied evidence (`/memory dream research`).
    Manual,
    /// Every Dream gets an isolated web-research follow-up.
    Required,
}

impl ResearchPolicy {
    /// Unknown values are treated as `disabled`.
    pub fn parse(value: &str) -> Self {
        match value {
            "manual" => Self::Manual,
            "required" => Self::Required,
            _ => Self::Disabled,
        }
    }
}

/// `memory.dreaming.promotionPolicy`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PromotionPolicy {
    /// Review approval may advance a Dream to `hypothesis` (default).
    ReviewOnly,
    /// Same trust as `review_only`.
    Manual,
    /// Additionally lets the local verifier validate web-researched successors.
    AutoValidate,
    /// Review cannot promote Dreams at all.
    Disabled,
}

impl PromotionPolicy {
    /// Unknown values are treated as `review_only`.
    pub fn parse(value: &str) -> Self {
        match value {
            "manual" => Self::Manual,
            "auto_validate" => Self::AutoValidate,
            "disabled" => Self::Disabled,
            _ => Self::ReviewOnly,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Self::ReviewOnly => "review_only",
            Self::Manual => "manual",
            Self::AutoValidate => "auto_validate",
            Self::Disabled => "disabled",
        }
    }

    /// Trust rank: `disabled` < `review_only` = `manual` < `auto_validate`.
    pub fn rank(self) -> u8 {
        match self {
            Self::Disabled => 0,
            Self::ReviewOnly | Self::Manual => 1,
            Self::AutoValidate => 2,
        }
    }

    /// The global-cap rule: a narrower scope (a project) can only keep or
    /// lower the global policy, never raise automatic trust above it.
    pub fn capped_by(self, global: Self) -> Self {
        if self.rank() > global.rank() {
            global
        } else {
            self
        }
    }

    /// Review governance implied by the policy.
    pub fn review_governance(self) -> ReviewGovernancePolicy {
        ReviewGovernancePolicy {
            allow_dream_hypothesis_promotion: self != Self::Disabled,
        }
    }
}

/// Dreaming configuration: resolved `memory.dreaming.*` settings plus the
/// memory-task model plan and child environment from the composition root.
#[derive(Debug, Clone, PartialEq)]
pub struct DreamingConfig {
    pub settings: MemoryDreamingSettings,
    /// Context-compilation (consolidation) model, the first `auto` fallback.
    pub consolidation_model: Option<String>,
    /// Pre-handler model, the second `auto` fallback.
    pub prehandler_model: Option<String>,
    pub discovery_roots: Vec<PathBuf>,
    pub cwd: String,
    pub agent_home: Option<PathBuf>,
}

impl DreamingConfig {
    pub fn new(settings: MemoryDreamingSettings) -> Self {
        Self {
            settings,
            consolidation_model: None,
            prehandler_model: None,
            discovery_roots: Vec::new(),
            cwd: String::new(),
            agent_home: None,
        }
    }

    /// The configured model unless it is `auto`, else the consolidation
    /// model, else the pre-handler model.
    pub fn resolved_model(&self) -> Option<String> {
        let nonempty = |value: &Option<String>| value.clone().filter(|value| !value.is_empty());
        nonempty(&self.settings.model)
            .filter(|model| model != "auto")
            .or_else(|| nonempty(&self.consolidation_model))
            .or_else(|| nonempty(&self.prehandler_model))
    }

    pub fn trigger(&self) -> DreamTrigger {
        DreamTrigger::parse(&self.settings.trigger).unwrap_or(DreamTrigger::PostTurn)
    }

    pub fn research_policy(&self) -> ResearchPolicy {
        ResearchPolicy::parse(&self.settings.research_policy)
    }

    pub fn promotion_policy(&self) -> PromotionPolicy {
        PromotionPolicy::parse(&self.settings.promotion_policy)
    }

    /// Records sent to the child: `maxCandidates` clamped to 2..=16.
    pub fn candidate_limit(&self) -> usize {
        (self.settings.max_candidates as usize).clamp(2, 16)
    }

    pub fn idle_duration(&self) -> Duration {
        Duration::from_secs(u64::from(self.settings.idle_seconds.max(1)))
    }

    pub fn interval_duration(&self) -> Duration {
        Duration::from_secs(u64::from(self.settings.interval_seconds.max(1)))
    }

    pub fn timeout(&self) -> Duration {
        Duration::from_millis(self.settings.timeout_ms.max(1) as u64)
    }

    /// Verifier settings for `auto_validate`.
    pub fn verifier_config(&self) -> VerifierConfig {
        VerifierConfig {
            enabled: self.promotion_policy() == PromotionPolicy::AutoValidate,
            verifier_model: self.settings.verifier_model.clone(),
            max_model_input_tokens: self.settings.max_model_input_tokens as usize,
            timeout: self.timeout(),
            discovery_roots: self.discovery_roots.clone(),
            cwd: (!self.cwd.is_empty()).then(|| PathBuf::from(&self.cwd)),
            agent_home: self.agent_home.clone(),
        }
    }
}

/// A queued cycle.
#[derive(Debug, Clone, PartialEq)]
pub struct PendingDream {
    /// Random UUID: selection seed and part of the claim identity.
    pub cycle_id: String,
    pub trigger: DreamTrigger,
    pub scope: MemoryScope,
    /// Caller-supplied research evidence for the resulting Dream.
    pub evidence: Option<DreamResearchEvidence>,
}

/// Runtime status (`/memory dream status`, RPC `dream` status).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct DreamingStatus {
    pub enabled: bool,
    pub trigger: String,
    pub idle_seconds: u64,
    pub idle_for_seconds: u64,
    pub waiting_for_idle: bool,
    pub running: bool,
    pub pending_runs: usize,
    pub completed_runs: u64,
    pub failed_runs: u64,
    pub last_started_at: Millis,
    pub last_completed_at: Millis,
    pub last_error: String,
    pub last_dream_id: String,
    pub last_review_id: String,
}

impl DreamingStatus {
    /// RPC status payload (archived field names).
    pub fn to_json(&self) -> Value {
        json!({
            "enabled": self.enabled,
            "trigger": self.trigger,
            "idle_seconds": self.idle_seconds,
            "idle_for_seconds": self.idle_for_seconds,
            "waiting_for_idle": self.waiting_for_idle,
            "running": self.running,
            "pending": self.pending_runs,
            "completed": self.completed_runs,
            "failed": self.failed_runs,
            "last_error": self.last_error,
            "last_dream_id": self.last_dream_id,
            "last_review_id": self.last_review_id,
        })
    }
}

/// Random UUID v4 string.
pub fn generate_cycle_id() -> String {
    let mut bytes: [u8; 16] = rand::random();
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    let hex: String = bytes.iter().map(|byte| format!("{byte:02x}")).collect();
    format!(
        "{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    )
}

/// What the scheduler wants next.
#[derive(Debug, PartialEq)]
pub(crate) enum Poll {
    Ready(Box<PendingDream>),
    WaitUntil(Instant),
    WaitForSignal,
}

#[derive(Debug)]
struct State {
    scope: MemoryScope,
    pending: VecDeque<PendingDream>,
    last_activity: Instant,
    foreground_active: bool,
    next_dream_at: Option<Instant>,
    new_candidate_count: usize,
    stopping: bool,
    running: bool,
    completed_runs: u64,
    failed_runs: u64,
    last_started_at: Millis,
    last_completed_at: Millis,
    last_error: String,
    last_dream_id: String,
    last_review_id: String,
}

impl State {
    fn idle_deadline(&self, config: &DreamingConfig) -> Instant {
        self.last_activity + config.idle_duration()
    }

    fn front_ready(&self, config: &DreamingConfig, now: Instant) -> bool {
        match self.pending.front() {
            None => false,
            Some(front) if front.trigger != DreamTrigger::PostTurn => true,
            Some(_) => !self.foreground_active && now >= self.idle_deadline(config),
        }
    }

    /// The scheduling decision (archived `process_episodes` Dreaming part).
    fn poll(&mut self, config: &DreamingConfig, interval_enabled: bool, now: Instant) -> Poll {
        let idle_deadline = self.idle_deadline(config);
        if interval_enabled {
            let next = *self
                .next_dream_at
                .get_or_insert_with(|| now + config.interval_duration());
            if !self.stopping
                && self.pending.is_empty()
                && !self.foreground_active
                && now >= next
                && now >= idle_deadline
            {
                self.pending.push_back(PendingDream {
                    cycle_id: generate_cycle_id(),
                    trigger: DreamTrigger::Interval,
                    scope: self.scope.clone(),
                    evidence: None,
                });
                self.next_dream_at = Some(now + config.interval_duration());
            }
        }
        if self.front_ready(config, now) {
            let dream = self.pending.pop_front().expect("front exists");
            self.running = true;
            self.last_started_at = now_ms();
            self.last_error.clear();
            return Poll::Ready(Box::new(dream));
        }
        if self.foreground_active {
            return Poll::WaitForSignal;
        }
        if interval_enabled {
            let next = self.next_dream_at.unwrap_or(now);
            return Poll::WaitUntil(next.max(idle_deadline));
        }
        match self.pending.front() {
            Some(front) if front.trigger == DreamTrigger::PostTurn => {
                Poll::WaitUntil(idle_deadline)
            }
            _ => Poll::WaitForSignal,
        }
    }
}

struct Shared {
    store: Arc<MemoryStore>,
    config: DreamingConfig,
    child: Option<Arc<dyn IsolatedChild>>,
    cancel: CancelFlag,
    state: Mutex<State>,
    wake: Condvar,
}

impl Shared {
    fn lock(&self) -> MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|error| error.into_inner())
    }

    fn interval_enabled(&self) -> bool {
        self.config.settings.enabled
            && self.child.is_some()
            && self.config.trigger() == DreamTrigger::Interval
    }

    fn run(&self, dream: &PendingDream) -> DreamCycleReport {
        let report = match &self.child {
            Some(child) => cycle::run_dream_cycle(
                &cycle::CycleContext {
                    store: &self.store,
                    config: &self.config,
                    child: child.as_ref(),
                    child_handle: child.clone(),
                    cancel: &self.cancel,
                },
                dream,
            ),
            None => DreamCycleReport {
                error: "no isolated Dreaming runner is configured".into(),
                ..Default::default()
            },
        };
        let mut state = self.lock();
        state.running = false;
        state.last_completed_at = now_ms();
        state.last_error = report.error.clone();
        state.last_dream_id = report.dream_id.clone();
        state.last_review_id = report.review_id.clone();
        if report.success {
            state.completed_runs += 1;
        } else {
            state.failed_runs += 1;
        }
        drop(state);
        self.wake.notify_all();
        report
    }
}

/// The Dreaming queue, scheduler and status for one runtime scope.
#[derive(Clone)]
pub struct DreamingService {
    shared: Arc<Shared>,
}

impl std::fmt::Debug for DreamingService {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("DreamingService")
            .field("config", &self.shared.config)
            .field("has_child", &self.shared.child.is_some())
            .finish()
    }
}

impl DreamingService {
    /// `scope` is the runtime scope; Dreams are stored in its durable
    /// (branch or repository) form. Without a `child` nothing can be queued.
    pub fn new(
        store: Arc<MemoryStore>,
        config: DreamingConfig,
        scope: MemoryScope,
        child: Option<Arc<dyn IsolatedChild>>,
    ) -> Self {
        Self {
            shared: Arc::new(Shared {
                store,
                config,
                child,
                cancel: CancelFlag::new(),
                state: Mutex::new(State {
                    scope: durable_memory_scope(scope),
                    pending: VecDeque::new(),
                    last_activity: Instant::now(),
                    foreground_active: false,
                    next_dream_at: None,
                    new_candidate_count: 0,
                    stopping: false,
                    running: false,
                    completed_runs: 0,
                    failed_runs: 0,
                    last_started_at: 0,
                    last_completed_at: 0,
                    last_error: String::new(),
                    last_dream_id: String::new(),
                    last_review_id: String::new(),
                }),
                wake: Condvar::new(),
            }),
        }
    }

    pub fn config(&self) -> &DreamingConfig {
        &self.shared.config
    }

    pub fn store(&self) -> &Arc<MemoryStore> {
        &self.shared.store
    }

    /// The durable scope Dreams are written to.
    pub fn scope(&self) -> MemoryScope {
        self.shared.lock().scope.clone()
    }

    /// Follows a session or branch change.
    pub fn set_scope(&self, scope: MemoryScope) {
        self.shared.lock().scope = durable_memory_scope(scope);
    }

    /// Queues a cycle. Refused when Dreaming is disabled, no child runner is
    /// configured, evidence is supplied while research is disabled, the
    /// worker is stopping or eight cycles are already queued. A new
    /// `post_turn` request replaces a queued one; `manual` jumps the queue.
    pub fn enqueue(&self, trigger: DreamTrigger, evidence: Option<DreamResearchEvidence>) -> bool {
        let config = &self.shared.config;
        if !config.settings.enabled || self.shared.child.is_none() {
            return false;
        }
        if evidence.is_some() && config.research_policy() == ResearchPolicy::Disabled {
            return false;
        }
        let cycle_id = generate_cycle_id();
        let mut state = self.shared.lock();
        if state.stopping {
            return false;
        }
        let scope = state.scope.clone();
        if trigger == DreamTrigger::PostTurn
            && let Some(existing) = state
                .pending
                .iter_mut()
                .find(|item| item.trigger == DreamTrigger::PostTurn)
        {
            existing.scope = scope;
            existing.cycle_id = cycle_id;
            return true;
        }
        if state.pending.len() >= MAX_PENDING_DREAMS {
            return false;
        }
        let dream = PendingDream {
            cycle_id,
            trigger,
            scope,
            evidence,
        };
        if trigger == DreamTrigger::Manual {
            state.last_activity = Instant::now();
            state.pending.push_front(dream);
        } else {
            state.pending.push_back(dream);
        }
        drop(state);
        self.shared.wake.notify_all();
        true
    }

    /// Foreground input, model execution or compaction happened.
    pub fn note_activity(&self) {
        self.shared.lock().last_activity = Instant::now();
        self.shared.wake.notify_all();
    }

    /// The foreground agent started or stopped running.
    pub fn set_foreground_active(&self, active: bool) {
        let mut state = self.shared.lock();
        state.foreground_active = active;
        state.last_activity = Instant::now();
        drop(state);
        self.shared.wake.notify_all();
    }

    /// Capture finished for a turn: `extracted` candidates were found and
    /// `inserted` were new. Queues a `post_turn` or `candidate_threshold`
    /// cycle per the configured trigger; returns whether one was queued.
    pub fn note_capture(&self, extracted: usize, inserted: usize) -> bool {
        let config = &self.shared.config;
        if extracted == 0 || !config.settings.enabled {
            return false;
        }
        match config.trigger() {
            DreamTrigger::PostTurn => self.enqueue(DreamTrigger::PostTurn, None),
            DreamTrigger::CandidateThreshold => {
                let threshold = (config.settings.candidate_threshold as usize).max(1);
                let due = {
                    let mut state = self.shared.lock();
                    state.new_candidate_count += inserted;
                    if state.new_candidate_count >= threshold {
                        state.new_candidate_count -= threshold;
                        true
                    } else {
                        false
                    }
                };
                due && self.enqueue(DreamTrigger::CandidateThreshold, None)
            }
            _ => false,
        }
    }

    pub fn status(&self) -> DreamingStatus {
        self.status_at(Instant::now())
    }

    fn status_at(&self, now: Instant) -> DreamingStatus {
        let config = &self.shared.config;
        let state = self.shared.lock();
        let idle_for = if state.foreground_active {
            0
        } else {
            now.saturating_duration_since(state.last_activity).as_secs()
        };
        let post_turn_pending = state
            .pending
            .iter()
            .any(|item| item.trigger == DreamTrigger::PostTurn);
        let interval_due = config.trigger() == DreamTrigger::Interval
            && state.next_dream_at.is_some_and(|next| now >= next);
        DreamingStatus {
            enabled: config.settings.enabled,
            trigger: config.settings.trigger.clone(),
            idle_seconds: config.idle_duration().as_secs(),
            idle_for_seconds: idle_for,
            waiting_for_idle: config.settings.enabled
                && (post_turn_pending || interval_due)
                && (state.foreground_active || now < state.idle_deadline(config)),
            running: state.running,
            pending_runs: state.pending.len(),
            completed_runs: state.completed_runs,
            failed_runs: state.failed_runs,
            last_started_at: state.last_started_at,
            last_completed_at: state.last_completed_at,
            last_error: state.last_error.clone(),
            last_dream_id: state.last_dream_id.clone(),
            last_review_id: state.last_review_id.clone(),
        }
    }

    /// Persistent Dreaming counters of the durable scope.
    pub fn statistics(&self) -> Result<DreamingRunStatistics> {
        Ok(self
            .shared
            .store
            .statistics(&self.scope(), RecallPolicy::Strict, false)?
            .dreaming)
    }

    /// Stores caller-supplied research as a separately reviewed successor of
    /// a pending Dream.
    pub fn add_research(
        &self,
        dream_id: &str,
        evidence: DreamResearchEvidence,
    ) -> Result<DreamArtifactResult> {
        self.note_activity();
        if self.shared.config.research_policy() == ResearchPolicy::Disabled {
            return invalid("Dreaming research successors are disabled by policy");
        }
        PostProcessingService::new(self.shared.store.clone()).remember_researched_successor(
            &self.scope(),
            dream_id,
            evidence,
        )
    }

    fn poll(&self, now: Instant) -> Poll {
        let interval = self.shared.interval_enabled();
        self.shared.lock().poll(&self.shared.config, interval, now)
    }

    /// Runs the next cycle that is ready at `now` on the calling thread.
    pub fn run_ready(&self) -> Option<DreamCycleReport> {
        match self.poll(Instant::now()) {
            Poll::Ready(dream) => Some(self.shared.run(&dream)),
            _ => None,
        }
    }

    /// Starts the background worker. Dropping or stopping the returned
    /// handle cancels running children, discards queued cycles and joins.
    pub fn start_worker(&self) -> DreamingWorker {
        let shared = self.shared.clone();
        let handle = std::thread::Builder::new()
            .name("qt-memory-dreaming".into())
            .spawn(move || worker_loop(&shared))
            .expect("spawn Dreaming worker thread");
        DreamingWorker {
            shared: self.shared.clone(),
            handle: Some(handle),
        }
    }
}

fn worker_loop(shared: &Shared) {
    loop {
        let dream = {
            let mut state = shared.lock();
            loop {
                if state.stopping {
                    state.pending.clear();
                    return;
                }
                let now = Instant::now();
                let interval = shared.interval_enabled();
                match state.poll(&shared.config, interval, now) {
                    Poll::Ready(dream) => break *dream,
                    Poll::WaitUntil(deadline) => {
                        let timeout = deadline
                            .saturating_duration_since(now)
                            .max(Duration::from_millis(10));
                        state = shared
                            .wake
                            .wait_timeout(state, timeout)
                            .unwrap_or_else(|error| error.into_inner())
                            .0;
                    }
                    Poll::WaitForSignal => {
                        state = shared
                            .wake
                            .wait(state)
                            .unwrap_or_else(|error| error.into_inner());
                    }
                }
            }
        };
        shared.run(&dream);
    }
}

/// Handle of the background Dreaming worker.
pub struct DreamingWorker {
    shared: Arc<Shared>,
    handle: Option<JoinHandle<()>>,
}

impl DreamingWorker {
    /// Cancels the running child, discards queued cycles and joins.
    pub fn stop(mut self) {
        self.shutdown();
    }

    fn shutdown(&mut self) {
        self.shared.lock().stopping = true;
        self.shared.cancel.cancel();
        self.shared.wake.notify_all();
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}

impl Drop for DreamingWorker {
    fn drop(&mut self) {
        self.shutdown();
    }
}

#[cfg(test)]
mod tests;
