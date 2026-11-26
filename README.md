# Orbit

<div align="center">
  <img src="assets/logo.png" alt="Orbit logo" width="300"/>
  <p>
    <a href="https://github.com/ryszardzmija/orbit/actions/workflows/ci.yaml">
      <img src="https://github.com/ryszardzmija/orbit/actions/workflows/ci.yaml/badge.svg" alt="CI"/>
    </a>
  </p>
</div>

Orbit is a resource-efficient, fault-tolerant Layer 4 TCP load balancer for Linux. Its
current runtime acts as a TCP reverse proxy to the first configured upstream, relaying
byte streams in both directions without interpreting application protocols.

Orbit caps admitted connections and forwarding buffer capacity. It applies backpressure
when peers read slowly, isolates connection-local failures, and gives established
connections time to finish during graceful shutdown.

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

In a second terminal, run Orbit from the repository root. Listen on port 8080 and point the
proxy to the echo server on port 7000.

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

## Testing

CTest runs GoogleTest unit tests and Python integration tests that exercise the proxy over
loopback TCP. CI also runs the suite with AddressSanitizer and UndefinedBehaviorSanitizer.
After building, run the Debug suite from the repository root.

```sh
ctest --preset debug
```

## Documentation

- [Configuration reference](docs/config.md)
- [Architecture](docs/architecture.md)
- [Engineering invariants](docs/invariants.md)
- [Development guide](docs/development.md)
- [Known limitations](docs/known-limitations.md)
- [Loopback benchmarks](benchmarks/README.md)
