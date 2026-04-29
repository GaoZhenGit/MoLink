# HTTP 代理支持设计文档

**日期**: 2026-04-29
**状态**: 待审批
**分支**: feature/http_proxy

---

## 1. 需求概述

worker 端需要在同一端口上支持 HTTP 代理和 SOCKS5 代理两种协议，通过 UI 开关二选一切换。HTTP 代理仅支持 CONNECT 方法（HTTPS 隧道），使用与 SOCKS5 相同的认证凭据。

## 2. 设计决策

### 2.1 协议选择方式
- 通过 Service 启动 Intent Extra 传递 `proxy_type` 参数（key: `"proxy_type"`）
- 可选值: `"socks5"`（默认）、`"http"`
- 不持久化配置，每次启动默认 SOCKS5
- 切换协议时：stopService → startService(new intent with new proxy_type)

### 2.2 实现方案
- **单一 Server，动态 Pipeline**: Netty ServerSocket 在 `initChannel` 中根据当前 proxyType 初始化不同的 ChannelHandler 链
- 不采用协议自动检测方案（复杂度不必要）

### 2.3 HTTP 代理行为
- 仅支持 CONNECT 方法（隧道模式）
- 需要认证：使用 BuildConfig.SOCKS_USERNAME/PASSWORD 验证 Proxy-Authorization header
- 未配置凭据时跳过认证（与 SOCKS5 行为一致）
- 域名由本端 DNS 解析（与 SOCKS5H 行为一致）

## 3. 架构设计

### 3.1 系统结构

```
[access] → [USB/ADB] → [Socks5ProxyService]
                              │
                              ├── SOCKS5 模式
                              │   └── Socks5StateHandler → ForwardHandler
                              │
                              └── HTTP 模式
                                  └── HttpRequestDecoder → HttpProxyStateHandler → ForwardHandler
```

### 3.2 复用现有组件
- `ConnectionRecord` — 连接记录（无需修改）
- `ConnectionLifecycleManager` — 生命周期管理（无需修改）
- `ForwardHandler` — 双向数据转发（无需修改）
- `StatusHttpServer` — HTTP 状态服务（无需修改）

### 3.3 新增组件
- `HttpProxyStateHandler` — HTTP CONNECT 协议状态机
- `ProxyProtocol` — 枚举类，定义代理协议类型

### 3.4 修改现有组件
- `Socks5ProxyService` — 接收 proxyType 参数，动态选择 pipeline
- `MainActivity` — 添加 Toggle Switch，UI 状态联动

## 4. 详细设计

### 4.1 Socks5ProxyService 变更

**新增字段：**
```java
private ProxyProtocol proxyType = ProxyProtocol.SOCKS5;
```

**onStartCommand 变更：**
- 从 intent 读取 `proxy_type` extra，默认为 `"socks5"`
- 存储到 `proxyType` 字段
- `startServer()` 根据 proxyType 构建不同的 pipeline

**新增方法：**
```java
public ProxyProtocol getProxyType() { return proxyType; }
public int getPort(); // 返回当前代理端口
```

**startSocks5Server 重命名为 startServer：**
- 根据 proxyType 决定 pipeline 配置
- SOCKS5: 添加 `Socks5StateHandler`
- HTTP: 添加 `HttpRequestDecoder` + `HttpProxyStateHandler`

**通知栏更新：**
- 显示当前代理协议类型（"SOCKS5 代理服务运行中" 或 "HTTP 代理服务运行中"）

### 4.2 HttpProxyStateHandler

**职责：**
- 解析 HTTP CONNECT 请求
- 验证 Proxy-Authorization header（Basic Auth）
- 解析目标地址（IP 或域名）
- 建立到目标服务器的连接
- 发送 200 Connection Established 响应
- 切换 pipeline 到 ForwardHandler

**协议流程：**
1. 客户端发送 `CONNECT target:port HTTP/1.1`
2. 可选: 客户端发送 `Proxy-Authorization: Basic <credentials>`
3. 服务端验证凭据（如需认证）
4. 失败: 返回 `407 Proxy Authentication Required`
5. 成功: DNS 解析目标地址，建立 TCP 连接
6. 返回 `HTTP/1.1 200 Connection Established`
7. 从 pipeline 移除自身，添加 ForwardHandler

**状态机：**
```
WAIT_REQUEST → handleConnect() → WAIT_AUTH (如需认证)
WAIT_AUTH → handleAuth() → CONNECTING
CONNECTING → connectToTarget() → FORWARDING (切换 pipeline)
```

