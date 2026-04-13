# MoLink E2E 测试脚本（PowerShell → Python）

## Context

将 `test/test.ps1` 转换为跨平台 Python 脚本，替代 PowerShell 实现 E2E 自动化测试。两个子项目（worker/access）构建产物已存在，测试脚本用于验证整体链路是否通。

## 项目现状

- 项目为纯 Java 项目，无 Python 依赖
- `test/test.ps1` 为唯一测试脚本，7 个线性步骤
- 构建产物路径已知，测试时需要覆盖构建过程
- 当前没有 `docs/` 目录，需要新建

## 关键决策

| 决策项 | 选择 | 原因 |
|--------|------|------|
| 命令执行 | subprocess 标准库 | 零外部依赖，跨平台 |
| 进度显示 | tqdm（唯一外部依赖） | 用户指定 |
| ADB 通信 | subprocess 调用系统 adb | 最简方式，用户指定 |
| 退出策略 | fail-fast（关键步骤）/ 汇总（SOCKS 测试） | 用户指定 |
| 服务验证 | jps -l（access）+ adb shell ps（worker） | 用户指定 |
| SOCKS 协议 | socks5h:// | DNS 解析由代理端完成，符合 MoLink 场景 |
| curl | 跨平台动态检测 curl 命令 | 用户指定 |
| logcat | 后台进程管理 | Step 7 启动（启动 worker 前），Summary 前停止 |

## 退出策略明细

| 步骤 | 失败行为 |
|------|---------|
| 1. ADB 检测 | fail-fast，退出码 1 |
| 2. 清理旧日志 | warn，继续 |
| 3. 停止服务 + 验证 | fail-fast，退出码 1 |
| 4. 构建 worker | fail-fast，退出码 1 |
| 5. 构建 access | fail-fast，退出码 1 |
| 6. 启动 logcat（后台进程） | warn，继续 |
| 7. 启动服务 | fail-fast，退出码 1 |
| 8. Curl 测试 | 记录 PASS/FAIL，继续，汇总 |

## 实现计划

### 文件结构

```
D:/project/MoLink/
├── test/
│   ├── test.ps1        # 保留（参考）
│   └── test.py         # 新建，跨平台测试脚本
```

### 依赖

- 标准库：`subprocess`, `time`, `datetime`, `pathlib`, `socket`, `shutil`, `sys`, `signal`, `os`
- 外部依赖：`tqdm`

### 配置常量

```python
ADB_PORT = 1080
ADB_TIMEOUT = 10
STOP_TIMEOUT = 30
BUILD_TIMEOUT = 60          # worker/access 构建均为 60s
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
```

### 核心函数

| 函数 | 作用 |
|------|------|
| `run_cmd(cmd, timeout, cwd, shell)` | subprocess.run 封装，返回 (returncode, stdout, stderr, elapsed) |
| `get_curl_cmd()` | 跨平台 curl，win32 用 `curl.exe`，其他用 `shutil.which` |
| `info/pass/fail/warn(msg)` | 颜色化打印 |
| `step_ok(step, desc, elapsed)` | 打印 [N] OK |
| `step_fail(step, desc)` | 打印 [N] FAIL，设置 exitCode |
| `parse_device_from_adb_devices(output)` | 解析 adb devices 输出，返回 serial |
| `wait_port(host, port, timeout)` | socket 检测端口就绪 |
| `verify_access_stopped()` | jps -l 确认 access 进程不存在 |
| `verify_worker_stopped(device)` | adb shell ps 确认 worker 进程不存在 |
| `cleanup_logs()` | 清理 logs/ 目录下的旧日志文件 |
| `cleanup(device, access_proc, logcat_proc)` | 清理所有服务和进程 |
| `summary_and_exit(device, results, elapsed)` | 汇总报告并退出 |

### Step 1: ADB 设备检测

- 执行 `adb devices`，解析 serial
- fail-fast：无设备时退出

### Step 2: 清理旧日志

- 删除 `logs/` 目录下所有文件（如果目录存在）
- 重建 `logs/` 目录，确保日志写入干净

### Step 4: 停止旧服务 + 验证

- access: `jps -l` → `kill`（Linux/macOS）或 `taskkill`（Windows）
- worker: `adb shell am force-stop com.molink.worker`
- **双重验证**：jps -l + adb shell ps，确认两者均不存在，否则 fail-fast

### Step 5: 构建 worker

- Windows: `gradlew.bat`，其他: `./gradlew`
- 参数: `clean assembleDebug --no-daemon`
- 超时: 60s，fail-fast

### Step 6: 构建 access

- Windows: `mvn.bat`，其他: `mvn`
- 参数: `clean package -DskipTests`
- 超时: 60s，fail-fast

### Step 7: 启动 logcat（后台进程）

- **在启动 worker service 前执行**，防止遗漏日志
- `adb logcat -c` 清空日志缓冲区
- `adb logcat` 启动为后台进程（Popen）
- stdout 重定向到 `logs/worker.log`
- 进程引用保存，Summary 前调用 `.terminate()`

### Step 8: 启动服务

- 安装 APK（存在时）
- 启动 MainActivity，sleep 2s
- `adb shell am startservice` 启动 worker SOCKS5 service
- sleep SERVICE_WAIT
- 启动 access 后台进程（subprocess.Popen）
- 等待 8080 端口就绪（socket，超时 20s），fail-fast

### Step 9: Curl 测试

#### 7a: 直连测试（网络可达性验证）

- `curl -v <url> --max-time 15`
- PASS: 记录并 break；FAIL: warn 后尝试下一个 URL

#### 7b: SOCKS 代理测试（核心用例）

- `curl -v -x socks5h://127.0.0.1:1080 <url> --max-time 30`
- 控制台打印完整请求头 + 响应头 + 响应体（-v 输出到 stderr，合并打印）
- PASS: `step_ok`，break；FAIL: `step_fail`，记录，继续尝试下一个 URL
- 汇总所有 URL 的 PASS/FAIL 状态

### Summary: 清理 + 报告

1. 停止 logcat 后台进程（`.terminate()`）
2. 停止 worker（`adb shell am force-stop`）
3. 停止 access（`kill` / `taskkill`）
4. 写报告到 `logs/test-report.log`
5. 打印汇总：Overall / 耗时 / 各步骤结果
6. `sys.exit(exitCode)`

## 验证方案

1. **本地运行**: `python test/test.py`，验证 8 个步骤依次执行
2. **关键步骤失败**: 断开 Android 设备，验证 Step 1 fail-fast
3. **服务验证**: 停止旧服务后，检查 Step 3 是否正确 fail-fast
4. **日志完整性**: 确认 `logs/worker.log` 中包含 worker 启动相关日志
5. **清理验证**: 运行后检查无残留 molink-access java 进程
6. **跨平台**: Windows 测试通过后，在 Linux/macOS 上复验

## 文件清单

| 操作 | 路径 |
|------|------|
| 新建 | `D:\project\MoLink\test\test.py` |
| 保留 | `D:\project\MoLink\test\test.ps1` |
| 新建 | `D:\project\MoLink\docs\superpowers\specs\2026-04-13-molink-e2e-python-design.md` |
