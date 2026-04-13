"""
MoLink E2E Test Script - Cross-platform Python implementation
"""
import datetime
import sys
import time
import shutil
import socket
import subprocess
import re
import os
from pathlib import Path
from dataclasses import dataclass
from typing import List, Optional

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

ADB_PORT = 1080
ADB_TIMEOUT = 10
STOP_TIMEOUT = 30
BUILD_TIMEOUT = 120
INSTALL_TIMEOUT = 60
SERVICE_WAIT = 10
LOGCAT_TIMEOUT = 15
PROXY_TIMEOUT = 30

PROJECT_ROOT = Path("D:/project/MoLink")
WORKER_DIR = PROJECT_ROOT / "molink-worker"
ACCESS_DIR = PROJECT_ROOT / "molink-access"
ACCESS_JAR = ACCESS_DIR / "target/molink-access-1.0.0.jar"
LOGS_DIR = PROJECT_ROOT / "logs"
APK_PATH = WORKER_DIR / "app/build/outputs/apk/debug/app-debug.apk"

# ---------------------------------------------------------------------------
# Result type
# ---------------------------------------------------------------------------

@dataclass
class CmdResult:
    returncode: int
    stdout: str
    stderr: str
    elapsed: float
    timed_out: bool = False


# ---------------------------------------------------------------------------
# Color printing
# ---------------------------------------------------------------------------

FOREGROUND_COLORS = {
    "gray": "90",
    "green": "92",
    "red": "91",
    "yellow": "93",
    "cyan": "96",
    "magenta": "95",
}

STYLE_RESET = "\033[0m"


def _colored(text: str, color: str) -> str:
    code = FOREGROUND_COLORS.get(color, "")
    if not code or sys.platform == "win32" and not sys.stdout.isatty():
        return text
    return f"\033[{code}m{text}{STYLE_RESET}"


def info(msg: str) -> None:
    print(_colored(f"  {msg}", "gray"))


def pass_(msg: str) -> None:
    print(_colored(f"  {msg}", "green"))


def fail(msg: str) -> None:
    print(_colored(f"  {msg}", "red"))


def warn(msg: str) -> None:
    print(_colored(f"  {msg}", "yellow"))


def step_ok(step: int, desc: str, elapsed: int) -> None:
    print(_colored(f"[{step}] OK {desc} ({elapsed}s)", "green"))


def step_fail(step: int, desc: str) -> None:
    print(_colored(f"[{step}] FAIL {desc}", "red"))


# ---------------------------------------------------------------------------
# Command execution
# ---------------------------------------------------------------------------

def run_cmd(cmd: List[str], timeout: int, cwd: Optional[Path] = None,
            print_output: bool = False) -> CmdResult:
    start = time.time()
    timed_out = False
    shell = False
    if sys.platform == "win32":
        for i, arg in enumerate(cmd):
            if arg.endswith(".bat") or arg.endswith(".cmd"):
                bat_path = (Path(cwd) / arg if cwd else Path(arg)).resolve()
                if not bat_path.exists():
                    which_path = shutil.which(arg)
                    bat_path = Path(which_path) if which_path else bat_path
                bat_str = f'"{bat_path}"' if " " in str(bat_path) else str(bat_path)
                sub_cmd = cmd[i+1:]
                sub_str = " ".join(sub_cmd) if sub_cmd else ""
                cmd_str = f"{bat_str} {sub_str}" if sub_str else bat_str
                proc = subprocess.run(
                    cmd_str,
                    capture_output=True,
                    text=True,
                    timeout=timeout,
                    cwd=str(cwd) if cwd else None,
                    shell=True,
                )
                elapsed = time.time() - start
                if print_output:
                    if proc.stdout:
                        print(proc.stdout)
                    if proc.stderr:
                        print(proc.stderr, file=sys.stderr)
                return CmdResult(
                    returncode=proc.returncode,
                    stdout=proc.stdout,
                    stderr=proc.stderr,
                    elapsed=elapsed,
                    timed_out=False,
                )
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=str(cwd) if cwd else None,
            shell=shell,
        )
        elapsed = time.time() - start
        if print_output:
            if proc.stdout:
                print(proc.stdout)
            if proc.stderr:
                print(proc.stderr, file=sys.stderr)
        return CmdResult(
            returncode=proc.returncode,
            stdout=proc.stdout,
            stderr=proc.stderr,
            elapsed=elapsed,
            timed_out=False,
        )
    except subprocess.TimeoutExpired:
        elapsed = time.time() - start
        try:
            proc.kill()
            stdout = proc.stdout.read() if proc.stdout else ""
            stderr = proc.stderr.read() if proc.stderr else ""
        except Exception:
            stdout = ""
            stderr = ""
        return CmdResult(
            returncode=-1,
            stdout=stdout,
            stderr=f"Command timed out after {timeout}s",
            elapsed=elapsed,
            timed_out=True,
        )


