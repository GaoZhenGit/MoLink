# MoLink E2E Test 重构实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 重构 `test/test.py`：添加 Ctrl-C 信号处理、优化 logcat 收集为轮询 PID 方式、worker service 启动增加重试 + 验证、CLI 增加 `--no-build` 参数。

**Architecture:** 在 `main()` 入口注册信号处理器确保 Ctrl-C 时也能清理子进程；logcat 采集重构为 `LogcatCollector` 类，在独立线程中轮询 PID，找到后启动 `adb logcat --pid` 子进程；worker service 启动封装为 `start_worker_service_with_retry()` 函数，内部重试 3 次、每次用 `dumpsys activity services` 验证。

**Tech Stack:** Python 3 标准库（`argparse`, `signal`, `threading`, `subprocess`, `dataclasses`）

---

## 文件变更概览

- **修改**: `test/test.py`

---

## Task 1: 添加信号处理器

- **修改**: `test/test.py`

在 `main()` 函数最开头（在任何耗时的 adb 命令之前）注册 `SIGINT` 和 `SIGTERM` 信号处理器，收到信号时调用 `cleanup()` 清理后以退出码 0 退出。

由于 `cleanup()` 需要 `device`、`access_proc`、`logcat_proc` 三个变量，而这些变量在 `main()` 开头时尚未赋值，需要用 `global` 或将信号处理器改为延迟注册（所有变量赋值后再注册）。

**推荐方案**：在 `main()` 开头定义信号处理器闭包，使用 `nonlocal` 访问外层变量；或者在 `cleanup()` 前判断变量是否为 `None`。

```python
def main() -> None:
    # global references for signal handler
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
    ...
    # 每次赋值后更新
    _cleanup_data["device"] = device
    _cleanup_data["access_proc"] = access_proc
    _cleanup_data["logcat_proc"] = logcat_proc
```

- [ ] **Step 1: 在 `main()` 开头添加信号处理器框架（闭包 + `_cleanup_data` dict）**

在 `def main():` 的第一行后立即添加：
```python
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
```

- [ ] **Step 2: 在 `cleanup()` 调用处更新 `_cleanup_data`**

在步骤 7（启动服务）末尾，`access_proc` 和 `logcat_proc` 赋值后各加一行：
```python
    _cleanup_data["device"] = device
    _cleanup_data["access_proc"] = access_proc
    _cleanup_data["logcat_proc"] = logcat_proc
```

- [ ] **Step 3: 提交**
```bash
git add test/test.py
git commit -m "feat(test): add SIGINT/SIGTERM handler to cleanup on Ctrl-C"
```

---

## Task 2: LogcatCollector 类

- **修改**: `test/test.py`

将原来的 `start_logcat(device) -> subprocess.Popen` 和 `stop_logcat(proc)` 重构为 `LogcatCollector` 类，文件路径改为 `logs/molink-worker.log`。

### 类设计

