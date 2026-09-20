# Persistent native subagents

The `subagent` tool supports asynchronous logical workers in addition to its
existing synchronous single, parallel, and chain modes. Each worker uses an
isolated native `qt` process per turn and resumes the same JSONL session on
follow-up. No JavaScript runtime or additional dependency is required.

| Action | Arguments | Behavior |
| --- | --- | --- |
| `spawn` | `agent`, `task`, optional `name`, `fork`, `model`, `cwd` | Returns the child identity and session path without waiting for the model. |
| `list_workers` | none | Returns child status, generation, permissions, and mailbox state. |
| `send` | `target`, `message`, optional `wake` (false) | Enqueues a message. An idle worker stays idle unless explicitly woken. A running worker consumes queued messages between turns. |
| `follow_up` | `target`, `task` | Enqueues a task and wakes the existing worker. |
| `wait` | `target`, optional `timeout_ms` (0–60000) | Waits for the worker's scheduled turns, with an explicit timeout result. |
| `interrupt` | `target` | Cancels the current or scheduled turn while retaining the worker identity and session. |

Targets are canonical paths such as `/root/reviewer`. Names are unique within
the parent; spawning an existing name fails. Use `follow_up` to reuse it.
Interruption terminates the child process; it does not preserve an in-flight
provider connection. Completed turns remain available in its session.

`fork` accepts `"none"` (default), `"all"`, or a positive integer specifying the
last N parent user turns. The snapshot is captured before tool execution and
includes assistant/tool messages. Calls still running at the fork point receive
an explicit interrupted result in the child's independent history. Forks record
parent session identity and mode; unavailable or malformed forks fail explicitly.

The parent runtime owns completion delivery. Interactive mode displays results
and schedules a parent follow-up; print/JSON mode remains alive for pending
workers and feeds their results back into the parent. JSON and RPC streams emit
`subagent_event` frames. Completions carry child identity, generation, status,
output, and a per-child event ID. RPC also exposes a typed command, for example:

```json
{"type":"subagent","id":"request-1","action":"spawn","arguments":{"agent":"reviewer","name":"review","task":"Review the current diff","fork":"all"}}
```

Interactive commands include `/subagent spawn ROLE TASK`, `/subagent workers`,
`/subagent send PATH MESSAGE`, `/subagent follow_up PATH TASK`,
`/subagent interrupt PATH`, and `/subagent wait PATH`. `/subagent call JSON`
accepts the complete tool arguments, including name and fork settings.

Worker indexes and sessions live below
`<agent_home>/subagents/<parent-session-hash>/`. Files are private to the owner;
an exclusive lock protects the index while a coordinator owns it. Reloading a
parent retains child identities, mailboxes, launch settings, lineage, and
permissions. In-flight work interrupted by shutdown/crash requires an explicit
follow-up. Runtime shutdown cancels and joins workers. On Linux, cancellation
walks descendants across process groups; other Unix systems retain process-group
cancellation.

Delegation preserves workspace restrictions, role trust, recursion limits, and
parent tool permissions. Current permissions are checked again before waking a
restored worker. Concurrency is bounded per coordinator and to four worker
processes per host process. Parent harnesses are never mutated by child threads.

Delivery is acknowledged in the persisted index. As with other local session
writes, a crash between external receipt and durable acknowledgment can replay
an event; consumers can deduplicate by parent session, child, and event ID.
