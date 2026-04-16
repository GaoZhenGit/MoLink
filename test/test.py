"""
MoLink E2E Test Script - Cross-platform Python implementation
"""
import argparse
import datetime
import signal
import sys
import time
import shutil
import socket
import subprocess
import re
import os
from pathlib import Path
import threading
from dataclasses import dataclass
from typing import List, Optional

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

ADB_PORT = 1080
ADB_TIMEOUT = 10
STOP_TIMEOUT = 15
BUILD_TIMEOUT = 120
INSTALL_TIMEOUT = 15
SERVICE_WAIT = 10
LOGCAT_TIMEOUT = 15
PROXY_TIMEOUT = 15

# SOCKS5 认证测试凭证（需与 molink-worker/app/build.gradle 中 BuildConfig 保持一致）
SOCKS_AUTH_USER = "socks5"
SOCKS_AUTH_PASS = "password123"

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


def step_start(step: str, title: str) -> None:
    """打印步骤开始行（cyan）。"""
    print(_colored(f"[{step}] {title}", "cyan"))


def step_ok(step: str, desc: str, elapsed: int) -> None:
    print(_colored(f"[{step}] PASS {desc} ({elapsed}s)", "green"))


def step_fail(step: str, desc: str) -> None:
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
        # 先杀掉可能占用日志文件的 adb logcat 进程
        run_cmd(["adb", "logcat", "-c"], timeout=5)
        try:
            shutil.rmtree(LOGS_DIR)
        except PermissionError:
            # Windows 下 logcat 子进程可能未完全退出，静默忽略
            pass
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


MAX_SERVICE_RETRIES = 3
SERVICE_RETRY_INTERVALS = [5, 10, 15]  # seconds between retries

def is_service_running(device: str) -> bool:
    """Return True if Socks5ProxyService is running on device."""
    r = run_cmd(
        [
            "adb", "-s", device, "shell", "dumpsys",
            "activity", "services",
            "com.molink.worker/.Socks5ProxyService"
        ],
        timeout=10,
    )
    if r.returncode != 0:
        return False
    return "Socks5ProxyService" in r.stdout


MAX_ACTIVITY_RETRIES = 3
ACTIVITY_RETRY_INTERVALS = [5, 10, 15]


def is_activity_running(device: str) -> bool:
    """Return True if MainActivity is in foreground on device."""
    r = run_cmd(
        [
            "adb", "-s", device, "shell", "dumpsys",
            "activity", "activities"
        ],
        timeout=10,
    )
    if r.returncode != 0:
        return False
    for line in r.stdout.splitlines():
        if "mResumedActivity" in line and "MainActivity" in line:
            return True
    return False


def start_mainactivity_with_retry(device: str) -> bool:
    """Launch MainActivity with up to 3 retries and foreground verification."""
    for attempt in range(1, MAX_ACTIVITY_RETRIES + 1):
        info(f"Launching MainActivity (attempt {attempt}/{MAX_ACTIVITY_RETRIES})...")
        r = run_cmd(
            [
                "adb", "-s", device, "shell", "am", "start",
                "-n", "com.molink.worker/.MainActivity"
            ],
            timeout=10,
            print_output=True,
        )
        print(r.stdout)
        time.sleep(2)
        if is_activity_running(device):
            return True
        if attempt < MAX_ACTIVITY_RETRIES:
            wait = ACTIVITY_RETRY_INTERVALS[attempt - 1]
            info(f"MainActivity not running, waiting {wait}s before retry...")
            time.sleep(wait)
    return False


def start_worker_service_with_retry(device: str) -> bool:
    """Start Socks5ProxyService with up to 3 retries and dumpsys verification."""
    for attempt in range(1, MAX_SERVICE_RETRIES + 1):
        info(f"Starting worker service (attempt {attempt}/{MAX_SERVICE_RETRIES})...")
        r = run_cmd(
            [
                "adb", "-s", device, "shell", "am", "startservice",
                "-n", "com.molink.worker/.Socks5ProxyService"
            ],
            timeout=15,
            print_output=True,
        )
        print(r.stdout)
        time.sleep(2)
        if is_service_running(device):
            return True
        if attempt < MAX_SERVICE_RETRIES:
            wait = SERVICE_RETRY_INTERVALS[attempt - 1]
            info(f"Service not running, waiting {wait}s before retry...")
            time.sleep(wait)
    return False


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

