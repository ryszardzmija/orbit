import concurrent.futures
from contextlib import ExitStack, redirect_stdout
import io
import os
import resource
import select
import shlex
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from unittest import mock


HEADER_SIZE = 4
SOCKET_TIMEOUT = 5
STARTUP_TIMEOUT = 5
SHUTDOWN_TIMEOUT = 5
KILL_TIMEOUT = 2
POLL_INTERVAL = 0.02
LOG_LIMIT = 32 * 1024
READY_MESSAGE = b"Starting proxying traffic..."
CLOCK_TICKS_PER_SECOND = os.sysconf("SC_CLK_TCK")


def process_cpu_seconds(pid):
    """Total user + system CPU time consumed by a process, in seconds."""
    with open(f"/proc/{pid}/stat") as stat_file:
        content = stat_file.read()
    # Field 2 (comm) is parenthesized and may itself contain spaces and parentheses, so parse
    # everything after the final ')'. The remainder starts at field 3 (state), making utime
    # (field 14) and stime (field 15) the 12th and 13th entries.
    fields_after_comm = content[content.rindex(")") + 1:].split()
    utime = int(fields_after_comm[11])
    stime = int(fields_after_comm[12])
    return (utime + stime) / CLOCK_TICKS_PER_SECOND


def send_message(sock, payload):
    header = struct.pack("!I", len(payload))
    sock.sendall(header + payload)


def recv_exact(sock, size):
    data = bytearray()
    deadline = time.monotonic() + SOCKET_TIMEOUT
    try:
        while len(data) < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError
            sock.settimeout(remaining)
            chunk = sock.recv(size - len(data))
            if not chunk:
                raise RuntimeError(
                    f"connection closed after {len(data)} of {size} expected bytes arrived"
                )
            data.extend(chunk)
    except TimeoutError as error:
        raise TimeoutError(
            f"timed out after {SOCKET_TIMEOUT}s receiving {len(data)} of {size} expected bytes"
        ) from error
    finally:
        sock.settimeout(SOCKET_TIMEOUT)
    return bytes(data)


def recv_message(sock):
    header = recv_exact(sock, HEADER_SIZE)
    payload_size = struct.unpack("!I", header)[0]
    return recv_exact(sock, payload_size)


def expect_eof(sock):
    data = sock.recv(1)
    if data:
        raise AssertionError(f"expected EOF, received {data!r}")


