# Known limitations and audit findings

Source baseline is `e3aec7b`. A source-confirmed finding follows from the implementation. For suspected defects, the code exposes a plausible path and the runtime outcome still needs verification. The existing test suite exercises selected behavior without establishing the untested cases below. High priority findings can affect correctness or process availability. Medium priority findings narrow guarantees or complicate operation. Low priority findings have limited current impact. Milestone references indicate proposed sequencing.

## LIM-001 — Full hangup drain bypasses buffer admission

- **Classification and priority** — Confirmed defect, high.
- **Source and evidence** — [`SessionManager::handleHangup`](../src/proxy/detail/session/session_manager.cpp) calls [`Forwarder::drain`](../src/proxy/detail/forwarding.cpp) whenever the other endpoint is eligible for writes by session state. The drain loops over receives and appends every chunk without checking admission. [`SendBuffer::write`](../src/proxy/detail/buffer/send_buffer.cpp) asserts `Accepting`, appends the entire chunk and may transition to `Paused`. Linux can report HUP even with `EPOLLIN` disabled. [`epoll_ctl(2)`](https://man7.org/linux/man-pages/man2/epoll_ctl.2.html).
- **Conditions and impact** — The hung-up endpoint has residual input and the surviving destination is already paused, or the drain crosses the high watermark with additional input still available. The next append to a paused queue fails the assertion in Debug, taking down the process. With assertions disabled, additional appends can extend occupancy beyond the ordinary one-chunk watermark overshoot.
- **Verification** — Source-confirmed contract violation. The existing client and backend full-hangup tests exercise payload delivery and EOF without establishing saturation at hangup. The assertion and release-build impact were not reproduced during this audit.
- **Follow-up** — In M1.1, define how the reactor defers reads of residual data while preserving byte order, EOF propagation and progress. A change that simply exits the drain still reaches the caller's unconditional `done_reading` and retirement. Leaving a hung-up fd registered without a progression policy can repeatedly report HUP. Add a deterministic regression that establishes a paused destination during hangup and checks bounded queue admission, payload delivery, EOF and idle reactor behavior after the destination resumes. Cover both directions.

## LIM-002 — Only the first configured upstream is used

- **Classification and priority** — Functional limitation, medium.
- **Source and evidence** — [`parseConfig`](../src/config/config.cpp) accepts an ordered vector. [`main`](../src/main.cpp) selects `config.upstreams.front()`. [`UpstreamDialer::tryCandidatesFrom`](../src/proxy/detail/upstream_dialer.cpp) walks the addresses resolved for that endpoint.
- **Conditions and impact** — Additional entries pass syntax validation but are neither resolved nor selected. A first-endpoint resolution failure aborts startup even if another entry would resolve. A first-endpoint outage leaves later configured endpoints unused.
- **Verification** — Source-confirmed. `ConfigTest.PreservesRepeatedUpstreamOrder` verifies parsing. Runtime routing among configured entries has no integration coverage.
- **Follow-up** — M1 should define selection, eligibility and accounting for new connections while keeping established sockets pinned. Define startup handling of partial resolution failures before enabling runtime routing.

## LIM-003 — A live event can outlast its read interest

- **Classification and priority** — Suspected defect, high.
- **Source and evidence** — [`ProxyReactor::start` and `handleEndpoint`](../src/proxy/proxy.cpp) look up the source and session but use the copied event mask for dispatch. [`SessionManager::handleHangup` and `getInterests`](../src/proxy/detail/session/session_manager.cpp) can mark one endpoint write-closed, retire it from epoll and remove the other endpoint's readable interest. `handleReadable` calls [`Forwarder::forward`](../src/proxy/detail/forwarding.cpp) without rechecking the destination's write state.
- **Conditions and impact** — One endpoint hangs up and the session remains alive to flush queued data to the other endpoint. A following event in the same batch for that still-registered endpoint may include an older readable flag. The read can target a retired peer, queue bytes for an undeliverable direction or fail a direct send and close the session before all surviving output flushes. The fd is still owned by the same session.
- **Verification** — Plausible source path. The actual batch ordering and downstream outcome have no targeted test or runtime reproduction.
- **Follow-up** — In M1.2, check current direction eligibility at dispatch, with explicit handling for error and hangup flags. Use a controlled batch regression to verify that an outdated readable flag cannot interrupt the surviving direction.

## LIM-004 — Pending dials have no application deadline

- **Classification and priority** — Functional limitation, high.
- **Source and evidence** — [`PendingConnection`](../src/proxy/detail/pending_connection.h) has an attempted fd and address index but no deadline. [`registerPendingConnection`](../src/proxy/proxy.cpp) registers only the attempted upstream for readiness. The timer is used for process shutdown. [`advanceDialAttempt`](../src/proxy/detail/upstream_dialer.cpp) runs only when that source dispatches.
- **Conditions and impact** — A slow or unreachable destination can occupy an accepted fd and a dial fd until Linux completes or fails the connect, or Orbit exits. A slow first candidate delays later addresses. An early client disconnect has no dedicated pending-side observation or cancellation.
- **Verification** — Source-confirmed absence of application deadlines and client-side watches. OS timing depends on system and network behavior and was not measured.
- **Follow-up** — Define an overall dial budget and any per-attempt budget, cancellation and ownership semantics before M1 relies on address or upstream retry. Account for time already spent when advancing candidates.

## LIM-005 — Admission and application buffer limits are absent

- **Classification and priority** — Functional limitation, high.
- **Source and evidence** — [`ProxyReactor::handleListener` and `registerPendingConnection`](../src/proxy/proxy.cpp) and [`SessionManager::add`](../src/proxy/detail/session/session_manager.cpp) have no admission cap. [`SendBuffer::write`](../src/proxy/detail/buffer/send_buffer.cpp) uses flow-control watermarks and allocates blocks on demand. There is no per-direction hard cap, strict per-session allocation budget or global accounting across queues. The requested listen backlog of 64 and the batch size of 16 regulate distinct parts of acceptance.
- **Conditions and impact** — Concurrent sessions, accumulated in-progress dials or slow receivers grow fd demand, userspace allocations and kernel TCP queue usage. Logical queue length omits block slack, node overhead and allocator retention. OS fd and memory limits can eventually constrain growth, with exhaustion behavior described in [LIM-013](#lim-013--resource-exhaustion-can-stop-the-reactor).
- **Verification** — Source-confirmed. `SendBufferTest.WriteCanExceedHighWatermarkInSingleCall` exercises logical overshoot. No aggregate resource bound is established.
- **Follow-up** — Before M1 expands admission paths, specify established and pending caps, hard buffer admission rules, global accounting, and behavior during overload. Measure application allocations, process memory, and kernel socket memory independently.

## LIM-006 — Pending dials have mixed graceful shutdown behavior

- **Classification and priority** — Functional limitation, medium.
- **Source and evidence** — [`handleShutdownRequest`, `shouldStop`, `handlePendingConnection` and `forceCloseAllSessions`](../src/proxy/proxy.cpp) remove the listener and count only sessions in `SessionManager`. Pending completion and candidate fallback have no shutdown-state guard. Hard stop explicitly removes established sessions, and pending fds survive until reactor destruction.
- **Conditions and impact** — While an established session keeps graceful processing active, a pending dial can become established or advance candidates. When the established map becomes empty, the reactor can exit with pending dials still in the map, discarding them through destruction. Whether an already-ready dial is handled can depend on event order relative to the shutdown request or last session close.
- **Verification** — Source-confirmed. The existing graceful and second-signal tests use established sessions.
- **Follow-up** — In a shutdown correctness task, define and test whether graceful shutdown cancels, finishes, or includes pending attempts in the drain condition. Align promotion, lookup removal and cancellation before implementing scheduled dial deadlines.

## LIM-007 — SendBufferConfig is disconnected from runtime

- **Classification and priority** — Functional limitation, low.
- **Source and evidence** — [`Config::send_buffer` and `SendBufferConfig::max_buffer_size`](../src/config/config.h) declare a 64 KiB default. [`parseConfig`](../src/config/config.cpp) exposes no buffer option, [`main`](../src/main.cpp) does not pass that field to the reactor, and [`ProxyReactor`](../src/proxy/proxy.h) sets hardcoded block and watermark values. The factory receives the latter values through `SessionManagerOptions`.
- **Conditions and impact** — Changing the config structure's default or setting its field in memory has no runtime effect. The field name suggests a strict capacity where the live settings use hysteresis.
- **Verification** — Source-confirmed. Buffer unit tests instantiate `SendBuffer` directly and do not validate runtime config wiring.
- **Follow-up** — Resolve the unused field when defining the memory policy. If introducing operator settings, validate relationships and document whether each controls logical occupancy, hard admission, or allocation accounting.

## LIM-008 — Source identities have no wrap policy

- **Classification and priority** — Architectural risk, low.
- **Source and evidence** — [`EpollPoller::add`](../src/proxy/detail/epoll/epoll_poller.cpp) stores a `SourceId` in `epoll_event.data.u64`. [`ReactorSources::find` and `remove`](../src/proxy/detail/sources/reactor_sources.cpp) and [`ProxyReactor::closeSession`](../src/proxy/proxy.cpp) make a retired source unresolvable, protecting against ordinary numeric fd reuse. [`MonotonicIdGenerator::getNextId`](../src/proxy/detail/id/generator.h) increments a `uint64_t` without wrap handling. Source, session and pending IDs use independent instances.
- **Conditions and impact** — A reused fd gets a fresh source ID during ordinary operation. If a generator traverses its 64-bit space, a subsequent ID may alias a live registration or a retained identity. The current lifetime of readiness batches and size of the ID space make wrap a low priority. Future delayed work can extend the lifetime of identities.
- **Verification** — Source-confirmed fd reuse protection and absent wrap policy. No ordinary fd reuse defect was established. The churn integration test gives indirect coverage and does not control fd reuse or inject stale events.
- **Follow-up** — In M1.2, test source invalidation and old events around teardown. Preserve ID-lifetime rules when adding timers, cancellation and scheduled work. Document generator exhaustion when extending identity APIs.

## LIM-009 — Reactor work budgets are incomplete

- **Classification and priority** — Architectural risk, medium.
- **Source and evidence** — [`ProxyReactor::handleListener`](../src/proxy/proxy.cpp) caps accept iterations at 16, and [`EpollPoller::wait`](../src/proxy/detail/epoll/epoll_poller.h) takes up to 1,024 events. A normal [`Forwarder::forward`](../src/proxy/detail/forwarding.cpp) and [`PendingDataSender::sendPending`](../src/proxy/detail/pending_data_sender.cpp) process one 4 KiB chunk each, apart from interrupted syscall retries. The hangup drain loops until would-block, EOF or error. [`UpstreamDialer::tryCandidatesFrom`](../src/proxy/detail/upstream_dialer.cpp) can scan all remaining candidates inside a handler.
- **Conditions and impact** — Residual hangup input, repeated accepts with many failing addresses, or a large ready batch can delay other ready sessions and shutdown handling. Accept iteration count does not bound the duration of the dial work done inside each iteration. The code has no handler time budget or service-latency guarantee.
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
- **Source and evidence** — [GoogleTest units](../tests/CMakeLists.txt) cover syntax, address formatting and queue logic. [Loopback integration](../tests/integration/proxy_sessions_test.py) covers established traffic, one upstream reset, churn, half-close, ordinary full hangup, idle CPU and signal escalation. The large-transfer test does not observe exact buffer occupancy or control partial syscall results. Tests do not drive a saturated hangup, a stale ready batch, pending dial failures and address fallback deterministically, pending shutdown, timer-only escalation, fd exhaustion, registration rollback faults or live IPv6 forwarding.
- **Conditions and impact** — Regressions in rare event combinations and failures can pass the existing suite. The harness still uses deliberate delays for reset, slow-reader and idle-CPU scenarios. Orbit's CLI rejects port zero, so the bind-after-release window for its listener still exists. It can cause a startup failure if another process takes the port.
- **Verification** — Confirmed by test inventory. Churn and transfer cases are useful indirect coverage with limited control over the targeted state.
- **Follow-up** — Add saturated hangup coverage in M1.1 and controlled stale-event coverage in M1.2. Select deterministic fault and timeout coverage when modifying the other paths. See LIM-016 for the M0.2 harness fixes.

## LIM-012 — Configuration and README overstated current guarantees

- **Classification and priority** — Documentation discrepancy, low.
- **Source and evidence** — At the audit baseline, [`docs/config.md`](config.md) described load balancing over multiple upstreams. [`main`](../src/main.cpp) selects a single configured endpoint. The [README](../README.md) promised bounded queueing through backpressure and broadly described failure isolation. [`Forwarder::drain`](../src/proxy/detail/forwarding.cpp), [`SendBuffer::write`](../src/proxy/detail/buffer/send_buffer.cpp) and [`ProxyReactor::handleDialError`](../src/proxy/proxy.cpp) expose the exceptions.
- **Conditions and impact** — Operators could assume additional endpoints participate in routing, watermarks cap memory, or every dial failure affects just one client.
- **Verification** — Source-confirmed discrepancy at `e3aec7b`. The documentation changes in M0.1 resolve the discrepancy. The underlying limitations remain tracked in LIM-001, LIM-002, LIM-005 and LIM-013.
- **Follow-up** — Revise the operator documentation with future forwarding or routing changes.

## LIM-013 — Resource exhaustion can stop the reactor

- **Classification and priority** — Architectural risk, high.
- **Source and evidence** — [`isRecoverableAcceptError` and `isFatalDialError`](../src/proxy/proxy.cpp) classify `EMFILE` and `ENFILE` as fatal via their default paths. The terminal dial classification checks every accumulated errno and discards the socket/connect distinction, even though [`tryCandidatesFrom`](../src/proxy/detail/upstream_dialer.cpp) uses that distinction to stop on socket creation failure. A final error list with an errno such as `EADDRNOTAVAIL` is also fatal. The fd and heap paths have no admission fallback or overload recovery mechanism.
- **Conditions and impact** — Hitting process-wide or system-wide fd limits on accept or dial can end the reactor and close unrelated streams. Exhaustion of local addresses or outbound ports can also be process-fatal if a dial ends with the corresponding error. Allocation failures from containers and buffers have no explicit catch path.
- **Verification** — Source-confirmed status routing. Resource exhaustion and allocation failure were not induced in validation.
- **Follow-up** — Establish overload behavior before expanding upstream fan-out. Review the terminal dial error policy by syscall and errno, and add tests for isolation and cleanup under recoverable resource pressure.

## LIM-014 — Failed write shutdown may leave a session waiting

- **Classification and priority** — Suspected defect, medium.
- **Source and evidence** — [`halfCloseIfReady`, `shouldClose` and endpoint handlers](../src/proxy/detail/session/session_manager.cpp) leave `half_close_sent` unset on `shutdown(SHUT_WR)` failure and return a keep-open result with an error. Retry depends on a later relevant readiness event. The normal close predicate requires both successful write shutdowns, and there is no per-session timer.
- **Conditions and impact** — If shutdown fails after input is consumed and the output queue is empty, the half-close can remain unpropagated until another relevant event, error, hangup or process shutdown. The event sequence matters, since a later HUP or ERR can still close the session.
- **Verification** — The failed-syscall behavior is source-confirmed. A persistent wait after failure is plausible and unverified.
- **Follow-up** — Investigate in a shutdown correctness task with controlled failure coverage. Define the retry or close policy and verify pending-data and session cleanup behavior.

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