_logcat_collector: Optional["LogcatCollector"] = None


class LogcatCollector:
    """Collect worker logs via background thread with PID polling.

    1. Background thread: poll `adb shell pidof com.molink.worker` until PID found (max 60s).
    2. PID found: run `adb logcat --pid=<pid>` (blocking), write to file.
    3. Timeout: run `adb logcat` (all logs), write to file.
    4. stop() terminates the active subprocess.
    """

    LOG_FILE = LOGS_DIR / "molink-worker.log"
    POLL_INTERVAL = 1      # seconds
    POLL_TIMEOUT = 60      # seconds

    def __init__(self, device: str):
        self.device = device
        self._active_proc: Optional[subprocess.Popen] = None
        self._thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()

    def _wait_for_worker_pid(self) -> Optional[str]:
        """Poll worker PID via pidof (fallback to ps) until found or timeout."""
        deadline = time.time() + self.POLL_TIMEOUT
        while time.time() < deadline:
            if self._stop_event.is_set():
                return None
            r = run_cmd(
                ["adb", "-s", self.device, "shell", "pidof", "com.molink.worker"],
                timeout=5,
            )
            if r.returncode == 0:
                for line in r.stdout.splitlines():
                    line = line.strip()
                    if line.isdigit():
                        return line
            # Fallback: try ps if pidof unavailable or no output
            r = run_cmd(
                ["adb", "-s", self.device, "shell", "ps", "-A"],
                timeout=5,
            )
            if r.returncode == 0:
                for line in r.stdout.splitlines():
                    parts = line.split()
                    if len(parts) >= 9 and "com.molink.worker" in parts[-1]:
                        pid = parts[0].strip()
                        if pid.isdigit():
                            return pid
            time.sleep(self.POLL_INTERVAL)
        return None

    def _collector_loop(self) -> None:
        """Background thread: poll for PID first, then start adb logcat --pid (blocking)."""
        log_file = None
        try:
            run_cmd(["adb", "-s", self.device, "logcat", "-c"], timeout=10)
            info("Polling worker PID...")
            pid = self._wait_for_worker_pid()
            if self._stop_event.is_set():
                return
            log_file = open(self.LOG_FILE, "w", encoding="utf-8")
            if pid:
                info(f"Worker PID: {pid}, starting filtered logcat")
                self._active_proc = subprocess.Popen(
                    ["adb", "-s", self.device, "logcat", f"--pid={pid}"],
                    stdout=log_file,
                    stderr=subprocess.DEVNULL,
                )
            else:
                warn("Worker PID not found, starting raw logcat")
                self._active_proc = subprocess.Popen(
                    ["adb", "-s", self.device, "logcat"],
                    stdout=log_file,
                    stderr=subprocess.DEVNULL,
                )
        except Exception as e:
            warn(f"LogcatCollector error: {e}")
            if log_file and not log_file.closed:
                log_file.close()

    def start(self) -> subprocess.Popen:
        """Start background thread and return immediately. Proc may be None until PID found."""
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._collector_loop, daemon=True)
        self._thread.start()
        return self._active_proc or None

    def stop(self, proc: subprocess.Popen) -> None:
        """Stop the thread and whichever subprocess is active."""
        self._stop_event.set()
        if self._active_proc and self._active_proc.poll() is None:
            self._active_proc.terminate()
            try:
                self._active_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._active_proc.kill()
        if self._thread:
            self._thread.join(timeout=5)


def start_logcat(device: str) -> subprocess.Popen:
    """Start adb logcat as background process, filter only com.molink.worker process logs."""
    global _logcat_collector
    _logcat_collector = LogcatCollector(device)
    return _logcat_collector.start()


def stop_logcat(proc: subprocess.Popen) -> None:
    """Terminate the logcat background process."""
    if _logcat_collector:
        _logcat_collector.stop(proc)


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