def find_available_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class ProxyHarness:
    def __init__(self):
        self.resources = ExitStack()
        self.command = []
        self.process = None
        self.shutdown_requested = False
        self.exit_checked = False

    def start(self, orbit_command):
        self.command = list(orbit_command)
        self.log = self.resources.enter_context(tempfile.TemporaryFile())
        self.resources.callback(self.stop_and_report)
        self.backend_listener = self.resources.enter_context(socket.socket())
        self.backend_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.backend_listener.bind(("127.0.0.1", 0))
        self.backend_listener.listen()
        self.backend_listener.setblocking(False)
        backend_port = self.backend_listener.getsockname()[1]
        self.proxy_port = find_available_port()
        self.command.extend([
            "--listen-host", "127.0.0.1",
            "--listen-port", str(self.proxy_port),
            "--upstream", f"127.0.0.1:{backend_port}",
            "--log-level", "info",
        ])
        self.process = subprocess.Popen(
            self.command,
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )
        self.wait_for_readiness()

    def require_running(self, operation):
        exit_code = self.process.poll()
        if exit_code is not None:
            raise RuntimeError(f"Orbit exited with status {exit_code} while {operation}")

    def wait_for_readiness(self):
        self.wait_for_log(READY_MESSAGE, "starting")

    def wait_for_log(self, message, operation):
        deadline = time.monotonic() + STARTUP_TIMEOUT
        while (remaining := deadline - time.monotonic()) > 0:
            self.require_running(operation)
            if message in os.pread(self.log.fileno(), LOG_LIMIT, 0):
                return
            time.sleep(min(POLL_INTERVAL, remaining))
        raise TimeoutError(f"Orbit did not log {message!r} within {STARTUP_TIMEOUT}s")

    def connect_client(self):
        self.require_running("connecting a client")
        return self.resources.enter_context(
            socket.create_connection(("127.0.0.1", self.proxy_port), timeout=SOCKET_TIMEOUT)
        )

    def accept_backend(self):
        deadline = time.monotonic() + SOCKET_TIMEOUT
        while (remaining := deadline - time.monotonic()) > 0:
            self.require_running("waiting for the upstream connection")
            readable, _, _ = select.select(
                [self.backend_listener], [], [], min(POLL_INTERVAL, remaining)
            )
            if readable:
                try:
                    backend, _ = self.backend_listener.accept()
                except BlockingIOError:
                    continue
                self.resources.enter_context(backend)
                backend.settimeout(SOCKET_TIMEOUT)
                return backend
        raise TimeoutError(f"backend did not receive Orbit's connection within {SOCKET_TIMEOUT}s")

    def connect_session(self):
        return self.connect_client(), self.accept_backend()

    def request_shutdown(self):
        self.require_running("requesting shutdown")
        self.process.terminate()
        self.shutdown_requested = True

    def wait_for_shutdown_request(self):
        # Orbit blocks SIGTERM and consumes it through signalfd. Wait for the
        # first request to leave the process-wide pending mask before sending another.
        deadline = time.monotonic() + SHUTDOWN_TIMEOUT
        sigterm_mask = 1 << (signal.SIGTERM - 1)
        while (remaining := deadline - time.monotonic()) > 0:
            self.require_running("waiting for Orbit to receive SIGTERM")
            with open(f"/proc/{self.process.pid}/status") as status:
                for line in status:
                    if line.startswith("ShdPnd:"):
                        if (int(line.split()[1], 16) & sigterm_mask) == 0:
                            return
                        break
                else:
                    raise RuntimeError("Orbit's /proc status has no ShdPnd field")
            time.sleep(min(POLL_INTERVAL, remaining))
        raise TimeoutError(f"Orbit did not consume SIGTERM within {SHUTDOWN_TIMEOUT}s")

    def wait_for_clean_exit(self):
        try:
            exit_code = self.process.wait(timeout=SHUTDOWN_TIMEOUT)
        except subprocess.TimeoutExpired as error:
            raise TimeoutError(f"Orbit did not exit within {SHUTDOWN_TIMEOUT}s") from error
        self.exit_checked = True
        if exit_code != 0:
            raise RuntimeError(f"Orbit exited with status {exit_code} during shutdown")

    def stop_and_report(self):
        try:
            if self.process is None or self.exit_checked:
                return
            exit_code = self.process.poll()
            if exit_code is not None and not self.shutdown_requested:
                raise RuntimeError(f"Orbit exited unexpectedly with status {exit_code}")
            if exit_code is None:
                if not self.shutdown_requested:
                    self.process.terminate()
                try:
                    exit_code = self.process.wait(timeout=SHUTDOWN_TIMEOUT)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    try:
                        self.process.wait(timeout=KILL_TIMEOUT)
                    except subprocess.TimeoutExpired as error:
                        raise TimeoutError(
                            f"could not reap Orbit within {KILL_TIMEOUT}s of SIGKILL"
                        ) from error
                    raise TimeoutError(
                        f"Orbit required SIGKILL after {SHUTDOWN_TIMEOUT}s of cleanup"
                    )
            if exit_code != 0:
                raise RuntimeError(f"Orbit exited with status {exit_code} during cleanup")
        finally:
            self.report()

    def report(self):
        print(f"Orbit command: {shlex.join(self.command)}")
        if self.process is not None:
            print(f"Orbit PID: {self.process.pid}, exit status: {self.process.returncode}")
        log_fd = self.log.fileno()
        size = os.fstat(log_fd).st_size
        print("Orbit stdout and stderr:")
        if size <= LOG_LIMIT:
            print(os.pread(log_fd, size, 0).decode(errors="replace"))
        else:
            half = LOG_LIMIT // 2
            print(os.pread(log_fd, half, 0).decode(errors="replace"))
            print(f"... omitted {size - LOG_LIMIT} bytes ...")
            print(os.pread(log_fd, half, size - half).decode(errors="replace"))

    def close(self):
        self.resources.close()


def finish_session(client, backend):
    client.shutdown(socket.SHUT_WR)
    expect_eof(backend)
    backend.shutdown(socket.SHUT_WR)
    expect_eof(client)


# Large enough to exercise multi-read forwarding, small enough that sendall never blocks.
HANGUP_PAYLOAD = bytes(range(256)) * 512