**认证实现：**
- 解析 `Proxy-Authorization: Basic <base64(username:password)>` header
- 与 BuildConfig.SOCKS_USERNAME/PASSWORD 比对
- 未配置凭据时跳过认证

**错误响应：**
- 400 Bad Request — 请求格式错误
- 405 Method Not Allowed — 非 CONNECT 方法
- 407 Proxy Authentication Required — 认证失败
- 502 Bad Gateway — 目标连接失败

### 4.3 MainActivity 变更

**新增控件：**
```xml
<androidx.appcompat.widget.SwitchCompat
    android:id="@+id/proxyProtocolSwitch"
    android:text="HTTP"
    ... />
```

**布局变更：**
- 在状态栏下方、统计区上方添加新行
- 左侧标签文字 "代理协议"
- 右侧 SwitchCompat 控件，关闭=SOCKS5，开启=HTTP

**交互逻辑：**
```
Toggle Switch 切换事件:
  → 获取当前 proxyType
  → stopService(currentIntent)
  → startService(newIntent with new proxyType)
```

**UI 状态联动：**
- `showRunning()`: 显示 "SOCKS5 运行中" 或 "HTTP 运行中"
- `showStopped()`: 显示 "SOCKS5 服务已停止" 或 "HTTP 服务已停止"
- 服务停止时 Switch 禁用（灰色）
- 服务启动时根据当前 proxyType 同步 Switch 状态

### 4.4 ProxyProtocol 枚举

```java
public enum ProxyProtocol {
    SOCKS5("SOCKS5"),
    HTTP("HTTP");

    private final String displayName;

    ProxyProtocol(String displayName) {
        this.displayName = displayName;
    }

    public String getDisplayName() { return displayName; }

    public static ProxyProtocol fromString(String value) {
        if ("http".equalsIgnoreCase(value)) return HTTP;
        return SOCKS5;
    }
}
```

## 5. 数据流

### 5.1 SOCKS5 模式（不变）
```
Client → SOCKS5 Handshake → Socks5StateHandler
  → Auth → CONNECT → DNS → Connect to Target
  → 200 OK → ForwardHandler (双向)
```

### 5.2 HTTP 模式（新增）
```
Client → HTTP CONNECT → HttpRequestDecoder → HttpProxyStateHandler
  → Auth → DNS → Connect to Target
  → 200 Connection Established → ForwardHandler (双向)
```

## 6. 错误处理

| 场景 | 处理方式 |
|------|----------|
| HTTP 请求非 CONNECT 方法 | 返回 405，关闭连接 |
| HTTP 认证失败 | 返回 407，关闭连接 |
| 目标地址 DNS 解析失败 | 返回 502，关闭连接 |
| 目标服务器连接失败 | 返回 502，关闭连接 |
| 端口被占用 | 记录错误，停止服务 |
| 切换协议时连接未完全关闭 | 等待 Graceful Shutdown 完成 |

## 7. 测试方案

### 7.1 自动化测试
通过 `test/test.py` 扩展测试用例：

**SOCKS5 模式测试（现有，保持不变）：**
- 基本连接、认证、CONNECT、域名解析

**HTTP 模式测试（新增）：**
- CONNECT 请求基本功能
- HTTP 认证（有效/无效凭据）
- 无认证配置时跳过认证
- 非 CONNECT 方法返回 405
- DNS 解析（IP vs 域名）

**切换测试（新增）：**
- 通过 Intent Extra 启动 HTTP 模式
- 切换协议后服务正常重启

### 7.2 手动测试
- `adb shell am startservice --es proxy_type http` 启动 HTTP 模式
- 配置客户端使用 HTTP 代理 127.0.0.1:1080
- 验证 HTTPS 隧道正常工作
- 验证认证拒绝无效凭据

## 8. 文件变更清单

| 文件 | 变更类型 | 说明 |
|------|----------|------|
| `molink-worker/.../Socks5ProxyService.java` | 修改 | 接收 proxyType，动态 pipeline |
| `molink-worker/.../MainActivity.java` | 修改 | 添加 Toggle Switch |
| `molink-worker/.../activity_main.xml` | 修改 | 添加 SwitchCompat 控件 |
| `molink-worker/.../HttpProxyStateHandler.java` | 新增 | HTTP CONNECT 协议状态机 |
| `molink-worker/.../ProxyProtocol.java` | 新增 | 代理协议枚举 |

## 9. 注意事项

- Gradle 版本、AGP 版本、Android 框架版本均不修改
- 端口号保持不变（BuildConfig.SOCKS_PORT）
- 认证凭据复用 BuildConfig.SOCKS_USERNAME/PASSWORD
- HTTP 模式仅支持 CONNECT，不实现透明代理
