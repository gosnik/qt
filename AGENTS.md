# qt Agent Instructions

## ⚡ Fast-Track Execution Rules
- **Skip Exploratory Searches:** Do not read raw files to look for definitions. Use `grep`, `git grep`, or specialized symbol search tools immediately.
- **No Drafts / No Placeholders:** Write complete production-ready code on the first pass. Never leave `// TODO` or `// Implement here` blocks.
- **Parallel Sub-Agents:** Spin up sub-agents instantly for decoupled tasks.

## Scope & Priorities
- **Target:** Native C++17 repository at the repo root.
- **Parity Source:** `reference/pi` (TypeScript/JavaScript). Treat as behavioral truth. Do not edit unless explicitly requested.
- **Secondary References:** Read and use `REFS.md` when implementing features. Treat it as the approved index for non-`reference/pi` reference material when the primary parity source is silent or ambiguous.
- **Dependency Lock:** Use only libraries pinned in `platformio.ini` and documented in `docs/dependencies.md`. Never add new dependencies.

## High-Velocity Parity Workflow
1. **Locate Contract:** Find the reference contract in `reference/pi/packages/**` or `reference/pi/docs/**`.
2. **Check Secondary Index:** Read `REFS.md` for relevant secondary references before borrowing patterns from non-`reference/pi` sources.
3. **Locate Native Owner:** Find destination files in `src/**` or `include/**`.
4. **Verify Gaps:** Record the task and relevant acceptance criteria in `PLAN.md` before coding; consult the linked specifications and compatibility notes.
5. **Behavior Over Structure:** Replicate exact CLI, RPC, TUI, and session outcomes. Do not copy line-by-line TypeScript or Node.js internal patterns.
6. **Lookup Hierarchy:**
    1. Reference docs (for intent)
    2. Reference source (implementation details)
    3. `REFS.md` indexed secondary references (only when the primary source is silent or thin)

## C++17 Architecture Directives
- **Ownership & Safety:** Enforce RAII, strict `const`-correctness, clear object lifecycles, and explicit invariants.
- **Dependency Injection:** Use constructor injection for required dependencies. Avoid service locators.
- **Composition over Inheritance:** Prefer composition. Use polymorphism only when dynamic runtime dispatch is required.
- **Lean Interfaces:** Use focused, role-specific interfaces. Do not build abstract "god interfaces" or wrap single-implementation classes unnecessarily.

## State Tracking (`PLAN.md`)
- **Pre-read Required:** Read `PLAN.md` before writing any implementation code.
- **Atomic Updates:** Update checkboxes immediately upon finishing a component.
- **Strict Completion:** Mark `[x]` only if code and edge-case behavior fully satisfy the item.
- **Partial Shifts:** Leave items `[ ]` if partially done. Detail the missing gap explicitly in the final chat response. Do not let context drift out of the markdown files.

## Verification & Definition of Done (DoD)
- **Automated Verification:** Run the available build and framework checks before claiming success.
- **Artifact Updates:** Every complete change requires updating `docs/upstream-compatibility.md` when compatibility behavior changes.