class ProxySessionIntegrationTest(unittest.TestCase):
    def setUp(self):
        self.proxy = ProxyHarness()
        self.addCleanup(self.proxy.close)
        self.proxy.start([self.orbit_binary])

    def test_socket_failure_is_isolated_to_its_session(self):
        failed_client, failed_backend = self.proxy.connect_session()
        healthy_client, healthy_backend = self.proxy.connect_session()

        send_message(healthy_client, b"before-failure")
        self.assertEqual(b"before-failure", recv_message(healthy_backend))

        linger = struct.pack("ii", 1, 0)
        failed_backend.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
        failed_backend.close()
        time.sleep(0.05)

        send_message(healthy_client, b"after-failure")
        self.assertEqual(b"after-failure", recv_message(healthy_backend))
        send_message(healthy_backend, b"healthy-response")
        self.assertEqual(b"healthy-response", recv_message(healthy_client))

        failed_client.close()
        finish_session(healthy_client, healthy_backend)
        self.proxy.request_shutdown()
        self.proxy.wait_for_clean_exit()

    def test_session_churn_does_not_disturb_an_active_session(self):
        persistent_client, persistent_backend = self.proxy.connect_session()

        for sequence in range(12):
            client, backend = self.proxy.connect_session()
            request = f"request-{sequence}".encode()
            response = f"response-{sequence}".encode()

            send_message(client, request)
            self.assertEqual(request, recv_message(backend))
            send_message(backend, response)
            self.assertEqual(response, recv_message(client))
            finish_session(client, backend)

        send_message(persistent_client, b"persistent-request")
        self.assertEqual(b"persistent-request", recv_message(persistent_backend))
        send_message(persistent_backend, b"persistent-response")
        self.assertEqual(b"persistent-response", recv_message(persistent_client))

        finish_session(persistent_client, persistent_backend)
        self.proxy.request_shutdown()
        self.proxy.wait_for_clean_exit()

    def test_client_half_close_is_propagated_after_data(self):
        client, backend = self.proxy.connect_session()
        request = bytes(range(256)) * 1024
        response = b"response-after-client-half-close"

        client.sendall(request)
        client.shutdown(socket.SHUT_WR)

        self.assertEqual(request, recv_exact(backend, len(request)))
        expect_eof(backend)

        backend.sendall(response)
        backend.shutdown(socket.SHUT_WR)

        self.assertEqual(response, recv_exact(client, len(response)))
        expect_eof(client)

        self.proxy.request_shutdown()
        self.proxy.wait_for_clean_exit()

    def test_backend_half_close_keeps_client_to_backend_direction_open(self):
        client, backend = self.proxy.connect_session()
        response = b"backend-finished-responding"
        request = bytes(reversed(range(256))) * 512

        backend.sendall(response)
        backend.shutdown(socket.SHUT_WR)

        self.assertEqual(response, recv_exact(client, len(response)))
        expect_eof(client)

        client.sendall(request)
        client.shutdown(socket.SHUT_WR)

        self.assertEqual(request, recv_exact(backend, len(request)))
        expect_eof(backend)

        self.proxy.request_shutdown()
        self.proxy.wait_for_clean_exit()

    def test_large_bidirectional_transfers_survive_backpressure(self):
        client, backend = self.proxy.connect_session()
        client_payload = bytes(range(256)) * 4096
        backend_payload = bytes(reversed(range(256))) * 4096

        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            send = executor.submit(client.sendall, client_payload)
            time.sleep(0.05)
            self.assertEqual(client_payload, recv_exact(backend, len(client_payload)))
            send.result(timeout=SOCKET_TIMEOUT)

        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            send = executor.submit(backend.sendall, backend_payload)
            time.sleep(0.05)
            self.assertEqual(backend_payload, recv_exact(client, len(backend_payload)))
            send.result(timeout=SOCKET_TIMEOUT)

        finish_session(client, backend)
        self.proxy.request_shutdown()
        self.proxy.wait_for_clean_exit()

    def test_graceful_shutdown_waits_for_active_session(self):
        client, backend = self.proxy.connect_session()

        send_message(client, b"request-before-shutdown")
        self.assertEqual(b"request-before-shutdown", recv_message(backend))

        self.proxy.request_shutdown()
        self.proxy.wait_for_shutdown_request()
        self.assertIsNone(self.proxy.process.poll())

        send_message(client, b"request-during-shutdown")
        self.assertEqual(b"request-during-shutdown", recv_message(backend))
        send_message(backend, b"response-during-shutdown")
        self.assertEqual(b"response-during-shutdown", recv_message(client))

        finish_session(client, backend)
        self.proxy.wait_for_clean_exit()

    def test_client_full_hangup_delivers_its_data_without_loss(self):
        # Drive the client socket into a full hangup (EPOLLHUP): the proxy half-closes the client
        # (after the backend finishes) and then the client closes its own write side, so both
        # directions of the client socket are shut. The data the client sent earlier must still
        # be delivered to the backend in full, and the session must tear down cleanly.
        client, backend = self.proxy.connect_session()
        backend_payload = b"backend-response-before-client-hangup"

        client.sendall(HANGUP_PAYLOAD)

        # Backend finishes its own direction; the proxy propagates the half-close to the client.
        backend.sendall(backend_payload)
        backend.shutdown(socket.SHUT_WR)
        self.assertEqual(backend_payload, recv_exact(client, len(backend_payload)))
        expect_eof(client)

        # The client closes its write side too: the client socket is now fully hung up (EPOLLHUP).
        client.shutdown(socket.SHUT_WR)

        # The client's payload must still reach the backend, followed by a clean EOF.
        self.assertEqual(HANGUP_PAYLOAD, recv_exact(backend, len(HANGUP_PAYLOAD)))
        expect_eof(backend)

        self.proxy.request_shutdown()
        self.proxy.wait_for_clean_exit()

    def test_backend_full_hangup_delivers_its_data_without_loss(self):
        # Symmetric to the client-hangup case: the backend socket reaches a full hangup while the
        # backend's earlier payload must still be delivered to the client without loss.
        client, backend = self.proxy.connect_session()
        client_payload = b"client-request-before-backend-hangup"

        backend.sendall(HANGUP_PAYLOAD)

        client.sendall(client_payload)
        client.shutdown(socket.SHUT_WR)
        self.assertEqual(client_payload, recv_exact(backend, len(client_payload)))
        expect_eof(backend)

        backend.shutdown(socket.SHUT_WR)

        self.assertEqual(HANGUP_PAYLOAD, recv_exact(client, len(HANGUP_PAYLOAD)))
        expect_eof(client)

        self.proxy.request_shutdown()
        self.proxy.wait_for_clean_exit()

    def test_hung_up_endpoint_does_not_keep_the_reactor_busy(self):
        # Guards against a regression in which a hung-up endpoint keeps producing events: EPOLLHUP
        # is reported unconditionally by epoll, so a reactor that left such an endpoint registered
        # with an empty interest mask would busy-loop. After driving the client into a full hangup,
        # the reactor must stay idle (blocked in epoll_wait) rather than burning CPU.
        client, backend = self.proxy.connect_session()
        response = b"backend-response-before-idle-hangup"

        client.sendall(HANGUP_PAYLOAD)
        backend.sendall(response)
        backend.shutdown(socket.SHUT_WR)
        self.assertEqual(response, recv_exact(client, len(response)))
        expect_eof(client)
        self.assertEqual(HANGUP_PAYLOAD, recv_exact(backend, len(HANGUP_PAYLOAD)))
        client.shutdown(socket.SHUT_WR)

        # Let things settle, then confirm the reactor is not burning CPU.
        measurement_window = 1.0
        time.sleep(0.1)
        cpu_before = process_cpu_seconds(self.proxy.process.pid)
        time.sleep(measurement_window)
        cpu_used = process_cpu_seconds(self.proxy.process.pid) - cpu_before
        self.assertLess(
            cpu_used,
            0.25,
            f"reactor used {cpu_used:.3f}s of CPU over {measurement_window}s while idle; "
            "a hung-up endpoint is likely spinning",
        )

        expect_eof(backend)
        self.proxy.request_shutdown()
        self.proxy.wait_for_clean_exit()

    def test_second_shutdown_signal_force_closes_active_sessions(self):
        client, backend = self.proxy.connect_session()

        send_message(client, b"request-before-hard-shutdown")
        self.assertEqual(b"request-before-hard-shutdown", recv_message(backend))

        self.proxy.request_shutdown()
        self.proxy.wait_for_shutdown_request()
        self.assertIsNone(self.proxy.process.poll())

        self.proxy.request_shutdown()
        self.proxy.wait_for_clean_exit()
        expect_eof(client)
        expect_eof(backend)