# ---------------------------------------------------------------------------
# Utilities
# ---------------------------------------------------------------------------

def parse_device_from_adb_devices(output: str) -> Optional[str]:
    for line in output.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            return parts[0]
    return None


def get_curl_cmd() -> str:
    if sys.platform == "win32":
        return "curl.exe"
    return shutil.which("curl") or "curl"


# ---------------------------------------------------------------------------
# Service stop & verify
# ---------------------------------------------------------------------------

def cleanup_logs() -> None:
    """Remove all files in logs/ directory and recreate it."""
    import shutil
    if LOGS_DIR.exists():
        shutil.rmtree(LOGS_DIR)
    LOGS_DIR.mkdir(parents=True, exist_ok=True)


def stop_access_service() -> None:
    """Find molink-access process via jps -l and kill it."""
    r = run_cmd(["jps", "-l"], timeout=5, print_output=True)
    print(r.stdout)
    for line in r.stdout.splitlines():
        if "com.molink.access.AccessApplication" in line:
            parts = line.strip().split()
            if len(parts) >= 1:
                pid = parts[0]
                if sys.platform == "win32":
                    r2 = run_cmd(["taskkill", "/F", "/PID", pid], timeout=5, print_output=True)
                else:
                    r2 = run_cmd(["kill", "-9", pid], timeout=5, print_output=True)
                print(r2.stdout)
                time.sleep(1)
                break


def stop_worker_service(device: str) -> None:
    """Force-stop worker service via ADB."""
    r = run_cmd(["adb", "-s", device, "shell", "am", "force-stop", "com.molink.worker"],
                timeout=STOP_TIMEOUT, print_output=True)
    print(r.stdout)
    time.sleep(2)


def verify_access_stopped() -> bool:
    """Return True if molink-access process is NOT running."""
    r = run_cmd(["jps", "-l"], timeout=5)
    for line in r.stdout.splitlines():
        if "com.molink.access.AccessApplication" in line:
            return False
    return True


def verify_worker_stopped(device: str) -> bool:
    """Return True if worker process is NOT running on device."""
    r = run_cmd(["adb", "-s", device, "shell", "ps"], timeout=10)
    for line in r.stdout.splitlines():
        if "com.molink.worker" in line:
            return False
    return True


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def build_worker() -> CmdResult:
    """Run gradle build for worker (Android)."""
    gradle = "gradlew.bat" if sys.platform == "win32" else "./gradlew"
    cmd = [gradle, "clean", "assembleDebug", "--no-daemon"]
    print(f"  $ {' '.join(cmd)}")
    result = run_cmd(cmd, timeout=BUILD_TIMEOUT, cwd=WORKER_DIR, print_output=True)
    return result


def build_access() -> CmdResult:
    """Run maven build for access (Windows)."""
    mvn = "mvn.cmd" if sys.platform == "win32" else "mvn"
    cmd = [mvn, "clean", "package", "-DskipTests"]
    print(f"  $ {' '.join(cmd)}")
    result = run_cmd(cmd, timeout=BUILD_TIMEOUT, cwd=ACCESS_DIR, print_output=True)
    return result


# ---------------------------------------------------------------------------
# Logcat
# ---------------------------------------------------------------------------

def start_logcat(device: str) -> subprocess.Popen:
    """Start adb logcat as background process, filter only com.molink.worker process logs."""
    log_file = open(LOGS_DIR / "worker.log", "w", encoding="utf-8")
    # 查找 worker 进程 PID（pidof 精确匹配包名）
    pid = None
    cmd_pidof = ["adb", "-s", device, "shell", "pidof", "com.molink.worker"]
    print(f"  $ {' '.join(cmd_pidof)}")
    r = run_cmd(cmd_pidof, timeout=10, print_output=True)
    for line in r.stdout.splitlines():
        line = line.strip()
        if line and line.isdigit():
            pid = line
            break
    if pid:
        info(f"Worker PID: {pid}")
        cmd = ["adb", "-s", device, "logcat", "--pid=" + pid]
        print(f"  $ {' '.join(cmd)}")
        proc = subprocess.Popen(cmd, stdout=log_file, stderr=subprocess.DEVNULL)
    else:
        warn("Could not find worker PID, collecting all logs")
        cmd = ["adb", "-s", device, "logcat"]
        print(f"  $ {' '.join(cmd)}")
        proc = subprocess.Popen(cmd, stdout=log_file, stderr=subprocess.DEVNULL)
    return proc


