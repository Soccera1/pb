"""Input metadata, special-file, and independent rate-window model checks."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest

from test_pb import PB, PV, run


class Edges(unittest.TestCase):
    def test_repeated_stdin_is_counted_once_without_closing_it(self):
        with tempfile.TemporaryFile() as source:
            source.write(b'hello')
            source.seek(0)
            p = subprocess.run([PB, '-n', '-', '-'], stdin=source, capture_output=True, timeout=3)
        self.assertEqual((p.returncode, p.stdout, p.stderr), (0, b'hello', b'100\n'))

    def test_directory_size_matches_regular_file_entries(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first = root / 'first'
            first.write_bytes(b'x' * 3000)
            os.link(first, root / 'hardlink')
            (root / 'symlink').symlink_to('first')
            (root / 'nested').mkdir()
            (root / 'nested' / 'second').write_bytes(b'y' * 500)
            os.mkfifo(root / 'fifo')
            p = run('-qS', '-s', '@' + str(root), data=b'z' * 20000)
            self.assertEqual((p.returncode, len(p.stdout)), (0, 6500))
            if PV:
                reference = run('-qS', '-s', '@' + str(root), data=b'z' * 20000, binary=PV)
                self.assertEqual(p.stdout, reference.stdout)
            p = run('-qS', '-s', '@' + str(root / 'symlink'), data=b'z' * 20000)
            self.assertEqual((p.returncode, len(p.stdout)), (0, 3000))

    def test_growing_file_is_copied_beyond_the_initial_estimate(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, destination = Path(tmp) / 'source', Path(tmp) / 'destination'
            source.write_bytes(b'a' * 500)
            p = subprocess.Popen([PB, '-nb', '-i', '0.02', '-L', '2000',
                                  '-o', str(destination), str(source)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                p.stderr.readline()
                with source.open('ab') as f: f.write(b'b' * 500)
                p.communicate(timeout=3)
                self.assertEqual(p.returncode, 0)
                self.assertEqual(destination.read_bytes(), b'a' * 500 + b'b' * 500)
            finally:
                if p.poll() is None: p.kill()
                p.communicate()

    def test_stream_socket_input_and_output(self):
        writer, input_fd = socket.socketpair()
        output_fd, reader = socket.socketpair()
        data = bytes(range(256)) * 2000
        p = subprocess.Popen([PB, '-q'], stdin=input_fd, stdout=output_fd, stderr=subprocess.PIPE)
        input_fd.close()
        output_fd.close()
        errors = []
        def produce():
            try:
                writer.sendall(data)
                writer.shutdown(socket.SHUT_WR)
            except OSError as exc:
                errors.append(exc)
        producer = threading.Thread(target=produce, daemon=True)
        producer.start()
        try:
            reader.settimeout(3)
            chunks = []
            while chunk := reader.recv(65536): chunks.append(chunk)
            producer.join(timeout=2)
            p.communicate(timeout=3)
            self.assertEqual(p.returncode, 0)
            self.assertEqual(errors, [])
            self.assertEqual(b''.join(chunks), data)
        finally:
            if p.poll() is None: p.kill()
            p.communicate()
            writer.close()
            reader.close()
            producer.join(timeout=2)

    def test_broken_output_pipe_finishes_silently_like_pv(self):
        with tempfile.NamedTemporaryFile() as source:
            source.write(b'x' * 2000000)
            source.flush()
            for arguments in [[], ['-C']]:
                p = subprocess.Popen([PB, '-q', *arguments, source.name],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                p.stdout.close()
                try:
                    self.assertEqual(p.wait(timeout=3), 0)
                    self.assertEqual(p.stderr.read(), b'')
                finally:
                    if p.poll() is None: p.kill()
                    p.wait()
                    p.stderr.close()

    def test_socket_backpressure_is_interruptible(self):
        with tempfile.TemporaryFile() as source:
            source.write(b'x' * 2000000)
            for args in [[], ['-C']]:
                source.seek(0)
                out, reader = socket.socketpair()
                p = subprocess.Popen([PB, '-q', *args], stdin=source, stdout=out,
                                     stderr=subprocess.PIPE)
                out.close()
                try:
                    time.sleep(.1)
                    p.terminate()
                    self.assertEqual(p.wait(timeout=3), 32)
                finally:
                    if p.poll() is None: p.kill()
                    p.communicate()
                    reader.close()

    def test_many_rate_samples_match_a_piecewise_linear_model(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            source, executable = Path(tmp) / 'rates.c', Path(tmp) / 'rates'
            source.write_text(r'''
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pb_rates.h"
int main(void) {
    for (unsigned i=0;i<=10000;i++) {
        unsigned long long amount=i<=4000 ? i*10ULL : 40000ULL+(i-4000)*2ULL;
        if (!rates_sample(i/1000.0,amount,0,10)) return 1;
    }
    printf("%.6f\n",rates_average(10,52000));
    /* With a one-second window, the last second has constant 2000/s slope. */
    if (!rates_reset(0,0,0,1)) return 1;
    for (unsigned i=0;i<=10000;i++) {
        unsigned long long amount=i<=4000 ? i*10ULL : 40000ULL+(i-4000)*2ULL;
        if (!rates_sample(i/1000.0,amount,0,1)) return 1;
    }
    printf("%.6f\n",rates_average(10,52000));
    free(rates.samples);
    return 0;
}
''')
            subprocess.run(['cc', '-std=c11', '-I', str(root), str(source), '-lm', '-o', str(executable)],
                           check=True, capture_output=True)
            p = subprocess.run([str(executable)], check=True, capture_output=True)
            first, second = map(float, p.stdout.splitlines())
            self.assertAlmostEqual(first, 5200, places=5)
            self.assertAlmostEqual(second, 2000, places=5)


if __name__ == '__main__':
    unittest.main(verbosity=2)