class AdmissionIntegrationTest(unittest.TestCase):
    def test_backlogged_client_runs_after_established_session_releases_slot(self):
        proxy = ProxyHarness()
        self.addCleanup(proxy.close)
        proxy.start([self.orbit_binary, "--max-connections", "1", "--max-pending-connections", "1"])
        client, backend = proxy.connect_session()
        queued = proxy.connect_client()
        send_message(queued, b"from-backlog")
        send_message(client, b"still-forwarding")
        self.assertEqual(b"still-forwarding", recv_message(backend))
        ready, _, _ = select.select([proxy.backend_listener], [], [], 0.15)
        self.assertEqual([], ready)

        finish_session(client, backend)
        second_backend = proxy.accept_backend()
        self.assertEqual(b"from-backlog", recv_message(second_backend))
        send_message(second_backend, b"admitted")
        self.assertEqual(b"admitted", recv_message(queued))
        finish_session(queued, second_backend)
        proxy.request_shutdown()
        proxy.wait_for_clean_exit()

    def test_descriptor_pressure_recovers_while_existing_session_stays_open(self):
        proxy = ProxyHarness()
        self.addCleanup(proxy.close)
        proxy.start([self.orbit_binary])
        client, backend = proxy.connect_session()
        send_message(client, b"ready")
        self.assertEqual(b"ready", recv_message(backend))
        pid = proxy.process.pid
        original = resource.prlimit(pid, resource.RLIMIT_NOFILE)
        open_fds = {int(name) for name in os.listdir(f"/proc/{pid}/fd")}
        first_free = next(fd for fd in range(max(open_fds) + 2) if fd not in open_fds)
        resource.prlimit(pid, resource.RLIMIT_NOFILE, (first_free, original[1]))
        try:
            queued = proxy.connect_client()
            send_message(queued, b"waiting-for-descriptors")
            proxy.wait_for_log(b"Admission paused after accept4", "exhausting descriptors")
            send_message(backend, b"still-forwarding")
            self.assertEqual(b"still-forwarding", recv_message(client))
        finally:
            resource.prlimit(pid, resource.RLIMIT_NOFILE, original)

        recovered = proxy.accept_backend()
        self.assertEqual(b"waiting-for-descriptors", recv_message(recovered))
        finish_session(queued, recovered)
        finish_session(client, backend)
        proxy.request_shutdown()
        proxy.wait_for_clean_exit()


