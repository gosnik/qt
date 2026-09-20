# Concise C++ Guide for Agents

Use this guide when designing, writing, reviewing, or modifying C++ systems.

Goal: produce clear, modern, maintainable C++ with explicit ownership, safe lifetimes, strong types, testable boundaries, measurable performance, and minimal unnecessary architecture.

---

## 1. Core Principles

- Start with requirements and constraints, not patterns.
- Prefer simple, explicit, readable code.
- Express intent in types, names, interfaces, and module boundaries.
- Make illegal states hard to express.
- Keep invariants local and enforce them at construction or boundaries.
- Separate domain rules from infrastructure, frameworks, transport, storage, hardware, and UI.
- Prefer compile-time checking and standard library facilities.
- Encapsulate unsafe, platform-specific, or messy code behind small tested interfaces.
- Avoid over-engineering; add abstraction only when it improves safety, testability, replacement, or clarity.
- Use tools continuously: warnings, formatting, sanitizers, static analysis, and CI.

---

## 2. Requirements and Architecture

- Capture functional requirements as observable behavior.
- Capture non-functional requirements as measurable budgets: latency, memory, throughput, startup time, binary size, power, uptime, safety, security, log volume, recovery time.
- Identify the architectural boundary being changed before coding.
- Keep modules cohesive; each module should have one primary reason to change.
- Make dependencies explicit and acyclic.
- Depend inward toward stable domain abstractions.
- Isolate volatile decisions behind narrow interfaces.
- Do not leak persistence, networking, threading, framework, or hardware details into domain APIs.
- Record significant architecture decisions with context, decision, consequences, and rollback path.
- Prefer a modular monolith until service boundaries, ownership, deployment, and operational maturity justify splitting.

---

## 3. Interfaces

- Make interfaces narrow, stable, and behavior-focused.
- Prefer commands and queries over generic manager objects.
- Use strong domain types instead of primitive strings, integers, booleans, or flags.
- Avoid boolean parameter traps; use named option types.
- Define ownership, lifetime, mutability, threading, blocking, and error behavior.
- Prefer references for required non-null parameters.
- Prefer `std::span` for non-owning array views.
- Prefer `std::string_view` for non-owning string parameters.
- Return domain results, not framework artifacts.
- Use `[[nodiscard]]` for values that must be checked.
- Validate, normalize, and bound-check at trust boundaries.
- Version external APIs, serialized messages, persisted formats, and cross-module contracts.

---

## 4. Functions

- Make each function perform one logical operation.
- Keep functions short, focused, and locally understandable.
- Prefer return values over output parameters.
- Use structured return types for multiple results.
- Pass cheap values by value.
- Pass expensive read-only objects by `const&`.
- Pass mutable required objects by non-const reference.
- Mark functions `const` when they do not modify observable state.
- Mark functions `noexcept` only when genuinely guaranteed.
- Use lambdas for local behavior, not hidden architecture.

---

## 5. Ownership and Lifetime

- Use RAII for every resource: memory, files, sockets, locks, handles, transactions, hardware sessions.
- Prefer automatic storage and value members.
- Use `std::unique_ptr` for exclusive dynamic ownership.
- Use `std::shared_ptr` only for real shared ownership.
- Use references for non-null, non-owning access.
- Make ownership obvious at API boundaries.
- Prefer value semantics for simple domain objects.
- Avoid global mutable state; inject dependencies explicitly.
- Use immutable configuration and shared data where practical.

---

## 6. Classes and Structs

- Use `struct` for passive data with no invariant.
- Use `class` when enforcing invariants.
- Keep data private when invariants matter.
- Initialize every member; prefer default member initializers.
- Establish invariants in constructors.
- Prefer member initializer lists.
- Make single-argument constructors `explicit`.
- Prefer regular value types and the Rule of Zero.
- If custom ownership is required, follow the Rule of Five.
- Make destructors release resources and remain `noexcept`.
- Prefer factory functions when construction can fail or needs complex setup.

---

## 7. Error Handling

- Use one error strategy consistently per boundary.
- Use exceptions for exceptional failures when the project allows them.
- Use `std::expected<T, E>` or a project equivalent for recoverable errors when exceptions are unavailable or inappropriate.
- Use `std::optional<T>` for optional values.
- Fail early and preserve invariants after failure.
- Use assertions for programmer errors and impossible states.
- Never ignore failures; propagate, translate, log, or handle them deliberately.
- At distributed, I/O, or hardware boundaries, assume calls can fail, timeout, duplicate, reorder, or partially complete.

---

## 8. Type Safety