```python
class LogcatCollector:
    LOG_FILE = LOGS_DIR / "molink-worker.log"
    POLL_INTERVAL = 1          # seconds
    POLL_TIMEOUT = 30           # seconds
    SERVICE_NAME = "com.molink.worker/.Socks5ProxyService"

    def __init__(self, device: str):
        self.device = device
        self._proc: Optional[subprocess.Popen] = None
        self._thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()

    def _verify_service_running(self) -> bool:
        """Check if Socks5ProxyService is running via dumpsys."""
        r = run_cmd(
            ["adb", "-s", self.device, "shell", "dumpsys",
             "activity", "services", self.SERVICE_NAME],
            timeout=10,
        )
        return r.returncode == 0 and self.SERVICE_NAME.split("/")[-1] in r.stdout

    def _wait_for_worker_pid(self) -> Optional[str]:
        """Poll pidof until worker PID found or timeout."""
        deadline = time.time() + self.POLL_TIMEOUT
        while time.time() < deadline:
            if self._stop_event.is_set():
                return None
            r = run_cmd(
                ["adb", "-s", self.device, "shell", "pidof", "com.molink.worker"],
                timeout=5,
            )
            for line in r.stdout.splitlines():
                line = line.strip()
                if line.isdigit():
                    return line
            time.sleep(self.POLL_INTERVAL)
        return None

    def _collector_loop(self) -> None:
        """Thread target: collect raw logs until PID found, then switch to --pid."""
        # 1. Start raw logcat immediately (clears buffer first)
        run_cmd(["adb", "-s", self.device, "logcat", "-c"], timeout=10)
        log_file = open(self.LOG_FILE, "w", encoding="utf-8")
        raw_proc: Optional[subprocess.Popen] = None
        try:
            raw_proc = subprocess.Popen(
                ["adb", "-s", self.device, "logcat"],
                stdout=log_file,
                stderr=subprocess.DEVNULL,
            )
            # 2. Poll for PID
            pid = self._wait_for_worker_pid()
            # 3. Stop raw collection
            if raw_proc and raw_proc.poll() is None:
                raw_proc.terminate()
                try:
                    raw_proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    raw_proc.kill()
            log_file.close()
            if self._stop_event.is_set():
                return
            # 4. Switch to --pid logcat
            if pid:
                info(f"Worker PID: {pid}, switching to filtered logcat")
            else:
                warn("Worker PID not found in 30s, collecting all logs")
            mode = f"--pid={pid}" if pid else "(all logs)"
            info(f"Logcat mode: {mode}")
            log_file = open(self.LOG_FILE, "a", encoding="utf-8")
            cmd = (
                ["adb", "-s", self.device, "logcat", f"--pid={pid}"]
                if pid
                else ["adb", "-s", self.device, "logcat"]
            )
            self._proc = subprocess.Popen(cmd, stdout=log_file, stderr=subprocess.DEVNULL)
        except Exception as e:
            warn(f"LogcatCollector error: {e}")
            if not log_file.closed:
                log_file.close()
            if raw_proc and raw_proc.poll() is None:
                raw_proc.terminate()

    def start(self) -> subprocess.Popen:
        """Start background thread, return the --pid logcat Popen (or None)."""
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._collector_loop, daemon=True)
        self._thread.start()
        return self._proc  # may be None if PID not yet found

    def stop(self, proc: subprocess.Popen) -> None:
        """Stop the thread and the logcat subprocess."""
        self._stop_event.set()
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        if self._thread:
            self._thread.join(timeout=5)
```

### start_logcat / stop_logcat 兼容层

保留原有函数签名，只在内部委托给 `LogcatCollector`：

```python
_logcat_collector: Optional[LogcatCollector] = None

def start_logcat(device: str) -> subprocess.Popen:
    global _logcat_collector
    _logcat_collector = LogcatCollector(device)
    return _logcat_collector.start()

def stop_logcat(proc: subprocess.Popen) -> None:
    if _logcat_collector:
        _logcat_collector.stop(proc)
```

- [ ] **Step 1: 新增 `LogcatCollector` 类（完整实现如上），放在 `start_logcat` 函数定义处之前**

- [ ] **Step 2: 将 `start_logcat` / `stop_logcat` 改为委托给 `LogcatCollector` 的兼容层**

- [ ] **Step 3: 更新所有 `LOGS_DIR / "worker.log"` 为 `LOGS_DIR / "molink-worker.log"`（`cleanup_logs()` 引用 `LOGS_DIR` 是目录，不受影响）**

用 `replace_all` 全局替换：`"worker.log"` → `"molink-worker.log"`

- [ ] **Step 4: 更新 `cleanup_logs()` 中 `shutil.rmtree(LOGS_DIR)` — 由于 `stop_logcat` 已保证文件句柄释放，直接删除目录即可，无需额外处理**

- [ ] **Step 5: 提交**
```bash
git add test/test.py
git commit -m "feat(test): refactor logcat into LogcatCollector with PID polling"
```

---

## Task 3: Worker Service 启动重试 + 验证

- **修改**: `test/test.py`

新增 `start_worker_service_with_retry(device: str) -> bool` 函数，放在 `stop_worker_service()` 附近。

```python
MAX_SERVICE_RETRIES = 3
SERVICE_RETRY_INTERVALS = [5, 10, 15]  # seconds between retries (cumulative with attempt)


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
    # Match the service class name in output
    return "Socks5ProxyService" in r.stdout


def start_worker_service_with_retry(device: str) -> bool:
    """Start Socks5ProxyService with up to 3 retries and verification."""
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
```