class IdleExpirationIntegrationTest(unittest.TestCase):
    def test_configured_idle_timeout_closes_established_tcp_pair(self):
        proxy = ProxyHarness()
        self.addCleanup(proxy.close)
        proxy.start([self.orbit_binary, "--idle-timeout-ms", "250"])
        client, backend = proxy.connect_session()
        send_message(client, b"established")
        self.assertEqual(b"established", recv_message(backend))
        expect_eof(client)
        expect_eof(backend)
        proxy.request_shutdown()
        proxy.wait_for_clean_exit()


class ProxyHarnessTest(unittest.TestCase):
    def setUp(self):
        self.proxy = ProxyHarness()
        self.addCleanup(self.proxy.close)

    def start_child(self, script):
        self.proxy.start([sys.executable, "-u", "-c", textwrap.dedent(script)])

    def test_early_exit_fails_even_with_zero_status(self):
        with self.assertRaisesRegex(RuntimeError, "exited with status 0 while starting"):
            self.start_child("print('startup failed')")
        report = io.StringIO()
        with redirect_stdout(report):
            with self.assertRaisesRegex(RuntimeError, "exited unexpectedly with status 0"):
                self.proxy.close()
        self.assertIn("\nstartup failed\n", report.getvalue())
        self.assertEqual(-1, self.proxy.backend_listener.fileno())
        self.assertTrue(self.proxy.log.closed)

    def test_nonzero_shutdown_retains_output_larger_than_a_pipe(self):
        self.start_child("""
            import os
            import signal
            import sys

            def fail_shutdown(*_):
                print("x" * (2 * 1024 * 1024))
                print("shutdown failure", file=sys.stderr)
                os._exit(86)

            signal.signal(signal.SIGTERM, fail_shutdown)
            print("Starting proxying traffic...")
            while True:
                signal.pause()
        """)
        self.proxy.request_shutdown()
        with self.assertRaisesRegex(RuntimeError, "exited with status 86 during shutdown"):
            self.proxy.wait_for_clean_exit()
        report = io.StringIO()
        with redirect_stdout(report):
            self.proxy.close()
        self.assertIn("exit status: 86", report.getvalue())
        self.assertIn("bytes ...", report.getvalue())
        self.assertIn("\nshutdown failure\n", report.getvalue())

    def test_cleanup_kills_and_reaps_unresponsive_child(self):
        self.start_child("""
            import signal

            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            print("Starting proxying traffic...")
            while True:
                signal.pause()
        """)
        with mock.patch.object(sys.modules[__name__], "SHUTDOWN_TIMEOUT", 0.1):
            with self.assertRaisesRegex(TimeoutError, "required SIGKILL"):
                self.proxy.close()
        self.assertEqual(-signal.SIGKILL, self.proxy.process.returncode)
        self.assertEqual(-1, self.proxy.backend_listener.fileno())
        self.assertTrue(self.proxy.log.closed)


if __name__ == "__main__":
    orbit_binary = sys.argv.pop(1)
    ProxySessionIntegrationTest.orbit_binary = orbit_binary
    AdmissionIntegrationTest.orbit_binary = orbit_binary
    IdleExpirationIntegrationTest.orbit_binary = orbit_binary
    unittest.main(verbosity=2, buffer=True)
