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
streams to the first configured upstream server without interpreting the protocol (before load balancing is implemented).

## Design

Orbit uses nonblocking sockets and a single `epoll` reactor to manage the listener, upstream
connections, and active sessions. Each client is paired with its upstream socket, allowing many
streams to make progress without dedicating a thread to each connection.

Data is forwarded directly when possible. If one side cannot keep up, Orbit buffers pending data
and normally pauses reads until the queue falls to its low watermark. Watermarks guide flow control.
A hard send or receive error closes the affected session. Some dial and reactor failures end the
process.

On shutdown, Orbit stops accepting new clients and gives established sessions time to finish before
forcing remaining connections closed.

See the [architecture](docs/architecture.md) for the event loop, connection lifecycle and shutdown
behavior, the [engineering invariants](docs/invariants.md) for correctness requirements, and
[known limitations](docs/known-limitations.md) for the current gaps.

## Building

On Linux, the `debug` preset uses GCC 15, CMake 3.28 or newer, Ninja, Git and Python 3.10 or
newer. CMake fetches the pinned dependencies during configuration.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The [development guide](docs/development.md) covers tool installation, the `release` and
`sanitizers` presets, test selection and CI diagnostics.

## Trying it out

You can try the proxy without a ShaleDB server by putting `cat` behind `socat` as a simple
echoing upstream. In one terminal, start the upstream:

```sh
socat -v TCP-LISTEN:7000,reuseaddr,fork EXEC:/bin/cat
```

Start Orbit in another terminal:

```sh
./build/debug/orbit \
  --listen-host 127.0.0.1 \
  --listen-port 8080 \
  --upstream 127.0.0.1:7000
```

Then send a message through the proxy:

```sh
printf 'hello through orbit\n' | socat - TCP:127.0.0.1:8080
```

You should see the same line returned through the proxy. Running the client command from more than
one terminal is also a quick way to try multiple simultaneous sessions.

## Configuration

At least one upstream is required. Pass an endpoint as `--upstream HOST:PORT`, using
`--upstream [ADDRESS]:PORT` for IPv6, and repeat the option to configure multiple upstreams. Orbit
currently forwards connections through the first configured upstream.

Orbit listens on `0.0.0.0:8080` by default and logs at `info` level; these can be changed with
`--listen-host`, `--listen-port`, and `--log-level`. Options can also be set through corresponding
`ORBIT_` environment variables. `ORBIT_UPSTREAMS` accepts a comma-separated list of upstreams.
See [configuration](docs/config.md) for validation rules and the full option reference.
