# Mix 模式设计文档

**日期**: 2026-04-30
**状态**: 待审批
**分支**: feature/mix-mode

---

## 1. 需求概述

worker 端同一端口同时支持 SOCKS5 和 HTTP 代理，根据客户端请求的首字节自动识别协议，无需手动切换。Mix 模式作为 UI 中 SOCKS5 / HTTP / MIX 的第三个选项，默认选中。

## 2. 设计决策

- **协议检测**: 首字节 `0x05` → SOCKS5，其他 → HTTP（CONNECT）
- **检测超时**: `ReadTimeoutHandler(5s)` 防止半连接挂起，检测完成后移除
- **协议切换**: 仅修改 `proxyType` 字段，不重启服务（`ChannelInitializer` 每连接回调读取当前值）
- **HTTP 方法**: 仅支持 CONNECT（与现有 HTTP 模式一致）
- **默认协议**: 首次启动默认 MIX
- **连接日志**: `ConnectionRecord` 增加 `protocol` 字段，日志列表区分显示

## 3. 架构设计

### 3.1 MIX 模式 Pipeline

```
ReadTimeoutHandler(5s) → MixProtocolDetector → [动态分支]
                             │
                             ├── 0x05 → Socks5StateHandler
                             │              → ForwardHandler
                             │
                             └── 其他 → HttpRequestDecoder
                                       → HttpObjectAggregator
                                       → HttpProxyStateHandler
                                       → ForwardHandler
```

`MixProtocolDetector` 检测完成后移除自身和 `ReadTimeoutHandler`，后续由 `IdleStateHandler(30s)` 接管空闲管理。

### 3.2 三种模式对比

```
SOCKS5:  Socks5StateHandler → ForwardHandler
HTTP:    HttpRequestDecoder → HttpObjectAggregator → HttpProxyStateHandler → ForwardHandler
MIX:     ReadTimeoutHandler → MixProtocolDetector → [上述二选一]
```

### 3.3 复用现有组件

- `Socks5StateHandler` — SOCKS5 协议处理（MIX 模式下被 MixProtocolDetector 路由到，其内置 HTTP 拒绝逻辑作为 SOCKS5 独模式安全网保留）
- `HttpProxyStateHandler` — HTTP CONNECT 协议处理（同上，其 `ProtocolDetectingHandler` 作为 HTTP 独模式安全网保留）
- `ForwardHandler` — 双向数据转发，不感知协议
- `ConnectionLifecycleManager` — 生命周期管理
- `StatusHttpServer` — HTTP 状态服务

## 4. 详细设计

### 4.1 ProxyProtocol 枚举

新增 MIX 值:

```java
public enum ProxyProtocol {
    SOCKS5("SOCKS5"),
    HTTP("HTTP"),
    MIX("MIX");

    private final String displayName;

    ProxyProtocol(String displayName) { this.displayName = displayName; }
    public String getDisplayName() { return displayName; }

    public static ProxyProtocol fromString(String value) {
        if ("http".equalsIgnoreCase(value)) return HTTP;
        if ("mix".equalsIgnoreCase(value)) return MIX;
        return SOCKS5;
    }
}
```

### 4.2 MixProtocolDetector（新增）

`ByteToMessageDecoder`，peek 首字节并动态构建下游 pipeline。

```java
public class MixProtocolDetector extends ByteToMessageDecoder {

    @Override
    protected void decode(ChannelHandlerContext ctx, ByteBuf in, List<Object> out) {
        if (in.readableBytes() < 1) return;

        byte firstByte = in.getByte(in.readerIndex());

        if (firstByte == 0x05) {
            ctx.pipeline().addLast(new Socks5StateHandler());
        } else {
            ctx.pipeline().addLast("httpDecoder", new HttpRequestDecoder());
            ctx.pipeline().addLast("httpAggregator", new HttpObjectAggregator(8192));
            ctx.pipeline().addLast(new HttpProxyStateHandler());
        }

        ctx.pipeline().remove("readTimeoutHandler");
        ctx.pipeline().remove(this);
    }
}
```

### 4.3 Socks5ProxyService 变更

- 默认协议改为 `ProxyProtocol.MIX`
- `startServer()` 增加 MIX 分支
- `getProxyType()` 已有，无需修改

```java
// startServer() 中 childHandler 的 MIX 分支
if (proxyType == ProxyProtocol.MIX) {
    ch.pipeline().addLast(new ReadTimeoutHandler(5, TimeUnit.SECONDS));
    ch.pipeline().addLast(new MixProtocolDetector());
} else if (proxyType == ProxyProtocol.HTTP) {
    // 现有 HTTP pipeline（不变）
} else {
    // 现有 SOCKS5 pipeline（不变）
}
```

切换协议仅修改字段（新连接自动生效，已有连接不受影响）:
```java
public void setProxyType(ProxyProtocol type) {
    this.proxyType = type;
}
```

