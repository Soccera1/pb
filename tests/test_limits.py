"""Regression tests above the old fixed display/control/history limits."""
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time
import unittest

from test_pb import PB, run


class Limits(unittest.TestCase):
    def test_large_last_written_ring_wraps_without_losing_bytes(self):
        amount = 2 * 1024 * 1024
        data = b'0123456789abcdef' * (3 * 1024 * 1024 // 16)
        p = run('-f', '-A', amount, '-F', f'%{amount}A', data=data)
        self.assertEqual((p.returncode, p.stdout), (0, data))
        self.assertEqual(p.stderr.strip(b'\r\n'), data[-amount:])

    def test_large_previous_line_prefix(self):
        amount = 2 * 1024 * 1024
        data = b'L' * (amount + 100) + b'\npartial'
        p = run('-fC', '-F', f'%{amount}L', data=data)
        self.assertEqual((p.returncode, p.stdout), (0, data))
        self.assertEqual(p.stderr.strip(b'\r\n'), b'L' * amount)

    def test_width_and_height_above_old_limit(self):
        p = run('-fp', '-w', '20000', '-H', '20000', '-s', '10', data=b'abcde')
        self.assertEqual(p.returncode, 0, p.stderr[:100])
        self.assertEqual(len(p.stderr.strip(b'\r\n')), 20000)
        self.assertIn(b'50%', p.stderr)
        self.assertEqual(run('-q', '-w', '4294967297', '-H', '4294967297').returncode, 0)

    def ready(self, p, pidfile):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if pidfile.exists() and run('-R', p.pid).returncode == 0:
                return
            if p.poll() is not None:
                self.fail(f'target exited with {p.returncode}')
            time.sleep(.01)
        self.fail('control socket did not become ready')

    def test_large_remote_name_and_format(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pidfile, log = root / 'pid', root / 'log'
            with log.open('wb') as stderr:
                p = subprocess.Popen([PB, '-nb', '-i', '0.02', '-P', str(pidfile)],
                                     stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=stderr)
                try:
                    self.ready(p, pidfile)
                    p.stdin.write(b'hello')
                    p.stdin.flush()
                    name = 'name' * 1000
                    literal = 'long-format-' * 3000
                    update = run('-R', p.pid, '-N', name, '-F', '%N|' + literal + '|%b')
                    self.assertEqual(update.returncode, 0, update.stderr)
                    p.communicate(timeout=3)
                    output = log.read_bytes()
                    self.assertIn(name.encode() + b':|' + literal.encode() + b'|5', output)
                finally:
                    if p.poll() is None: p.kill()
                    p.communicate()

    def query_history(self, kind):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            amount = 300000  # Larger than both the old history frame and a socket packet.
            pidfile, query_log = root / 'pid', root / 'query'
            if kind == 'A':
                data = b'0123456789abcdef' * 25000
                arguments = ['-A', str(amount)]
                expected = data[-amount:]
            else:
                data = b'line' * 80000 + b'\npartial'
                arguments = ['-F', f'%{amount}L']
                expected = data[:amount]
            p = subprocess.Popen([PB, '-qC', '-P', str(pidfile), *arguments],
                                 stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
            q = None
            try:
                self.ready(p, pidfile)
                p.stdin.write(data)
                p.stdin.flush()
                time.sleep(.05)
                with query_log.open('wb') as output:
                    q = subprocess.Popen([PB, '-Q', str(p.pid), '-nF', f'%{amount}{kind}', '-i', '0.03'],
                                         stdout=subprocess.PIPE, stderr=output)
                    deadline = time.monotonic() + 3
                    while query_log.stat().st_size < amount + 1 and time.monotonic() < deadline:
                        time.sleep(.01)
                    self.assertGreaterEqual(query_log.stat().st_size, amount + 1)
                    p.communicate(timeout=3)
                    q.communicate(timeout=3)
                self.assertEqual((p.returncode, q.returncode), (0, 0))
                self.assertEqual(query_log.read_bytes().splitlines()[-1], expected)
            finally:
                for child in [p, q]:
                    if child is not None:
                        if child.poll() is None: child.kill()
                        child.communicate()

    def test_large_queried_last_written_history(self):
        self.query_history('A')

    def test_large_queried_previous_line(self):
        self.query_history('L')

    def test_remote_no_splice_changes_engine_without_losing_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pidfile, destination = root / 'pid', root / 'data'
            p = subprocess.Popen([PB, '-q', '-P', str(pidfile), '-o', str(destination)],
                                 stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
            q = None
            try:
                self.ready(p, pidfile)
                p.stdin.write(b'hello')
                p.stdin.flush()
                q = subprocess.Popen([PB, '-Q', str(p.pid), '-nF', '%T', '-i', '0.02'],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                self.assertTrue(select.select([q.stderr], [], [], 2)[0])
                self.assertEqual(q.stderr.readline(), b'{----}\n')
                self.assertEqual(run('-R', p.pid, '-C').returncode, 0)
                p.stdin.write(b'world')
                p.stdin.flush()
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    if select.select([q.stderr], [], [], .1)[0] and q.stderr.readline() == b'{  0%}\n':
                        break
                else:
                    self.fail('remote --no-splice did not change the engine')
                p.communicate(timeout=3)
                q.communicate(timeout=3)
                self.assertEqual(destination.read_bytes(), b'helloworld')
            finally:
                for child in [p, q]:
                    if child is not None:
                        if child.poll() is None: child.kill()
                        child.communicate()


if __name__ == '__main__':
    unittest.main(verbosity=2)
