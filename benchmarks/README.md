# Orbit loopback benchmarks

Run from the repository root on Linux with Python 3.10 or newer, `iperf3` with
`--json-stream` support, IPv4 loopback TCP and access to the Orbit child's `/proc` entries.
`pidstat` and `ss` are useful for independent spot checks. The scripts sample `/proc`
directly. Check the tools and `/proc` access before running the benchmarks.

```sh
command -v iperf3 pidstat ss python3
iperf3 --help | grep -- --json-stream
test -r /proc/self/stat && test -r /proc/self/status && test -d /proc/self/fd
```

Build the optimized, non-sanitized binary with the Release preset. For build dependencies
and offline configuration, see the [development guide](../docs/development.md).

```sh
cmake --preset release
cmake --build --preset release
python3 -B benchmarks/throughput.py
python3 -B benchmarks/latency.py
ORBIT_MAX_CONNECTIONS=1000 python3 -B benchmarks/idle_connections.py
```

Run the benchmarks sequentially on a quiet host. All endpoints bind to `127.0.0.1`.
The scripts launch their own Orbit with one upstream and `info` logging. Override
the default `build/release/orbit` with `--orbit PATH`. Use `--help` for other options.
Orbit and iPerf3 require nonzero listen ports, so another process could claim a port
between the benchmark's port selection and bind. Rerun on a bind failure.

For the idle benchmark, set `ORBIT_MAX_CONNECTIONS` to at least the largest requested
session count. The default Orbit limit is 256. If setting it below 64, also set
`ORBIT_MAX_PENDING_CONNECTIONS` at or below that limit. The idle benchmark opens
one connection at a time. See the [configuration reference](../docs/config.md).

## Metrics

- **Throughput.** Three trials for each combination of direct or proxied traffic and
  one or four client-to-backend data streams. Each trial has a dedicated one-shot
  iPerf3 server. The default warm-up is two seconds (`--warmup`), followed by ten
  measured seconds (`--seconds`). The primary metric is aggregate receiver throughput
  in decimal Gbit/s from iPerf3's `sum_received`. The summary is the median of
  successful trials, with the successful and requested trial counts shown.
- **CPU and RSS under load.** CPU is the change in Orbit's user plus system CPU time
  divided by elapsed wall time. 100% represents one logical CPU. The sampler times the
  approximate measurement window from iPerf3's client start event plus the configured
  warm-up. It samples Orbit's RSS about every 200 ms and reports the peak sampled value
  in KiB. The output includes the sampling window and iPerf3's receiver duration.
- **Latency.** One persistent connection per route. Each sends one 64-byte payload at a
  time and verifies its echo. Defaults are 1,000 warm-up and 20,000 timed exchanges.
  Client and Python backend enable `TCP_NODELAY`. RTT uses a monotonic clock and
  includes the Python client and echo server work. Values are in microseconds. p50
  is the median and p95 and p99 use nearest rank. A failed exchange ends that trial
  and the output includes the number of successes, failures and skipped samples.
- **Idle resources.** A single Orbit process holds increasing numbers of verified
  sessions (`--counts`, default 100, 500 and 1,000). A one-byte request and reply
  confirm each upstream connection before leaving the pair idle. The inherited soft
  file-descriptor limit can reduce or skip a target. The initial and post-close
  measurements show whether the descriptor count returns to its initial value.
  `VmRSS` measures resident process pages in KiB, including allocator and other process
  memory. Application-managed buffer bytes and kernel socket memory need separate
  measurements. The allocator can retain resident pages after connection closure.

The scripts report failed workloads and exit nonzero. Run the focused benchmark
checks with `python3 -B -m unittest discover -s benchmarks`.