- [ ] **Step 1: 新增 `is_service_running()` 和 `start_worker_service_with_retry()` 函数**

- [ ] **Step 2: 将步骤 7 中的 worker service 启动逻辑替换为 `start_worker_service_with_retry(device)`，3 次都失败则走 `summary_and_exit` 退出**

在步骤 7 中，删除原有的：
```python
r = run_cmd(
    ["adb", "-s", device, "shell", "am", "startservice",
     "-n", "com.molink.worker/.Socks5ProxyService"],
    timeout=15, print_output=True,
)
print(r.stdout)
if r.returncode != 0:
    step_fail(step, "Worker service start failed")
    results[step] = ("FAIL", "Worker service start failed")
    cleanup(device, access_proc, logcat_proc)
    summary_and_exit(device, results, start_time, 1)
time.sleep(SERVICE_WAIT)
```

替换为：
```python
if not start_worker_service_with_retry(device):
    step_fail(step, "Worker service start failed after 3 retries")
    results[step] = ("FAIL", "Worker service failed after 3 retries")
    cleanup(device, access_proc, logcat_proc)
    summary_and_exit(device, results, start_time, 1)
```

- [ ] **Step 3: 提交**
```bash
git add test/test.py
git commit -m "feat(test): add worker service retry + dumpsys verification"
```

---

## Task 4: 添加 argparse

- **修改**: `test/test.py`

在 `main()` 入口处添加 `argparse` 解析。

- [ ] **Step 1: 在文件顶部 import 添加 `argparse`**（如果还没有）

检查现有 import 列表，添加 `import argparse`（Python 3.2+ 标准库）。

- [ ] **Step 2: 在 `main()` 函数开头 `step = "1"` 之前插入参数解析**

```python
    import argparse
    parser = argparse.ArgumentParser(description="MoLink E2E Test")
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="跳过 worker 和 access 构建，直接运行测试",
    )
    args = parser.parse_args()
```

- [ ] **Step 3: 将步骤 4（构建 worker）和步骤 5（构建 access）包裹为 `if not args.no_build:`**

在步骤 4 开头加：
```python
    if args.no_build:
        info("Skipping worker build (--no-build)")
        results["4"] = ("SKIP", "Skipped by --no-build")
        step_ok("4", "Worker build skipped", 0)
    else:
```

将步骤 4 末尾的：
```python
    step_ok(step, "Worker built", elapsed)
    results[step] = ("PASS", f"Built in {elapsed}s")
```
保持不变。后续逻辑无需 `else:` 包裹，if 内 `continue` 或 `summary_and_exit` 即可提前退出。

同样处理步骤 5。

- [ ] **Step 4: 提交**
```bash
git add test/test.py
git commit -m "feat(test): add --no-build CLI argument to skip build steps"
```

---

## Task 5: 最终验证与汇总

- [ ] **Step 1: 检查所有变更完整性**

确认：
- `LOGS_DIR / "molink-worker.log"` 贯穿全文
- `signal` 已在顶部 import
- `start_worker_service_with_retry` 在步骤 7 中被调用
- `--no-build` 控制步骤 4、5 的执行

- [ ] **Step 2: 运行 `python test/test.py --help` 验证 argparse 正常工作**

预期输出包含 `--no-build` 选项说明。

- [ ] **Step 3: 提交最终变更**
```bash
git add test/test.py
git commit -m "refactor(test): complete E2E script refactor

- LogcatCollector: poll PID before starting filtered logcat
- Worker service: retry 3x with dumpsys verification
- Signal handler: cleanup on Ctrl-C
- CLI: --no-build to skip build steps"
```

---

## 自检清单

| 检查项 | 状态 |
|--------|------|
| `signal` 在 `test/test.py` 顶部 import | |
| Ctrl-C 时 `cleanup()` 能正确清理 logcat 子进程 | |
| `logs/molink-worker.log` 文件名全部一致 | |
| `start_worker_service_with_retry` 返回 bool，失败走 `summary_and_exit` | |
| `python test/test.py --help` 显示 `--no-build` | |
| `--no-build` 时步骤 4、5 跳过，步骤 6+ 继续执行 | |