- Use strong types for units, IDs, states, registers, protocol fields, and domain concepts.
- Prefer scoped enums: `enum class`.
- Prefer `std::variant` over unsafe unions or inheritance for closed alternatives.
- Prefer `std::optional` over sentinel values.
- Avoid implicit narrowing conversions.
- Use `static_cast` only when conversion intent is clear.
- Prefer signed integers for arithmetic.
- Use unsigned integers mainly for bit masks, hardware registers, sizes at API boundaries, and modular arithmetic.
- Use fixed-width integer types at binary, hardware, and protocol boundaries.

---

## 9. Memory, Bounds, and Resource Safety

- Prefer containers over raw arrays.
- Use `std::array` for fixed-size arrays.
- Use `std::vector` for dynamic arrays.
- Use `std::span` for views.
- Check bounds when indexing external or untrusted data.
- Prefer iterators, ranges, and algorithms over manual indexing.
- Avoid raw allocation and manual cleanup.
- Encapsulate unavoidable C APIs behind safe wrappers.
- Keep secrets out of source, logs, crashes, and test fixtures.

---

## 10. Constants, Expressions, and Algorithms

- Prefer `const` by default.
- Use `constexpr` for compile-time constants, fixed rules, tables, and validation.
- Limit variable scope.
- Prefer initialization over assignment.
- Use `{}` initialization.
- Keep expressions simple.
- Prefer range-based `for` and standard algorithms.
- Use `auto` when the type is obvious or improves maintainability.
- Use explicit types when they improve readability or avoid surprises.

---

## 11. Templates and Generic Code

- Use templates for real generic algorithms or types.
- Use concepts or constraints when available.
- Prefer standard concepts.
- Keep template interfaces readable.
- Use `static_assert` for clear compile-time diagnostics.
- Avoid generic code that hides domain intent or creates hard-to-read errors.
- Use CRTP only when static polymorphism is valuable and simpler alternatives are worse.

---

## 12. Inheritance and Polymorphism

- Use inheritance for substitutable interfaces, not code reuse by default.
- Prefer composition for reuse.
- Keep base interfaces small and stable.
- Give polymorphic base classes a virtual destructor.
- Mark overrides with `override`.
- Use `final` when further inheritance is not intended.
- Prefer `std::variant` plus `std::visit` for closed type sets with value semantics.
- Avoid unnecessary virtual dispatch in hot paths.

---

## 13. Concurrency

- Prefer message passing, tasks, or immutable shared data over shared mutable state.
- Use RAII lock types.
- Keep critical sections short.
- Define thread-safety in type contracts.
- Avoid calling unknown or user-provided code while holding locks.
- Use atomics only for simple, well-understood synchronization.
- Make cancellation, shutdown, ownership, and error propagation explicit.
- Check race-prone code with sanitizers and stress runs.

---

## 14. Embedded and Systems C++

- Prefer static allocation where required by the platform.
- Avoid dynamic allocation in hard real-time paths.
- Make ownership, lifetime, and interrupt context explicit.
- Encapsulate `volatile` hardware access.
- Keep interrupt handlers minimal.
- Use strong types for units, registers, states, IDs, and protocol fields.
- Keep domain logic independent from hardware drivers and RTOS primitives.
- Avoid exceptions and RTTI only when the project or platform requires it.
- Provide project-specific alternatives for `expected`, assertions, logging, and allocation when the standard facility is unavailable.

---

## 15. Build, Packaging, and CI

- Use CMake targets as architecture boundaries.
- Prefer target-specific include paths, compile options, definitions, and link dependencies.
- Avoid global CMake state.
- Keep headers self-contained; include what you use.
- Minimize header dependencies; use forward declarations where practical.
- Keep implementation details in `.cpp` files.
- Separate library, executable, benchmark, and example targets.
- Pin and isolate third-party dependencies.
- Make cross-compilation and toolchain files explicit.
- Treat warnings as errors for owned code.
- CI should run formatting, linting, static analysis, sanitizers, and packaging checks for representative targets.
- Produce release artifacts from CI, not developer laptops.

---

## 16. Verification Boundaries

- Keep pure domain logic independent from I/O, clocks, randomness, threads, hardware, and global state.
- Wrap time, random, network, filesystem, hardware, and external services behind interfaces.
- Keep verification deterministic, local to the boundary being changed, and independent from execution order, real time, or uncontrolled globals.

---

## 17. Security and Robustness

- Treat all external input as hostile.
- Validate, normalize, and bound-check at boundaries.
- Prefer memory-safe C++ idioms over raw allocation and manual lifetime management.
- Avoid undefined behavior; use sanitizers in CI.
- Use least privilege for processes, containers, files, credentials, and network access.
- Pin and audit dependencies.
- Threat-model high-risk boundaries: parsing, authentication, authorization, update, storage, network, IPC, and firmware flashing.
- Fail closed where safety or security is involved.
- Log security-relevant events without leaking sensitive data.

