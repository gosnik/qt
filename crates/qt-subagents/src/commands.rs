//! `/subagent` slash command handlers (archived interactive mode), as
//! front-end independent library functions.
//!
//! `parse_subagent_command` classifies the arguments; listing and help
//! produce transcript lines directly, worker subcommands become `subagent`
//! tool arguments that the front end runs as a tracked tool call (or through
//! [`run_subagent_command`]).

use std::path::Path;

use serde_json::{Value, json};

use qt_core::tools::{Tool, ToolContext, ToolOutput};

use crate::definitions::{SubagentDefinition, SubagentSource, discover_subagents};
use crate::tool::SubagentTool;

/// What a `/subagent` invocation asks for.
#[derive(Debug, Clone, PartialEq)]
pub enum SubagentCommand {
    /// Bare `/subagent`: show the role and workflow menu ([`menu_items`]).
    Menu,
    /// `/subagent help`.
    Help,
    /// `/subagent list [user|project|both]` with the validated scope.
    List(String),
    /// A worker subcommand: run the `subagent` tool with these arguments.
    ToolCall {
        arguments: Value,
        /// Transcript label, `/subagent <arguments>`.
        label: String,
    },
}

fn split_first_argument(text: &str) -> (&str, &str) {
    let text = text.trim();
    match text.find(char::is_whitespace) {
        Some(index) => (&text[..index], text[index..].trim()),
        None => (text, ""),
    }
}

/// Lines shown when the tool is not registered for this runtime.
pub fn tool_unavailable_lines() -> Vec<String> {
    vec![
        "The subagent tool is disabled for this runtime.".into(),
        "Enable built-in tools or include subagent in the --tools allowlist.".into(),
    ]
}

/// Parse `/subagent` arguments. `Err` carries the lines to show instead.
pub fn parse_subagent_command(arguments: &str) -> Result<SubagentCommand, Vec<String>> {
    let (first, rest) = split_first_argument(arguments);
    let subcommand = first.to_ascii_lowercase();
    match subcommand.as_str() {
        "spawn" | "workers" | "send" | "follow_up" | "interrupt" | "wait" | "call" => {
            let (target, payload) = split_first_argument(rest);
            let tool_arguments = match subcommand.as_str() {
                "call" => match serde_json::from_str::<Value>(rest) {
                    Ok(value @ Value::Object(_)) => value,
                    Ok(_) => {
                        return Err(vec![
                            "invalid subagent request: arguments must be an object".into(),
                        ]);
                    }
                    Err(e) => return Err(vec![format!("invalid subagent request: {e}")]),
                },
                "workers" => json!({"action": "list_workers"}),
                "spawn" => json!({"action": "spawn", "agent": target, "task": payload}),
                other => {
                    let mut value = json!({"action": other, "target": target});
                    match other {
                        "send" => value["message"] = json!(payload),
                        "follow_up" => value["task"] = json!(payload),
                        "wait" => value["timeout_ms"] = json!(1000),
                        _ => {}
                    }
                    value
                }
            };
            Ok(SubagentCommand::ToolCall {
                arguments: tool_arguments,
                label: format!("/subagent {}", arguments.trim()),
            })
        }
        "list" => {
            let scope = if rest.is_empty() {
                "user".to_string()
            } else {
                rest.to_ascii_lowercase()
            };
            if !matches!(scope.as_str(), "user" | "project" | "both") {
                return Err(vec!["usage: /subagent list [user|project|both]".into()]);
            }
            Ok(SubagentCommand::List(scope))
        }
        "help" => Ok(SubagentCommand::Help),
        "" => Ok(SubagentCommand::Menu),
        _ => Err(vec![
            format!("unknown subagent command: {first}"),
            "usage: /subagent [list [user|project|both]|help]".into(),
        ]),
    }
}

/// `/subagent help` lines.
pub fn help_lines(agent_home: &Path) -> Vec<String> {
    vec![
        "Subagent menu:".into(),
        "  /subagent                  Open the role and workflow menu".into(),
        "  /subagent list [scope]     List user, project, or both scopes".into(),
        "  /subagent help             Show this guidance".into(),
        String::new(),
        format!(
            "User roles: {}",
            agent_home.join("agents").to_string_lossy().replace('\\', "/")
        ),
        "Project roles: nearest ancestor .agents/ and .pi/agents/ directories".into(),
        "Selecting Use this agent asks for a task and launches one tracked subagent tool call without a provider request.".into(),
        "  /subagent spawn <role> <task>       Start a persistent worker".into(),
        "  /subagent workers                   List worker identities and state".into(),
        "  /subagent send <path> <message>      Queue a message without waking an idle worker".into(),
        "  /subagent follow_up <path> <task>    Reuse or wake a worker".into(),
        "  /subagent interrupt <path>           Interrupt the current child turn".into(),
        "  /subagent wait <path>                Wait up to one second".into(),
        "  /subagent call <JSON>                Full tool arguments, including name and fork".into(),
    ]
}

