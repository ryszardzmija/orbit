# Configuration

Orbit accepts options on the command line or through the corresponding environment variables. An explicit command-line value takes precedence over the corresponding environment value. Orbit validates the selected timeouts, resource settings and upstream list after applying that precedence.

An empty resource environment variable fails validation when its CLI option is absent. CLI11 ignores empty environment values for other options. It also skips values of `ORBIT_LISTEN_PORT` and `ORBIT_LOG_LEVEL` that fail their validators, leaving the defaults. Use `--help` to list options and `--version` to print the version.

| Option | Environment variable | Default and validation |
| --- | --- | --- |
| `--listen-host` | `ORBIT_LISTEN_HOST` | `0.0.0.0`. Use an IPv4 or IPv6 address, or a host name that resolves to a bindable address. Use `::` for the IPv6 wildcard. Orbit binds the first listener candidate it can initialize successfully. |
| `--listen-port` | `ORBIT_LISTEN_PORT` | `8080`. Accepted range is 1 through 65535. |
| `--upstream` | `ORBIT_UPSTREAMS` | Required. Supply at least one endpoint. Repeat the option or use a comma-separated list. |
| `--connect-timeout-ms` | `ORBIT_CONNECT_TIMEOUT_MS` | `10000` (10 seconds). Decimal milliseconds from 1 through 2147483647. Invalid CLI or environment values fail startup. |
| `--idle-timeout-ms` | `ORBIT_IDLE_TIMEOUT_MS` | `0` (disabled). Decimal milliseconds from 0 through 2147483647. Invalid CLI or environment values fail startup. |
| `--buffer-high-watermark-bytes` | `ORBIT_BUFFER_HIGH_WATERMARK_BYTES` | `262144` (256 KiB). Hard maximum logical queued payload in each direction. Decimal bytes from 4096 through 16777216. |
| `--buffer-low-watermark-bytes` | `ORBIT_BUFFER_LOW_WATERMARK_BYTES` | `196608` (192 KiB). A paused direction resumes at or below this occupancy. Decimal bytes from 0 through one less than the selected high watermark. |
| `--max-connections` | `ORBIT_MAX_CONNECTIONS` | `256`. Total admitted connections, including upstream establishment and established sessions. Decimal count from 1 through 65535. |
| `--max-pending-connections` | `ORBIT_MAX_PENDING_CONNECTIONS` | `64`. Admitted connections undergoing upstream establishment. Decimal count from 1 through the selected total limit. |
| `--buffer-budget-bytes` | `ORBIT_BUFFER_BUDGET_BYTES` | `67108864` (64 MiB). Aggregate forwarding block capacity, including outstanding reservations. Decimal bytes from 4096 through 1073741824 in multiples of 4096. |
| `--log-level` | `ORBIT_LOG_LEVEL` | `info`. Accepted names are `trace`, `debug`, `info`, `warn`, `error`, `critical`, and `off`, case-insensitively. CLI11 also accepts their numeric values 0 through 6 in that order. |

Use `HOST:PORT` for DNS names and IPv4 addresses and `[ADDRESS]:PORT` for IPv6. A host must be nonempty. An upstream port must consist of decimal digits with a value from 1 through 65535. Parsing checks endpoint syntax. Resolution and binding take place during startup.

```sh
./build/debug/orbit --upstream 127.0.0.1:7000 --upstream '[2001:db8::1]:7000'
ORBIT_UPSTREAMS='127.0.0.1:7000,[2001:db8::1]:7000' ./build/debug/orbit
```

All upstream entries are parsed in input order. Only the first configured endpoint is resolved and used at runtime. Each new connection can try later resolved addresses for that endpoint if earlier addresses fail. If resolution of the first endpoint fails, startup fails.

The connect timeout covers the full upstream establishment sequence, including fallback and deferred work. Once the reactor observes expiry, it closes the client and any pending upstream socket. A pending client can half-close its request and wait for a response if establishment succeeds in time.

When enabled, the idle interval starts on session establishment. A positive socket receive or send on either endpoint restarts it when Orbit observes the transfer strictly before expiry. Expiration closes both sockets and discards queued output, including during half-close, backpressure or graceful shutdown. See [reactor deadlines](architecture.md#reactor-deadlines) for timing details.

Graceful shutdown has a separate fixed 30-second deadline starting at the first processed shutdown request. An established session can reach its own idle deadline earlier. Processing a second shutdown request forces closure.

When the high watermark is 192 KiB or less, set the low watermark below it. The reactor allocates payload in 4 KiB blocks. See [forwarding and backpressure](architecture.md#forwarding-and-backpressure) for the bound on allocated capacity.

The defaults allow up to 512 connection-owned descriptors and limit how many clients can tie up that capacity in upstream establishment. Size the limits for the deployment's process and system descriptor budgets and allow headroom for non-connection descriptors. If lowering the total below 64, also lower the pending limit.

At either limit Orbit pauses acceptance. Waiting clients occupy the kernel listen backlog, so a client-side TCP connect can succeed before Orbit accepts it or opens its upstream connection. If the backlog fills, connection establishment can stall or fail under the host's TCP policy.

Descriptor and other supported resource failures and accept permission denials pause acceptance. Orbit reevaluates admission after a 100 ms cooldown using the shared timer. Upstream completion queries can still run during cooldown, while candidates that need a fresh socket wait. A connection that encounters a resource failure during establishment can be closed. See [failure handling](architecture.md#failure-handling).

The shared budget can be smaller than a directional watermark. Its 4 KiB minimum allows one block to back a receive. Under saturation, directions share released blocks in FIFO order and keep using any free space in their own existing tail block. Size the budget for the amount of backpressure and concurrent traffic the deployment should tolerate. See [shared forwarding payload budget](architecture.md#shared-forwarding-payload-budget).
