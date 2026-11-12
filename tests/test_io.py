"""Kernel-copy and device semantics, with injected unsupported-kernel paths."""
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import sys
import select
import tempfile
import time
import unittest

from test_pb import PB, run


class KernelIO(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.shim_tmp = tempfile.TemporaryDirectory()
        cls.shim = Path(cls.shim_tmp.name) / 'io.so'
        source = Path(__file__).with_name('fault_io.c')
        subprocess.run([shutil.which('cc'), '-shared', '-fPIC', '-o', str(cls.shim),
                        str(source), '-ldl'], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.shim_tmp.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.source = self.root / 'source'
        self.target = self.root / 'target'
        self.data = bytes(range(256)) * 2000
        self.source.write_bytes(self.data)
        self.log = self.root / 'fast.log'
        self.env = {**os.environ, 'LD_PRELOAD': str(self.shim), 'PB_TEST_FAST_LOG': str(self.log)}

    def tearDown(self):
        self.tmp.cleanup()

    def test_copy_file_range_regular_files(self):
        p = run('-f', '-F', '%T', '-o', self.target, self.source, env=self.env)
        self.assertEqual((p.returncode, self.target.read_bytes()), (0, self.data))
        self.assertIn(b'copy', self.log.read_bytes())
        self.assertIn(b'{--->}', p.stderr)

    def test_splice_between_pipes(self):
        p = run('-f', '-F', '%T', data=self.data, env=self.env)
        self.assertEqual((p.returncode, p.stdout), (0, self.data))
        self.assertIn(b'splice', self.log.read_bytes())
        self.assertIn(b'{----}', p.stderr)

    def test_no_splice_disables_all_acceleration(self):
        p = run('-qC', '-o', self.target, self.source, env=self.env)
        self.assertEqual((p.returncode, self.target.read_bytes()), (0, self.data))
        self.assertFalse(self.log.exists())

    def test_unsupported_copy_uses_intermediate_pipe(self):
        env = {**self.env, 'PB_TEST_COPY_UNSUPPORTED': '1'}
        p = run('-f', '-F', '%T', '-o', self.target, self.source, env=env)
        self.assertEqual((p.returncode, self.target.read_bytes()), (0, self.data))
        self.assertIn(b'{-||-}', p.stderr)
        self.assertIn(b'splice', self.log.read_bytes())

    def test_pending_pipe_data_survives_fallback(self):
        env = {**self.env, 'PB_TEST_COPY_UNSUPPORTED': '1', 'PB_TEST_DRAIN_UNSUPPORTED': '1'}
        p = run('-q', '-o', self.target, self.source, env=env)
        self.assertEqual((p.returncode, self.target.read_bytes()), (0, self.data))
        self.assertIn(b'splice', self.log.read_bytes())

    def test_unsupported_splice_falls_back_to_read_write(self):
        env = {**self.env, 'PB_TEST_SPLICE_UNSUPPORTED': '1'}
        p = run('-q', data=self.data, env=env)
        self.assertEqual((p.returncode, p.stdout), (0, self.data))
        self.assertIn(b'splice', self.log.read_bytes())

    def test_buffer_and_last_written_options_disable_splice(self):
        for args in [('-T',), ('-A', '10'), ('-B', '4K'), ('-l',), ('-O',)]:
            if self.log.exists(): self.log.unlink()
            p = run('-q', *args, data=self.data[:1000], env=self.env)
            self.assertEqual((p.returncode, p.stdout), (0, self.data[:1000]))
            self.assertFalse(self.log.exists(), args)

    def test_accelerated_history_fields_show_unavailable_data(self):
        p = run('-fF', '%8A|%10L', data=self.data, env=self.env)
        self.assertEqual(p.stderr.strip(b'\r\n'), b'........|          ')

    def test_infer_block_device_capacity(self):
        device = self.root / 'fake-block-input'
        device.write_bytes(b'block data' * 10)
        p = run('-nC', device, env=self.env)
        self.assertEqual((p.returncode, p.stdout, p.stderr), (0, device.read_bytes(), b'100\n'))

    def test_unknown_input_stops_at_output_device_capacity(self):
        device = self.root / 'fake-block-output'
        device.write_bytes(b'?' * 64)
        p = run('-qC', '-o', device, data=b'x' * 1000, env=self.env)
        self.assertEqual((p.returncode, device.read_bytes()), (0, b'x' * 64))

    def test_watch_block_device_uses_ioctl_capacity(self):
        device = self.root / 'fake-block-watch'
        device.write_bytes(b'x' * 64)
        with device.open('rb') as f:
            f.seek(32)
            child = subprocess.Popen([sys.executable, '-c', 'import sys; sys.stdin.buffer.read()'],
                                     stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                     pass_fds=(f.fileno(),))
            watcher = subprocess.Popen([PB, '-d', f'{child.pid}:{f.fileno()}', '-n', '-i', '0.02'],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env)
            try:
                self.assertTrue(select.select([watcher.stderr], [], [], 2)[0])
                self.assertEqual(watcher.stderr.readline(), b'50\n')
                child.communicate(timeout=2)
                watcher.communicate(timeout=2)
                self.assertEqual(watcher.returncode, 0)
            finally:
                for process in [child, watcher]:
                    if process.poll() is None: process.kill()
                    process.communicate()

    def test_sync_flushes_block_devices(self):
        device = self.root / 'fake-block-output'
        device.write_bytes(b'?' * 64)
        p = run('-qCY', '-o', device, data=b'x' * 64, env=self.env)
        self.assertEqual((p.returncode, device.read_bytes()), (0, b'x' * 64))
        self.assertIn(b'sync', self.log.read_bytes())

    def test_device_alias_is_rejected(self):
        device = self.root / 'fake-block-input'
        device.write_bytes(b'preserved')
        p = run('-qC', '-o', device, device, env=self.env)
        self.assertEqual((p.returncode, device.read_bytes()), (4, b'preserved'))

    def test_aligned_direct_io_files(self):
        data = bytes(range(256)) * 32
        self.source.write_bytes(data)
        p = run('-qK', '-B', '4K', '-o', self.target, self.source)
        if p.returncode:
            self.assertEqual(p.returncode, 16, p.stderr)
            self.assertIn(b'Invalid argument', p.stderr)
            self.skipTest('filesystem does not support this direct-I/O alignment')
        self.assertEqual(self.target.read_bytes(), data)

    def test_output_pipe_grows_to_match_input_pipe(self):
        read_end, write_end = os.pipe()
        fcntl.fcntl(write_end, fcntl.F_SETPIPE_SZ, 4096)
        p = subprocess.Popen([PB, '-qC'], stdin=subprocess.PIPE, stdout=write_end,
                             stderr=subprocess.PIPE)
        try:
            input_capacity = fcntl.fcntl(p.stdin.fileno(), fcntl.F_GETPIPE_SZ)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                if fcntl.fcntl(write_end, fcntl.F_GETPIPE_SZ) == input_capacity:
                    break
                time.sleep(.01)
            self.assertEqual(fcntl.fcntl(write_end, fcntl.F_GETPIPE_SZ), input_capacity)
            p.communicate(b'data', timeout=2)
            self.assertEqual(os.read(read_end, 4), b'data')
        finally:
            if p.poll() is None: p.kill()
            p.communicate()
            os.close(read_end)
            os.close(write_end)


if __name__ == '__main__':
    unittest.main(verbosity=2)
