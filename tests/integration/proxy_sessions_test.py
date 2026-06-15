import concurrent.futures
import os
import signal
import socket
import struct
import subprocess
import sys
import time
import unittest


HEADER_SIZE = 4
SOCKET_TIMEOUT = 5
ORBIT_BINARY = sys.argv.pop(1)
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
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError(
                f"connection closed after {len(data)} of {size} expected bytes arrived"
            )
        data.extend(chunk)
    return bytes(data)


def recv_message(sock):
    header = recv_exact(sock, HEADER_SIZE)
    payload_size = struct.unpack("!I", header)[0]
    return recv_exact(sock, payload_size)


def expect_eof(sock):
    data = sock.recv(1)
    if data:
        raise AssertionError(f"expected EOF, received {data!r}")


def set_up_listener():
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    listener.settimeout(SOCKET_TIMEOUT)
    return listener


def find_available_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def connect_with_retry(port):
    deadline = time.monotonic() + SOCKET_TIMEOUT
    while time.monotonic() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=0.2)
            sock.settimeout(SOCKET_TIMEOUT)
            return sock
        except OSError:
            time.sleep(0.02)
    raise RuntimeError("proxy did not begin listening")


class ProxyHarness:
    def __init__(self, orbit_binary):
        self.backend_listener = set_up_listener()
        backend_port = self.backend_listener.getsockname()[1]
        self.proxy_port = find_available_port()
        self.sockets = []
        self.output = None

        self.process = subprocess.Popen(
            [
                orbit_binary,
                "--listen-host",
                "127.0.0.1",
                "--listen-port",
                str(self.proxy_port),
                "--upstream-host",
                "127.0.0.1",
                "--upstream-port",
                str(backend_port),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

    def connect_session(self):
        client = connect_with_retry(self.proxy_port)
        backend, _ = self.backend_listener.accept()
        backend.settimeout(SOCKET_TIMEOUT)
        self.sockets.extend((client, backend))
        return client, backend

    def wait(self, timeout=SOCKET_TIMEOUT):
        exit_code = self.process.wait(timeout=timeout)
        self._collect_output()
        return exit_code

    def close(self):
        for sock in reversed(self.sockets):
            try:
                sock.close()
            except OSError:
                pass
        self.backend_listener.close()

        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

        if self.output is None:
            self._collect_output()

    def _collect_output(self):
        self.output = self.process.stdout.read()
        self.process.stdout.close()


def finish_session(client, backend):
    client.shutdown(socket.SHUT_WR)
    expect_eof(backend)
    backend.shutdown(socket.SHUT_WR)
    expect_eof(client)


# Large enough to exercise multi-read forwarding, small enough that sendall never blocks.
HANGUP_PAYLOAD = bytes(range(256)) * 512


class ProxySessionIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.orbit_binary = ORBIT_BINARY

    def setUp(self):
        self.proxy = ProxyHarness(self.orbit_binary)

    def tearDown(self):
        self.proxy.close()

    def assert_clean_exit(self):
        exit_code = self.proxy.wait()
        self.assertEqual(0, exit_code, self.proxy.output)

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
        self.proxy.process.send_signal(signal.SIGTERM)
        self.assert_clean_exit()

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
        self.proxy.process.send_signal(signal.SIGTERM)
        self.assert_clean_exit()

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

        self.proxy.process.send_signal(signal.SIGTERM)
        self.assert_clean_exit()

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

        self.proxy.process.send_signal(signal.SIGTERM)
        self.assert_clean_exit()

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
        self.proxy.process.send_signal(signal.SIGTERM)
        self.assert_clean_exit()

    def test_graceful_shutdown_waits_for_active_session(self):
        client, backend = self.proxy.connect_session()

        send_message(client, b"request-before-shutdown")
        self.assertEqual(b"request-before-shutdown", recv_message(backend))

        self.proxy.process.send_signal(signal.SIGTERM)
        time.sleep(0.05)
        self.assertIsNone(self.proxy.process.poll(), self.proxy.output)

        send_message(client, b"request-during-shutdown")
        self.assertEqual(b"request-during-shutdown", recv_message(backend))
        send_message(backend, b"response-during-shutdown")
        self.assertEqual(b"response-during-shutdown", recv_message(client))

        finish_session(client, backend)
        self.assert_clean_exit()

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

        self.proxy.process.send_signal(signal.SIGTERM)
        self.assert_clean_exit()

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

        self.proxy.process.send_signal(signal.SIGTERM)
        self.assert_clean_exit()

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
        self.proxy.process.send_signal(signal.SIGTERM)
        self.assert_clean_exit()

    def test_second_shutdown_signal_force_closes_active_sessions(self):
        client, backend = self.proxy.connect_session()

        send_message(client, b"request-before-hard-shutdown")
        self.assertEqual(b"request-before-hard-shutdown", recv_message(backend))

        self.proxy.process.send_signal(signal.SIGTERM)
        time.sleep(0.05)
        self.assertIsNone(self.proxy.process.poll(), self.proxy.output)

        self.proxy.process.send_signal(signal.SIGTERM)
        self.assert_clean_exit()
        expect_eof(client)
        expect_eof(backend)


if __name__ == "__main__":
    unittest.main(verbosity=2)
