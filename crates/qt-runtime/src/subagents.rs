//! Subagent wiring: the `subagent` tool (children are `qt-cli --mode json`
//! processes), a persistent-worker coordinator per parent session and the
//! delivery of worker completions into the parent conversation.

use std::path::PathBuf;
use std::sync::{Arc, Mutex, RwLock};

use async_channel::Receiver;
use futures::future::BoxFuture;
use qt_core::agent::AgentEvent;
use qt_core::engine::{Conversation, Engine};
use qt_core::tools::{Tool, ToolContext, ToolOutput};
use qt_subagents::coordinator::{CoordinatorOptions, SubagentCoordinator};
use qt_subagents::runner::{ChildRunner, ProcessChildRunner};
use qt_subagents::tool::{ParentState, SubagentTool, SubagentToolOptions};
use serde_json::Value;

/// The `subagent` tool whose coordinator follows the active conversation.
/// Registered once with the engine; [`SubagentSubsystem::bind`] swaps the
/// tool behind it when the conversation changes.
pub struct SubagentToolProxy {
    current: Arc<RwLock<SubagentTool>>,
}

impl Tool for SubagentToolProxy {
    fn name(&self) -> &'static str {
        "subagent"
    }
    fn description(&self) -> String {
        self.current.read().expect("subagent lock").description()
    }
    fn snippet(&self) -> &'static str {
        self.current.read().expect("subagent lock").snippet()
    }
    fn guidelines(&self) -> &'static [&'static str] {
        self.current.read().expect("subagent lock").guidelines()
    }
    fn parameters(&self) -> Value {
        self.current.read().expect("subagent lock").parameters()
    }
    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        let tool = self.current.read().expect("subagent lock").clone();
        tool.execute(args, ctx)
    }
}

/// Receives the event stream of runs started by worker completions.
pub type RunSink = Arc<dyn Fn(Receiver<AgentEvent>) + Send + Sync>;

/// The subagent tool, its runner and the coordinator of the active
/// parent session.
pub struct SubagentSubsystem {
    agent_home: PathBuf,
    runner: Arc<dyn ChildRunner>,
    parent: Arc<Mutex<Option<Conversation>>>,
    current: Arc<RwLock<SubagentTool>>,
    coordinator: Mutex<Option<Arc<SubagentCoordinator>>>,
}

impl SubagentSubsystem {
    /// `executable` overrides the child binary (default: discovered `qt-cli`).
    pub fn new(agent_home: PathBuf, executable: Option<PathBuf>) -> Self {
        let runner: Arc<dyn ChildRunner> = Arc::new(match executable {
            Some(exe) => ProcessChildRunner::new(exe),
            None => ProcessChildRunner::discover(),
        });
        let parent: Arc<Mutex<Option<Conversation>>> = Arc::new(Mutex::new(None));
        let tool = SubagentTool::new(Self::options(&agent_home, &runner, &parent, None));
        Self {
            agent_home,
            runner,
            parent,
            current: Arc::new(RwLock::new(tool)),
            coordinator: Mutex::new(None),
        }
    }

    fn options(
        agent_home: &std::path::Path,
        runner: &Arc<dyn ChildRunner>,
        parent: &Arc<Mutex<Option<Conversation>>>,
        coordinator: Option<Arc<SubagentCoordinator>>,
    ) -> SubagentToolOptions {
        let state_source = parent.clone();
        let mut options = SubagentToolOptions::new(agent_home)
            .with_runner(runner.clone())
            .with_parent_state(Arc::new(move || {
                let conversation = state_source.lock().expect("parent lock").clone();
                match conversation {
                    Some(c) => ParentState {
                        active_tools: Some(c.active_tools()),
                        model: c.model(),
                    },
                    None => ParentState::default(),
                }
            }));
        if let Some(c) = coordinator {
            options = options.with_coordinator(c);
        }
        options
    }

    /// The tool to register with the engine.
    pub fn tool(&self) -> Arc<dyn Tool> {
        Arc::new(SubagentToolProxy {
            current: self.current.clone(),
        })
    }

    /// The tool bound to the active conversation's coordinator.
    pub fn current_tool(&self) -> SubagentTool {
        self.current.read().expect("subagent lock").clone()
    }

    pub fn runner(&self) -> &Arc<dyn ChildRunner> {
        &self.runner
    }

    /// The coordinator of the bound conversation.
    pub fn coordinator(&self) -> Option<Arc<SubagentCoordinator>> {
        self.coordinator.lock().expect("coordinator lock").clone()
    }

    /// Bind to `conversation`: open the coordinator of its session (closing
    /// the previous one) and, when `delivery` is given, deliver worker
    /// completions into the conversation as follow-ups (runs they start go
    /// to the sink).
    pub fn bind(&self, engine: &Engine, conversation: &Conversation, delivery: Option<RunSink>) {
        *self.parent.lock().expect("parent lock") = Some(conversation.clone());
        let session_id = conversation.session_id();
        let previous = self.coordinator.lock().expect("coordinator lock").take();
        if let Some(old) = previous {
            if old.parent_session_id() == session_id {
                *self.coordinator.lock().expect("coordinator lock") = Some(old.clone());
                if let Some(sink) = delivery {
                    qt_subagents::attach_conversation(&old, conversation, sink);
                }
                return;
            }
            engine.spawn(async move { old.shutdown().await });
        }
        let mut options =
            CoordinatorOptions::new_in(&self.agent_home, session_id, engine.runtime_handle());
        options.concurrency = options.concurrency.max(1);
        let coordinator = Arc::new(SubagentCoordinator::open(options, self.runner.clone()));
        if let Some(sink) = delivery {
            qt_subagents::attach_conversation(&coordinator, conversation, sink);
        } else {
            let snapshot_source = conversation.clone();
            coordinator.set_context_snapshot(Arc::new(move || snapshot_source.agent_messages()));
        }
        *self.current.write().expect("subagent lock") = SubagentTool::new(Self::options(
            &self.agent_home,
            &self.runner,
            &self.parent,
            Some(coordinator.clone()),
        ));
        *self.coordinator.lock().expect("coordinator lock") = Some(coordinator);
    }

    /// Stop every worker of the bound coordinator.
    pub async fn shutdown(&self) {
        let coordinator = self.coordinator.lock().expect("coordinator lock").take();
        if let Some(c) = coordinator {
            c.shutdown().await;
        }
    }
}
