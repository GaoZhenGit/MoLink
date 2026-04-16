# MoLink E2E Test Script 重构设计

**日期**: 2026-04-16
**状态**: 已批准

## 1. 信号处理

在 `main()` 入口处注册 `signal.signal(signal.SIGINT, ...)` 和 `signal.signal(signal.SIGTERM, ...)`，收到信号时调用 `cleanup()` 清理所有子进程（logcat、access）后以退出码 0 退出。解决 Ctrl-C 时 logcat 子进程（adb.exe）未终止导致下次运行时 `logs/molink-worker.log` 文件被占用的问题。

## 2. Logcat 子进程（`LogcatCollector` 类）

封装为独立类，暴露 `start(device) -> subprocess.Popen` 和 `stop(proc)` 接口。

**子进程内部逻辑**：
1. 启动 `adb logcat -c` 同步清屏
2. 轮询 `adb shell pidof com.molink.worker`，间隔 1s，最多 30s
3. 找到 PID 后执行 `adb logcat --pid=<pid>`，输出到 `logs/molink-worker.log`
4. 30s 内未找到 PID，降级为全量 `adb logcat` 写入同一文件

**日志文件路径**：`logs/molink-worker.log`

## 3. Worker Service 启动重试 + 验证

新增函数 `start_worker_service_with_retry(device) -> bool`：

```
for attempt in [1, 2, 3]:
    run `am startservice ... Socks5ProxyService`
    sleep 2
    verify: `dumpsys activity services .../.Socks5ProxyService`
    if service running → return True
    if not last attempt → sleep (5 * attempt) seconds
return False
```

验证命令：`adb shell dumpsys activity services com.molink.worker/.Socks5ProxyService`，输出中查找 service 组件确认运行状态。3 次全部失败则测试退出。

## 4. CLI 参数（argparse）

```python
parser = argparse.ArgumentParser(description="MoLink E2E Test")
parser.add_argument("--no-build", action="store_true",
                    help="跳过 worker 和 access 构建")
args = parser.parse_args()
```

- 无参数：执行完整流程（步骤 4 构建 worker + 步骤 5 构建 access）
- `--no-build`：跳过步骤 4、5，直接进入步骤 6

## 5. 文件路径变更

| 变更 | 原值 | 新值 |
|------|------|------|
| worker 日志文件名 | `logs/worker.log` | `logs/molink-worker.log` |

## 6. 改动范围

仅修改 `test/test.py`，不涉及 `molink-worker` 和 `molink-access` 代码。
