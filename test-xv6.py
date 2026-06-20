#!/usr/bin/env python3

#
# python script that tests xv6 without having to boot it and type to its shell
#
# ./test-xv6.py usertests  (runs usertests)
# ./test-xv6.py -q usertests (runs the quick tests of usertests)
# ./test-xv6.py crash  (runs the crash tests)
# ./test-xv6.py log (runs the log crash test)

import argparse, os, inspect, re, signal, subprocess, sys, time, select, fcntl
from subprocess import run

parser = argparse.ArgumentParser(description="xv6 automated test script")
parser.add_argument('testrex', help="test name or regular expression")
parser.add_argument("-q", action='store_true', help="usertests quick")
args = parser.parse_args()

def set_nonblocking(fd):
    """Set file descriptor to non-blocking mode"""
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

class QEMU(object):

    def __init__(self, reset=False):
        if reset:
            self.build_xv6()
            self.reset_fs()
        q = ["make", "qemu"]
        self.proc = subprocess.Popen(q, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT)
        self.output = ""
        self.outbytes = bytearray()
        # Set stdout to non-blocking
        set_nonblocking(self.proc.stdout.fileno())
        # Wait for QEMU to start and show shell prompt
        self.wait_for_prompt()

    def wait_for_prompt(self, timeout=30):
        """Wait for xv6 shell prompt"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.read()
            if '$ ' in self.output or 'init:' in self.output:
                time.sleep(0.5)  # Give it a moment to stabilize
                return True
            time.sleep(0.2)
        print("Warning: Shell prompt not detected, continuing anyway...")
        return False

    def reset_fs(self):
        try:
            run(["rm", "-f", "fs.img"], check=False)
            run(["make", "fs.img"], check=True)
        except subprocess.CalledProcessError as e:
            print(f"Command failed with exit code {e.returncode}")

    def build_xv6(self):
        try:
            run(["make", "kernel/kernel"], check=True)
        except subprocess.CalledProcessError as e:
            print(f"Command failed with exit code {e.returncode}")

    def save_output(self):
        try:
            with open("test-xv6.out", "w") as f:
                f.write(self.output)
        except OSError as e:
            print("Failed to save output. Error:", e)

    def cmd(self, c):
        """Send command to QEMU"""
        if self.proc.poll() is not None:
            print("Error: QEMU process has terminated")
            return
        if isinstance(c, str):
            c = c.encode('utf-8')
        try:
            self.proc.stdin.write(c)
            self.proc.stdin.flush()
        except BrokenPipeError:
            print("Error: Broken pipe when sending command")

    def crash(self):
        """Kill QEMU process to simulate crash"""
        try:
            ps = run(['ps', '-opid', '--no-headers', '--ppid', str(self.proc.pid)], 
                     stdout=subprocess.PIPE, encoding='utf8')
            kids = [int(line) for line in ps.stdout.splitlines()]
            if len(kids) == 0:
                print("no qemu child process found")
                return
            print("kill", kids[0])
            os.kill(kids[0], signal.SIGKILL)
        except Exception as e:
            print(f"Error during crash: {e}")

    def stop(self):
        """Terminate QEMU process"""
        try:
            self.proc.terminate()
            self.proc.wait(timeout=5)
        except:
            self.proc.kill()

    def read(self):
        """Read available output from QEMU (non-blocking)"""
        try:
            while True:
                ready, _, _ = select.select([self.proc.stdout], [], [], 0.1)
                if not ready:
                    break
                buf = os.read(self.proc.stdout.fileno(), 4096)
                if not buf:
                    break
                self.outbytes.extend(buf)
        except (BlockingIOError, OSError):
            pass
        self.output = self.outbytes.decode("utf-8", "replace")

    def lines(self):
        return self.output.splitlines()

    def error(self, msg="match failed"):
        print(f"FAIL: {msg}")
        print("--- Last output ---")
        print(self.output[-2000:] if len(self.output) > 2000 else self.output)
        print("--- End output ---")
        self.save_output()
        self.stop()
        sys.exit(1)

    def match(self, *regexps, exit=True):
        """Check if any line matches the given patterns"""
        lines = self.lines()
        last = -1
        for i, line in enumerate(lines):
            if any(re.search(r, line) for r in regexps):
                last = i
        if last == -1 and exit:
            self.error(f"pattern not found: {regexps}")
        l = ""
        if last >= 0:
            l = lines[last]
            print(f"  matched: {l}")
        return last >= 0, l

    def monitor(self, *regexps, progress="", timeout=300):
        """Monitor output until pattern matches or timeout"""
        deadline = time.time() + timeout
        last_progress = ""
        while True:
            time.sleep(1)
            timeleft = deadline - time.time()
            if timeleft < 0:
                self.error(f"timeout waiting for: {regexps}")
            if self.proc.poll() is not None:
                self.read()
                self.error("QEMU process terminated unexpectedly")
            self.read()
            ok, _ = self.match(*regexps, exit=False)
            if ok:
                return
            if progress:
                ok, line = self.match(progress, exit=False)
                if ok and line != last_progress:
                    print(f"  progress: {line}")
                    last_progress = line

def crash_log():
    q = QEMU(True)
    q.cmd("logstress f0 f1 f2 f3 f4 f5\n")
    time.sleep(2)
    q.crash()
    q.stop()

def recover_log():
    q = QEMU()
    time.sleep(2)
    q.read()
    ok, _ = q.match('^recovering', exit=False)
    if ok:
        q.cmd("ls\n")
        time.sleep(2)
        q.read()
        q.match('f5')
    q.stop()
    return ok

def forphan():
    q = QEMU(True)
    q.cmd("forphan\n")
    time.sleep(5)
    q.read()
    q.match('wait')
    q.crash()
    q.stop()

def dorphan():
    q = QEMU(True)
    q.cmd("dorphan\n")
    time.sleep(5)
    q.read()
    q.match('wait')
    q.crash()
    q.stop()

def recover_orphan():
    q = QEMU()
    time.sleep(2)
    q.read()
    q.match('^ireclaim')
    q.stop()

def test_log():
    print("Test recovery of log")
    for i in range(5):
        crash_log()
        ok = recover_log()
        if ok:
            print("OK")
            return
        print("log attempt ", i+1)
    print("FAIL")
    sys.exit(1)

def test_forphan():
    print("Test recovery of an orphaned file")
    forphan()
    recover_orphan()
    print("OK")

def test_dorphan():
    print("Test recovery of an orphaned file")
    dorphan()
    recover_orphan()
    print("OK")

def test_crash():
    test_log()
    test_forphan()
    test_dorphan()

def test_usertests(test=""):
    print("Running usertests...")
    timeout = 600
    opt = ""
    if args.q:
        opt = " -q"
        timeout = 300
    elif test != "":
        opt += " " + test
    q = QEMU(True)
    q.cmd("usertests" + opt + "\n")
    q.monitor('ALL TESTS PASSED', progress='test', timeout=timeout)
    print("usertests: OK")
    q.stop()

def test_grind():
    print("Running grind...")
    q = QEMU(True)
    q.cmd("grind\n")
    q.monitor('grind: tests OK', timeout=120)
    print("grind: OK")
    q.stop()

def main():
    print(f"xv6 test runner: {args}")
    rex = r'%s' % args.testrex
    funcs = [(obj,name) for name,obj in inspect.getmembers(sys.modules[__name__]) 
                     if (inspect.isfunction(obj) and 
                         name.startswith('test'))]
    none = True
    for (f,n) in funcs:
        if re.search(rex, n):
            none = False
            print(f"\n=== Running {n} ===")
            f()
    if none:
        test_usertests(test=args.testrex)

if __name__ == "__main__":
    main()
