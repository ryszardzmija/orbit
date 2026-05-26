# Orbit

<div align="center">
  <img src="assets/logo.png" alt="Orbit Logo" width="300"/>
  <p>
    <a href="https://github.com/ryszardzmija/orbit/actions/workflows/ci.yaml">
      <img src="https://github.com/ryszardzmija/orbit/actions/workflows/ci.yaml/badge.svg" alt="CI"/>
    </a>
  </p>
</div>

## Overview

Orbit is intended to become a gateway to ShaleDB, a Dynamo-like distributed key-value store. For
now, it is a transparent TCP reverse proxy: clients connect to Orbit and Orbit forwards their byte
streams to one configured upstream server without interpreting the protocol.

## Design

Orbit uses nonblocking sockets and a single `epoll` reactor to manage the listener, upstream
connections, and active sessions. Each client is paired with its upstream socket, allowing many
streams to make progress without dedicating a thread to each connection.

Data is forwarded directly when possible. If one side cannot keep up, Orbit buffers pending data
and pauses reads until the buffer drains, applying backpressure rather than allowing unbounded
queueing. Routine socket and forwarding failures are isolated to the affected session, while
failures that compromise reactor state remain fatal.

On shutdown, Orbit stops accepting new clients and gives established sessions time to finish before
forcing remaining connections closed.

## Building

Orbit requires Linux, CMake 3.28 or newer, Ninja, and a C++23 compiler. Configuration fetches its
dependencies if they are not already present.

```sh
cmake -S . -B build -G Ninja -DORBIT_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Trying it out

You can try the proxy without a ShaleDB server by putting `cat` behind `socat` as a simple
echoing upstream. In one terminal, start the upstream:

```sh
socat -v TCP-LISTEN:7000,reuseaddr,fork EXEC:/bin/cat
```

Start Orbit in another terminal:

```sh
./build/orbit \
  --listen-host 127.0.0.1 \
  --listen-port 8080 \
  --upstream-host 127.0.0.1 \
  --upstream-port 7000
```

Then send a message through the proxy:

```sh
printf 'hello through orbit\n' | socat - TCP:127.0.0.1:8080
```

You should see the same line returned through the proxy. Running the client command from more than
one terminal is also a quick way to try multiple simultaneous sessions.

## Configuration

The upstream host and port are required. Orbit listens on `0.0.0.0:8080` by default and logs at
`info` level; these can be changed with `--listen-host`, `--listen-port`, and `--log-level`.
Options can also be set through corresponding `ORBIT_` environment variables, such as
`ORBIT_UPSTREAM_HOST` and `ORBIT_UPSTREAM_PORT`.
