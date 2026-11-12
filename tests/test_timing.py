"""Clock and moving-window behavior under stalls, sparse display updates and jobs."""
import math
import datetime
import os
import select
import signal
import subprocess
import time
import unittest

from test_pb import PB


class Timing(unittest.TestCase):
    def stopped(self, p):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            pid, status = os.waitpid(p.pid, os.WNOHANG | os.WUNTRACED)
            if pid:
                self.assertTrue(os.WIFSTOPPED(status), status)
                return
            time.sleep(.01)
        self.fail('process did not stop')

    def finish(self, p):
        if p.poll() is None:
            p.kill()
        p.communicate()

    def test_average_window_after_long_initial_stall(self):
        p = subprocess.Popen([PB, '-n', '-m', '1', '-i', '0.05', '-F', '%t %b %a'],
                             stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE)
        try:
            self.assertTrue(select.select([p.stderr], [], [], 2)[0])
            p.stderr.readline()
            time.sleep(1.15)
            p.stdin.write(b'x' * 1000)
            p.stdin.flush()
            deadline = time.monotonic() + .5
            average = 0
            while time.monotonic() < deadline:
                if select.select([p.stderr], [], [], .1)[0]:
                    _, count, rate = p.stderr.readline().split()
                    if int(count) == 1000:
                        average = float(rate)
                        break
            self.assertGreater(average, 900)
            self.assertLess(average, 1100)
            p.communicate(timeout=2)
        finally:
            self.finish(p)

    def test_window_sampling_does_not_depend_on_display_interval(self):
        p = subprocess.Popen([PB, '-n', '-m', '1', '-i', '5', '-F', '%a'],
                             stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE)
        try:
            p.stdin.write(b'x' * 1000)
            p.stdin.flush()
            time.sleep(1.25)
            _, err = p.communicate(timeout=2)
            self.assertEqual(p.returncode, 0)
            self.assertLess(float(err.splitlines()[-1]), 1)
        finally:
            self.finish(p)

    def test_wait_timer_starts_at_first_transferred_byte(self):
        p = subprocess.Popen([PB, '-nW', '-i', '5', '-F', '%t'],
                             stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE)
        try:
            time.sleep(.25)
            p.stdin.write(b'data')
            p.stdin.flush()
            time.sleep(.12)
            _, err = p.communicate(timeout=2)
            elapsed = float(err.splitlines()[-1])
            self.assertGreater(elapsed, .08)
            self.assertLess(elapsed, .24)
        finally:
            self.finish(p)

    def test_job_control_excludes_pause_from_display_but_accrues_rate_budget(self):
        p = subprocess.Popen([PB, '-ntb', '-i', '0.05', '-L', '2000'],
                             stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE, preexec_fn=os.setpgrp)
        began = time.monotonic()
        try:
            p.stdin.write(b'x' * 1000)
            p.stdin.flush()
            self.assertTrue(select.select([p.stderr], [], [], 2)[0])
            p.stderr.readline()
            p.send_signal(signal.SIGTSTP)
            self.stopped(p)
            paused = time.monotonic()
            time.sleep(.3)
            p.send_signal(signal.SIGCONT)
            pause_time = time.monotonic() - paused
            _, err = p.communicate(timeout=3)
            wall = time.monotonic() - began
            elapsed, count = err.splitlines()[-1].split()
            self.assertEqual((p.returncode, int(count)), (0, 1000))
            self.assertAlmostEqual(float(elapsed), wall - pause_time, delta=.08)
            self.assertGreaterEqual(wall, .45)
            self.assertLess(wall, .75)
        finally:
            self.finish(p)

    def test_both_monitor_processes_resume_after_group_stop(self):
        p = subprocess.Popen([PB, '-ntb', '-i', '0.05', '-L', '2000', '-M', 'both', '--', 'cat'],
                             stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE, preexec_fn=os.setpgrp)
        try:
            p.stdin.write(b'x' * 1000)
            p.stdin.flush()
            self.assertTrue(select.select([p.stderr], [], [], 2)[0])
            p.stderr.readline()
            os.killpg(p.pid, signal.SIGTSTP)
            self.stopped(p)
            time.sleep(.15)
            os.killpg(p.pid, signal.SIGCONT)
            _, err = p.communicate(timeout=3)
            self.assertEqual(p.returncode, 0, err)
            counts = [line.split()[1] for line in err.splitlines() if len(line.split()) >= 2]
            self.assertGreaterEqual(counts.count(b'1000'), 2, err)
        finally:
            self.finish(p)

    def test_absolute_eta_is_not_capped_at_ten_years(self):
        p = subprocess.Popen([PB, '-n', '-s', '1000000000000000', '-i', '0.02', '-F', '%I'],
                             stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE)
        try:
            p.stdin.write(b'x')
            p.stdin.flush()
            self.assertTrue(select.select([p.stderr], [], [], 2)[0])
            line = p.stderr.readline().decode()
            year = int(line.split()[1].split('-')[0])
            self.assertGreater(year, datetime.datetime.now().year + 10)
            p.communicate(timeout=2)
        finally:
            self.finish(p)

    def test_eta_without_a_rate_is_unknown(self):
        p = subprocess.Popen([PB, '-n', '-s', '10', '-i', '0.02', '-F', '%e'],
                             stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE)
        try:
            self.assertTrue(select.select([p.stderr], [], [], 2)[0])
            self.assertEqual(p.stderr.readline(), b'ETA --:--:--\n')
            p.communicate(timeout=2)
        finally:
            self.finish(p)

    def test_fractional_rate_does_not_truncate_to_unlimited(self):
        began = time.monotonic()
        p = subprocess.run([PB, '-q', '-L', '0.5'], input=b'x',
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=4)
        elapsed = time.monotonic() - began
        self.assertEqual((p.returncode, p.stdout), (0, b'x'))
        self.assertGreaterEqual(elapsed, 1.8)
        self.assertLess(elapsed, 3.5)

    def test_statistics_remain_finite_for_regular_and_empty_transfers(self):
        for data in [b'', b'x' * 10000]:
            p = subprocess.run([PB, '-v'], input=data, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, timeout=3)
            self.assertEqual(p.returncode, 0)
            values = p.stderr.decode().split('= ')[1].split(' ')[0].split('/')
            self.assertEqual(len(values), 4)
            self.assertTrue(all(math.isfinite(float(value)) for value in values))


if __name__ == '__main__':
    unittest.main(verbosity=2)