/// `/subagent list <scope>` lines.
pub fn discovery_lines(agent_home: &Path, cwd: &Path, scope: &str) -> Vec<String> {
    let discovered = discover_subagents(agent_home, cwd, scope);
    let mut lines = vec![format!("Subagents (scope: {scope}):")];
    if discovered.agents.is_empty() {
        lines.push("  No executable subagents discovered.".into());
    }
    for agent in &discovered.agents {
        lines.push(format!(
            "  {} [{}] - {}",
            agent.name, agent.source, agent.description
        ));
    }
    if let Some(dir) = &discovered.project_agents_directory {
        lines.push(format!(
            "  project definitions: {}",
            dir.to_string_lossy().replace('\\', "/")
        ));
    }
    for diagnostic in &discovered.diagnostics {
        lines.push(format!("  warning: {diagnostic}"));
    }
    if scope == "project" || scope == "both" {
        lines.push(
            "Project roles are repository-controlled and require explicit trust before execution."
                .into(),
        );
    }
    lines
}

/// "View details" lines for one definition.
pub fn definition_lines(agent: &SubagentDefinition) -> Vec<String> {
    let mut lines = vec![
        format!("Subagent: {}", agent.name),
        format!("  source: {}", agent.source),
        format!("  description: {}", agent.description),
        format!(
            "  model: {}",
            agent.model.as_deref().unwrap_or("inherit parent selection")
        ),
        format!(
            "  thinking: {}",
            agent
                .thinking
                .as_deref()
                .unwrap_or("inherit parent selection")
        ),
        format!(
            "  project context: {}",
            if agent.inherit_project_context {
                "inherit"
            } else {
                "isolated"
            }
        ),
        format!(
            "  skills: {}",
            if agent.inherit_skills {
                "inherit"
            } else {
                "isolated"
            }
        ),
        format!(
            "  max child depth: {}",
            agent
                .max_subagent_depth
                .map_or("runtime default".to_string(), |d| d.to_string())
        ),
        format!(
            "  tools: {}",
            if agent.tools.is_empty() {
                "none".to_string()
            } else {
                agent.tools.join(", ")
            }
        ),
    ];
    if let Some(path) = &agent.file_path {
        lines.push(format!(
            "  definition: {}",
            path.to_string_lossy().replace('\\', "/")
        ));
    }
    lines
}

/// One entry of the bare `/subagent` menu.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MenuItem {
    /// `agent:<name>`, `__parallel__`, `__chain__`, `__project__` or `__help__`.
    pub value: String,
    pub label: String,
    pub description: String,
}

/// Title of the bare `/subagent` menu.
pub const MENU_TITLE: &str = "Subagents · user + builtins";
/// Composer text for the parallel menu entry.
pub const PARALLEL_PROMPT: &str = "Use the subagent tool in parallel mode for these tasks: ";
/// Composer text for the chain menu entry.
pub const CHAIN_PROMPT: &str = "Use the subagent tool as a sequential chain for this workflow: ";

/// The role and workflow menu: user-scope agents, then the workflow entries.
pub fn menu_items(agent_home: &Path, cwd: &Path) -> Vec<MenuItem> {
    let user = discover_subagents(agent_home, cwd, "user");
    let project_roles = discover_subagents(agent_home, cwd, "project")
        .agents
        .iter()
        .filter(|a| a.source == SubagentSource::Project)
        .count();
    let mut items: Vec<MenuItem> = user
        .agents
        .iter()
        .map(|agent| MenuItem {
            value: format!("agent:{}", agent.name),
            label: agent.name.clone(),
            description: format!("[{}] {}", agent.source, agent.description),
        })
        .collect();
    items.push(MenuItem {
        value: "__parallel__".into(),
        label: "Parallel delegation".into(),
        description: "Prepare an ordered, bounded parallel subagent request".into(),
    });
    items.push(MenuItem {
        value: "__chain__".into(),
        label: "Sequential chain".into(),
        description: "Prepare a chain whose steps can consume {previous}".into(),
    });
    items.push(MenuItem {
        value: "__project__".into(),
        label: "Inspect project roles".into(),
        description: format!(
            "{project_roles} repository-controlled roles; execution requires trust"
        ),
    });
    items.push(MenuItem {
        value: "__help__".into(),
        label: "Help and role locations".into(),
        description: "Show discovery paths and supported behavior".into(),
    });
    items
}

/// Tool arguments for the menu's "Use this agent" action.
pub fn use_agent_arguments(agent: &str, task: &str) -> Option<Value> {
    let task = task.trim();
    (!task.is_empty()).then(|| json!({"agent": agent, "task": task}))
}

/// Transcript label of a "Use this agent" tool call.
pub fn use_agent_label(agent: &str, task: &str) -> String {
    format!("Delegate to subagent \"{agent}\": {}", task.trim())
}

/// Transcript lines for a finished tool call.
pub fn tool_result_lines(output: &ToolOutput) -> Vec<String> {
    let text = output.text_content();
    let mut lines: Vec<String> = text.lines().map(str::to_string).collect();
    if output.is_error && !lines.is_empty() {
        lines[0] = format!("error: {}", lines[0]);
    }
    lines
}

