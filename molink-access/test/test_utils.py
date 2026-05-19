"""MoLink test utilities."""
import hashlib
import subprocess
import time
import os
import tempfile
import random
import string

MOLINK_EXE = os.path.join(os.path.dirname(__file__), "..", "build", "molink.exe")
LOG_FILE = os.path.join(os.path.dirname(__file__), "..", "build", "molinkd.log")


def run_molink(*args, timeout=30):
    """Run molink.exe with args, return (returncode, stdout, stderr)."""
    cmd = "molink " + " ".join(args)
    p = subprocess.run(
        [MOLINK_EXE] + list(args),
        capture_output=True, text=True, timeout=timeout
    )
    out = p.stdout.strip()
    # Print command + key output for visibility
    short = out if len(out) < 120 else out[:100] + "..."
    print(f"\n  $ {cmd}\n    -> ({p.returncode}) {short}")
    return p.returncode, out, p.stderr.strip()


def wait_for_status(expected, timeout=10, interval=0.5):
    """Poll molink status until expected string appears or timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        rc, out, _ = run_molink("status")
        if expected in out:
            return True, out
        time.sleep(interval)
    return False, run_molink("status")[1]


def wait_until(predicate, timeout=10, interval=0.5):
    """Wait until predicate() returns True."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return False


def md5_file(path):
    """Return MD5 hex digest of file."""
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()


def random_content(size=1024):
    """Generate random binary content of given size."""
    return bytes(random.getrandbits(8) for _ in range(size))


def random_name(length=8):
    """Generate random alphabetic name."""
    return "".join(random.choices(string.ascii_lowercase, k=length))


def log_tail(lines=30):
    """Return last N lines of daemon log."""
    if not os.path.exists(LOG_FILE):
        return "(log file not found)"
    with open(LOG_FILE, "r") as f:
        all_lines = f.readlines()
    return "".join(all_lines[-lines:])

def log_error_count():
    """Count [ERROR] lines in log."""
    if not os.path.exists(LOG_FILE):
        return -1
    count = 0
    with open(LOG_FILE, "r") as f:
        for line in f:
            if "[ERROR]" in line:
                count += 1
    return count