def test_socks_proxy(curl: str, urls: List[str],
                    username: Optional[str] = None,
                    password: Optional[str] = None,
                    port: Optional[int] = None) -> tuple:
    """Test HTTP via SOCKS5h proxy. Returns (passed, url, elapsed)."""
    target_port = port if port is not None else ADB_PORT
    if username and password:
        proxy_url = f"socks5h://{username}:{password}@127.0.0.1:{target_port}"
    else:
        proxy_url = f"socks5h://127.0.0.1:{target_port}"
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

    # Signal handler for Ctrl-C cleanup
    _cleanup_data = {"device": None, "access_proc": None, "logcat_proc": None}

    def _sig_handler(signum, frame):
        print("\n[Signal] Caught signal, cleaning up...")
        cleanup(
            _cleanup_data["device"] or "",
            _cleanup_data["access_proc"],
            _cleanup_data["logcat_proc"],
        )
        sys.exit(0)

    signal.signal(signal.SIGINT, _sig_handler)
    signal.signal(signal.SIGTERM, _sig_handler)

    exit_code = 0
    results = {}  # {step_label: (status, description)}
    device = None
    access_proc = None
    logcat_proc = None

    # --- Banner ---
    print("")
    print(f"{'='*50}" + " MoLink E2E Test " + "="*(16))
    print(f"Start: {start_time.strftime('%Y-%m-%d %H:%M:%S')}")
    print("")

    parser = argparse.ArgumentParser(description="MoLink E2E Test")
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="跳过 worker 和 access 构建，直接运行测试",
    )
    args = parser.parse_args()

    # --- Step 1: Detect ADB device ---
    step = "1"
    t = time.time()
    step_start(step, "Detect ADB device")
    r = run_cmd(["adb", "devices"], timeout=ADB_TIMEOUT)
    device = parse_device_from_adb_devices(r.stdout)
    elapsed = int(time.time() - t)
    if not device:
        step_fail(step, "No Android device detected")
        results[step] = ("FAIL", "No device")
        summary_and_exit(device, results, start_time, 1)
    step_ok(step, f"ADB device {device} detected", elapsed)
    results[step] = ("PASS", f"Device {device}")
    print("")

    # --- Step 2: Stop old services + verify ---
    step = "2"
    t = time.time()
    step_start(step, "Stop old services")
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
    print("")

    # --- Step 3: Cleanup logs ---
    step = "3"
    t = time.time()
    step_start(step, "Cleanup logs")
    cleanup_logs()
    elapsed = int(time.time() - t)
    step_ok(step, "Logs cleaned", elapsed)
    results[step] = ("PASS", "Logs cleaned")
    print("")

    # --- Step 4: Build worker ---
    if args.no_build:
        info("Skipping worker build (--no-build)")
        results["4"] = ("SKIP", "Skipped by --no-build")
        step_ok("4", "Worker build skipped", 0)
    else:
        step = "4"
        t = time.time()
        step_start(step, "Build worker (Android)")
        r = build_worker()
        elapsed = int(time.time() - t)
        if r.timed_out or r.returncode != 0 or "BUILD SUCCESSFUL" not in r.stdout:
            step_fail(step, "Worker build failed")
            results[step] = ("FAIL", f"Build failed ({elapsed}s)")
            summary_and_exit(device, results, start_time, 1)
        step_ok(step, "Worker built", elapsed)
        results[step] = ("PASS", f"Built in {elapsed}s")
    print("")

    # --- Step 5: Build access ---
    if args.no_build:
        info("Skipping access build (--no-build)")
        results["5"] = ("SKIP", "Skipped by --no-build")
        step_ok("5", "Access build skipped", 0)
    else:
        step = "5"
        t = time.time()
        step_start(step, "Build access (Windows)")
        r = build_access()
        elapsed = int(time.time() - t)
        if r.timed_out or r.returncode != 0 or not ACCESS_JAR.exists():
            step_fail(step, "Access build failed")
            results[step] = ("FAIL", f"Build failed ({elapsed}s)")
            summary_and_exit(device, results, start_time, 1)
        step_ok(step, "Access built", elapsed)
        results[step] = ("PASS", f"Built in {elapsed}s")
    print("")

    # --- Step 6: Start logcat ---
    step = "6"
    t = time.time()
    step_start(step, "Start logcat (background)")
    run_cmd(["adb", "-s", device, "logcat", "-c"], timeout=10)
    logcat_proc = start_logcat(device)
    _cleanup_data["device"] = device
    _cleanup_data["logcat_proc"] = logcat_proc
    elapsed = int(time.time() - t)
    step_ok(step, "Logcat started", elapsed)
    results[step] = ("PASS", "Logcat collecting")
    print("")

    # --- Step 7: Start services ---
    step = "7"
    t = time.time()
    step_start(step, "Start services")
    if APK_PATH.exists():
        info("Installing APK...")
        r = run_cmd(["adb", "-s", device, "install", "-r", str(APK_PATH)],
                    timeout=INSTALL_TIMEOUT, print_output=True)
        print(r.stdout)
        if r.stderr:
            print(r.stderr, file=sys.stderr)
        info("Launching MainActivity...")
        if not start_mainactivity_with_retry(device):
            step_fail(step, "MainActivity launch failed after 3 retries")
            results[step] = ("FAIL", "MainActivity failed after 3 retries")
            cleanup(device, access_proc, logcat_proc)
            summary_and_exit(device, results, start_time, 1)
        time.sleep(2)

    info("Starting worker service...")
    if not start_worker_service_with_retry(device):
        step_fail(step, "Worker service start failed after 3 retries")
        results[step] = ("FAIL", "Worker service failed after 3 retries")
        cleanup(device, access_proc, logcat_proc)
        summary_and_exit(device, results, start_time, 1)
    time.sleep(SERVICE_WAIT)

    info("Starting access...")
    access_env = os.environ.copy()
    access_env["MOLINK_SOCKS_USERNAME"] = SOCKS_AUTH_USER
    access_env["MOLINK_SOCKS_PASSWORD"] = SOCKS_AUTH_PASS
    access_proc = subprocess.Popen(
        [shutil.which("java") or "java",
         "-Dfile.encoding=UTF-8", "-jar", str(ACCESS_JAR)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=str(PROJECT_ROOT),
        env=access_env,
    )
    _cleanup_data["access_proc"] = access_proc
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
    print("")

    # ================================================================
    # Phase 2: API Tests
    # ================================================================
    curl = get_curl_cmd()

    # --- Step 8: GET /molink/devices ---
    step = "8"
    t = time.time()
    step_start(step, "GET /molink/devices")
    curl_cmd = [curl, "-s", "http://127.0.0.1:8080/molink/devices"]
    print(f"  $ {' '.join(curl_cmd)}")
    r = run_cmd(curl_cmd, timeout=20, print_output=True)
    elapsed = int(time.time() - t)
    devices = []
    try:
        import json
        api_data = json.loads(r.stdout)
        print(f"  Pretty:\n{json.dumps(api_data, indent=2)}")
        devices = api_data.get("devices", [])
        alive = [d for d in devices if d.get("forwarderAlive")]
        if len(alive) > 0:
            step_ok(step, f"{len(alive)} device(s) forwarderAlive=true", elapsed)
            results[step] = ("PASS", f"{len(alive)} forwarder alive")
        else:
            step_fail(step, "no device with forwarderAlive=true")
            results[step] = ("FAIL", "no forwarder alive")
            exit_code = 1
    except Exception as e:
        step_fail(step, str(e))
        results[step] = ("FAIL", str(e))
        exit_code = 1
    print("")

    # --- Step 9: GET /molink/config ---
    step = "9"
    t = time.time()
    step_start(step, "GET /molink/config")
    curl_cmd = [curl, "-s", "http://127.0.0.1:8080/molink/config"]
    print(f"  $ {' '.join(curl_cmd)}")
    r = run_cmd(curl_cmd, timeout=20, print_output=True)
    elapsed = int(time.time() - t)
    try:
        import json
        cfg = json.loads(r.stdout)
        print(f"  Response: {json.dumps(cfg, indent=2)}")
        if "localPort" in cfg and "remotePort" in cfg and "apiPort" in cfg:
            step_ok(step, f"config OK: {cfg}", elapsed)
            results[step] = ("PASS", f"localPort={cfg['localPort']}")
        else:
            step_fail(step, f"missing fields: {cfg}")
            results[step] = ("FAIL", "missing fields")
            exit_code = 1
    except Exception as e:
        step_fail(step, str(e))
        results[step] = ("FAIL", str(e))
        exit_code = 1
    print("")

    # --- Step 10: POST /molink/devices/{deviceId}/test ---
    step = "10"
    t = time.time()
    step_start(step, "POST /molink/devices/{deviceId}/test")
    try:
        import json
        alive = [d for d in devices if d.get("forwarderAlive")]
        if not alive:
            step_fail(step, "no device with forwarderAlive=true")
            results[step] = ("FAIL", "no alive device")
            exit_code = 1
        else:
            device_id = alive[0]["deviceId"]
            test_url = f"http://127.0.0.1:8080/molink/devices/{device_id}/test"
            curl_test = [curl, "-s", "-X", "POST", test_url]
            print(f"  $ {' '.join(curl_test)}")
            r2 = run_cmd(curl_test, timeout=20, print_output=True)
            elapsed = int(time.time() - t)
            try:
                test_result = json.loads(r2.stdout)
            except Exception:
                step_fail(step, "access service unresponsive")
                results[step] = ("FAIL", "access service unresponsive")
                exit_code = 1
            else:
                print(f"  Response: {json.dumps(test_result, indent=2)}")
                if test_result.get("passed") is True:
                    step_ok(step, f"passed=true for device {device_id}", elapsed)
                    results[step] = ("PASS", f"device {device_id} proxy reachable")
                else:
                    step_fail(step, f"passed={test_result.get('passed')}, error={test_result.get('error')}")
                    results[step] = ("FAIL", test_result.get("error"))
                    exit_code = 1
    except Exception as e:
        step_fail(step, str(e))
        results[step] = ("FAIL", str(e))
        exit_code = 1
    print("")

    # --- Step 11: POST /molink/devices/{fakeId}/test ---
    step = "11"
    t = time.time()
    step_start(step, "POST /molink/devices/{fakeId}/test")
    curl_test = [curl, "-s", "-X", "POST", "http://127.0.0.1:8080/molink/devices/fake-id-123/test"]
    print(f"  $ {' '.join(curl_test)}")
    r = run_cmd(curl_test, timeout=20, print_output=True)
    elapsed = int(time.time() - t)
    try:
        import json
        result = json.loads(r.stdout)
        print(f"  Response: {json.dumps(result, indent=2)}")
        if result.get("passed") is False and result.get("error") == "Device not found":
            step_ok(step, "Device not found returned correctly", elapsed)
            results[step] = ("PASS", "Device not found")
        else:
            step_fail(step, f"unexpected: {result}")
            results[step] = ("FAIL", f"unexpected: {result}")
            exit_code = 1
    except Exception as e:
        step_fail(step, str(e))
        results[step] = ("FAIL", str(e))
        exit_code = 1
    print("")

    # ================================================================
    # Phase 3: SOCKS Proxy Tests
    # ================================================================

    # --- Step 12: Direct connection (no proxy) ---
    step = "12"
    t = time.time()
    step_start(step, "Direct connection (no proxy)")
    direct_passed, direct_url, direct_elapsed = test_direct_connection(curl, TEST_URLS)
    elapsed = int(time.time() - t)
    if direct_passed:
        step_ok(step, f"OK via {direct_url}", elapsed)
        results[step] = ("PASS", f"Direct OK via {direct_url}")
    else:
        step_fail(step, "Direct unreachable")
        results[step] = ("FAIL", "Direct unreachable")
        exit_code = 1
    print("")

    # --- Step 13: SOCKS wrong credentials ---
    step = "13"
    t = time.time()
    step_start(step, "SOCKS wrong credentials")
    wrong_passed, _, _ = test_socks_proxy(
        curl, TEST_URLS, username="admin", password="wrongpassword")
    elapsed = int(time.time() - t)
    if not wrong_passed:
        step_ok(step, "Wrong credentials rejected", elapsed)
        results[step] = ("PASS", "Wrong credentials rejected")
    else:
        step_fail(step, "Wrong credentials accepted (should be rejected)")
        results[step] = ("FAIL", "Wrong credentials accepted")
        exit_code = 1
    print("")

    # --- Step 14: SOCKS no credentials ---
    step = "14"
    t = time.time()
    step_start(step, "SOCKS no credentials")
    noauth_passed, _, _ = test_socks_proxy(curl, TEST_URLS)
    elapsed = int(time.time() - t)
    if not noauth_passed:
        step_ok(step, "No credentials rejected", elapsed)
        results[step] = ("PASS", "No credentials rejected")
    else:
        step_fail(step, "No credentials accepted (should be rejected)")
        results[step] = ("FAIL", "No credentials accepted")
        exit_code = 1
    print("")

    # --- Step 15: SOCKS correct credentials ---
    step = "15"
    t = time.time()
    step_start(step, "SOCKS correct credentials")
    # 获取第一个存活设备的本地端口
    try:
        import json
        alive = [d for d in devices if d.get("forwarderAlive")]
        proxy_port = alive[0].get("localPort", 1080) if alive else 1080
    except Exception:
        proxy_port = 1080
    proxy_passed, proxy_url, proxy_elapsed = test_socks_proxy(
        curl, TEST_URLS, username=SOCKS_AUTH_USER, password=SOCKS_AUTH_PASS,
        port=proxy_port)
    elapsed = int(time.time() - t)
    if proxy_passed:
        step_ok(step, f"OK via {proxy_url} ({proxy_elapsed}s)", elapsed)
        results[step] = ("PASS", f"Proxy OK via {proxy_url}")
    else:
        step_fail(step, f"Proxy unreachable with correct credentials")
        results[step] = ("FAIL", "Proxy unreachable")
        exit_code = 1
    print("")

    # ================================================================
    # Phase 4: Degradation Test
    # ================================================================

    # --- Step 16: Worker stop -> SOCKS5 unreachable via /test ---
    step = "16"
    t = time.time()
    step_start(step, "Worker 停止后 POST /test 验证")
    info("Stopping worker service...")
    run_cmd(
        ["adb", "-s", device, "shell", "am", "force-stop", "com.molink.worker"],
        timeout=STOP_TIMEOUT, print_output=True,
    )
    time.sleep(2)

    elapsed = int(time.time() - t)
    try:
        import json
        # 从 Step 8 获取的 devices 列表中找到第一个存活设备
        alive = [d for d in devices if d.get("forwarderAlive")]
        if not alive:
            step_fail(step, "no device with forwarderAlive=true")
            results[step] = ("FAIL", "no alive device")
            exit_code = 1
        else:
            device_id = alive[0]["deviceId"]
            test_url = f"http://127.0.0.1:8080/molink/devices/{device_id}/test"
            curl_test = [curl, "-s", "-X", "POST", test_url]
            print(f"  $ {' '.join(curl_test)}")
            r2 = run_cmd(curl_test, timeout=20, print_output=True)
            try:
                test_result = json.loads(r2.stdout)
            except Exception:
                step_fail(step, "access service unresponsive")
                results[step] = ("FAIL", "access service unresponsive")
                exit_code = 1
            else:
                print(f"  Response: {json.dumps(test_result, indent=2)}")
                # Dadb TCP forward 仍存活（TCP 隧道有效），但 SOCKS5 服务已停止，期望 passed=false
                if test_result.get("passed") is False and test_result.get("error") is not None:
                    step_ok(step, f"Dadb alive, proxy unreachable (correct): {test_result.get('error')}", elapsed)
                    results[step] = ("PASS", test_result.get("error"))
                elif test_result.get("passed") is True:
                    step_fail(step, "passed=true but worker is stopped")
                    results[step] = ("FAIL", "worker down but passed=true")
                    exit_code = 1
                else:
                    step_fail(step, f"unexpected: {test_result}")
                    results[step] = ("FAIL", str(test_result))
                    exit_code = 1
    except Exception as e:
        step_fail(step, str(e))
        results[step] = ("FAIL", str(e))
        exit_code = 1
    print("")

    # --- Summary + Cleanup ---
    cleanup(device, access_proc, logcat_proc)
    summary_and_exit(device, results, start_time, exit_code)


if __name__ == "__main__":
    main()
