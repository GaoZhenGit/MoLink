# MoLink 双端状态显示功能设计

## Context

用户在新版需求文档中增加了"双端状态显示"功能：

- **worker 端（Android）**：在界面上显示代理服务状态（SOCKS 是否运行、端口、连接数等）
- **access 端（Windows）**：提供 REST API 显示连接状态、设备信息、服务运行状态、代理连通性

本设计聚焦于 **access 端如何感知 worker 端状态**，以及 **如何验证 SOCKS 代理通道是否通畅**。

## 整体架构

```
Access (Windows)                           Worker (Android)
┌────────────────────────┐              ┌─────────────────────┐
│ WorkerStatusTracker   │◄── dadb ─────►│ HTTP Server (8081) │
│  · 轮询 /api/status   │  forward     │  GET /api/status     │
│  · 缓存 worker 状态   │  tcp:18081→  │  → socksRunning      │
│                        │    8081      │  → uptime            │
│                        │              │  → memoryUsage       │
└────────────────────────┘              │  → connectionCount   │
        │ SOCKS 代理连通性测试           └─────────────────────┘
        │
        ▼
┌────────────────────────┐              ┌─────────────────────┐
│ Socks5HealthChecker   │──────────────►│ SOCKS5 (端口 1080)  │
│  · 通过代理发测试请求  │  socks5h://   │  → 互联网            │
│  · 验证通道通畅       │  127.0.0.1:   │                     │
│                        │    1080       │                     │
└────────────────────────┘              └─────────────────────┘
```

两条监控路径**职责独立**：
- **Worker HTTP Server（8081）** → worker 进程自身状态（是否运行、CPU/内存、连接数）
- **SOCKS ping 测试** → 代理通道是否通畅（能访问互联网）

## Worker 端

### 技术选型

使用 **NanoHTTPD**（纯 Java 嵌入式 HTTP Server，~60KB，无需 Android 特有 API，Android 8.1+ 完全兼容）。

### HTTP Server

**端口：** 固定 `8081`（独立于 SOCKS 端口 1080）

**启动方式：** 在 `Socks5ProxyService.onStartCommand()` 中同时启动 NanoHTTPD，stop 时一并停止

**依赖（gradle）：**
```
implementation 'org.nanohttpd:nanohttpd:2.3.1'
```

### 接口设计

**GET /api/status** — 完整状态
```json
{
  "socksRunning": true,
  "socksPort": 1080,
  "uptime": 3600,
  "memoryUsage": 45.2,
  "connectionCount": 3,
  "timestamp": 1744567890000
}
```

字段说明：
| 字段 | 类型 | 说明 |
|------|------|------|
| socksRunning | boolean | SOCKS 服务是否运行 |
| socksPort | int | SOCKS 监听端口（固定 1080） |
| uptime | long | 服务启动秒数 |
| memoryUsage | double | 内存使用率（%） |
| connectionCount | int | 当前活跃连接数 |
| timestamp | long | 服务器时间戳 |

**Worker MainActivity UI：** 保持现有逻辑不变，从 `Socks5ProxyService` 获取状态并显示。

## Access 端

### 新增组件

| 类 | 职责 |
|----|------|
| `WorkerStatusTracker` | 封装 dadb forward + HTTP 轮询，缓存 worker 状态 |
| `Socks5HealthChecker` | 通过 SOCKS 代理发测试请求，验证通道通畅 |
| `StatusController`（改造） | `/api/status` 增加 `workerStatus` 和 `proxyHealth` 字段 |

### Dadb 端口转发

Access 启动后，在同一 dadb session 上新增一条端口转发：
```
tcp:18081 → tcp:8081（Worker HTTP Server）
```

与现有 SOCKS 端口转发（`tcp:1080 → tcp:1080`）并行，共享同一个 dadb 连接。

### WorkerStatusTracker

- **轮询间隔：** 10 秒
- **Dadb forward：** 用 `adbClient.forward(18081, 8081)` 建立转发
- **HTTP 请求：** `http://127.0.0.1:18081/api/status`
- **失败处理：** 记录错误，缓存置为 unavailable，不中断主流程
- **状态暴露：** 提供 `WorkerStatus getWorkerStatus()` 方法供 Controller 调用

### Socks5HealthChecker

- **轮询间隔：** 10 秒（与 WorkerStatusTracker 错开，避免并发）
- **测试方式：** 通过 SOCKS 代理（`socks5h://127.0.0.1:1080`）发 HTTPS GET 请求到 `https://httpbin.org/ip`
- **超时：** 10 秒
- **指标：** `available`（布尔）、`latencyMs`（延迟毫秒）、`lastCheck`（时间戳）
- **失败处理：** 记录错误，available 置为 false

### Access /api/status 扩展

**改造前：**
```json
{
  "connected": true,
  "deviceSerial": "RF8N12345",
  "localPort": 1080,
  "remotePort": 1080,
  "reconnectCount": 2,
  "uptime": 7200
}
```

**改造后：**
```json
{
  "connected": true,
  "deviceSerial": "RF8N12345",
  "localPort": 1080,
  "remotePort": 1080,
  "reconnectCount": 2,
  "uptime": 7200,
  "workerStatus": {
    "socksRunning": true,
    "socksPort": 1080,
    "uptime": 3600,
    "memoryUsage": 45.2,
    "connectionCount": 3,
    "lastSeen": 1744567890000
  },
  "proxyHealth": {
    "available": true,
    "latencyMs": 230,
    "lastCheck": 1744567890000
  }
}
```

`workerStatus` 和 `proxyHealth` 字段在 worker 未响应或未连接时可能不存在（向后兼容）。

## 文件变更清单

| 操作 | 路径 |
|------|------|
| 修改 | `molink-worker/app/build.gradle` — 添加 nanohttpd 依赖 |
| 新增 | `molink-worker/app/src/main/java/com/molink/worker/StatusHttpServer.java` — NanoHTTPD HTTP Server |
| 修改 | `molink-worker/app/src/main/java/com/molink/worker/Socks5ProxyService.java` — 启动/停止时管理 HTTP Server |
| 修改 | `molink-access/src/main/java/com/molink/access/adb/AdbClientManager.java` — 新增 forward() 方法支持 |
| 新增 | `molink-access/src/main/java/com/molink/access/status/WorkerStatusTracker.java` — 轮询 worker HTTP 状态 |
| 新增 | `molink-access/src/main/java/com/molink/access/status/Socks5HealthChecker.java` — SOCKS 通道连通性测试 |
| 修改 | `molink-access/src/main/java/com/molink/access/controller/StatusController.java` — 整合 workerStatus + proxyHealth |
| 修改 | `molink-access/src/main/java/com/molink/access/AccessApplication.java` — 启动时初始化 Tracker 和 Checker |

## 验证方案

1. **Worker HTTP Server 独立验证：** 断开 access，直接用 `adb forward tcp:18081 tcp:8081` 后访问 `http://127.0.0.1:18081/api/status`
2. **Access 端 `/api/status` 验证：** 启动完整链路后访问 access 的 `/api/status`，确认 `workerStatus` 和 `proxyHealth` 字段存在
3. **Worker 服务停止场景：** 停止 worker 服务，验证 access 的 `workerStatus.socksRunning` 变为 false
4. **网络中断场景：** 断开 Android 网络，验证 `proxyHealth.available` 变为 false
5. **E2E 测试脚本更新：** 在 `test/test.py` 的 Step 8 中增加对 `/api/status` 新字段的验证