### 4.4 ConnectionRecord 变更

新增 `protocol` 字段:

```java
public class ConnectionRecord {
    public final String clientIp;
    public final String targetHost;
    public final int targetPort;
    public final long connectTime;
    public final String protocol;  // "SOCKS5" 或 "HTTP"
}
```

`Socks5StateHandler` 构造时传入 `"SOCKS5"`，`HttpProxyStateHandler` 传入 `"HTTP"`。

### 4.5 MainActivity 变更

- RadioGroup 新增 MIX RadioButton
- 协议切换逻辑简化：移除 stopService/startService 重启逻辑，改为调用 `service.setProxyType()`
- 移除 `isSwitching` 标志、`setEnabled(false)` 等复杂处理
- RadioButton 始终可点击

```java
protocolRadioGroup.setOnCheckedChangeListener((group, checkedId) -> {
    Socks5ProxyService svc = Socks5ProxyService.getInstance();
    ProxyProtocol newType;
    if (checkedId == R.id.socks5Radio) newType = ProxyProtocol.SOCKS5;
    else if (checkedId == R.id.httpRadio) newType = ProxyProtocol.HTTP;
    else newType = ProxyProtocol.MIX;

    if (svc != null && svc.isRunning()) {
        svc.setProxyType(newType);
    }
});
```

### 4.6 activity_main.xml 变更

RadioGroup 增加第三个 RadioButton:

```xml
<RadioButton
    android:id="@+id/mixRadio"
    android:layout_width="wrap_content"
    android:layout_height="wrap_content"
    android:text="MIX"
    android:textSize="14sp"
    android:checked="true"
    android:layout_marginStart="8dp"/>
```

### 4.7 ConnectionLogAdapter 变更

列表项 binder 增加协议标签显示（如蓝色 "SOCKS5" / 橙色 "HTTP"）。

## 5. 数据流

### MIX 模式 - SOCKS5 连接
```
Client → ReadTimeoutHandler → MixProtocolDetector(检测0x05)
  → Socks5StateHandler → 握手/Auth/CONNECT/DNS
  → ForwardHandler (双向透传)
```

### MIX 模式 - HTTP 连接
```
Client → ReadTimeoutHandler → MixProtocolDetector(检测非0x05)
  → HttpRequestDecoder → HttpObjectAggregator
  → HttpProxyStateHandler → Auth/CONNECT/DNS
  → ForwardHandler (双向透传)
```

## 6. 错误处理

| 场景 | 处理 |
|------|------|
| 5秒内未收到数据 | `ReadTimeoutHandler` 关闭连接 |
| 非 SOCKS5/HTTP 协议 | 路由到 HTTP pipeline，`HttpProxyStateHandler` 返回 400/405 |
| DNS 解析失败 | 返回 502，关闭连接 |
| 目标连接失败 | 返回 502，关闭连接 |
| 认证失败 | SOCKS5 返回 0x01，HTTP 返回 407 |
| 空闲超时 30 秒 | `IdleStateHandler` 关闭连接 |

## 7. 测试方案

- 基本: HTTP + SOCKS5 客户端同时连接同一端口，各自正常通信
- 协议边界: 构造非标准首字节，验证优雅拒绝
- 大文件: 持续下载 >30 秒，验证连接稳定
- 空闲超时: 隧道建立后 30 秒无数据，验证关闭
- 握手超时: 建立 TCP 后不发数据，验证 5 秒超时
- 协议切换: 服务运行中切换 MIX→SOCKS5→HTTP，已有连接不断、新连接正确
- ConnectionRecord: 验证日志中协议字段显示正确

## 8. 文件变更清单

| 文件 | 变更类型 | 说明 |
|------|----------|------|
| `molink-worker/.../MixProtocolDetector.java` | 新增 | 首字节协议检测器 |
| `molink-worker/.../ProxyProtocol.java` | 修改 | 新增 MIX 枚举值 |
| `molink-worker/.../Socks5ProxyService.java` | 修改 | MIX pipeline，默认 MIX，新增 setProxyType() |
| `molink-worker/.../ConnectionRecord.java` | 修改 | 新增 protocol 字段 |
| `molink-worker/.../MainActivity.java` | 修改 | MIX RadioButton，简化切换逻辑（不重启服务） |
| `molink-worker/.../activity_main.xml` | 修改 | 新增 MIX RadioButton |
| `molink-worker/.../ConnectionLogAdapter.java` | 修改 | 列表项显示协议标签 |

## 9. 注意事项

- **禁止提交 git**：实施过程中不得自行 git commit，由用户决定何时提交
- Gradle 版本、AGP 版本、Android 框架版本均不修改
- 端口号不变（BuildConfig.SOCKS_PORT）
- 认证凭据复用 BuildConfig.SOCKS_USERNAME/PASSWORD
- HTTP 仅支持 CONNECT，不实现透明代理
- 协议切换不重启服务，新连接即时生效
