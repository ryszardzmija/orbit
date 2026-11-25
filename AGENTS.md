# Orbit engineering instructions

## Project vision

Build a resource-efficient, fault-tolerant Layer 4 TCP load balancer for Linux in modern C++. Correct TCP behavior, predictable resource use, operational resilience, maintainability, and measured performance take precedence over feature breadth. The current runtime is a transparent reverse proxy for the first configured upstream.

## Repository navigation

- Read [architecture](docs/architecture.md), [engineering invariants](docs/invariants.md) and [known limitations](docs/known-limitations.md) before changing the data plane. See [configuration](docs/config.md) for the operator interface.
- Start at [`src/main.cpp`](src/main.cpp). The reactor and its helpers live in [`src/proxy/`](src/proxy/), with sockets and resolution in [`src/net/`](src/net/). Configuration, shared ownership types, logging and signal masking live in [`src/config/`](src/config/), [`src/common/`](src/common/), [`src/logging/`](src/logging/) and [`src/signal/`](src/signal/).
- GoogleTest suites live under [`tests/config/`](tests/config/), [`tests/net/`](tests/net/) and [`tests/proxy/`](tests/proxy/). The Python loopback harness lives in [`tests/integration/`](tests/integration/).
- For instructions on running Release loopback benchmarks, see [Benchmarks](benchmarks/README.md).
- Build rules live in [`CMakeLists.txt`](CMakeLists.txt) and module CMake files. See [development](docs/development.md) for the CI matrix, toolchains and failure diagnostics. Follow the repository's [`.clang-format`](.clang-format) and existing C++ conventions.

## Build and validation

Use the `debug`, `release` and `sanitizers` presets for the CI configurations. Build before running CTest. Run applicable configurations, including the combined ASan/UBSan suite for runtime changes.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug -L '^unit$'
ctest --preset debug -L '^integration$'
ctest --preset debug
git diff --check
```

Substitute `release` or `sanitizers` in all three command types to validate those configurations. The loopback suite needs access to its Orbit child's `/proc/<pid>/stat` and `/proc/<pid>/status`. The sanitizer suite requires an environment that permits LeakSanitizer's process inspection.

## Engineering principles

- Preserve TCP byte-stream transparency and independent forwarding directions. Keep data-plane sockets nonblocking and preserve explicit, move-only RAII ownership for descriptors.
- Respect lifecycle, readiness and backpressure invariants. Bound work per handler and account for pending as well as established connections. Define resource limits and overload behavior explicitly, and distinguish logical occupancy from allocated and kernel memory.
- Preserve failure isolation between independent sessions. Treat documented gaps as engineering work when the assigned scope includes them.
- Prefer straightforward implementations and existing abstractions. Justify new dependencies and base performance work on reproducible measurements. Preserve valid configuration semantics unless the task requires changing them.

## Architectural boundaries

Keep the single-threaded data-plane reactor unless the assigned task explicitly authorizes evaluating or changing it. Additional concurrency models, application-layer routing, TLS termination, general-purpose HTTP infrastructure, database-specific routing, distributed control planes, and third-party frameworks outside project needs require an explicit architectural decision.

## Task scope

Implement only the assigned task. Inspect the relevant sources, invariants and tests first. Reuse existing abstractions where appropriate and avoid unrelated refactors, formatting, dependency upgrades or work from later milestones. Surface conflicts with invariants for resolution. Proceed only with lawful and ethical actions, and explain any concern that prevents proceeding.

## Testing requirements

Add regression tests for verified defects and tests for new behavior and its relevant failure paths. Prefer observable synchronization and bound socket operations, polling and subprocess waits. Register cleanup as resources are acquired and preserve child exit status and output on failure. Preserve the intent and coverage of existing tests. Report the validation commands attempted, which checks passed or failed, and why any applicable check could not run.

## Documentation requirements

Update the relevant docs when behavior, ownership, component responsibilities, the session lifecycle, invariants, configuration, or limitations change. Mark resolved findings with their disposition when the history remains useful.

## Definition of done

Meet the task's acceptance criteria. Bring implementation and documentation into agreement. Preserve invariants or record the authorized revision. Run applicable tests and accurately disclose failures, known limitations and unresolved risks. Review the diff for scope.

## Completion report

Report what changed, the files involved, architectural decisions, tests and checks run, acceptance criteria verification, known limitations, unresolved issues, and checks you could not perform.
