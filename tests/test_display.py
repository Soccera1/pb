"""Display behavior tests, including a small terminal model for shared rows."""
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time
import unicodedata
import unittest

from test_pb import PB, PV, run


def visual(data):
    return data.strip(b'\r\n')


def cells(text):
    return sum(0 if unicodedata.combining(c) else 2 if unicodedata.east_asian_width(c) in 'WF' else 1 for c in text)


class Terminal:
    def __init__(self, data, height=8, width=60):
        self.height, self.width = height, width
        self.grid = [[' '] * width for _ in range(height)]
        self.row = self.col = 0
        self.saved = (0, 0)
        self.parse(data.decode('utf-8', errors='replace'))

    def newline(self):
        self.row += 1
        if self.row >= self.height:
            self.grid.pop(0)
            self.grid.append([' '] * self.width)
            self.row = self.height - 1

    def parse(self, text):
        i = 0
        while i < len(text):
            if text.startswith('\x1b[', i):
                match = re.match(r'\x1b\[([0-9;?]*)([@-~])', text[i:])
                if not match:
                    break
                raw, op = match.groups()
                args = [int(n) if n else 1 for n in raw.split(';') if not n.startswith('?')]
                n = args[0] if args else 1
                if op in 'Hf':
                    self.row = min(self.height - 1, max(0, n - 1))
                    self.col = min(self.width - 1, max(0, (args[1] if len(args) > 1 else 1) - 1))
                elif op == 'A': self.row = max(0, self.row - n)
                elif op == 'B': self.row = min(self.height - 1, self.row + n)
                elif op == 'C': self.col = min(self.width - 1, self.col + n)
                elif op == 'D': self.col = max(0, self.col - n)
                elif op == 's': self.saved = self.row, self.col
                elif op == 'u': self.row, self.col = self.saved
                elif op == 'K':
                    start = 0 if raw == '2' else min(self.col, self.width)
                    self.grid[self.row][start:] = [' '] * (self.width - start)
                i += len(match.group())
                continue
            if text.startswith('\x1b]', i):
                match = re.match(r'\x1b\].*?(?:\x07|\x1b\\)', text[i:], re.S)
                if not match:
                    break
                i += len(match.group())
                continue
            c = text[i]
            i += 1
            if c == '\r': self.col = 0
            elif c == '\n': self.newline()
            elif ord(c) >= 32:
                if self.col >= self.width:
                    self.col = 0
                    self.newline()
                self.grid[self.row][self.col] = c
                self.col += max(1, cells(c))

    @property
    def lines(self):
        return [''.join(row).rstrip() for row in self.grid]


