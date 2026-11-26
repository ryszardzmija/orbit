# Known limitations

See the [engineering invariants](invariants.md) for the scope of Orbit's guarantees.

## LIM-002 — Only the first configured upstream is used

Orbit parses all configured endpoints and resolves the first at startup. It can try the
resolved addresses of that endpoint when establishing a connection. Later endpoints
remain unused, including after a resolution or connection failure. Orbit resolves
addresses only at startup and does no active health checking. Established streams stay
on their original socket pairs. See [upstream selection](architecture.md#upstream-selection-and-dialing).

## LIM-004 — A disconnected client can occupy a pending slot

Orbit watches the upstream socket during connection establishment. A client that
disconnects during that phase can keep its admission and sockets until the dial
completes, fails, is cancelled, or reaches the connect deadline. A slow first address
can consume the entire deadline before another address is tried.

## LIM-008 — Source and session IDs can wrap

The source and session ID generators each use a 64-bit counter without a wrap policy.
New source IDs protect against stale events after ordinary fd reuse. A wrap can alias
a live registration, session or retained identity. See [epoll registration and dispatch](architecture.md#epoll-registration-and-dispatch).

## LIM-009 — Work budgets bound operation counts

The [per-turn limits](architecture.md#work-per-reactor-turn) give ready connections,
deadlines and shutdown requests opportunities to run. Wall-clock service latency
depends on readiness volume, scheduling, syscall cost, hash table maintenance,
allocation and teardown. Startup name resolution can block. Maximum service latency
under mixed traffic and the cost of dial and shutdown bursts remain unmeasured.

## LIM-010 — Runtime observability is limited to logs

Orbit writes asynchronous logs to stdout and can report queue overruns at shutdown.
The queue can drop older messages under load. Runtime occupancy, throughput, latency
and overload metrics are unavailable. The [loopback benchmarks](../benchmarks/README.md)
provide reproducible external measurements.

## LIM-011 — Fault and readiness tests leave gaps

Controlled tests cover selected readiness, I/O and resource failures. Loopback TCP
tests exercise IPv4 and include a process descriptor-limit regression. Coverage gaps
include live IPv6 forwarding, system-wide resource pressure and unexpected kernel
`EPOLL_CTL_DEL` errors. Some reset, slow-reader and idle-CPU scenarios still rely on
timed delays. The integration harness releases a probe port before Orbit binds it, so
a concurrent process can claim the port and cause startup to fail. See [test diagnostics](development.md#test-deadlines-and-diagnostics).

## LIM-013 — Resource bounds and recovery have defined scope

Directional watermarks bound logical queued payload, the unsent bytes in a queue.
Allocated payload capacity counts each live block at its full size. The global budget
charges that capacity together with outstanding reservations for blocks a receive may
need. Queue and waiter metadata, allocator overhead, scratch space, other process memory
and kernel TCP memory fall outside the budget. Process RSS includes other resident
pages and can remain elevated after blocks are freed.

Each admission owns at most two connection descriptors. The listener, epoll instance,
timer, signal descriptor and other open files also count toward process and system
descriptor limits. Persistent descriptor pressure keeps admission paused between retries.

Orbit recovers from the syscall errors and allocation failures listed in
[failure handling](architecture.md#failure-handling). Abseil flat hash table mutations
lack exception safety. Allocation failures there, unsupported heap failures and reactor
infrastructure or integrity failures can stop the process. Linux can also terminate it
under system memory pressure.

## LIM-015 — Invalid environment settings can revert to defaults

CLI11 skips values of `ORBIT_LISTEN_PORT` and `ORBIT_LOG_LEVEL` that fail their
validators. When the corresponding CLI option is absent, Orbit can therefore start
on the default port or at the default log level without a diagnostic. Explicit
invalid CLI values fail validation. See [configuration](config.md).

## LIM-017 — GNU UBSan and Abseil are incompatible

GCC 15.2.0 with the combined ASan/UBSan flags rejects a function-pointer comparison
in the pinned Abseil hash policy header. The sanitizer preset uses Clang 22 and
instruments Abseil and Orbit together because ASan changes the hash-table layout.
[Sanitizer reports](development.md#sanitizer-reports), [Abseil issue](https://github.com/abseil/abseil-cpp/issues/1634).

## LIM-018 — Forwarding progress depends on payload capacity becoming available

If slow destinations hold every charged block, unrelated directions can wait even
when their destinations are writable or their sources have reached EOF. A receive
needs capacity in case it returns payload. Existing tail space remains usable.
Consumption, hangup and teardown release capacity, and eligible waiters get FIFO
read opportunities as blocks become available. Enabled idle expiry can also release
capacity. An application that waits for traffic in a paused direction before reading
elsewhere can remain stalled until a peer makes progress or a session ends.
