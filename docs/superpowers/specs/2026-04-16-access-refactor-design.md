# MoLink Access 重构设计文档

**日期：** 2026/04/16
**目标：** 重构 molink-access，简化架构，基于 dadb TcpForwarder 实现纯 TCP 转发

---

## 1. 背景与目标

### 现状问题

现有 `AdbForwarder` 绕过 dadb `TcpForwarder`，自行用 `dadb.open()` + 手动线程实现转发，存在以下问题：

- 重复造轮子，dadb `TcpForwarder` 已完整实现相同逻辑
- `PortForwarder` 类为死代码，从未被使用
- 架构不清晰，职责划分混乱

### 重构目标

1. **简化架构**：直接使用 dadb `TcpForwarder`，删除冗余代码
2. **保留 dadb 库**：继续使用 `dev.mobile:dadb:1.2.10`
3. **TCP 纯转发**：access 不解析 SOCKS5 协议，只做透明 TCP 隧道
4. **连接生命周期管理**：每个 TCP 连接 = 独立 AdbStream，断开时自动清理资源（dadb TcpForwarder 内部处理）
5. **不引入 Netty**：现有方案已满足需求，无需增加复杂度

---

## 2. 技术方案

### 核心依赖

- `dev.mobile:dadb:1.2.10` — ADB 协议通信（核心）
- Spring Boot 2.7.x — 应用框架
- `info.picocli:picocli` — CLI 参数解析（已引入）

**无新增依赖，不引入 Netty。**

### dadb TcpForwarder 行为确认

基于字节码分析，`dadb.forwarding.TcpForwarder` 内部：

- 每个 `accept()` 的连接独立管理
- `dadb.open("tcp:PORT")` 创建独立 AdbStream
- finally 块中同时关闭 AdbStream + Socket，EOF 时自动清理
- 使用 `CachedThreadPool`，每个连接 2 个线程（足够满足需求）

### 架构设计

```
应用端 (curl/浏览器)
    │
    ▼
dadb.TcpForwarder (本地端口 1080)
    │ ServerSocket.accept()
    │
    ├── 连接 1 ──→ AdbStream A ──→ ADB USB ──→ worker :1081
    ├── 连接 2 ──→ AdbStream B ──→ ADB USB ──→ worker :1081
    └── 连接 N ──→ AdbStream N ──→ ADB USB ──→ worker :1081
```

- access 在本地端口 1080 监听（SOCKS5 协议由 worker 端处理）
- 每个应用端 TCP 连接通过 `dadb.open()` 独立转发到 worker 的 `:1081`（worker SOCKS5 服务端口）
- 断开时 dadb TcpForwarder 内部自动关闭对应 AdbStream，无需手动管理

---

## 3. 模块设计

### 保留的组件

| 组件 | 职责 |
|------|------|
| `AccessApplication` | Spring Boot 启动入口，bean 初始化 |
| `AdbClientManager` | ADB 连接管理，自动重连 |
| `MolinkProperties` | 配置绑定（端口、认证信息） |
| `StatusController` | REST API (`/api/status`, `/api/config`) |

### 修改的组件

| 组件 | 变更 |
|------|------|
| `StatusController` | `/api/status` 改为查询 TcpForwarder 是否存活，并自行通过 SOCKS5 代理发起 HTTPS 请求测试连通性 |

### 新增的组件

| 组件 | 职责 |
|------|------|
| `ForwarderRunner` | 封装 dadb TcpForwarder 生命周期，启动后阻塞 |
| `ProxyHealthChecker` | 通过 SOCKS5 代理发起 HTTPS 请求，检测链路连通性 |

### 删除的组件

| 组件 | 原因 |
|------|------|
| `AdbForwarder` | 冗余，dadb TcpForwarder 替代 |
| `PortForwarder` | 死代码，从未使用 |

---

## 4. `/api/status` 接口重设计

### 原有逻辑

通过 `curl -x socks5h://localhost:1080` 子进程检测链路连通性，每次请求都启动新进程。

### 新逻辑

`StatusController` 持有 `TcpForwarder` 实例（通过 `ForwarderRunner` 注入），直接在接口内部发起 HTTP 请求通过 SOCKS5 代理测试连通性。

### `GET /api/status` 响应格式

