import argparse
import os
import resource
import socket
import sys
import time
from contextlib import ExitStack

from _support import DEFAULT_ORBIT, HOST, POLL_INTERVAL, TIMEOUT, fd_count, listener, orbit
from _support import require_running, rss_kib, run


def establish(sockets, backend, port):
    client = sockets.enter_context(socket.create_connection((HOST, port), timeout=TIMEOUT))
    peer, _ = backend.accept()
    sockets.enter_context(peer).settimeout(TIMEOUT)
    client.sendall(b"?")
    if peer.recv(1) != b"?":
        raise RuntimeError("upstream did not receive the verification byte")
    peer.sendall(b"!")
    if client.recv(1) != b"!":
        raise RuntimeError("client did not receive the upstream reply")


def show_sample(label, count, proxy):
    require_running(proxy)
    fds = fd_count(proxy.pid)
    print(f"{label}: {count} verified sessions, RSS {rss_kib(proxy.pid)} KiB, {fds} open FDs",
          flush=True)
    return fds


def after_close(proxy, baseline_fds):
    deadline = time.monotonic() + TIMEOUT
    while fd_count(proxy.pid) != baseline_fds:
        if (remaining := deadline - time.monotonic()) <= 0:
            break
        time.sleep(min(POLL_INTERVAL, remaining))
    if show_sample("after closing peers", 0, proxy) != baseline_fds:
        raise RuntimeError(f"Orbit did not return to {baseline_fds} open FDs within {TIMEOUT}s")


def measure(backend, port, proxy, counts):
    baseline_fds = show_sample("initial", 0, proxy)
    soft_limit, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
    capacity = max(counts)
    if soft_limit != resource.RLIM_INFINITY:
        # Orbit and the Python driver each hold two fds per session. Leave room
        # for resource sampling and other transient descriptors in each process.
        base = max(baseline_fds, fd_count(os.getpid()))
        capacity = min(capacity, max(0, (soft_limit - base - 8) // 2))
    print(f"soft RLIMIT_NOFILE {soft_limit}, testing up to {capacity} sessions", flush=True)

    count = 0
    try:
        with ExitStack() as sockets:
            for requested in counts:
                target = min(requested, capacity)
                if target < requested and target == count:
                    print(f"requested {requested}: SKIPPED (FD limit, holding {count})", flush=True)
                    continue
                while count < target:
                    establish(sockets, backend, port)
                    count += 1
                label = f"requested {requested}"
                if target < requested:
                    label += " (reduced for FD limit)"
                show_sample(label, count, proxy)
    except (OSError, RuntimeError) as error:
        print(f"requested {requested}: FAILED after verifying {count} sessions ({error})",
              file=sys.stderr, flush=True)
        raise
    finally:
        after_close(proxy, baseline_fds)


def main():
    parser = argparse.ArgumentParser(description="Measure Orbit's resources with idle TCP sessions")
    parser.add_argument("--orbit", default=DEFAULT_ORBIT, help="optimized Release executable")
    parser.add_argument("--counts", nargs="+", type=int, default=[100, 500, 1000],
                        help="positive connection targets (default 100 500 1000)")
    args = parser.parse_args()
    if min(args.counts) < 1:
        parser.error("connection targets must be positive; zero is sampled automatically")
    with listener() as backend, orbit(args.orbit, backend.getsockname()[1]) as (port, proxy):
        measure(backend, port, proxy, sorted(set(args.counts)))


if __name__ == "__main__":
    run(main)
