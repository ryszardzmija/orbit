# Orbit

<div align="center">
  <img src="assets/logo.png" alt="Orbit logo" width="300"/>
  <p>
    <a href="https://github.com/ryszardzmija/orbit/actions/workflows/ci.yaml">
      <img src="https://github.com/ryszardzmija/orbit/actions/workflows/ci.yaml/badge.svg" alt="CI"/>
    </a>
  </p>
</div>

Orbit is a TCP reverse proxy for Linux. It provides a client-facing address for a backend
service and relays traffic in both directions, so applications can keep using their existing
protocols.

Orbit uses Linux `epoll` to handle concurrent connections. It limits total and pending
admission and shares a forwarding payload budget across connections. It manages traffic
flow when peers read slowly and supports graceful shutdown, giving active connections
time to finish.

The project is evolving into a resource-efficient, fault-tolerant Layer 4 TCP load balancer.
Development priorities include distributing connections across upstreams, handling upstream
failures, and keeping resource use predictable under load.

## Getting started

Orbit is written in C++23. To build it on Linux, use GCC 15, CMake 3.28 or newer, Ninja, Git,
and Python 3.10 or newer. CMake fetches pinned dependencies from GitHub during the first
configure. The echo example also requires `socat`.

```sh
git clone https://github.com/ryszardzmija/orbit.git
cd orbit
cmake --preset debug
cmake --build --preset debug
```

In one terminal, start a TCP echo server on loopback.

```sh
socat TCP4-LISTEN:7000,bind=127.0.0.1,reuseaddr,fork EXEC:/bin/cat
```

In a second terminal, run Orbit from the repository root. The listen options set where clients
connect. Use `--upstream HOST:PORT` to point Orbit to the echo server.

```sh
./build/debug/orbit \
  --listen-host 127.0.0.1 \
  --listen-port 8080 \
  --upstream 127.0.0.1:7000
```

From a third terminal, send a line to Orbit. You should see `hello through orbit` echoed back.

```sh
printf 'hello through orbit\n' | socat - TCP:127.0.0.1:8080
```

Orbit currently uses the first configured upstream. Find all options and environment variables
in the [configuration reference](docs/config.md).

## Testing

CTest runs GoogleTest unit tests and Python integration tests that exercise the proxy over
loopback TCP. CI also runs the suite with AddressSanitizer and UndefinedBehaviorSanitizer.
After building, run the Debug suite from the repository root.

```sh
ctest --preset debug
```

## Performance

The Release loopback benchmarks compare direct TCP connections with connections through Orbit
for throughput and echo round-trip latency. They also measure Orbit's CPU and resident memory
under load, and track memory and open file descriptors as idle sessions accumulate. The
methodology and commands are in the [benchmark guide](benchmarks/README.md).

## Documentation

- [Architecture](docs/architecture.md)
- [Engineering invariants](docs/invariants.md)
- [Development guide](docs/development.md)
- [Known limitations](docs/known-limitations.md)
