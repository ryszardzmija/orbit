import argparse
import json
import math
import statistics
import subprocess
import sys
import time
from contextlib import nullcontext

from _support import DEFAULT_ORBIT, HOST, TIMEOUT, child, cpu_seconds, free_port, orbit
from _support import require_running, rss_kib, run, wait_for_log


def monitor(client, log, proxy, warmup, duration):
    wait_for_log(client, log, b'"start"')
    time.sleep(warmup)
    require_running(client)
    start = time.monotonic()
    initial_cpu = cpu_seconds(proxy.pid)
    peak_rss = rss_kib(proxy.pid)
    end, cpu = start, initial_cpu
    while (remaining := start + duration - end) > 0 and client.poll() is None:
        time.sleep(min(0.2, remaining))
        cpu = cpu_seconds(proxy.pid)
        end = time.monotonic()
        peak_rss = max(peak_rss, rss_kib(proxy.pid))
    if end == start:
        raise RuntimeError("iPerf3 exited before the Orbit sampling window")
    elapsed = end - start
    return 100 * (cpu - initial_cpu) / elapsed, peak_rss, elapsed


def receiver_result(log, streams):
    log.seek(0)
    try:
        for line in log:
            event = json.loads(line)
            if event["event"] == "error":
                raise RuntimeError(f"iPerf3 reported an error: {event['data']}")
            if event["event"] == "end":
                result = event["data"]
                rate = result["sum_received"]["bits_per_second"]
                seconds = result["sum_received"]["seconds"]
                if (len(result["streams"]) == streams
                        and math.isfinite(rate) and rate > 0
                        and math.isfinite(seconds) and seconds > 0):
                    return rate / 1e9, seconds
                break
    except (ValueError, TypeError, KeyError) as error:
        raise RuntimeError("invalid iPerf3 JSON output") from error
    raise RuntimeError(f"no valid iPerf3 receiver summary for {streams} streams")


def measure(args, streams, proxied):
    backend_port = free_port()
    server_command = [
        "iperf3", "-s", "-1", "-4", "-B", HOST, "-p", str(backend_port), "--forceflush",
    ]
    with child(server_command) as (server, server_log):
        wait_for_log(server, server_log, f"Server listening on {backend_port}".encode())
        route = orbit(args.orbit, backend_port) if proxied else nullcontext((backend_port, None))
        with route as (port, proxy):
            client_command = [
                "iperf3", "-c", HOST, "-4", "-B", HOST, "-p", str(port),
                "-P", str(streams), "-O", str(args.warmup), "-t", str(args.seconds),
                "-i", "0", "--json-stream", "--forceflush", "--connect-timeout", "5000",
            ]
            with child(client_command) as (client, client_log):
                if proxy is None:
                    client.wait(timeout=args.warmup + args.seconds + 2 * TIMEOUT)
                    resources = ()
                else:
                    resources = monitor(client, client_log, proxy, args.warmup, args.seconds)
                    client.wait(timeout=TIMEOUT)
                rate, seconds = receiver_result(client_log, streams)
            server.wait(timeout=TIMEOUT)
    return rate, seconds, resources


def main():
    parser = argparse.ArgumentParser(description="Compare direct and proxied TCP throughput")
    parser.add_argument("--orbit", default=DEFAULT_ORBIT, help="optimized Release executable")
    parser.add_argument("--warmup", type=int, default=2, help="iPerf3 omitted seconds (default 2)")
    parser.add_argument("--seconds", type=int, default=10, help="measured seconds (default 10)")
    parser.add_argument("--trials", type=int, default=3, help="trials per configuration (default 3)")
    args = parser.parse_args()
    if args.warmup < 0 or args.seconds < 1 or args.trials < 1:
        parser.error("warm-up must be nonnegative, and seconds and trials must be positive")

    print(f"iPerf3 IPv4 loopback, {args.warmup}s omitted, {args.seconds}s measured, "
          f"{args.trials} trials per configuration", flush=True)
    rates = {(streams, route): [] for streams in (1, 4) for route in ("direct", "orbit")}
    failed = False
    for trial in range(1, args.trials + 1):
        for streams, route in rates:
            label = f"trial {trial}, {streams} streams, {route}"
            try:
                rate, seconds, resources = measure(args, streams, route == "orbit")
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                print(f"{label}: FAILED ({error})", file=sys.stderr, flush=True)
                failed = True
                continue
            rates[streams, route].append(rate)
            result = f"{rate:.3f} Gbit/s receiver over {seconds:.3f}s"
            if resources:
                cpu, peak_rss, window = resources
                result += f", Orbit CPU {cpu:.1f}%, peak RSS {peak_rss} KiB over {window:.3f}s"
            print(f"{label}: {result}", flush=True)

    for (streams, route), values in rates.items():
        if values:
            print(f"{streams} streams, {route}: median {statistics.median(values):.3f} Gbit/s "
                  f"({len(values)}/{args.trials} successful trials)")
    return int(failed)


if __name__ == "__main__":
    run(main)