---

## 18. Performance

- Define performance budgets before optimizing.
- First write clear, correct code.
- Measure with representative workloads and release builds.
- Prefer better algorithms and data layout over micro-optimizations.
- Minimize allocations in hot paths.
- Prefer contiguous storage and cache-friendly access.
- Avoid unnecessary copies, locks, virtual dispatch, string construction, and shared ownership in hot paths.
- Use move semantics deliberately; do not assume moves are free.
- Keep hot-path abstractions cheap and visible to the optimizer.
- Keep performance checks in CI when regressions matter.
- Do not sacrifice correctness or clarity unless measurements justify it.

---

## 19. Services, Deployment, and Operations

- Use services to separate deployable business capabilities, not arbitrary classes.
- Split services only when ownership, data boundaries, deployment, monitoring, and rollback are mature.
- Set explicit timeouts, retries, backoff, idempotency, and circuit breakers for remote calls.
- Prefer coarse-grained service calls over chatty interfaces.
- Add structured logs, metrics, traces, health checks, readiness, liveness, and graceful shutdown from the start.
- Build minimal reproducible containers; run as non-root; pin base images.
- Configure through environment or mounted config, not baked-in secrets.
- Keep deployment scripts and infrastructure definitions versioned with the code.
- Make rollback and compatibility checks part of delivery.

---

## 20. Documentation

- Keep architecture documentation close to code.
- Document important decisions, boundaries, runtime views, deployment views, and failure modes.
- Prefer small diagrams showing ownership, dependencies, and data flow.
- Explain why important abstractions exist, not just what they do.
- Delete stale documentation quickly.
- Keep public comments useful; avoid repeating obvious code.

---

## 21. Pattern Guidance

Use patterns only when they reduce coupling, clarify ownership, improve testability, or isolate change.

- **Strategy** — interchangeable algorithms behind a stable interface, function object, or template parameter.
- **Adapter** — wrap incompatible, external, legacy, C, or hardware APIs.
- **Facade** — provide a small stable API over a complex subsystem.
- **Factory / Abstract Factory** — centralize non-trivial construction, variants, dependencies, or compatible object families.
- **Builder** — safely construct complex immutable objects or objects with many options.
- **Repository / Unit of Work** — isolate persistence and coordinate related persistence changes.
- **Command** — represent actions for queues, undo, logging, retries, or deferred execution.
- **Observer / Publish-Subscribe** — decouple event producers and consumers; make subscription lifetime explicit.
- **State / Finite State Machine** — make mode-dependent behavior and transitions explicit.
- **Decorator / Proxy** — add behavior or controlled access without changing the wrapped interface.
- **Null Object** — replace scattered null checks with safe default behavior.
- **Visitor** — operate over closed type sets; prefer `std::variant` plus `std::visit` when suitable.
- **Singleton** — avoid by default; use only for truly unique process-wide services with controlled lifetime.

---

## 22. Default Choices

| Problem | Prefer |
|---|---|
| Dynamic array | `std::vector<T>` |
| Fixed array | `std::array<T, N>` |
| Non-owning array view | `std::span<T>` |
| String parameter | `std::string_view` |
| Optional value | `std::optional<T>` |
| Recoverable error | `std::expected<T, E>` or project equivalent |
| Exclusive ownership | `std::unique_ptr<T>` |
| Shared ownership | `std::shared_ptr<T>` only when necessary |
| Closed runtime alternatives | `std::variant` |
| Open runtime polymorphism | Small interface plus virtual dispatch |
| Resource cleanup | RAII object |
| Iteration | Range-for, ranges, or standard algorithm |
| Constants | `constexpr` |
| Domain states | `enum class` |
| Required object parameter | `T&` / `const T&` |
| Optional object parameter | `T*` |
| Byte buffer | `std::span<std::byte>` |
| Complex construction | Factory or builder |
| External dependency | Adapter behind an interface |
| Volatile infrastructure choice | Port/interface plus adapter |

---

## 23. Agent Review Checklist

Before finalizing a change, verify:

- What requirement or constraint drives the change?
- Which architectural boundary is affected?
- Are ownership, lifetime, threading, blocking, and error behavior explicit?
- Are dependencies directed toward stable abstractions?
- Is domain logic independent from infrastructure?
- Are inputs validated at trust boundaries?
- Are failures handled deliberately?
- Are build files, CI assumptions, deployment, logging, and configuration still correct?
- Is the design easier to test, measure, review, and rollback?

---

## 24. Minimal Rule

Prefer a change when it makes the code simpler, safer, more explicit, easier to test, easier to review, and not slower in any measured or meaningful way.
