# Configuration

Orbit accepts options on the command line or through the corresponding environment variables. An explicit command-line value takes precedence over that option's environment value. Empty environment values are skipped. Values for `ORBIT_LISTEN_PORT` or `ORBIT_LOG_LEVEL` that fail their CLI11 validators are silently skipped, leaving the defaults. Invalid endpoint syntax in a supplied upstream list is a parsing error.

| Option | Environment variable | Default and validation |
| --- | --- | --- |
| `--listen-host` | `ORBIT_LISTEN_HOST` | `0.0.0.0`. Use an IPv4 or IPv6 address, or a host name that resolves to a bindable address. Use `::` for the IPv6 wildcard. Orbit binds the first listener candidate it can initialize successfully. |
| `--listen-port` | `ORBIT_LISTEN_PORT` | `8080`. Accepted range is 1 through 65535. |
| `--upstream` | `ORBIT_UPSTREAMS` | Required. Supply at least one endpoint. Repeat the option or use a comma-separated list. |
| `--log-level` | `ORBIT_LOG_LEVEL` | `info`. Accepted names are `trace`, `debug`, `info`, `warn`, `error`, `critical`, and `off`, case-insensitively. CLI11 also accepts their numeric values 0 through 6 in that order. |

Use `HOST:PORT` for DNS names and IPv4 addresses and `[ADDRESS]:PORT` for IPv6. A host must be nonempty. An upstream port must consist of decimal digits with a value from 1 through 65535. Parsing checks endpoint syntax. Resolution and binding take place during startup.

```sh
./build/debug/orbit --upstream 127.0.0.1:7000 --upstream '[2001:db8::1]:7000'
ORBIT_UPSTREAMS='127.0.0.1:7000,[2001:db8::1]:7000' ./build/debug/orbit
```

All upstream entries are parsed in input order. Only the first configured endpoint is resolved and used at runtime. Each new connection can try later resolved addresses for that endpoint if earlier addresses fail. If resolution of the first endpoint fails, startup fails.

Buffer watermarks are set in the reactor. The internal `Config::send_buffer` field has no CLI or environment option and does not control runtime buffering. See [upstream selection and dialing](architecture.md#upstream-selection-and-dialing) and [forwarding and backpressure](architecture.md#forwarding-and-backpressure).
