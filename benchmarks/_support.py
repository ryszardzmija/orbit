import os
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path


HOST = "127.0.0.1"
TIMEOUT = 5
POLL_INTERVAL = 0.02
DEFAULT_ORBIT = Path(__file__).resolve().parents[1] / "build/release/orbit"


@contextmanager
def child(command):
    # A file keeps verbose children from blocking on an unread pipe.
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            try:
                yield process, log
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=TIMEOUT)
                    except subprocess.TimeoutExpired as error:
                        process.kill()
                        process.wait(timeout=2)
                        raise RuntimeError("child required SIGKILL during cleanup") from error
            if process.returncode != 0:
                raise RuntimeError(f"child exited with status {process.returncode}")
        except BaseException:
            size = os.fstat(log.fileno()).st_size
            tail = os.pread(log.fileno(), 8192, max(0, size - 8192))
            print(f"{shlex.join(command)} (exit {process.returncode})", file=sys.stderr)
            print(tail.decode(errors="replace"), file=sys.stderr, end="")
            raise


def require_running(process):
    if (status := process.poll()) is not None:
        raise RuntimeError(f"child exited unexpectedly with status {status}")


def wait_for_log(process, log, marker):
    deadline = time.monotonic() + TIMEOUT
    while (remaining := deadline - time.monotonic()) > 0:
        require_running(process)
        if marker in os.pread(log.fileno(), 8192, 0):
            return
        time.sleep(min(POLL_INTERVAL, remaining))
    raise TimeoutError(f"timed out waiting for {marker!r}")


def free_port():
    with socket.socket() as probe:
        probe.bind((HOST, 0))
        return probe.getsockname()[1]


@contextmanager
def orbit(binary, upstream_port):
    port = free_port()
    command = [
        str(Path(binary).resolve()), "--listen-host", HOST, "--listen-port", str(port),
        "--upstream", f"{HOST}:{upstream_port}", "--log-level", "info",
    ]
    with child(command) as (process, log):
        wait_for_log(process, log, b"Starting proxying traffic...")
        yield port, process
        require_running(process)


@contextmanager
def listener():
    with socket.socket() as sock:
        sock.settimeout(TIMEOUT)
        sock.bind((HOST, 0))
        sock.listen()
        yield sock


def exchange(sock, payload):
    deadline = time.monotonic() + TIMEOUT
    sock.settimeout(TIMEOUT)
    sock.sendall(payload)
    response = bytearray()
    while len(response) < len(payload):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("echo reply timed out")
        sock.settimeout(remaining)
        chunk = sock.recv(len(payload) - len(response))
        if not chunk:
            raise RuntimeError(f"echo closed after {len(response)} of {len(payload)} bytes")
        response.extend(chunk)
    if response != payload:
        raise RuntimeError("echo payload mismatch")


def cpu_seconds(pid):
    stat = Path(f"/proc/{pid}/stat").read_text()
    # The comm field is parenthesized and can contain spaces and parentheses.
    fields = stat[stat.rindex(")") + 1:].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def rss_kib(pid):
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    raise RuntimeError(f"process {pid} has no VmRSS field")


def fd_count(pid):
    return len(os.listdir(f"/proc/{pid}/fd"))


def run(main):
    def interrupt(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupt)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit("benchmark interrupted")
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        sys.exit(f"benchmark failed: {error}")
