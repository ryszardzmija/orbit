# Known limitations and audit findings

Source baseline is `e3aec7b`. A source-confirmed finding follows from the implementation. For suspected defects, the code exposes a plausible path and the runtime outcome still needs verification. The existing test suite exercises selected behavior without establishing the untested cases below. High priority findings can affect correctness or process availability. Medium priority findings narrow guarantees or complicate operation. Low priority findings have limited current impact. Milestone references indicate proposed sequencing.

## LIM-001 — Full hangup drain bypasses buffer admission

- **Disposition** — Resolved in M1.1. The former unbounded drain could append to a paused queue and then mark unread input consumed. All residual reads now share ordinary admission, with one chunk per drain step and continuation through destination writes.
- **Verification** — Both endpoint roles are covered with a saturated socket, a verified paused queue, and more bytes left in the source socket than fit in one receive. The test verifies ordered delivery and EOF after resumption. A separate test drives would-block before residual data arrives.

## LIM-002 — Only the first configured upstream is used

- **Classification and priority** — Functional limitation, medium.
- **Source and evidence** — [`parseConfig`](../src/config/config.cpp) accepts an ordered vector. [`main`](../src/main.cpp) selects `config.upstreams.front()`. [`UpstreamDialer::tryCandidatesFrom`](../src/proxy/detail/upstream_dialer.cpp) walks the addresses resolved for that endpoint.
- **Conditions and impact** — Additional entries pass syntax validation but are neither resolved nor selected. A first-endpoint resolution failure aborts startup even if another entry would resolve. A first-endpoint outage leaves later configured endpoints unused.
- **Verification** — Source-confirmed. `ConfigTest.PreservesRepeatedUpstreamOrder` verifies parsing. Runtime routing among configured entries has no integration coverage.
- **Follow-up** — The later routing milestone should define selection, eligibility and accounting for new connections while keeping established sockets pinned. Define startup handling of partial resolution failures before enabling runtime routing.

## LIM-003 — A live event can outlast its read interest

- **Disposition** — Resolved in M1.2. Every event resolves its value identity in the current registry. Directional I/O checks current eligibility, and retirement or session teardown ends dispatch of the remaining flags. Error and full hangup have explicit precedence.
- **Verification** — Controlled dispatch tests retain an old event across actual numeric fd reuse, retire a source while its peer still owns queued output, and deliver stale readable and writable flags across hangup and teardown.

## LIM-004 — Pending dials have no application deadline

- **Disposition** — M1.4 bounds the entire establishment sequence with a configurable 10-second default. A single monotonic timer serves pending dials, enabled idle deadlines and graceful shutdown. Readiness at or after expiration cannot promote an attempt. The M1-A02 correction also checks the original deadline immediately before `connect()`. A controlled loopback regression verifies cleanup when the deadline expires during fallback socket creation.
- **Remaining scope** — A client disconnect has no dedicated pending watch, so Orbit can retain both sockets for the rest of the finite budget. A slow first address can use that whole budget. Tests exercise controlled fallback, ready-versus-expired ordering and timer-driven cleanup.

## LIM-005 — Admission and application buffer limits are absent