class Formats(unittest.TestCase):
    def test_unknown_sequences_are_preserved(self):
        for fmt in ['%q|%{unknown}|%12q', 'tail%', 'tail%{timer', '%123456789unknown']:
            p = run('-fF', fmt, data=b'x')
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(visual(p.stderr), fmt.encode())

    def test_previous_complete_line_and_null_delimiter(self):
        cases = [((), b'first\nsecond\npartial', b'second    '),
                 (('-0',), b'one\0two\0tail', b'two       ')]
        for args, data, expected in cases:
            p = run('-f', '-B', '3', *args, '-F', '%10{previous-line}', data=data)
            self.assertEqual((p.returncode, p.stdout), (0, data))
            self.assertEqual(visual(p.stderr), expected)

    def test_previous_line_prefix_across_large_buffer(self):
        data = b'a' * 150000 + b'\npartial'
        p = run('-f', '-B', '17', '-F', '%30L', data=data)
        self.assertEqual(visual(p.stderr), b'a' * 30)

    def test_last_written_width_enables_capture(self):
        data = b'begin' + bytes(range(32, 127)) * 3
        p = run('-f', '-B', '7', '-F', '%100A', data=data)
        self.assertEqual(visual(p.stderr), data[-100:])

    def test_automatic_bar_width_and_multiple_bars(self):
        for fmt in ['prefix|%p|suffix', '%p|%p']:
            p = run('-f', '-w', '60', '-s', '10', '-F', fmt, data=b'abc')
            self.assertEqual(cells(visual(p.stderr).decode()), 60)
        p = run('-f', '-w', '10', '-s', '10', '-F', '%p|%p', data=b'abc')
        self.assertEqual(visual(p.stderr), b'|')

    def test_explicit_bar_width(self):
        p = run('-f', '-w', '60', '-s', '10', '-F', '%20p|%10{progress-bar-only}', data=b'abc')
        self.assertEqual(len(visual(p.stderr)), 31)
        self.assertEqual(visual(p.stderr).count(b'30%'), 1)

    def test_wide_text_does_not_break_width_allocation(self):
        p = run('-f', '-w', '30', '-s', '10', '-F', '列|%p', data=b'abc',
                env={**os.environ, 'LC_ALL': 'C.utf8'})
        self.assertEqual(cells(visual(p.stderr).decode()), 30)

    def test_sgr_names_numeric_codes_and_dumb_fallback(self):
        fmt = '%{sgr:green,bold,bg-blue,4}ok%{sgr:reset}'
        p = run('-fF', fmt, env={**os.environ, 'TERM': 'xterm'})
        self.assertEqual(visual(p.stderr), b'\x1b[32;1;44;4mok\x1b[0m')
        p = run('-fF', fmt, env={**os.environ, 'TERM': 'dumb'})
        self.assertEqual(visual(p.stderr), b'ok')

    def test_all_field_aliases_and_colour_keywords(self):
        aliases = [('progress', 'p'), ('timer', 't'), ('eta', 'e'), ('fineta', 'I'),
                   ('rate', 'r'), ('average-rate', 'a'), ('bytes', 'b'),
                   ('transferred', 'b'), ('buffer-percent', 'T'), ('last-written', 'A'),
                   ('previous-line', 'L'), ('name', 'N')]
        for name, short in aliases:
            p = run('-Cf', '-s', '10', '-N', 'alias', '-F',
                    '%20{' + name + '}|%20' + short, data=b'abc\n')
            left, right = visual(p.stderr).split(b'|')
            self.assertEqual(left, right, name)
        codes = {'reset': 0, 'none': 0, 'bold': 1, 'dim': 2, 'italic': 3,
                 'underscore': 4, 'underline': 4, 'blink': 5, 'reverse': 7,
                 'no-bold': 22, 'no-dim': 22, 'no-italic': 23, 'no-underscore': 24,
                 'no-underline': 24, 'no-blink': 25, 'no-reverse': 27}
        colours = ['black', 'red', 'green', 'yellow', 'blue', 'magenta', 'cyan', 'white']
        for index, colour in enumerate(colours):
            codes[colour] = codes['fg-' + colour] = 30 + index
            codes['bg-' + colour] = 40 + index
        codes.update({'brown': 33, 'fg-brown': 33, 'bg-brown': 43,
                      'fg-default': 39, 'bg-default': 49})
        for keyword, code in codes.items():
            p = run('-fF', '%{sgr:' + keyword + '}ok',
                    env={**os.environ, 'TERM': 'xterm'})
            expected = f'\x1b[{code}mok' + ('\x1b[0m' if code else '')
            self.assertEqual(visual(p.stderr), expected.encode(), keyword)

    def test_utf8_bar_styles_and_ascii_fallback(self):
        for style in ['block', 'granular', 'shaded']:
            p = run('-f', '-s', '10', '-F', '%10{bar-' + style + '}', data=b'abc',
                    env={**os.environ, 'LC_ALL': 'C.utf8'})
            self.assertTrue(any(byte >= 128 for byte in p.stderr))
            self.assertEqual(cells(visual(p.stderr).decode()), 10)
            p = run('-f', '-s', '10', '-F', '%10{bar-' + style + '}', data=b'abc',
                    env={**os.environ, 'LC_ALL': 'C'})
            self.assertTrue(all(byte < 128 for byte in p.stderr))

    def test_conemu_option_and_field_reset_on_exit(self):
        for args in [('-9', '-p'), ('-F', '%{progress-conemu}')]:
            p = run('-f', '-s', '10', *args, data=b'abcde')
            self.assertIn(b'\x1b]9;4;1;50\x1b\\', p.stderr)
            self.assertTrue(p.stderr.endswith(b'\x1b]9;4;0;0\x1b\\'))
        p = run('-q9', data=b'abc')
        self.assertEqual(p.stderr, b'')

    def test_extra_window_title(self):
        p = run('-nb', '-x', 'windowtitle:copied=%b', data=b'hello')
        self.assertEqual((p.returncode, p.stdout), (0, b'hello'))
        self.assertIn(b'\x1b]0;copied=5\x07', p.stderr)

    def test_process_title_updates_without_damaging_options(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / 'output'
            pidfile = Path(tmp) / 'pidfile'
            p = subprocess.Popen([PB, '-f', '-i', '0.02', '-F', '%b', '-o', str(target),
                                  '-P', str(pidfile), '-x', 'process:bytes=%b'],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
            try:
                for data, amount in [(b'hello', b'5'), (b'world', b'10')]:
                    p.stdin.write(data)
                    p.stdin.flush()
                    deadline = time.monotonic() + 2
                    while time.monotonic() < deadline:
                        title = Path(f'/proc/{p.pid}/cmdline').read_bytes()
                        if b'bytes=' + amount in title:
                            break
                        time.sleep(.01)
                    else:
                        self.fail(f'process title did not update: {title!r}')
                p.communicate(timeout=3)
                self.assertEqual(p.returncode, 0)
                self.assertEqual(target.read_bytes(), b'helloworld')
                self.assertFalse(pidfile.exists())
            finally:
                if p.poll() is None:
                    p.kill()
                p.communicate()

    def test_invalid_extra_destination(self):
        for spec in ['', 'bogus', ':format']:
            self.assertEqual(run('-x', spec).returncode, 64)

    @unittest.skipUnless(PV, 'pv oracle not installed')
    def test_reference_literal_and_bar_formats(self):
        for fmt in ['%q|%{unknown}|%12q', 'tail%', '%20p', '%10{progress-bar-only}',
                    '%{progress-amount-only}', '%T', '%p|suffix']:
            args = ['-Cf', '-w', '50', '-s', '10', '-F', fmt]
            expected = run(*args, data=b'abc', binary=PV)
            actual = run(*args, data=b'abc')
            self.assertEqual(visual(actual.stderr), visual(expected.stderr), fmt)

    @unittest.skipUnless(PV, 'pv oracle not installed')
    def test_unknown_size_numeric_reference(self):
        for data in [b'', b'a', b'abcd' * 100]:
            for args in [('-n',), ('-n', '-s', '0'), ('-nF', '%{progress-amount-only}')]:
                self.assertEqual(run(*args, data=data).stderr,
                                 run(*args, data=data, binary=PV).stderr)


class Cursor(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.master, self.slave = pty.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack('HHHH', 8, 60, 0, 0))
        self.processes = []
        self.data = b''

    def tearDown(self):
        for p in self.processes:
            if p.poll() is None:
                p.kill()
            p.communicate()
        os.close(self.master)
        os.close(self.slave)
        self.tmp.cleanup()

    def start(self, *args):
        p = subprocess.Popen([PB, '-c', '-w', '60', '-i', '0.03', *args],
                             stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                             stderr=self.slave,
                             env={**os.environ, 'TMPDIR': self.tmp.name, 'LC_ALL': 'C.utf8'})
        self.processes.append(p)
        p.stdin.write(b'hello')
        p.stdin.flush()
        return p

    def pump(self, duration=.15):
        until = time.monotonic() + duration
        while time.monotonic() < until:
            if select.select([self.master], [], [], max(0, until - time.monotonic()))[0]:
                self.data += os.read(self.master, 65536)
        return Terminal(self.data).lines

    def test_independent_instances_have_distinct_rows(self):
        a = self.start('-F', 'first:%b')
        b = self.start('-F', 'second:%b')
        lines = self.pump()
        first = [i for i, line in enumerate(lines) if 'first:5 B' in line]
        second = [i for i, line in enumerate(lines) if 'second:5 B' in line]
        self.assertEqual(len(first), 1, lines)
        self.assertEqual(len(second), 1, lines)
        self.assertNotEqual(first, second)
        a.communicate(timeout=2)
        b.stdin.write(b'world')
        b.stdin.flush()
        lines = self.pump()
        self.assertTrue(any('first:5 B' in line for line in lines), lines)
        self.assertTrue(any('second:10 B' in line for line in lines), lines)
        b.communicate(timeout=2)

    def test_dead_owner_slot_is_reused(self):
        a = self.start('-F', 'killed:%b')
        self.pump()
        a.kill()
        a.communicate()
        b = self.start('-F', 'replacement:%b')
        lines = self.pump()
        self.assertTrue(any('replacement:5 B' in line for line in lines), lines)
        b.communicate(timeout=2)

    def test_both_command_monitors_use_shared_rows(self):
        p = self.start('-F', '%N %b', '-M', 'both', '--', 'cat')
        lines = self.pump()
        self.assertTrue(any('in: 5 B' in line for line in lines), lines)
        self.assertTrue(any('out: 5 B' in line for line in lines), lines)
        p.communicate(timeout=2)

    def test_nonterminal_cursor_degrades_without_escape_codes(self):
        p = run('-cnb', data=b'hello')
        self.assertEqual((p.returncode, p.stdout, p.stderr), (0, b'hello', b'5\n'))


class LargeDisplay(unittest.TestCase):
    def setUp(self):
        self.master, self.slave = pty.openpty()
        self.resize(12, 100)
        self.processes = []

    def tearDown(self):
        for p in self.processes:
            if p.poll() is None: p.kill()
            p.communicate()
        os.close(self.master)
        os.close(self.slave)

    def resize(self, height, width):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack('HHHH', height, width, 0, 0))

    def read(self, duration=.15):
        data = b''
        until = time.monotonic() + duration
        while time.monotonic() < until:
            if select.select([self.master], [], [], max(0, until-time.monotonic()))[0]:
                data += os.read(self.master, 65536)
        return data

    def start(self, *args, **kwargs):
        p = subprocess.Popen([PB, '-i', '.02', *args], stdin=subprocess.PIPE,
                             stdout=subprocess.DEVNULL, stderr=self.slave,
                             env={**os.environ, 'LC_ALL': 'C.utf8'}, **kwargs)
        self.processes.append(p)
        p.stdin.write(b'hello')
        p.stdin.flush()
        return p

    def test_name_and_remote_name_are_visible_in_large_display(self):
        p = self.start('-N', 'initial-name')
        self.assertIn(b'initial-name', self.read())
        self.assertEqual(run('-R', p.pid, '-N', 'updated-name').returncode, 0)
        data = self.read()
        self.assertIn(b'updated-name', data)
        self.assertTrue(any('updated-name' in line for line in Terminal(data, 12, 100).lines))
        p.communicate(timeout=2)

    def test_width_and_height_resize_switch_between_large_and_compact(self):
        p = self.start('-N', 'resizing')
        self.assertIn('▉'.encode(), self.read())
        for height, width in [(12, 60), (5, 100)]:
            self.resize(height, width)
            p.send_signal(signal.SIGWINCH)
            self.read(.1)  # consume the clearing transition
            data = self.read()
            self.assertIn(b'5 B', data)
            self.assertNotIn('▉'.encode(), data)
            self.resize(12, 100)
            p.send_signal(signal.SIGWINCH)
            self.assertIn('▉'.encode(), self.read())
        p.communicate(timeout=2)

    def test_style_option_selects_compact_bar(self):
        p = self.start('-u', 'plain', '-s', '10')
        data = self.read()
        self.assertIn(b'50%', data)
        self.assertNotIn('▉'.encode(), data)
        p.communicate(timeout=2)

    def test_output_monitor_preserves_interactive_controlling_terminal(self):
        def terminal_session():
            os.setsid()
            fcntl.ioctl(self.slave, termios.TIOCSCTTY, 0)
        import sys
        script = 'print("ready",flush=True); print("received="+input(),flush=True)'
        p = subprocess.Popen([PB, '-qM', 'out', '--', sys.executable, '-c', script],
                             stdin=self.slave, stdout=subprocess.PIPE, stderr=self.slave,
                             preexec_fn=terminal_session)
        self.processes.append(p)
        self.assertTrue(select.select([p.stdout], [], [], 2)[0])
        self.assertEqual(p.stdout.readline(), b'ready\n')
        os.write(self.master, b'interactive input\n')
        output, _ = p.communicate(timeout=3)
        self.assertEqual((p.returncode, output), (0, b'received=interactive input\n'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