def stop_logcat(proc: subprocess.Popen) -> None:
    """Terminate the logcat background process."""
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


# ---------------------------------------------------------------------------
# Curl testing
# ---------------------------------------------------------------------------

def wait_port(host: str, port: int, timeout: int) -> bool:
    """Wait for a TCP port to become available."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1):
                return True
        except (OSError, socket.timeout):
            time.sleep(0.5)
    return False


def test_direct_connection(curl: str, urls: List[str]) -> tuple:
    """Test direct HTTP connection without proxy. Returns (passed, url, elapsed)."""
    for url in urls:
        start = time.time()
        cmd = [curl, "-i", url, "--max-time", "15"]
        print(f"  $ {' '.join(cmd)}")
        r = run_cmd(cmd, timeout=20, print_output=True)
        elapsed = time.time() - start
        if r.returncode == 0 or "HTTP/" in r.stdout or '"origin"' in r.stdout:
            return (True, url, elapsed)
    return (False, urls[-1], 0)


def test_socks_proxy(curl: str, urls: List[str]) -> tuple:
    """Test HTTP via SOCKS5h proxy. Returns (passed, url, elapsed)."""
    proxy_url = f"socks5h://127.0.0.1:{ADB_PORT}"
    for url in urls:
        start = time.time()
        cmd = [curl, "-i", "-x", proxy_url, url, "--max-time", str(PROXY_TIMEOUT)]
        print(f"  $ {' '.join(cmd)}")
        r = run_cmd(cmd, timeout=PROXY_TIMEOUT + 10, print_output=True)
        elapsed = time.time() - start
        if r.returncode == 0 or "HTTP/" in r.stdout or '"origin"' in r.stdout:
            return (True, url, elapsed)
        fail(f"SOCKS proxy FAIL: {url}")
    return (False, urls[-1], 0)


# ---------------------------------------------------------------------------
# Cleanup and summary
# ---------------------------------------------------------------------------

def cleanup(device: str, access_proc: subprocess.Popen, logcat_proc: subprocess.Popen) -> None:
    """Stop all services: logcat, worker, access."""
    stop_logcat(logcat_proc)
    r = run_cmd(
        ["adb", "-s", device, "shell", "am", "force-stop", "com.molink.worker"],
        timeout=10, print_output=True,
    )
    print(r.stdout)
    if access_proc and access_proc.poll() is None:
        pid = str(access_proc.pid)
        if sys.platform == "win32":
            run_cmd(["taskkill", "/F", "/PID", pid], timeout=5)
        else:
            run_cmd(["kill", "-9", pid], timeout=5)


def summary_and_exit(device: str, results: dict, start_time, exit_code: int) -> None:
    """Write summary report and exit."""
    import datetime
    elapsed = int((datetime.datetime.now() - start_time).total_seconds())
    overall = "ALL PASS" if exit_code == 0 else "SOME FAILURES"

    print("")
    print(f"{'='*40}" + "  MoLink E2E Test Summary ")
    print(f"{'='*50}")
    color = "green" if exit_code == 0 else "red"
    print(_colored(f"Overall: {overall} ({elapsed}s)", color))

    report_lines = [
        f"MoLink E2E Test Report",
        f"Start: {start_time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"Duration: {elapsed}s",
        f"Result: {overall}",
        f"Device: {device}",
        f"Logs: {LOGS_DIR}",
        "",
        "Step results:",
    ]
    for step, (status, desc) in results.items():
        report_lines.append(f"  [{step}] {status}: {desc}")
        print(f"  [{step}] {status}: {desc}")

    LOGS_DIR.mkdir(parents=True, exist_ok=True)
    report_path = LOGS_DIR / "test-report.log"
    report_path.write_text("\n".join(report_lines), encoding="utf-8")
    print(f"\nReport: {report_path}")

    sys.exit(exit_code)


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------

TEST_URLS = ["https://httpbin.org/ip", "https://myip.ipip.net"]


def main() -> None:
    """Run the full E2E test workflow."""
    start_time = datetime.datetime.now()

    exit_code = 0
    results = {}  # {step_label: (status, description)}
    device = None
    access_proc = None
    logcat_proc = None

    # --- Banner ---
    print("")
    print(f"{'='*50}" + " MoLink E2E Test " + "="*(16))
    print(f"Start: {start_time.strftime('%Y-%m-%d %H:%M:%S')}")

    # --- Step 1: Detect ADB device ---
    step = "1"
    t = time.time()
    print(f"[{step}] Detect ADB device...")
    r = run_cmd(["adb", "devices"], timeout=ADB_TIMEOUT)
    device = parse_device_from_adb_devices(r.stdout)
    elapsed = int(time.time() - t)
    if not device:
        step_fail(step, "No Android device detected")
        results[step] = ("FAIL", "No device")
        summary_and_exit(device, results, start_time, 1)
    step_ok(step, f"ADB device {device} detected", elapsed)
    results[step] = ("PASS", f"Device {device}")

    # --- Step 2: Stop old services + verify ---
    step = "2"
    t = time.time()
    print(f"[{step}] Stop old services...")
    stop_access_service()
    stop_worker_service(device)
    time.sleep(2)
    access_ok = verify_access_stopped()
    worker_ok = verify_worker_stopped(device)
    elapsed = int(time.time() - t)
    if not access_ok or not worker_ok:
        step_fail(step, "Service still running after stop")
        results[step] = ("FAIL", "Service not stopped")
        summary_and_exit(device, results, start_time, 1)
    step_ok(step, "Services stopped and verified", elapsed)
    results[step] = ("PASS", "Services stopped")

    # --- Step 3: Cleanup logs ---
    step = "3"
    t = time.time()
    print(f"[{step}] Cleanup logs...")
    cleanup_logs()
    elapsed = int(time.time() - t)
    step_ok(step, "Logs cleaned", elapsed)
    results[step] = ("PASS", "Logs cleaned")

    # --- Step 4: Build worker ---
    step = "4"
    t = time.time()
    print(f"[{step}] Build worker (Android)...")
    r = build_worker()
    elapsed = int(time.time() - t)
    if r.timed_out or r.returncode != 0 or "BUILD SUCCESSFUL" not in r.stdout:
        step_fail(step, "Worker build failed")
        results[step] = ("FAIL", f"Build failed ({elapsed}s)")
        summary_and_exit(device, results, start_time, 1)
    step_ok(step, "Worker built", elapsed)
    results[step] = ("PASS", f"Built in {elapsed}s")

    # --- Step 5: Build access ---
    step = "5"
    t = time.time()
    print(f"[{step}] Build access (Windows)...")
    r = build_access()
    elapsed = int(time.time() - t)
    if r.timed_out or r.returncode != 0 or not ACCESS_JAR.exists():
        step_fail(step, "Access build failed")
        results[step] = ("FAIL", f"Build failed ({elapsed}s)")
        summary_and_exit(device, results, start_time, 1)
    step_ok(step, "Access built", elapsed)
    results[step] = ("PASS", f"Built in {elapsed}s")

    # --- Step 6: Start logcat (BEFORE starting worker service) ---
    step = "6"
    t = time.time()
    print(f"[{step}] Start logcat (background)...")
    run_cmd(["adb", "-s", device, "logcat", "-c"], timeout=10)
    logcat_proc = start_logcat(device)
    elapsed = int(time.time() - t)
    step_ok(step, "Logcat started", elapsed)
    results[step] = ("PASS", "Logcat collecting")

    # --- Step 7: Start services ---
    step = "7"
    t = time.time()
    print(f"[{step}] Start services...")

    if APK_PATH.exists():
        info("Installing APK...")
        r = run_cmd(["adb", "-s", device, "install", "-r", str(APK_PATH)],
                    timeout=INSTALL_TIMEOUT, print_output=True)
        print(r.stdout)
        if r.stderr:
            print(r.stderr, file=sys.stderr)
        info("Launching MainActivity...")
        r = run_cmd(
            ["adb", "-s", device, "shell", "am", "start",
             "-n", "com.molink.worker/.MainActivity"],
            timeout=10, print_output=True,
        )
        print(r.stdout)
        time.sleep(2)

    info("Starting worker service...")
    r = run_cmd(
        ["adb", "-s", device, "shell", "am", "startservice",
         "-n", "com.molink.worker/.Socks5ProxyService"],
        timeout=15, print_output=True,
    )
    print(r.stdout)
    time.sleep(SERVICE_WAIT)

    info("Starting access...")
    access_proc = subprocess.Popen(
        [shutil.which("java") or "java",
         "-Dfile.encoding=UTF-8", "-jar", str(ACCESS_JAR)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=str(PROJECT_ROOT),
    )
    time.sleep(5)

    info("Waiting for access API (port 8080)...")
    api_ready = wait_port("127.0.0.1", 8080, 20)
    elapsed = int(time.time() - t)
    if access_proc.poll() is not None:
        step_fail(step, "Access process exited immediately")
        results[step] = ("FAIL", "Access exited")
        cleanup(device, access_proc, logcat_proc)
        summary_and_exit(device, results, start_time, 1)
    if not api_ready:
        step_fail(step, "Access API not ready")
        results[step] = ("FAIL", "API not ready")
        cleanup(device, access_proc, logcat_proc)
        summary_and_exit(device, results, start_time, 1)
    step_ok(step, "Services started", elapsed)
    results[step] = ("PASS", "All services running")

    # --- Step 8: Curl tests ---
    step = "8"
    t = time.time()
    print(f"[{step}] Curl tests...")
    curl = get_curl_cmd()
    elapsed = int(time.time() - t)

    # 8a: Direct connection
    print("  [8a] Direct connection test...")
    direct_passed, direct_url, direct_elapsed = test_direct_connection(curl, TEST_URLS)
    if direct_passed:
        pass_(f"Direct OK: {direct_url}")
    else:
        warn("Direct connection unreachable (network issue)")

    # 8b: SOCKS proxy (core test)
    print("  [8b] SOCKS proxy test...")
    proxy_passed, proxy_url, proxy_elapsed = test_socks_proxy(curl, TEST_URLS)
    elapsed = int(time.time() - t)
    if proxy_passed:
        step_ok(step, f"SOCKS proxy OK ({proxy_url})", elapsed)
        results[step] = ("PASS", f"Proxy OK via {proxy_url}")
    else:
        step_fail(step, "SOCKS proxy test failed")
        results[step] = ("FAIL", "Proxy unreachable")
        exit_code = 1

    # 8c: Access /api/status verification（请求时实时 curl 测试代理）
    print("  [8c] Access /api/status verification (curl)...")
    curl_cmd = [curl, "-s", "http://127.0.0.1:8080/api/status"]
    print(f"  $ {' '.join(curl_cmd)}")
    r = run_cmd(curl_cmd, timeout=20, print_output=True)
    print(f"  Response:\n{r.stdout}")

    try:
        import json
        api_data = json.loads(r.stdout)
        print(f"  Pretty:\n{json.dumps(api_data, indent=2)}")

        ph = api_data.get("proxyHealth", {})
        ph_available = ph.get("available", None)

        if ph_available is True:
            pass_(f"proxyHealth available=true, latencyMs={ph.get('latencyMs')}")
        else:
            warn(f"proxyHealth available={ph_available}（SOCKS 代理尚未就绪或有延迟）")
    except Exception as e:
        warn(f"Could not verify /api/status: {e}")

    # --- Step 8d: Worker 停止后 /api/status 应正确反映 SOCKS 不可用 ---
    step = "8d"
    t = time.time()
    print(f"[{step}] Worker 停止后 SOCKS 代理不可用验证...")
    info("Stopping worker service...")
    run_cmd(
        ["adb", "-s", device, "shell", "am", "force-stop", "com.molink.worker"],
        timeout=STOP_TIMEOUT, print_output=True,
    )

    # GET /api/status，触发实时 curl 测试 SOCKS 代理
    curl_cmd = [curl, "-s", "http://127.0.0.1:8080/api/status"]
    print(f"  $ {' '.join(curl_cmd)}")
    r = run_cmd(curl_cmd, timeout=20, print_output=True)
    print(f"  Response:\n{r.stdout}")

    try:
        import json
        api_data_after_stop = json.loads(r.stdout)
        ph = api_data_after_stop.get("proxyHealth", {})

        ph_available = ph.get("available", None)
        unavailable_reason = ph.get("unavailableReason", "")

        print(f"  proxyHealth: available={ph_available}, unavailableReason={unavailable_reason}")

        if ph_available is False:
            pass_(f"SOCKS 代理不可用状态正确: available={ph_available}, reason={unavailable_reason}")
            results[step] = ("PASS", f"proxyHealth.available={ph_available}, reason={unavailable_reason}")
        else:
            fail(f"SOCKS 代理不可用状态不正确: proxyHealth.available={ph_available}（期望 false）")
            results[step] = ("FAIL", f"proxyHealth.available={ph_available}（期望 false）")
            exit_code = 1
    except Exception as e:
        fail(f"无法验证停止后的状态: {e}")
        results[step] = ("FAIL", f"异常: {e}")
        exit_code = 1

    # --- Summary + Cleanup ---
    cleanup(device, access_proc, logcat_proc)
    summary_and_exit(device, results, start_time, exit_code)


if __name__ == "__main__":
    main()
