"""Conformance gates for operating-mode combinations and lifecycle status bits."""
import math
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

from test_pb import PB, run
import test_pb


class Audit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.shim_tmp = tempfile.TemporaryDirectory()
        cls.shim = Path(cls.shim_tmp.name) / 'fault.so'
        subprocess.run([shutil.which('cc'), '-shared', '-fPIC', '-o', str(cls.shim),
                        str(Path(__file__).with_name('fault_io.c')), '-ldl'],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.shim_tmp.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.children = []
        self.files = []
        self.env = {**os.environ, 'LD_PRELOAD': str(self.shim)}

    def tearDown(self):
        for p in reversed(self.children):
            if p.poll() is None: p.kill()
            p.communicate()
        for stream in self.files: stream.close()
        self.tmp.cleanup()

    def start(self, *args, **options):
        p = subprocess.Popen([PB, *map(str, args)], stdin=subprocess.PIPE,
                             stdout=options.pop('stdout', subprocess.DEVNULL),
                             stderr=options.pop('stderr', subprocess.PIPE), **options)
        self.children.append(p)
        return p

    def ready(self, p):
        until = time.monotonic() + 3
        while time.monotonic() < until:
            if run('-R', p.pid).returncode == 0: return
            self.assertIsNone(p.poll())
            time.sleep(.01)
        self.fail('remote endpoint did not become ready')

    def line(self, p, expected):
        until = time.monotonic() + 3
        while time.monotonic() < until:
            if select.select([p.stderr], [], [], .1)[0]:
                data = p.stderr.readline()
                if data.rstrip(b'\r\n') == expected: return
                if not data: self.fail(f'process exited: {p.poll()}')
        self.fail(f'no output line matching {expected!r}')

    def actor(self):
        first, second = self.root / 'first', self.root / 'second'
        first.write_bytes(b'x' * 10000)
        second.write_bytes(b'y' * 10000)
        p = subprocess.Popen([sys.executable, '-u', '-c', test_pb.Watch.CHILD,
                              str(first), str(second), 'pb-audit'],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.children.append(p)
        fd = int(p.stdout.readline())
        return p, fd

    def command(self, actor, text):
        actor.stdin.write((text + '\n').encode())
        actor.stdin.flush()
        return int(actor.stdout.readline())

    def test_remote_format_on_watch_and_pidfile_cleanup(self):
        actor, fd = self.actor()
        pidfile = self.root / 'watch.pid'
        watcher = self.start('-d', f'{actor.pid}:{fd}', '-nb', '-i', '0.02', '-P', pidfile)
        self.ready(watcher)
        self.assertEqual(int(pidfile.read_text()), watcher.pid)
        self.assertEqual(run('-R', watcher.pid, '-F', 'watched:%b').returncode, 0)
        self.command(actor, 'read 77')
        self.line(watcher, b'watched:77')
        self.command(actor, 'close')
        watcher.communicate(timeout=2)
        self.assertEqual(watcher.returncode, 0)
        self.assertFalse(pidfile.exists())

    def test_watch_window_expires_while_display_interval_is_long(self):
        actor, fd = self.actor()
        watcher = self.start('-d', f'{actor.pid}:{fd}', '-nF', '%a', '-m', '1', '-i', '5')
        self.ready(watcher)
        self.line(watcher, b'0.0000')
        self.command(actor, 'read 1000')
        self.assertEqual(run('-R', watcher.pid, '-F', '%b:%a').returncode, 0)
        until = time.monotonic() + 2
        while time.monotonic() < until:
            if select.select([watcher.stderr], [], [], .1)[0]:
                line = watcher.stderr.readline()
                if line.startswith(b'1000:') and float(line.split(b':')[1]) > 0: break
        else: self.fail('watcher did not observe the burst')
        time.sleep(1.2)
        self.assertEqual(run('-R', watcher.pid, '-i', '0.02').returncode, 0)
        self.line(watcher, b'1000:0.0000')
        self.command(actor, 'close')
        watcher.communicate(timeout=2)

    def test_query_is_remotely_configurable_and_has_a_pidfile(self):
        target = self.start('-q')
        self.ready(target)
        target.stdin.write(b'data')
        target.stdin.flush()
        pidfile = self.root / 'query.pid'
        query = self.start('-Q', target.pid, '-nb', '-i', '5', '-P', pidfile)
        self.ready(query)
        self.assertEqual(int(pidfile.read_text()), query.pid)
        self.assertEqual(run('-R', query.pid, '-F', 'queried:%b', '-i', '0.02').returncode, 0)
        self.line(query, b'queried:4')
        target.communicate(timeout=2)
        query.communicate(timeout=2)
        self.assertEqual(query.returncode, 0)
        self.assertFalse(pidfile.exists())

    def test_quiet_statistics_for_transfer_and_query(self):
        p = run('-qv', data=b'data')
        self.assertEqual(p.returncode, 0)
        self.assertIn(b'rate min/mean/max/stddev', p.stderr)
        target = self.start('-q')
        self.ready(target)
        query = self.start('-Q', target.pid, '-qv', '-i', '0.02')
        self.ready(query)
        target.stdin.write(b'query stats')
        target.stdin.flush()
        time.sleep(.1)
        target.communicate(timeout=2)
        _, output = query.communicate(timeout=2)
        values = output.decode().split('= ')[1].split(' ')[0].split('/')
        self.assertEqual(query.returncode, 0)
        self.assertTrue(all(math.isfinite(float(value)) for value in values))

    def test_remote_extra_display(self):
        target = self.start('-nb', '-i', '0.02')
        self.ready(target)
        target.stdin.write(b'data')
        target.stdin.flush()
        self.assertEqual(run('-R', target.pid, '-x', 'process:remote=%b').returncode, 0)
        until = time.monotonic() + 2
        while time.monotonic() < until:
            if b'remote=4' in Path(f'/proc/{target.pid}/cmdline').read_bytes(): break
            time.sleep(.01)
        else: self.fail('remote process-title update was ignored')
        target.communicate(timeout=2)

    def test_pending_intermediate_pipe_survives_remote_buffer_change(self):
        source, output, log = self.root / 'source', self.root / 'output', self.root / 'log'
        data = bytes(range(256)) * 2000
        source.write_bytes(data)
        env = {**self.env, 'PB_TEST_COPY_UNSUPPORTED': '1',
               'PB_TEST_DRAIN_WAITING': str(self.root / 'never-created'), 'PB_TEST_FAST_LOG': str(log)}
        p = self.start('-q', '-o', output, source, env=env)
        self.ready(p)
        until = time.monotonic() + 2
        while time.monotonic() < until:
            if log.exists() and b'pending\n' in log.read_bytes(): break
            time.sleep(.01)
        else: self.fail('input did not enter the intermediate pipe')
        self.assertEqual(run('-R', p.pid, '-B', '17').returncode, 0)
        p.communicate(timeout=4)
        self.assertEqual((p.returncode, output.read_bytes()), (0, data))

    def test_default_buffer_uses_filesystem_block_size_and_honors_override(self):
        source, log = self.root / 'large-blocksize-input', self.root / 'log'
        source.write_bytes(b'data')
        env = {**self.env, 'PB_TEST_ALLOC_LOG': str(log)}
        p = run('-qC', source, env=env)
        self.assertEqual(p.returncode, 0)
        if not log.exists():
            symbols = subprocess.run(['nm', PB], capture_output=True).stdout
            if b'__asan_init' in symbols:
                self.skipTest('ASan binds its own allocator; allocation tracing is checked in the normal build')
        self.assertIn(b'allocate:524288\n', log.read_bytes())
        log.unlink()
        p = run('-qC', '-B', '4K', source, env=env)
        self.assertEqual(p.returncode, 0)
        self.assertIn(b'allocate:4096\n', log.read_bytes())

    def test_size_from_block_device_path(self):
        device = self.root / 'fake-block-size'
        device.write_bytes(b'x' * 64)
        p = run('-qS', '-s', '@' + str(device), data=b'z' * 100, env=self.env)
        self.assertEqual((p.returncode, len(p.stdout)), (0, 64))

    def test_status_bits_for_stat_close_allocation_and_combined_errors(self):
        normal = self.root / 'normal'
        normal.write_bytes(b'data')
        stat_error = self.root / 'stat-error-input'
        stat_error.write_bytes(b'data')
        p = run('-q', stat_error, normal, env=self.env)
        self.assertEqual((p.returncode, p.stdout), (2, b'data'))
        close_error = self.root / 'close-error-input'
        close_error.write_bytes(b'data')
        p = run('-q', close_error, env=self.env)
        self.assertEqual((p.returncode, p.stdout), (8, b'data'))
        p = run('-qC', '-B', str(2**62), normal,
                env={**self.env, 'PB_TEST_ALLOC_FAIL': '1',
                     'ASAN_OPTIONS': os.environ.get('ASAN_OPTIONS', '') + ':allocator_may_return_null=1'})
        self.assertEqual(p.returncode, 64)
        p = run('-q', self.root / 'absent', self.root, normal)
        self.assertEqual((p.returncode, p.stdout), (18, b'data'))

    def test_adaptive_skips_and_double_error_suppression(self):
        first, second = self.root / 'fault-input-first', self.root / 'fault-input-second'
        data = b'abcdefghijklmnopqrst'
        first.write_bytes(data)
        second.write_bytes(data)
        p = run('-qEE', first, second, env=self.env)
        expected = b'abcdefgh' + bytes(7) + b'pqrst'
        self.assertEqual((p.returncode, p.stdout), (16, expected * 2))
        self.assertEqual(p.stderr.count(b'Input/output error'), 2)

    def test_numeric_watch_counts_preserve_large_integer_and_bit_precision(self):
        actor, fd = self.actor()
        amount = 2**62 + 123
        self.command(actor, f'seek {amount}')
        watcher = self.start('-d', f'{actor.pid}:{fd}', '-n8b', '-i', '0.02')
        self.line(watcher, str(amount * 8).encode())
        self.command(actor, 'close')
        watcher.communicate(timeout=2)
        self.assertEqual(watcher.returncode, 0)

    def test_watch_lists_can_nest_beyond_sixteen_levels(self):
        actor, fd = self.actor()
        spec = f'{actor.pid}:{fd}'
        for depth in range(40):
            path = self.root / f'watch-{depth}'
            path.write_text(spec + '\n')
            spec = '@' + str(path)
        watcher = self.start('-d', spec, '-nb', '-i', '.02')
        self.line(watcher, b'0')
        self.command(actor, 'close')
        watcher.communicate(timeout=2)
        self.assertEqual(watcher.returncode, 0)

    def test_watch_name_preserves_long_paths(self):
        directory = self.root
        for depth in range(8):
            directory /= str(depth) + 'x' * 60
            directory.mkdir()
        path = directory / 'untruncated-filename'
        path.write_bytes(b'data')
        actor = subprocess.Popen([sys.executable, '-u', '-c',
                                 'import sys; f=open(sys.argv[1]); print(f.fileno(),flush=True); sys.stdin.read()',
                                 str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        self.children.append(actor)
        fd = int(actor.stdout.readline())
        watcher = self.start('-d', f'{actor.pid}:{fd}', '-nfF', '%N', '-i', '.02')
        self.line(watcher, f'{actor.pid}:{fd} {path}:'.encode())
        actor.communicate(timeout=2)
        watcher.communicate(timeout=2)
        self.assertEqual(watcher.returncode, 0)

    def test_statistics_for_both_monitors_and_line_mode(self):
        p = run('-qvM', 'both', '--', 'cat', data=b'hello')
        self.assertEqual((p.returncode, p.stdout), (0, b'hello'))
        self.assertEqual(p.stderr.count(b'rate min/mean/max/stddev'), 2)
        p = run('-qlv', data=b'one\ntwo\n')
        self.assertEqual(p.returncode, 0)
        self.assertIn(b'rate min/mean/max/stddev', p.stderr)

    def test_remote_display_modifier_matrix(self):
        target = self.start('-f', '-i', '.02', '-s', '2000', env={**os.environ, 'LC_ALL': 'C.utf8'})
        self.ready(target)
        target.stdin.write(b'x' * 1000)
        target.stdin.flush()
        update = run('-R', target.pid, '-pteIrabT', '-A', '4', '-N', 'mutable',
                     '-8k', '-u', 'block', '-w', '90', '-H', '5', '-m', '2', '-g')
        self.assertEqual(update.returncode, 0, update.stderr)
        target.stdin.write(b'ABCD')
        target.stdin.flush()
        time.sleep(.08)
        _, output = target.communicate(timeout=2)
        self.assertEqual(target.returncode, 0)
        for fragment in [b'mutable:', b'8.03 kb', b'ABCD', b'50%', b'ETA ', b'FIN ', b'{  0%}']:
            self.assertIn(fragment, output)
        self.assertIn('█'.encode(), output)

    def test_remote_and_query_reject_file_arguments(self):
        for option in ['-R', '-Q']:
            self.assertEqual(run(option, '1', 'filename').returncode, 64)


if __name__ == '__main__':
    unittest.main(verbosity=2)