/// Handle `/subagent <arguments>` end to end, running worker subcommands
/// through `tool` (when it is registered) and returning transcript lines.
pub async fn run_subagent_command(
    tool: Option<&SubagentTool>,
    arguments: &str,
    ctx: ToolContext,
) -> Vec<String> {
    let Some(tool) = tool else {
        return tool_unavailable_lines();
    };
    let agent_home = tool.options().agent_home.clone();
    match parse_subagent_command(arguments) {
        Err(lines) => lines,
        Ok(SubagentCommand::Help) => help_lines(&agent_home),
        Ok(SubagentCommand::List(scope)) => discovery_lines(&agent_home, &ctx.cwd, &scope),
        Ok(SubagentCommand::Menu) => {
            let mut lines = vec![format!("{MENU_TITLE}:")];
            for item in menu_items(&agent_home, &ctx.cwd) {
                lines.push(format!("  {} - {}", item.label, item.description));
            }
            lines
        }
        Ok(SubagentCommand::ToolCall { arguments, .. }) => {
            tool_result_lines(&tool.execute(arguments, ctx).await)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::definitions::builtin_subagents;

    #[test]
    fn parses_worker_subcommands() {
        let Ok(SubagentCommand::ToolCall { arguments, label }) =
            parse_subagent_command("spawn reviewer  Review the diff ")
        else {
            panic!("expected tool call");
        };
        assert_eq!(
            arguments,
            json!({"action": "spawn", "agent": "reviewer", "task": "Review the diff"})
        );
        assert_eq!(label, "/subagent spawn reviewer  Review the diff");
        let parse = |text: &str| match parse_subagent_command(text) {
            Ok(SubagentCommand::ToolCall { arguments, .. }) => arguments,
            other => panic!("unexpected {other:?}"),
        };
        assert_eq!(parse("WORKERS"), json!({"action": "list_workers"}));
        assert_eq!(
            parse("send /root/a hello there"),
            json!({"action": "send", "target": "/root/a", "message": "hello there"})
        );
        assert_eq!(
            parse("follow_up a next"),
            json!({"action": "follow_up", "target": "a", "task": "next"})
        );
        assert_eq!(
            parse("wait a"),
            json!({"action": "wait", "target": "a", "timeout_ms": 1000})
        );
        assert_eq!(
            parse("interrupt a"),
            json!({"action": "interrupt", "target": "a"})
        );
        assert_eq!(
            parse(r#"call {"action":"spawn","agent":"worker","task":"t","fork":"all"}"#)["fork"],
            "all"
        );
    }

    #[test]
    fn errors_and_listing() {
        assert_eq!(
            parse_subagent_command("call [1]").unwrap_err(),
            ["invalid subagent request: arguments must be an object"]
        );
        assert!(
            parse_subagent_command("call {").unwrap_err()[0]
                .starts_with("invalid subagent request: ")
        );
        assert_eq!(
            parse_subagent_command("list everything").unwrap_err(),
            ["usage: /subagent list [user|project|both]"]
        );
        assert_eq!(
            parse_subagent_command("bogus").unwrap_err(),
            [
                "unknown subagent command: bogus",
                "usage: /subagent [list [user|project|both]|help]"
            ]
        );
        assert_eq!(parse_subagent_command(""), Ok(SubagentCommand::Menu));
        assert_eq!(
            parse_subagent_command("list BOTH"),
            Ok(SubagentCommand::List("both".into()))
        );
        assert_eq!(parse_subagent_command(" help "), Ok(SubagentCommand::Help));
    }

    #[test]
    fn menu_and_lines() {
        let home = tempfile::tempdir().unwrap();
        let repo = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(repo.path().join(".pi/agents")).unwrap();
        std::fs::write(
            repo.path().join(".pi/agents/p.md"),
            "---\nname: proj\ndescription: project role\n---\n",
        )
        .unwrap();
        let items = menu_items(home.path(), repo.path());
        assert_eq!(items[0].value, "agent:planner");
        assert_eq!(
            items[0].description,
            "[builtin] Design an implementation plan without changing files"
        );
        assert_eq!(items.len(), 8);
        assert_eq!(
            items[6].description,
            "1 repository-controlled roles; execution requires trust"
        );
        let lines = discovery_lines(home.path(), repo.path(), "both");
        assert_eq!(lines[0], "Subagents (scope: both):");
        assert!(lines.contains(&"  proj [project] - project role".to_string()));
        assert_eq!(
            lines.last().unwrap(),
            "Project roles are repository-controlled and require explicit trust before execution."
        );
        let details = definition_lines(&builtin_subagents()[0]);
        assert_eq!(details[0], "Subagent: scout");
        assert_eq!(details[4], "  thinking: inherit parent selection");
        assert_eq!(details[7], "  max child depth: 0");
        assert!(details[8].starts_with("  tools: read, symbol"));
        assert_eq!(help_lines(Path::new("/h"))[5], "User roles: /h/agents");
        assert_eq!(
            use_agent_arguments("scout", "  find x "),
            Some(json!({"agent": "scout", "task": "find x"}))
        );
        assert_eq!(use_agent_arguments("scout", " "), None);
    }
}