```json
{
  "forwarderAlive": true,
  "connectionState": "CONNECTED",
  "deviceSerial": "xxxxxxxx",
  "proxyHealth": {
    "reachable": true,
    "latencyMs": 123,
    "testUrl": "https://www.google.com",
    "error": null
  },
  "localPort": 1080,
  "remotePort": 1081,
  "uptime": 3600
}
```

字段说明：

| 字段 | 类型 | 说明 |
|------|------|------|
| `forwarderAlive` | boolean | TcpForwarder 是否存活（`TcpForwarder.State != STOPPED`） |
| `connectionState` | string | ADB 连接状态：`WAITING` / `CONNECTED` / `DISCONNECTED` |
| `deviceSerial` | string | USB 连接设备序列号 |
| `proxyHealth.reachable` | boolean | SOCKS5 代理链路是否可达 |
| `proxyHealth.latencyMs` | long | 请求耗时（毫秒） |
| `proxyHealth.testUrl` | string | 本次测试目标 URL |
| `proxyHealth.error` | string | 错误信息（reachable=false 时填写） |
| `localPort` | int | access 本地监听端口 |
| `remotePort` | int | worker 远程端口 |
| `uptime` | long | 服务运行时长（秒） |

### 连通性检测实现

使用 Java 标准 `HttpURLConnection` 或 OkHttp（Spring Boot 自带）通过 SOCKS5 代理发起 GET 请求。测试 URL 按优先级尝试：
1. `https://www.google.com`
2. `https://www.baidu.com`

超时时间 10 秒，失败不影响接口返回，仅记录 `proxyHealth.error`。

---

## 5. 数据流

```
启动流程:
AccessApplication
  ├─ AdbClientManager.startAutoReconnect()   [后台线程，3s 轮询]
  ├─ waitForConnection()                     [阻塞等待设备连接]
  └─ ForwarderRunner.start()
          └─ new dadb.TcpForwarder(dadb, localPort=1080, remotePort=1081)
                  └─ TcpForwarder.start()    [内部 ServerSocket.accept() 循环]

转发流程（每条连接）:
  TcpForwarder.accept() → Socket
      └─ 新线程: AdbStream = dadb.open("tcp:1081")
          ├─ 线程 1: Socket source → AdbStream sink
          └─ 线程 2: AdbStream source → Socket sink
          └─ finally: 关闭 AdbStream + Socket

设备断开流程:
  AdbClientManager 检测到设备断开
      → ConnectionState → DISCONNECTED
      → TcpForwarder.close()
          └─ 关闭所有 AdbStream + ServerSocket
```

---

## 6. 错误处理

| 场景 | 处理方式 |
|------|----------|
| AdbStream 读/写错误 | finally 块自动关闭，dadb TcpForwarder 内部处理 |
| 设备 USB 断开 | `AdbClientManager` 轮询检测，触发重连 |
| 端口被占用 | 启动时捕获 `BindException`，输出友好错误信息 |
| TcpForwarder 启动失败 | 记录日志，优雅退出 |
| SOCKS5 代理连通性检测失败 | 记录 `proxyHealth.error`，接口仍正常返回 |

---

## 7. 配置

继续使用 `MolinkProperties`，无需新增配置项：

```yaml
molink:
  local-port: 1080      # access 本地监听端口
  remote-port: 1081     # worker SOCKS5 服务端口
  api-port: 8080        # REST API 端口
```

---

## 8. 限制与约束

1. **不处理 SOCKS5 协议**：access 为透明 TCP 隧道，SOCKS5 解析由 worker 负责
2. **不引入 Netty**：dadb `TcpForwarder` 使用 Okio + `CachedThreadPool`，满足需求
3. **不修改 worker**：worker 端代码保持不变
4. **不提交 git**：本次重构不提交任何更改
5. **保留 dadb**：核心通信库不变

---

## 9. 测试脚本更新

`test/test.py` 中对 `/api/status` 的测试用例需要更新，以适配新的响应格式和 `forwarderAlive` 字段。

---

## 10. 验收标准

- [ ] `mvn compile` 通过，无编译错误
- [ ] 删除 `AdbForwarder` 和 `PortForwarder`，无遗留引用
- [ ] `ForwarderRunner` 正确启动 `dadb.TcpForwarder`
- [ ] `AdbClientManager` 重连机制正常工作
- [ ] `/api/status` 返回 `forwarderAlive`、`connectionState`、`proxyHealth` 字段
- [ ] `/api/config` 正常响应
- [ ] 自动化测试 `test/test.py` 通过（同步更新测试用例）
