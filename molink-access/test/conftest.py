"""MoLink test fixtures, device detection, and result summary."""
import pytest
import os
import sys
import re
from test_utils import MOLINK_EXE, run_molink, wait_for_status, log_tail, log_error_count

# ---- per-session state ----
_results = {"L1": 0, "L1_fail": 0, "L2": 0, "L2_fail": 0, "L2_skip": 0, "L3": 0, "L3_fail": 0, "L3_skip": 0}
_device_serial = None
_device_authorized = False
_worker_running = False


def pytest_configure(config):
    # Level markers
    config.addinivalue_line("markers", "level1: no device required")
    config.addinivalue_line("markers", "level2: USB device required")
    config.addinivalue_line("markers", "level3: E2E proxy required")


def pytest_sessionstart(session):
    global _device_serial, _device_authorized, _worker_running

    if not os.path.exists(MOLINK_EXE) or os.path.getsize(MOLINK_EXE) < 1024 * 1024:
        pytest.exit("molink.exe not found or too small. Run build first.")

    # ---- device detection ----
    rc, out, _ = run_molink("devices")
    if rc != 0 or "No ADB devices found" in out:
        _device_serial = None
    else:
        # Parse: "#    SERIAL                 AUTH"
        for line in out.splitlines():
            m = re.match(r"\d+\s+(\S+)\s+(yes|no|\?)", line.strip())
            if m:
                _device_serial = m.group(1)
                _device_authorized = (m.group(2) == "yes")
                break

    # Check if worker is running (requires authorized device)
    if _device_serial and _device_authorized:
        run_molink("stop")
        run_molink("start")
        ok, status = wait_for_status("daemon=running", timeout=10)
        print(f"  Daemon start: {'OK' if ok else 'FAIL'} ({status[:80]})")
        if ok:
            run_molink("forward", "-p", "1080", "-r", "1081")
            import subprocess
            # Try multiple URLs and curl alternatives
            urls = [
                "http://httpbin.org/ip",
                "http://example.com",
                "http://www.baidu.com",
            ]
            for url in urls:
                try:
                    p = subprocess.run(
                        ["curl", "--socks5", "127.0.0.1:1080", "--proxy-user", "socks5:password123",
                         "--max-time", "5", "-s", "-o", os.devnull, "-w", "%{http_code}", url],
                        capture_output=True, text=True, timeout=10
                    )
                    code = p.stdout.strip()
                    if code == "200":
                        _worker_running = True
                        print(f"  SOCKS5 test: {url} -> 200 OK")
                        break
                    else:
                        print(f"  SOCKS5 test: {url} -> HTTP {code}")
                except Exception as e:
                    print(f"  SOCKS5 test: {url} -> error: {e}")
        run_molink("stop")

    print(f"\n----- Device detection -----")
    print(f"  Serial:     {_device_serial or 'N/A'}")
    print(f"  Authorized: {_device_authorized}")
    print(f"  Worker:     {_worker_running}")
    print(f"-----------------------------\n")


def pytest_runtest_setup(item):
    """Skip tests automatically based on level markers and device state."""
    if item.get_closest_marker("level2"):
        if not _device_serial:
            pytest.skip("no device connected")
    if item.get_closest_marker("level3"):
        if not _device_authorized or not _worker_running:
            pytest.skip("device not authorized or worker not running")


def _get_level(item):
    if item.get_closest_marker("level3"): return "L3"
    if item.get_closest_marker("level2"): return "L2"
    return "L1"


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    if call.when == "call":
        lvl = _get_level(item)
        if outcome.get_result().failed:
            _results[lvl + "_fail"] += 1
        elif outcome.get_result().skipped:
            _results[lvl + "_skip"] += 1
        else:
            _results[lvl] += 1


def pytest_sessionfinish(session, exitstatus):
    l1_t = _results["L1"] + _results["L1_fail"]
    l2_t = _results["L2"] + _results["L2_fail"] + _results["L2_skip"]
    l3_t = _results["L3"] + _results["L3_fail"] + _results["L3_skip"]

    errors = log_error_count()

    print("\n" + "=" * 40)
    print(" MoLink Test Summary")
    print("=" * 40)
    print(f"  Level 1 (no device):  {_results['L1']}/{l1_t} passed" + (f", {_results['L1_fail']} failed" if _results['L1_fail'] else ""))
    print(f"  Level 2 (USB device): {_results['L2']}/{l2_t} passed" +
          (f", {_results['L2_fail']} failed" if _results['L2_fail'] else "") +
          (f", {_results['L2_skip']} skipped" if _results['L2_skip'] else ""))
    print(f"  Level 3 (E2E proxy):  {_results['L3']}/{l3_t} passed" +
          (f", {_results['L3_fail']} failed" if _results['L3_fail'] else "") +
          (f", {_results['L3_skip']} skipped" if _results['L3_skip'] else ""))
    print(f"  Device: {_device_serial or 'N/A'} ({'authorized' if _device_authorized else 'not authorized'})")
    print(f"  Log errors: {errors if errors >= 0 else 'N/A'}")
    print("=" * 40)

    # On failure, dump log tail
    if exitstatus != 0:
        print("\n----- Last log -----")
        print(log_tail(30))
        print("---------------------\n")
