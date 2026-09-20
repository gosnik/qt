# Work Tracking Index

Current task tracking lives in [PLAN.md](PLAN.md). For implemented native
behavior and documented limitations, use
[upstream compatibility](docs/upstream-compatibility.md). Removing legacy
workstream plans does not imply that upstream parity is complete.

| Area | Authoritative tracking |
| --- | --- |
| Built-in governed memory | [MEM.md](MEM.md) |
| Memory review implementation and verification | [REVIEW.md](REVIEW.md) |
| Retrieval V2 and open acceptance gates | [mem_v2.md](mem_v2.md) |
| TUI performance | [PERFORMANCE.md](PERFORMANCE.md) |

Use the current specifications and explicit remaining gaps to select work.
Checked acceptance criteria cover their stated behavior; they do not close a
broader item whose current-state section still identifies missing functionality.
When closing an item, reconcile its status, acceptance criteria, and current-state
summary together, and record implementation or verification evidence.

Verification commands and coverage are indexed in
[the reference test map](docs/reference-test-map.md). Compatibility changes are
recorded in [upstream compatibility](docs/upstream-compatibility.md).
