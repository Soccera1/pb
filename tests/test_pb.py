#!/usr/bin/env python3
"""Black-box transfer and CLI regression tests; pv is an optional oracle."""
import json
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

PB = os.environ.get('PB_BINARY', str(Path(__file__).resolve().parents[1] / 'pb'))
PV = os.environ.get('PV_BINARY', shutil.which('pv'))


def run(*args, data=b'', binary=PB, **kw):
    return subprocess.run([binary, *map(str, args)], input=data,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=8, **kw)


class Transfer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.shim_tmp = tempfile.TemporaryDirectory()
        cls.shim = Path(cls.shim_tmp.name) / 'fault-io.so'
        cc = shutil.which('cc')
        if cc:
            source = Path(__file__).with_name('fault_io.c')
            subprocess.run([cc, '-shared', '-fPIC', '-o', str(cls.shim),
                            str(source), '-ldl'], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.shim_tmp.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.a = self.root / 'a'
        self.b = self.root / 'b'
        self.payload = bytes(range(256)) * 4097
        self.a.write_bytes(self.payload)
        self.b.write_bytes(b'end\n')

    def tearDown(self):
        self.tmp.cleanup()

    def test_unknown_size_pipeline(self):
        p = run(data=self.payload)
        self.assertEqual((p.returncode, p.stdout, p.stderr), (0, self.payload, b''))

    def test_concatenation_and_stdin(self):
        p = run('-q', self.a, '-', self.b, data=b'middle')
        self.assertEqual((p.returncode, p.stdout), (0, self.payload + b'middleend\n'))

    def test_empty_and_zero_size(self):
        for args in [[], ['-s', '0'], ['-s', '0', '-S']]:
            p = run(*args)
            self.assertEqual(p.returncode, 0, p.stderr)

    def test_totals_from_regular_files(self):
        p = run('-n', self.a, self.b)
        self.assertEqual(p.stderr, b'100\n')
        self.assertEqual(p.stdout, self.payload + b'end\n')

    def test_regular_stdin_offset(self):
        with self.a.open('rb') as f:
            f.seek(17)
            p = subprocess.run([PB, '-n'], stdin=f, capture_output=True, timeout=8)
        self.assertEqual(p.stdout, self.payload[17:])
        self.assertEqual(p.stderr, b'100\n')

    def test_line_and_null_counters(self):
        for flag, data, count in [('-l', b'one\ntwo\ntrailing', 2),
                                  ('-0', b'one\0two\0trailing', 2)]:
            p = run('-nb', flag, data=data)
            self.assertEqual(p.stdout, data)
            self.assertEqual(p.stderr, f'{count}\n'.encode())

    def test_line_total_inference(self):
        self.a.write_bytes(b'a\nb\nlast')
        p = run('-nl', self.a)
        self.assertEqual(p.stderr, b'100\n')

    def test_stop_bytes_and_lines(self):
        cases = [(['-s', '17'], self.payload, self.payload[:17]),
                 (['-s', '2', '-l'], b'a\nb\nc\n', b'a\nb\n'),
                 (['-s', '1', '-0'], b'a\0b\0', b'a\0'),
                 (['-s', '0'], self.payload, b'')]
        for args, data, expected in cases:
            p = run('-qS', *args, data=data)
            self.assertEqual((p.returncode, p.stdout), (0, expected))

    def test_size_is_estimate_without_stop(self):
        p = run('-q', '-s', '1', data=self.payload)
        self.assertEqual(p.stdout, self.payload)

    def test_suffixes_and_si_order(self):
        for args, size in [(['-s', '1K'], 1024), (['-k', '-s', '1K'], 1000),
                           (['-s', '1.5K'], 1536), (['-s', '1KiB'], 1024)]:
            p = run('-qS', *args, data=self.payload)
            self.assertEqual(len(p.stdout), size)

    def test_output_file_and_truncation(self):
        target = self.root / 'output'
        target.write_bytes(b'old' * 1000)
        p = run('-q', '-o', target, self.b)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(target.read_bytes(), b'end\n')
        self.assertEqual(p.stdout, b'')

    def test_same_file_preserved(self):
        for path in [self.a, self.root / 'link']:
            if path != self.a:
                path.symlink_to(self.a)
            p = run('-q', '-o', path, self.a)
            self.assertEqual(p.returncode, 4)
            self.assertEqual(self.a.read_bytes(), self.payload)

    def test_missing_file_continues(self):
        p = run('-q', self.root / 'absent', self.b)
        self.assertEqual((p.returncode, p.stdout), (2, b'end\n'))
        self.assertIn(b'absent', p.stderr)

    def test_line_rate_limit(self):
        start = time.monotonic()
        p = run('-ql', '-L', '10', data=b'a\n' * 5)
        elapsed = time.monotonic() - start
        self.assertEqual(p.stdout, b'a\n' * 5)
        self.assertGreaterEqual(elapsed, .45)
        self.assertLess(elapsed, .9)

    def test_skip_errors_directory_does_not_loop(self):
        p = run('-qE', self.root)
        self.assertEqual(p.returncode, 16)

    def test_termination_unopened_fifo(self):
        fifo = self.root / 'fifo'
        os.mkfifo(fifo)
        p = subprocess.Popen([PB, '-q', str(fifo)], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
        try:
            time.sleep(.05)
            p.terminate()
            self.assertEqual(p.wait(timeout=3), 32)
        finally:
            if p.poll() is None:
                p.kill()
            p.communicate()

    def test_read_error(self):
        p = run('-q', self.root)
        self.assertEqual(p.returncode, 16)
        self.assertIn(b'directory', p.stderr.lower())

    def test_read_error_continues_to_next_input(self):
        p = run('-q', self.root, self.b)
        self.assertEqual((p.returncode, p.stdout), (16, b'end\n'))
        p = run('-qU', '-', self.root, self.b)
        self.assertEqual((p.returncode, p.stdout), (16, b'end\n'))

    def test_write_error(self):
        if not Path('/dev/full').exists():
            self.skipTest('requires /dev/full')
        p = run('-q', '-o', '/dev/full', data=b'hello')
        self.assertEqual(p.returncode, 16)

    def test_discard(self):
        p = run('-Xnb', self.a)
        self.assertEqual((p.returncode, p.stdout), (0, b''))
        self.assertEqual(p.stderr, f'{len(self.payload)}\n'.encode())

    def test_store_and_forward_named_and_temporary(self):
        for name in ['-', str(self.root / 'spool')]:
            p = run('-qU', name, self.a)
            self.assertEqual((p.returncode, p.stdout), (0, self.payload))
            if name != '-':
                self.assertEqual(Path(name).read_bytes(), self.payload)

    def test_store_waits_for_eof(self):
        p = subprocess.Popen([PB, '-qU', '-'], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            p.stdin.write(b'first part')
            p.stdin.flush()
            self.assertEqual(select.select([p.stdout], [], [], .15)[0], [])
            out, err = p.communicate(b'last part', timeout=3)
            self.assertEqual((p.returncode, out, err), (0, b'first partlast part', b''))
        finally:
            if p.poll() is None:
                p.kill()
                p.communicate()

    def test_store_aliases_preserve_files(self):
        link = self.root / 'alias'
        link.symlink_to(self.a)
        p = run('-qU', link, self.a)
        self.assertEqual(p.returncode, 4)
        self.assertEqual(self.a.read_bytes(), self.payload)
        p = run('-qU', self.a, '-o', link, data=b'new')
        self.assertEqual(p.returncode, 4)
        self.assertEqual(self.a.read_bytes(), self.payload)

    def test_store_progress_uses_actual_size(self):
        p = run('-nU', '-', data=b'unknown length')
        self.assertEqual((p.returncode, p.stdout), (0, b'unknown length'))
        self.assertEqual(p.stderr.splitlines()[-1], b'100')

    def test_store_discard(self):
        spool = self.root / 'spool'
        p = run('-qXU', spool, data=b'data')
        self.assertEqual((p.returncode, p.stdout, spool.read_bytes()), (0, b'', b'data'))

    def test_recover_read_errors(self):
        if not self.shim.exists():
            self.skipTest('C compiler unavailable')
        faulty = self.root / 'fault-input'
        faulty.write_bytes(b'abcdefghijklmnopqrst')
        env = {**os.environ, 'LD_PRELOAD': str(self.shim)}
        for flags in ['-qE', '-qEE']:
            p = run(flags, '-Z', '4', faulty, env=env)
            self.assertEqual(p.returncode, 16)
            self.assertEqual(p.stdout, b'abcdefgh' + bytes(4) + b'mnopqrst')

    def test_partial_writes_and_interrupted_write(self):
        if not self.shim.exists():
            self.skipTest('C compiler unavailable')
        env = {**os.environ, 'LD_PRELOAD': str(self.shim), 'PB_TEST_PARTIAL': '1'}
        p = run('-qC', data=self.payload[:10000], env=env)
        self.assertEqual((p.returncode, p.stdout), (0, self.payload[:10000]))

    def test_sparse_overwrites_existing_bytes(self):
        target = self.root / 'existing'
        target.write_bytes(b'x' * 10000)
        with target.open('r+b') as f:
            p = subprocess.run([PB, '-qO'], input=bytes(4096), stdout=f,
                               stderr=subprocess.PIPE, timeout=3)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(target.read_bytes(), bytes(4096) + b'x' * 5904)

    def test_sparse_trailing_hole(self):
        target = self.root / 'sparse'
        data = b'prefix' + bytes(1024 * 1024)
        p = run('-qO', '-B', '4K', '-o', target, data=data)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(target.read_bytes(), data)
        self.assertLess(target.stat().st_blocks * 512, len(data) // 2)

    def test_sparse_pipe_fallback(self):
        p = run('-qO', data=bytes(12345))
        self.assertEqual(p.stdout, bytes(12345))

    def test_sync_regular_and_pipe(self):
        p = run('-qY', data=b'hello')
        self.assertEqual((p.returncode, p.stdout), (0, b'hello'))
        target = self.root / 'synced'
        p = run('-qY', '-o', target, data=b'hello')
        self.assertEqual((p.returncode, target.read_bytes()), (0, b'hello'))

    def test_buffer_boundaries(self):
        for size in ['1', '17', '4K', '128K']:
            p = run('-q', '-B', size, data=self.payload[:30000])
            self.assertEqual((p.returncode, p.stdout), (0, self.payload[:30000]))

    def test_rate_limit(self):
        start = time.monotonic()
        p = run('-q', '-L', '2000', data=b'x' * 1000)
        elapsed = time.monotonic() - start
        self.assertEqual(p.stdout, b'x' * 1000)
        self.assertGreaterEqual(elapsed, .45)
        self.assertLess(elapsed, 3)

    def test_numeric_format(self):
        p = run('-n', '-s', '4', '-F', '{"bytes":%b,"pct":%{progress-amount-only},"elapsed":%t}', data=b'abcd')
        record = json.loads(p.stderr)
        self.assertEqual(record['bytes'], 4)
        self.assertEqual(record['pct'], 100)
        self.assertGreaterEqual(record['elapsed'], 0)
        self.assertEqual(p.stdout, b'abcd')

    def test_last_written(self):
        p = run('-f', '-A', '5', '-F', '%5A', data=b'first\nlast\0')
        self.assertEqual(p.stderr.strip(), b'last.')

    def test_bits(self):
        p = run('-n8b', data=b'abcd')
        self.assertEqual(p.stderr, b'32\n')

    def test_delay_and_wait(self):
        p = run('-n', '-D', '5', data=b'fast')
        self.assertEqual(p.stderr, b'')
        p = run('-nW')
        self.assertEqual(p.stderr, b'')

    def test_pidfile_lifecycle(self):
        pidfile = self.root / 'pid'
        p = subprocess.Popen([PB, '-q', '-P', str(pidfile)], stdin=subprocess.PIPE,
                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 3
            while not pidfile.exists() and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertEqual(int(pidfile.read_text()), p.pid)
            p.communicate(b'done', timeout=3)
            self.assertEqual(p.returncode, 0)
            self.assertFalse(pidfile.exists())
        finally:
            if p.poll() is None:
                p.kill()
                p.communicate()

    def test_termination_stalled_input(self):
        p = subprocess.Popen([PB, '-q'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
        try:
            time.sleep(.05)
            p.send_signal(signal.SIGTERM)
            self.assertEqual(p.wait(timeout=3), 32)
        finally:
            if p.poll() is None:
                p.kill()
            p.communicate()

    def test_backpressure_termination(self):
        p = subprocess.Popen([PB, '-q', self.a], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
        try:
            time.sleep(.1)
            p.terminate()
            self.assertEqual(p.wait(timeout=3), 32)
        finally:
            if p.poll() is None:
                p.kill()
            p.communicate()

    def test_terminal_large_display(self):
        master, slave = pty.openpty()
        try:
            p = subprocess.Popen([PB, '-s', '4', '-w', '80'], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=slave)
            p.communicate(b'test', timeout=3)
            output = os.read(master, 65536)
            self.assertIn('┌'.encode(), output)
            self.assertIn(b'total', output)
            self.assertEqual(p.returncode, 0)
        finally:
            os.close(master)
            os.close(slave)

    def test_invalid_values(self):
        for args in [('-s', '-1'), ('-s', 'junk'), ('-s', '1junk'),
                     ('-s', '999999999999999999999999'), ('-B', '0'),
                     ('-i', '0'), ('-i', 'nan'), ('-i', 'inf'),
                     ('-w', '18446744073709551616'), ('-A', '999999999999999'),
                     ('-m', '1.5'), ('-w', '1e2'), ('-s', '1e3'), ('-Z', '4K'), ('-u', 'bogus')]:
            with self.subTest(args=args):
                self.assertNotEqual(run(*args).returncode, 0)

    @unittest.skipUnless(PV, 'pv oracle not installed')
    def test_pv_data_and_status_compatibility(self):
        cases = [[], ['-q'], ['-qS', '-s', '31'], ['-qS', '-s', '1K'],
                 ['-qS', '-s', '1.5K'], ['-qS', '-k', '-s', '1K'],
                 ['-qX'], ['-qO'], ['-q', '-B', '7'], ['-qY'],
                 ['-qU', '-'], ['-qS', '-l', '-s', '3'], ['-qS', '-0', '-s', '2']]
        for args in cases:
            with self.subTest(args=args):
                actual = run(*args, data=self.payload)
                expected = run(*args, data=self.payload, binary=PV)
                self.assertEqual((actual.returncode, actual.stdout),
                                 (expected.returncode, expected.stdout))

    @unittest.skipUnless(PV, 'pv oracle not installed')
    def test_pv_numeric_compatibility(self):
        for args in [['-nb'], ['-nb8'], ['-nlb'], ['-n0b'], ['-n', '-s', '10']]:
            with self.subTest(args=args):
                actual = run(*args, data=b'a\nb\0c\n')
                expected = run(*args, data=b'a\nb\0c\n', binary=PV)
                self.assertEqual(actual.stderr, expected.stderr)


class Remote(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.processes = []
        self.streams = []

    def tearDown(self):
        for p in self.processes:
            if p.poll() is None:
                p.kill()
            p.communicate()
        for f in self.streams:
            f.close()
        self.tmp.cleanup()

    def start(self, *args, data=None):
        pidfile = self.root / f'pid-{len(self.processes)}'
        p = subprocess.Popen([PB, '-P', str(pidfile), *map(str, args)],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
        self.processes.append(p)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if pidfile.exists() and run('-R', p.pid).returncode == 0:
                break
            if p.poll() is not None:
                self.fail(f'target exited: {p.stderr.read()!r}')
            time.sleep(.01)
        else:
            self.fail('target control socket never became ready')
        if data:
            p.stdin.write(data)
            p.stdin.flush()
        return p

    def test_remote_rate_change_and_disable(self):
        p = self.start('-q', '-L', '1000', data=b'x' * 4000)
        time.sleep(.1)
        start = time.monotonic()
        update = run('-R', p.pid, '-L', '0', '-B', '17')
        self.assertEqual(update.returncode, 0, update.stderr)
        out, err = p.communicate(timeout=2)
        self.assertEqual((p.returncode, out, err), (0, b'x' * 4000, b''))
        self.assertLess(time.monotonic() - start, 1)

    def test_remote_size_updates_stop_at_size(self):
        p = self.start('-qS', '-s', '4000', '-L', '1000', data=b'x' * 4000)
        update = run('-R', p.pid, '-s', '500')
        self.assertEqual(update.returncode, 0, update.stderr)
        out, _ = p.communicate(timeout=2)
        self.assertEqual((p.returncode, len(out)), (0, 500))

    def test_remote_display_settings(self):
        p = self.start('-nb', '-i', '.05', data=b'hello')
        update = run('-R', p.pid, '-F', 'changed:%b', '-N', 'new', '-s', '5')
        self.assertEqual(update.returncode, 0, update.stderr)
        out, err = p.communicate(timeout=2)
        self.assertEqual(out, b'hello')
        self.assertIn(b'changed:5', err)

    def test_continuous_query_does_not_consume_data(self):
        p = self.start('-q', '-s', '4000', '-L', '1000', data=b'x' * 4000)
        q = subprocess.Popen([PB, '-Q', str(p.pid), '-nb', '-i', '.05'],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.processes.append(q)
        self.assertTrue(select.select([q.stderr], [], [], 2)[0])
        first = int(q.stderr.readline())
        self.assertGreater(first, 0)
        self.assertLess(first, 4000)
        self.assertEqual(run('-R', p.pid, '-L', '0').returncode, 0)
        out, _ = p.communicate(timeout=2)
        query_out, query_err = q.communicate(timeout=2)
        self.assertEqual((p.returncode, out), (0, b'x' * 4000))
        self.assertEqual((q.returncode, query_out), (0, b''))
        self.assertTrue(all(line.isdigit() for line in query_err.splitlines()))
        self.assertEqual(query_err.splitlines()[-1], b'4000')

    def test_query_previous_line(self):
        p = self.start('-qC', data=b'first\nsecond\npartial')
        q = subprocess.Popen([PB, '-Q', str(p.pid), '-nF', '%10L', '-i', '.02'],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.processes.append(q)
        self.assertTrue(select.select([q.stderr], [], [], 2)[0])
        self.assertEqual(q.stderr.readline(), b'second    \n')
        p.communicate(timeout=2)
        q.communicate(timeout=2)
        self.assertEqual(q.returncode, 0)

    def test_query_matches_line_mode(self):
        p = self.start('-ql', data=b'a\nb\n')
        mismatch = run('-Q', p.pid, '-nb', '-i', '.01')
        self.assertEqual(mismatch.returncode, 1)
        q = subprocess.Popen([PB, '-Q', str(p.pid), '-nlb', '-i', '.02'],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.processes.append(q)
        self.assertTrue(select.select([q.stderr], [], [], 2)[0])
        self.assertEqual(q.stderr.readline(), b'2\n')
        p.communicate(timeout=2)
        q.communicate(timeout=2)
        self.assertEqual(q.returncode, 0)

    def test_query_rejects_changed_averaging_window(self):
        p = self.start('-q', data=b'data')
        q = subprocess.Popen([PB, '-Q', str(p.pid), '-nb', '-i', '.02'],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.processes.append(q)
        self.assertTrue(select.select([q.stderr], [], [], 2)[0])
        q.stderr.readline()
        self.assertEqual(run('-R', p.pid, '-m', '1').returncode, 0)
        _, err = q.communicate(timeout=2)
        self.assertEqual(q.returncode, 1)
        self.assertIn(b'no longer match', err)
        p.communicate(timeout=2)

    def test_remote_preserves_unspecified_options(self):
        p = self.start('-n8b', data=b'hello')
        self.assertEqual(run('-R', p.pid, '-B', '7').returncode, 0)
        out, err = p.communicate(timeout=2)
        self.assertEqual((out, err.splitlines()[-1]), (b'hello', b'40'))

    def test_query_receives_final_before_first_display_interval(self):
        p = self.start('-q', data=b'final bytes')
        q = subprocess.Popen([PB, '-Q', str(p.pid), '-nb', '-i', '5'],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.processes.append(q)
        time.sleep(.08)
        p.communicate(timeout=2)
        out, err = q.communicate(timeout=2)
        self.assertEqual((q.returncode, out, err), (0, b'', b'11\n'))

    def test_multiple_query_subscribers(self):
        p = self.start('-q', data=b'hello')
        queries = []
        for _ in range(3):
            q = subprocess.Popen([PB, '-Q', str(p.pid), '-nb', '-i', '.02'],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.processes.append(q)
            queries.append(q)
            self.assertTrue(select.select([q.stderr], [], [], 2)[0])
            self.assertEqual(q.stderr.readline(), b'5\n')
        p.communicate(timeout=2)
        for q in queries:
            out, err = q.communicate(timeout=2)
            self.assertEqual((q.returncode, out), (0, b''))
            self.assertEqual(err.splitlines()[-1], b'5')

    def test_malformed_control_packet_does_not_break_transfer(self):
        p = self.start('-q', data=b'untouched')
        path = Path(f'/tmp/pb-{os.getuid()}') / f'{p.pid}.sock'
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
            client.settimeout(2)
            client.connect(str(path))
            client.send(b'invalid')
            reply = client.recv(65536)
        version, error = struct.unpack_from('Ii', reply)
        self.assertEqual(version, 6)
        self.assertNotEqual(error, 0)
        out, _ = p.communicate(timeout=2)
        self.assertEqual((p.returncode, out), (0, b'untouched'))

    def test_query_interrupt_leaves_target_running(self):
        p = self.start('-q', data=b'unchanged')
        q = subprocess.Popen([PB, '-Q', str(p.pid), '-nb', '-i', '.02'],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.processes.append(q)
        self.assertTrue(select.select([q.stderr], [], [], 2)[0])
        q.terminate()
        q.communicate(timeout=2)
        self.assertEqual(q.returncode, 32)
        self.assertIsNone(p.poll())
        out, _ = p.communicate(timeout=2)
        self.assertEqual(out, b'unchanged')

    def test_dead_target_and_cleanup(self):
        p = self.start('-q')
        p.communicate(b'done', timeout=2)
        self.assertEqual(run('-R', p.pid, '-L', '0').returncode, 1)
        self.assertEqual(run('-Q', p.pid, '-n').returncode, 1)
        self.assertFalse((Path(f'/tmp/pb-{os.getuid()}') / f'{p.pid}.sock').exists())

    def test_invalid_pid_and_mutually_exclusive_modes(self):
        for args in [('-R', '0'), ('-Q', '-1'), ('-R', '4294967295'),
                     ('-R', '1', '-Q', '2')]:
            self.assertEqual(run(*args).returncode, 64)


class Watch(unittest.TestCase):
    CHILD = r"""
import ctypes, os, sys
ctypes.CDLL(None).prctl(15, sys.argv[3].encode(), 0, 0, 0)
fd = os.open(sys.argv[1], os.O_RDONLY)
print(fd, flush=True)
for line in sys.stdin:
    op, _, arg = line.strip().partition(' ')
    if op == 'read': os.read(fd, int(arg))
    elif op == 'seek': os.lseek(fd, int(arg), os.SEEK_SET)
    elif op == 'close': os.close(fd); fd = -1
    elif op == 'open': fd = os.open(sys.argv[2], os.O_RDONLY)
    elif op == 'replace':
        os.close(fd)
        fd = os.open(sys.argv[2], os.O_RDONLY)
    elif op == 'exit': break
    print(os.lseek(fd, 0, os.SEEK_CUR) if fd >= 0 else -1, flush=True)
"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.a = self.root / 'first'
        self.b = self.root / 'second'
        self.a.write_bytes(b'a' * 1000)
        self.b.write_bytes(b'b' * 2000)
        self.name = f'pbw{os.getpid()}'[:15]
        self.child = subprocess.Popen([sys.executable, '-u', '-c', self.CHILD,
                                       str(self.a), str(self.b), self.name],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE)
        self.fd = int(self.child.stdout.readline())
        self.watchers = []

    def tearDown(self):
        for p in [*self.watchers, self.child]:
            if p.poll() is None:
                p.kill()
            p.communicate()
        self.tmp.cleanup()

    def command(self, text):
        self.child.stdin.write((text + '\n').encode())
        self.child.stdin.flush()
        return int(self.child.stdout.readline())

    def start(self, spec, *args):
        p = subprocess.Popen([PB, '-d', str(spec), '-nb', '-i', '.02', *args],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.watchers.append(p)
        return p

    def expect_line(self, p, expected):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if select.select([p.stderr], [], [], .1)[0]:
                line = p.stderr.readline()
                if line.strip() == expected:
                    return
                if not line:
                    self.fail(f'watcher exited early: {p.poll()}')
        self.fail(f'watcher never reported {expected!r}')

    def test_descriptor_positions_without_consuming_file(self):
        p = self.start(f'{self.child.pid}:{self.fd}')
        self.expect_line(p, b'0')
        self.assertEqual(self.command('read 137'), 137)
        self.expect_line(p, b'137')
        self.assertEqual(self.command('pos'), 137)
        self.command('close')
        out, _ = p.communicate(timeout=2)
        self.assertEqual((p.returncode, out), (0, b''))
        self.assertIsNone(self.child.poll())

    def test_watch_waits_for_position_to_change(self):
        self.command('seek 10')
        p = self.start(f'{self.child.pid}:{self.fd}', '-W')
        self.assertEqual(select.select([p.stderr], [], [], .1)[0], [])
        self.command('read 7')
        self.expect_line(p, b'17')
        self.command('close')
        p.communicate(timeout=2)
        self.assertEqual(p.returncode, 0)

    def test_descriptor_reuse_finishes_specific_watch(self):
        p = self.start(f'{self.child.pid}:{self.fd}')
        self.expect_line(p, b'0')
        self.command('replace')
        p.communicate(timeout=2)
        self.assertEqual(p.returncode, 0)

    def test_process_watch_discovers_new_files(self):
        p = self.start(self.child.pid, '-F', '%N %b')
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if select.select([p.stderr], [], [], .1)[0] and b'first' in p.stderr.readline():
                break
        else:
            self.fail('initial file not discovered')
        self.command('replace')
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if select.select([p.stderr], [], [], .1)[0] and b'second' in p.stderr.readline():
                break
        else:
            self.fail('new file not discovered')
        self.child.communicate(timeout=2)
        p.communicate(timeout=2)
        self.assertEqual(p.returncode, 0)

    def test_process_watch_waits_with_no_open_files(self):
        p = self.start(self.child.pid)
        self.expect_line(p, b'0')
        self.command('close')
        time.sleep(.06)
        self.assertIsNone(p.poll())
        self.command('open')
        self.command('read 41')
        self.expect_line(p, b'41')
        self.child.communicate(timeout=2)
        p.communicate(timeout=2)
        self.assertEqual(p.returncode, 0)

    def test_unlinked_open_file_keeps_being_watched(self):
        p = self.start(f'{self.child.pid}:{self.fd}')
        self.expect_line(p, b'0')
        self.a.unlink()
        self.command('read 29')
        self.expect_line(p, b'29')
        self.command('close')
        p.communicate(timeout=2)
        self.assertEqual(p.returncode, 0)

    def test_process_name(self):
        p = self.start('=' + self.name)
        self.expect_line(p, b'0')
        self.command('read 23')
        self.expect_line(p, b'23')
        self.child.communicate(timeout=2)
        p.communicate(timeout=2)
        self.assertEqual(p.returncode, 0)

    def test_nested_list_and_size_override(self):
        inner = self.root / 'inner'
        outer = self.root / 'outer'
        inner.write_text(f'{self.child.pid}:{self.fd}\n')
        outer.write_text(f'@{inner}\n')
        p = self.start('@' + str(outer), '-s', '200', '-F', '%{progress-amount-only}')
        self.expect_line(p, b'0')
        self.command('seek 100')
        self.expect_line(p, b'50')
        self.command('close')
        p.communicate(timeout=2)
        self.assertEqual(p.returncode, 0)

    def test_watch_rejects_transfer_modes(self):
        for args in [('-L', '100'), ('-l',), ('-B', '4K'), ('-R', '1')]:
            p = run('-d', f'{self.child.pid}:{self.fd}', *args)
            self.assertEqual(p.returncode, 64)

    def test_missing_pid_and_recursive_list(self):
        self.assertEqual(run('-d', '2147483647:0').returncode, 2)
        recursive = self.root / 'recursive'
        recursive.write_text('@' + str(recursive) + '\n')
        self.assertEqual(run('-d', '@' + str(recursive)).returncode, 2)


class Monitor(unittest.TestCase):
    def test_all_sides_and_aliases_preserve_data(self):
        data = bytes(range(256)) * 2000
        for side in ['in', 'out', 'both', '0', '1', '2']:
            with self.subTest(side=side):
                p = run('-qM', side, '--', 'cat', data=data)
                self.assertEqual((p.returncode, p.stdout, p.stderr), (0, data, b''))

    def test_output_only_command_without_input(self):
        p = run('-qM', 'out', '--', sys.executable, '-c',
                'import sys; sys.stdout.buffer.write(b"generated" * 20000)')
        self.assertEqual((p.returncode, p.stdout), (0, b'generated' * 20000))

    def test_two_formats_and_input_output_counts(self):
        p = run('-n', '-F', 'input:%b', '-F', 'output:%b', '-M', 'both', '--',
                sys.executable, '-c',
                'import sys; sys.stdout.buffer.write(sys.stdin.buffer.read() * 2)',
                data=b'abcdefghij')
        self.assertEqual((p.returncode, p.stdout), (0, b'abcdefghij' * 2))
        self.assertIn(b'input:10', p.stderr)
        self.assertIn(b'output:20', p.stderr)

    def test_ratio_format(self):
        p = run('-f', '-F', 'input', '-F', 'ratio=%{ratio}', '-M', 'both', '--',
                sys.executable, '-c',
                'import sys; sys.stdout.buffer.write(sys.stdin.buffer.read() * 2)',
                data=b'abc')
        self.assertEqual(p.stdout, b'abcabc')
        self.assertIn(b'ratio=1:2', p.stderr)

    def test_output_file_all_sides(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / 'output'
            for side in ['in', 'out', 'both']:
                p = run('-q', '-o', target, '-M', side, '--', 'cat', data=b'file data')
                self.assertEqual((p.returncode, p.stdout, target.read_bytes()),
                                 (0, b'', b'file data'))

    def test_consumer_exits_early(self):
        for side in ['in', 'out', 'both']:
            p = run('-qM', side, '--', 'head', '-c', '1', data=b'x' * 1000000)
            self.assertEqual((p.returncode, p.stdout, p.stderr), (0, b'x', b''))

    def test_command_status_matches_pv(self):
        if not PV:
            self.skipTest('pv oracle not installed')
        for side in ['in', 'out', 'both']:
            args = ['-qM', side, '--', 'sh', '-c', 'exit 7']
            expected = run(*args, binary=PV)
            actual = run(*args)
            self.assertEqual((actual.returncode, actual.stdout),
                             (expected.returncode, expected.stdout))

    def test_monitor_startup_error_cleans_up(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = run('-q', '-P', Path(tmp) / 'missing' / 'pid', '-M', 'both', '--', 'cat')
            self.assertEqual(p.returncode, 1)

    def test_monitor_output_error_terminates_command(self):
        if not Path('/dev/full').exists():
            self.skipTest('requires /dev/full')
        script = 'import sys; sys.stdout.buffer.write(b"x" * 1000000)'
        for side in ['out', 'both']:
            p = run('-q', '-o', '/dev/full', '-M', side, '--', sys.executable, '-c', script)
            self.assertNotEqual(p.returncode, 0)
            self.assertIn(b'No space left', p.stderr)

    def test_invalid_monitor_modes(self):
        for args in [('-M', 'unknown'), ('-M', 'in'),
                     ('-U', '-', '-M', 'both', '--', 'cat'),
                     ('-R', '1', '-M', 'both', '--', 'cat')]:
            self.assertEqual(run(*args).returncode, 64)

    def test_both_pidfiles_and_query_output_monitor(self):
        with tempfile.TemporaryDirectory() as tmp:
            first, second = Path(tmp) / 'input-pid', Path(tmp) / 'output-pid'
            p = subprocess.Popen([PB, '-q', '-P', str(first), '-P', str(second),
                                  '-M', 'both', '--', 'cat'],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
            q = None
            try:
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline and not (first.exists() and second.exists()):
                    time.sleep(.01)
                self.assertEqual(int(first.read_text()), p.pid)
                output_pid = int(second.read_text())
                self.assertNotEqual(output_pid, p.pid)
                p.stdin.write(b'hello')
                p.stdin.flush()
                q = subprocess.Popen([PB, '-Q', str(output_pid), '-nb', '-i', '.02'],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                self.assertTrue(select.select([q.stderr], [], [], 2)[0])
                out, err = p.communicate(timeout=3)
                qo, qe = q.communicate(timeout=3)
                self.assertEqual((p.returncode, out, err), (0, b'hello', b''))
                self.assertEqual((q.returncode, qo, qe.splitlines()[-1]), (0, b'', b'5'))
                self.assertFalse(first.exists())
                self.assertFalse(second.exists())
            finally:
                for child in [p, q]:
                    if child is not None:
                        if child.poll() is None:
                            child.kill()
                        child.communicate()

    def test_termination_reaps_command_and_output_monitor(self):
        with tempfile.TemporaryDirectory() as tmp:
            command_pidfile = Path(tmp) / 'command'
            output_pidfile = Path(tmp) / 'output'
            script = ('import os,time; from pathlib import Path; '
                      f'Path({str(command_pidfile)!r}).write_text(str(os.getpid())); '
                      'time.sleep(20)')
            p = subprocess.Popen([PB, '-q', '-P', str(Path(tmp) / 'parent'),
                                  '-P', str(output_pidfile), '-M', 'both', '--',
                                  sys.executable, '-c', script], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline and not (command_pidfile.exists() and output_pidfile.exists()):
                    time.sleep(.01)
                command_pid = int(command_pidfile.read_text())
                output_pid = int(output_pidfile.read_text())
                p.terminate()
                p.communicate(timeout=3)
                self.assertEqual(p.returncode, 32)
                for pid in [command_pid, output_pid]:
                    self.assertFalse(Path(f'/proc/{pid}').exists())
            finally:
                if p.poll() is None:
                    p.kill()
                p.communicate()


if __name__ == '__main__':
    unittest.main(verbosity=2)
