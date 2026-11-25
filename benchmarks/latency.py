import argparse
import socket
import statistics
import sys
import threading
import time
from contextlib import contextmanager, nullcontext

from _support import DEFAULT_ORBIT, HOST, TIMEOUT, exchange, listener, orbit, run


@contextmanager
def echo_connection(backend, port):
    with socket.create_connection((HOST, port), timeout=TIMEOUT) as client:
        peer, _ = backend.accept()
        with peer:
            for sock in (client, peer):
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                sock.settimeout(TIMEOUT)
            errors = []

            def echo():
                try:
                    while data := peer.recv(65536):
                        peer.sendall(data)
                except OSError as error:
                    errors.append(error)

            worker = threading.Thread(target=echo, daemon=True)
            worker.start()
            try:
                yield client
            finally:
                client.close()
                worker.join(timeout=TIMEOUT + 1)
                if worker.is_alive():
                    raise TimeoutError("echo worker did not stop")
            if errors:
                raise RuntimeError(f"echo worker failed: {errors[0]}")


def measure(args, backend, payload, proxied):
    name = "orbit" if proxied else "direct"
    backend_port = backend.getsockname()[1]
    route = orbit(args.orbit, backend_port) if proxied else nullcontext((backend_port, None))
    samples = []
    phase = "setup"
    try:
        with route as (port, _), echo_connection(backend, port) as client:
            phase = "warm-up"
            for _ in range(args.warmup):
                exchange(client, payload)
            phase = "measurement"
            for _ in range(args.samples):
                start = time.perf_counter_ns()
                exchange(client, payload)
                samples.append(time.perf_counter_ns() - start)
            phase = "cleanup"
    except (OSError, RuntimeError) as error:
        failed = int(phase == "measurement")
        skipped = args.samples - len(samples) - failed
        print(f"{name}: FAILED in {phase} ({error}), successful={len(samples)}, "
              f"failed={failed}, skipped={skipped}", file=sys.stderr, flush=True)
        return False

    samples.sort()
    p50 = statistics.median(samples) / 1000
    p95 = samples[(95 * len(samples) + 99) // 100 - 1] / 1000
    p99 = samples[(99 * len(samples) + 99) // 100 - 1] / 1000
    print(f"{name}: p50={p50:.3f} us, p95={p95:.3f} us, p99={p99:.3f} us, "
          f"successful={len(samples)}, failed=0", flush=True)
    return True


def main():
    parser = argparse.ArgumentParser(description="Compare direct and proxied TCP echo RTT")
    parser.add_argument("--orbit", default=DEFAULT_ORBIT, help="optimized Release executable")
    parser.add_argument("--payload", type=int, default=64, help="payload bytes (default 64)")
    parser.add_argument("--warmup", type=int, default=1000, help="untimed echoes (default 1000)")
    parser.add_argument("--samples", type=int, default=20000, help="timed echoes (default 20000)")
    args = parser.parse_args()
    if args.warmup < 0 or args.samples < 1 or args.payload < 1:
        parser.error("warm-up must be nonnegative, and samples and payload must be positive")

    payload = (bytes(range(256)) * ((args.payload + 255) // 256))[:args.payload]
    print(f"TCP echo over IPv4 loopback, {args.payload} bytes, {args.warmup} warm-up exchanges, "
          f"{args.samples} timed exchanges per route", flush=True)
    with listener() as backend:
        direct = measure(args, backend, payload, proxied=False)
        proxied = measure(args, backend, payload, proxied=True)
    return int(not (direct and proxied))


if __name__ == "__main__":
    run(main)
