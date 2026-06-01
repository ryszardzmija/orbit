import signal
import socket
import struct
import subprocess
import sys
import time
from contextlib import ExitStack


HEADER_SIZE = 4


def send_message(sock, payload):
    header = struct.pack("!I", len(payload))
    sock.sendall(header + payload)


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("connection closed before complete message arrived")
        data.extend(chunk)
    return bytes(data)


def recv_message(sock):
    header = recv_exact(sock, HEADER_SIZE)
    payload_size = struct.unpack("!I", header)[0]
    return recv_exact(sock, payload_size)


def set_up_listener():
    backend_listener = socket.socket()
    backend_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    backend_listener.bind(("127.0.0.1", 0))
    backend_listener.listen()
    backend_listener.settimeout(2)

    return backend_listener


def find_available_port():
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def connect_with_retry(port):
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=0.2)
            sock.settimeout(2)
            return sock
        except OSError:
            time.sleep(0.02)
    raise RuntimeError("proxy did not begin listening")


def main():
    orbit_binary = sys.argv[1]
    
    with ExitStack() as stack:
        backend_listener = stack.enter_context(set_up_listener())
        backend_port = backend_listener.getsockname()[1]
        proxy_port = find_available_port()

        proxy = subprocess.Popen(
            [
                orbit_binary,
                "--listen-host", "127.0.0.1",
                "--listen-port", str(proxy_port),
                "--upstream-host", "127.0.0.1",
                "--upstream-port", str(backend_port),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

        try:
            with ExitStack() as sessions:
                client_a = sessions.enter_context(connect_with_retry(proxy_port))
                client_b = sessions.enter_context(connect_with_retry(proxy_port))

                backend_1, _ = backend_listener.accept()
                backend_1 = sessions.enter_context(backend_1)
                backend_2, _ = backend_listener.accept()
                backend_2 = sessions.enter_context(backend_2)

                backend_1.settimeout(2)
                backend_2.settimeout(2)

                send_message(client_a, b"request-from-a")
                send_message(client_b, b"request-from-b")

                request_1 = recv_message(backend_1)
                request_2 = recv_message(backend_2)

                backend_by_request = {
                    request_1: backend_1,
                    request_2: backend_2,
                }

                assert set(backend_by_request) == {
                    b"request-from-a",
                    b"request-from-b",
                }

                send_message(backend_by_request[b"request-from-a"], b"response-for-a")
                send_message(backend_by_request[b"request-from-b"], b"response-for-b")

                assert recv_message(client_a) == b"response-for-a"
                assert recv_message(client_b) == b"response-for-b"

            proxy.send_signal(signal.SIGTERM)
            exit_code = proxy.wait(timeout=2)
            assert exit_code == 0
        finally:
            if proxy.poll() is None:
                proxy.kill()
            proxy.wait()
            print(proxy.stdout.read())


if __name__ == "__main__":
    main()
