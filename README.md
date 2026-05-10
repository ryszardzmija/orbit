# Orbit

<div align="center">
  <img src="assets/logo.png" alt="Orbit Logo" width="300"/>
</div>

## Overview

Orbit is a reverse proxy for ShaleDB, a key-value store exposed through a gRPC interface. ShaleDB is
currently a single-node store, but its long-term direction is to become a Dynamo-style distributed
key-value store. Clients connect to Orbit, and Orbit connects on their behalf to a configured
ShaleDB server.

The project is still at the transport-layer stage. At the moment Orbit is focused on doing the
basic proxying work well: accepting a client connection, opening an upstream connection, forwarding
the underlying TCP stream in both directions, and applying backpressure when one side is slower than
the other.

The longer-term goal is for Orbit to become the client-facing layer for that distributed system.
Instead of making every client understand Dynamo-style routing and failure handling, Orbit should
eventually understand enough about ShaleDB's gRPC API and cluster layout to make those decisions on
the client's behalf.

## Building and running

Orbit requires Linux, CMake 3.28 or newer, Ninja, and a C++23 compiler. CMake fetches spdlog and
CLI11 during configuration. If tests are enabled, it also fetches GoogleTest.

Configure the project with tests enabled:

```sh
cmake -B build -G Ninja -DORBIT_BUILD_TESTS=ON
```

Build the project with:

```sh
cmake --build build
```

Run the test suite with:

```sh
ctest --test-dir build --output-on-failure
```

Run Orbit in front of a ShaleDB gRPC endpoint:

```sh
./build/orbit --upstream-host 127.0.0.1 --upstream-port 7000
```

In the current implementation Orbit does not terminate gRPC, parse HTTP/2 frames, or decode ShaleDB
protobuf messages. It forwards the connection transparently at the TCP layer, so the client still
needs to speak ShaleDB's gRPC protocol and the upstream should be a ShaleDB server.

By default Orbit listens on `0.0.0.0:8080`. The listen address, upstream address, ports, and log
level can be set with command-line flags:

```sh
./build/orbit \
  --listen-host 127.0.0.1 \
  --listen-port 8080 \
  --upstream-host 127.0.0.1 \
  --upstream-port 7000 \
  --log-level info
```

The same settings are also available through environment variables using the `ORBIT_` prefix, such
as `ORBIT_UPSTREAM_HOST` and `ORBIT_UPSTREAM_PORT`.

## Project structure

The proxy itself lives under `src/proxy`. This is where the epoll loop, session state, socket I/O,
pending writes, send buffers, and backpressure behavior are implemented.

Networking support is kept in `src/net`, including address resolution, listening, dialing, socket
options, and address formatting. Configuration parsing lives in `src/config`, logging setup lives in
`src/logging`, and small shared utilities live in `src/common`.

The current tests cover the block-based send buffer and socket-address formatting.

## Proxy design

Orbit accepts one downstream connection from a ShaleDB client and opens one upstream connection to
the configured ShaleDB server. Both sockets are switched to nonblocking mode and registered with
epoll. From there, the event loop forwards data until the session closes or a socket-level error
stops it.

epoll is used in level-triggered mode. Orbit always watches for readable sockets, but only watches
for writability when it has data buffered for that socket. That keeps the loop straightforward while
avoiding unnecessary writable events.

When data arrives, Orbit first tries to send it directly to the other endpoint. If the kernel cannot
accept the whole write, Orbit stores the remaining bytes in a user-space send buffer and flushes
them later.

The send buffer uses high and low watermarks. Once the buffer gets too full, Orbit stops reading
from the producing side. When the buffer drains far enough, reading resumes. This lets normal TCP
flow control push back on fast senders instead of letting memory grow without bound.

Orbit also propagates TCP half-closes. If one peer reaches EOF, Orbit drains any pending data that
still needs to cross the proxy before shutting down the corresponding outbound direction.

## Configuration

Configuration is currently provided through CLI flags or environment variables. The upstream host
and port are required. The listen host defaults to `0.0.0.0`, the listen port defaults to `8080`,
and the log level defaults to `info`.

The send-buffer watermarks are fixed in code for now. Each endpoint uses 4 KiB transfer blocks with
a 256 KiB high watermark and a 192 KiB low watermark.

## Current limitations

Orbit currently handles a single downstream-upstream session. After it accepts one client, it dials
one upstream and runs the proxy loop for that pair.

It is also still protocol-opaque. Orbit can carry a gRPC connection, but it does not yet understand
HTTP/2, parse ShaleDB protobuf messages, or do any of the Dynamo-style work that would make it more
useful than a transparent proxy.

There is no idle timeout, so a stalled half-closed session can stay open indefinitely. Error
handling is also coarse: a forwarding or socket error stops the current proxy run instead of being
isolated to one managed session.

Configuration is intentionally simple at this stage and does not yet support config files, multiple
upstreams, or runtime reloads.

## Roadmap

The next layer of work is to make the transport proxy more complete: multiple client sessions,
clearer session lifecycle management, idle timeouts, configurable buffer limits, and better error
isolation.

Once that foundation is in place, Orbit can start growing the ShaleDB-specific behavior that makes
clients simpler: health checks, request-aware routing, retry policy, load balancing, and eventually
the Dynamo-style coordination needed once ShaleDB grows a distribution layer.
