import io
import json
import signal
import sys
import unittest
from contextlib import redirect_stderr
from unittest.mock import Mock, patch

from _support import child, exchange, wait_for_log
from throughput import receiver_result


class EchoTest(unittest.TestCase):
    def test_partial_reads_complete_the_response(self):
        sock = Mock()
        sock.recv.side_effect = [b"a", b"bc"]
        exchange(sock, b"abc")
        self.assertEqual([call.args for call in sock.recv.call_args_list], [(3,), (2,)])

    def test_truncation_and_corruption_fail(self):
        for chunks, diagnostic in (([b"a", b""], "closed"), ([b"bad"], "mismatch")):
            with self.subTest(chunks=chunks):
                sock = Mock()
                sock.recv.side_effect = chunks
                with self.assertRaisesRegex(RuntimeError, diagnostic):
                    exchange(sock, b"abc")


class ThroughputTest(unittest.TestCase):
    def test_uses_receiver_rate_and_checks_stream_count(self):
        data = {
            "streams": [{}] * 4,
            "sum_sent": {"bits_per_second": 9e9},
            "sum_received": {"bits_per_second": 8e9, "seconds": 10.1},
        }
        log = io.BytesIO(json.dumps({"event": "end", "data": data}).encode())
        self.assertEqual(receiver_result(log, 4), (8.0, 10.1))
        with self.assertRaisesRegex(RuntimeError, "no valid.*receiver summary"):
            receiver_result(log, 1)

    def test_error_in_iperf_output_rejects_trial(self):
        log = io.BytesIO(b'{"event":"error","data":"connection reset"}\n')
        with self.assertRaisesRegex(RuntimeError, "connection reset"):
            receiver_result(log, 1)


class ChildTest(unittest.TestCase):
    def test_early_child_exit_preserves_status_and_output(self):
        stderr = io.StringIO()
        command = [sys.executable, "-c", "print('startup failed'); raise SystemExit(3)"]
        with redirect_stderr(stderr), self.assertRaisesRegex(RuntimeError, "status 3"):
            with child(command) as (process, log):
                wait_for_log(process, log, b"ready")
        self.assertIn("startup failed\n", stderr.getvalue())
        self.assertEqual(process.returncode, 3)

    def test_unresponsive_child_is_killed_and_reaped(self):
        script = "import signal; signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
        script += "print('ready', flush=True)\nwhile True: signal.pause()"
        with redirect_stderr(io.StringIO()), self.assertRaisesRegex(RuntimeError, "SIGKILL"):
            with child([sys.executable, "-c", script]) as (process, log):
                wait_for_log(process, log, b"ready")
                cleanup_timeout = patch("_support.TIMEOUT", 0.1)
                cleanup_timeout.start()
                self.addCleanup(cleanup_timeout.stop)
        self.assertEqual(process.returncode, -signal.SIGKILL)


if __name__ == "__main__":
    unittest.main()