- **M2.1 disposition** — Both directions enforce strict logical limits and account for allocated blocks. The sum of the two directional bounds limits the payload allocation attributable to one session.
- **Remaining scope** — Connection admission and aggregate payload capacity still require independent limits. Node overhead, process memory and kernel TCP queue usage are outside the directional bounds. See [LIM-013](#lim-013--resource-exhaustion-can-stop-the-reactor) for resource failure handling.

## LIM-006 — Pending dials have mixed graceful shutdown behavior

- **Disposition** — Resolved in M1.3. The first processed shutdown cancels every pending dial, invalidates its source, and stops admission. Established sessions alone drain until natural completion, a second signal or the 30-second deadline. Controlled tests exercise pending-only and mixed cases, late completions and timer escalation.

## LIM-007 — SendBufferConfig is disconnected from runtime

- **Disposition** — Resolved in M2.1. The old unused field was removed. Validated resource configuration supplies the high and low watermarks to both runtime queues.
- **Verification** — Configuration tests cover numeric ranges, relationships and precedence. A controlled reactor test verifies the configured receive limit, hysteresis, partial-send FIFO and would-block behavior.

## LIM-008 — Source identities have no wrap policy

- **Classification and priority** — Architectural risk, low.
- **Source and evidence** — [`EpollPoller::add`](../src/proxy/detail/epoll/epoll_poller.cpp) stores a `SourceId` in `epoll_event.data.u64`. [`ReactorSources::find` and `remove`](../src/proxy/detail/sources/reactor_sources.cpp) and [`ProxyReactor::closeSession`](../src/proxy/proxy.cpp) make a retired source unresolvable, protecting against ordinary numeric fd reuse. [`MonotonicIdGenerator::getNextId`](../src/proxy/detail/id/generator.h) increments a `uint64_t` without wrap handling. Source and session IDs use independent instances. A pending attempt uses its registration ID.
- **Conditions and impact** — A reused fd gets a fresh source ID during ordinary operation. If a generator traverses its 64-bit space, a subsequent ID may alias a live registration or a retained identity. The current lifetime of readiness batches and size of the ID space make wrap a low priority. Future delayed work can extend the lifetime of identities.
- **Verification** — Source-confirmed fd reuse protection and absent wrap policy. M1.2 confirmed ordinary fd reuse protection with a reused fd and a previous registration’s event.
- **Follow-up** — The generator still wraps after the full 64-bit space. M1.2 retained its existing lifetime assumption. Preserve identity checks for scheduled work.

## LIM-009 — Reactor work budgets are incomplete

- **Classification and priority** — Architectural risk, medium.
- **Source and evidence** — [`ProxyReactor::handleListener`](../src/proxy/proxy.cpp) caps accept iterations at 16, and [`EpollPoller::wait`](../src/proxy/detail/epoll/epoll_poller.h) takes up to 1,024 events. A normal [`Forwarder::forward`](../src/proxy/detail/forwarding.cpp) and [`PendingDataSender::sendPending`](../src/proxy/detail/pending_data_sender.cpp) process one 4 KiB chunk each, apart from interrupted syscall retries. M1.1 bounds residual reads to one chunk per handler. [`UpstreamDialer::tryCandidatesFrom`](../src/proxy/detail/upstream_dialer.cpp) can scan remaining candidates inside a handler until the overall connect deadline. Deadline expiration processes up to 64 keys per dispatch, with a due graceful shutdown taking priority.
- **Conditions and impact** — Repeated accepts with many failing addresses, or a large ready batch can delay other ready sessions and shutdown handling. Accept iteration count does not bound the duration of the dial work done inside each iteration. The code has no handler time budget or service-latency guarantee.
- **Verification** — Source-confirmed work bounds and unbudgeted paths. Starvation duration and throughput impact remain unmeasured.
- **Follow-up** — M0.3 measures single-connection echo latency. Measure mixed-workload latency and drain, dial and accept costs before selecting fairness budgets.

## LIM-010 — Logging is the runtime observability surface

- **Classification and priority** — Functional limitation, medium.
- **Source and evidence** — [`setUpLogger` and `LoggerGuard`](../src/logging/logger.cpp) provide configurable async stdout logging and a shutdown-time overrun warning. The reactor reports connected clients, dial and session setup, and errors. Neither [`ProxyReactor`](../src/proxy/proxy.cpp) nor [`SessionManager`](../src/proxy/detail/session/session_manager.cpp) exports connection, queue, latency, throughput or overload statistics. [The integration suite](../tests/integration/proxy_sessions_test.py) samples process CPU for an idle-HUP regression.
- **Conditions and impact** — Under sustained load or logging queue overflow, logs give limited evidence about bottlenecks, occupancy, fairness or the causes of rejection. The idle CPU test provides no throughput or tail latency measurement.
- **Verification** — Source and repository inventory. No performance claims were measured for this audit.
- **Disposition** — M0.3 adds scripts for repeatable external measurements. [Loopback benchmarks](../benchmarks/README.md)
- **Follow-up** — Define runtime connection, dial and buffer statistics.

## LIM-011 — Fault and readiness tests leave gaps

- **Classification and priority** — Testing gap, high.
- **Source and evidence** — [GoogleTest units](../tests/CMakeLists.txt) cover syntax, address formatting and queue logic. [Loopback integration](../tests/integration/proxy_sessions_test.py) covers established traffic, one upstream reset, churn, half-close, ordinary full hangup, idle CPU and signal escalation. The large-transfer test does not observe exact buffer occupancy or control partial syscall results. M1.1 adds controlled saturated hangups in both directions. M1.2 adds controlled stale batches and fd reuse. M1.3 covers pending cancellation, timer escalation and terminal write-shutdown failure. M1.4 covers address fallback, registration failure, timer cleanup and deadline races. M1.5 covers idle expiry, positive socket activity, stalled queued output and shutdown interaction. Remaining gaps include fd exhaustion, kernel DEL failure and live IPv6 forwarding.
- **M1-A01 disposition** — Resolved. The audit reproduced an expired idle session surviving when a later operation in the same notification refreshed its deadline. Controlled-clock tests verify separate I/O checks for combined readiness and deferred hangup reads, and test activity at the expiry boundary.
- **Conditions and impact** — Regressions in rare event combinations and failures can pass the existing suite. The harness still uses deliberate delays for reset, slow-reader and idle-CPU scenarios. Orbit's CLI rejects port zero, so the bind-after-release window for its listener still exists. It can cause a startup failure if another process takes the port.
- **Verification** — Confirmed by test inventory. Churn and transfer cases are useful indirect coverage with limited control over the targeted state.
- **Follow-up** — Select deterministic fault and timeout coverage when modifying the other paths. See LIM-016 for the M0.2 harness fixes.

## LIM-012 — Configuration and README overstated current guarantees

- **Classification and priority** — Documentation discrepancy, low.
- **Source and evidence** — At the audit baseline, [`docs/config.md`](config.md) described load balancing over multiple upstreams. [`main`](../src/main.cpp) selects a single configured endpoint. The [README](../README.md) promised bounded queueing through backpressure and broadly described failure isolation. [`Forwarder::drain`](../src/proxy/detail/forwarding.cpp), [`SendBuffer::write`](../src/proxy/detail/buffer/send_buffer.cpp) and [`ProxyReactor::handleDialError`](../src/proxy/proxy.cpp) expose the exceptions.
- **Conditions and impact** — Operators could assume additional endpoints participate in routing, watermarks cap memory, or every dial failure affects just one client.
- **Verification** — Source-confirmed discrepancy at `e3aec7b`. The documentation changes in M0.1 resolve the discrepancy. The underlying limitations remain tracked in LIM-001, LIM-002, LIM-005 and LIM-013.
- **Follow-up** — Revise the operator documentation with future forwarding or routing changes.

## LIM-013 — Resource exhaustion can stop the reactor

- **Classification and priority** — Architectural risk, high.
- **Source and evidence** — `EMFILE` and `ENFILE` remain process-fatal on accept or upstream socket creation. Connect and SO_ERROR failures are local after M1.4. A failed pending ADD is also local. The fd and heap paths have no admission fallback or overload recovery mechanism.
- **Conditions and impact** — Exhausting available descriptors or failing a heap allocation can still end the process and close independent sessions.
- **Verification** — Source-confirmed status routing. Resource exhaustion and allocation failure were not induced in validation.
- **Follow-up** — Establish overload behavior before expanding upstream fan-out. Review the terminal dial error policy by syscall and errno, and add tests for isolation and cleanup under recoverable resource pressure.

## LIM-014 — Failed write shutdown may leave a session waiting

- **Disposition** — Resolved in M1.3. Actual input EOF and an empty destination queue suffice to propagate FIN. EINTR retries the syscall and a terminal failure closes the pair locally. Tests drive EOF without RDHUP and force ENOTCONN using an unconnected socket.

## LIM-015 — Invalid environment settings can revert to defaults

- **Classification and priority** — Functional limitation, medium.
- **Source and evidence** — [`parseConfig`](../src/config/config.cpp) attaches CLI11 validators to listen port and log level. In the [pinned CLI11 v2.6.2 implementation](https://github.com/CLIUtils/CLI11/blob/v2.6.2/include/CLI/impl/App_inl.hpp), `App::_process_env` adds a nonempty environment value only when it passes the option's validators. Upstream endpoint syntax is checked separately by Orbit after parsing.
- **Conditions and impact** — With a setting absent from the CLI, an out-of-range `ORBIT_LISTEN_PORT` or invalid `ORBIT_LOG_LEVEL` can be discarded silently, leaving port 8080 or logging at `info`. An invalid explicit CLI value produces a diagnostic. An operator can see an unexpected listener port or log level during startup.
- **Verification** — Source-confirmed. A Debug CLI smoke check with `ORBIT_LISTEN_PORT=0` and an invalid log level reached listener setup on port 8080. The same port supplied on the command line was rejected during parsing.
- **Follow-up** — Decide whether to reject invalid environment values in a configuration task. Preserve explicit CLI precedence and add coverage for both input sources if changing validation.

## LIM-016 — Integration cleanup could hide child failures or block

- **Classification and priority** — Testing infrastructure defect, high, resolved in M0.2.
- **Source and evidence** — The pre-M0.2 [harness](../tests/integration/proxy_sessions_test.py) waited on an Orbit child with an unread output pipe, ignored exit status in teardown and used an unbounded wait after killing the child. A startup or upstream-accept failure could also lose ownership of a partially initialized fixture.
- **Disposition** — The harness now registers cleanup before startup, polls the existing readiness log and checks for early exits. It captures combined output in a temporary file and checks status on expected shutdown and cleanup. Regression cases exercise a zero-status early exit, a failing shutdown that writes 2 MiB of diagnostics, and bounded killing and reaping of an unresponsive child.
- **Follow-up** — Preserve failure propagation and bounded resource ownership when adding integration cases. The remaining coverage and port limitations are listed in LIM-011.

## LIM-017 — GNU UBSan and Abseil are incompatible

- **Classification and priority** — Toolchain compatibility limitation, medium.
- **Source and evidence** — GCC 15.2.0 with `-fsanitize=address,undefined` reports a non-constant function-pointer comparison in the pinned Abseil hash policy header when compiling Orbit's proxy. [Abseil's upstream report](https://github.com/abseil/abseil-cpp/issues/1634).
- **Disposition** — The sanitizer preset uses Clang 22. With mixed instrumentation, Orbit aborted during startup in Abseil's `CommonFields::set_capacity` because ASan changes the hash-table layout. The build now instruments Abseil in the same configuration as Orbit.
- **Follow-up** — Recheck compiler and Abseil compatibility when changing either toolchain. Keep Abseil's compiled code and the Orbit code that uses its headers on matching sanitizer settings.
