# Socks5ProxyService NIO 重构计划

## Context

**问题**：当前 `Socks5ProxyService` 每处理一个 SOCKS5 客户端连接创建 2 个专属 Thread，N 个并发连接 = 2N+2 线程，Android 上资源消耗严重。`t1.join()` 串行等待机制导致一个线程阻塞时另一个 join 无法及时执行。任意一端 EOF/FIN/异常的检测和双向关闭逻辑依赖手动的 `shutdownInput/close` 调用，容易遗漏。

**目标**：引入 Netty 4.1.x NIO，用线程池替代 per-connection 线程，单线程事件循环处理所有连接 I/O。ConnectionRecord 数据结构和 UI 逻辑保持不变。

**约束**：
- 严禁提交 git
- 功能验证由用户自行完成，只需保证编译通过
- UI 层 record list 显示逻辑不变，ConnectionRecord 数据结构保持不变
- 支持 SOCKS5H 协议（域名由代理服务端解析）
- 双端 30 秒无任何数据通过则强制断开双向连接并释放资源
- 使用线程池管理线程

---

## 架构设计

### 核心设计原则

- **状态机 Handler**：一个 `Socks5StateHandler` 处理 SOCKS5 协议全流程（握手→认证→CONNECT→转发），通过状态切换
- **共享 SessionContext**：每个 proxy 会话一个 context，存储 clientChannel / targetChannel / record / 双向结束标志 / 超时追踪
- **线程池**：Netty I/O 事件循环在 NioEventLoopGroup（可配置线程数）内执行，Handler 逻辑不额外创建线程
- **强制 30 秒空闲超时**：每个 ForwardHandler 通过 Netty 的 `IdleStateHandler` 触发双向关闭

### SessionContext（per 会话）

```java
class SessionContext {
    final Channel clientChannel;
    volatile Channel targetChannel;              // 异步建立
    final ConnectionRecord record;
    final AtomicInteger endedCount = new AtomicInteger(0);
    final AtomicBoolean destroyed = new AtomicBoolean(false);
    final long createdAt = System.currentTimeMillis();

    SessionContext(Channel client, ConnectionRecord record) { ... }
}
```

### Pipeline 生命周期

```
ServerSocket accept (IPv4 127.0.0.1 + IPv6 ::1 同步监听)
        ↓ channelActive
Socks5StateHandler  (状态机)
  ├─ 状态=WAIT_AUTH_METHOD   → handleAuthMethod()
  ├─ 状态=WAIT_AUTH           → handleUserAuth()
  ├─ 状态=WAIT_COMMAND        → handleConnect()
  │                              解析域名 → 异步 DNS 解析
  │                              异步建立 targetChannel
  │                              connect 成功后发送响应
  │                              在两个 channel.pipeline() 添加:
  │                                IdleStateHandler(30s) → ForwardHandler
  └─ 状态=FORWARDING         → ForwardHandler 接管，Socks5StateHandler 不再处理
```

**SOCKS5H 支持**：`addrType == 0x03` 时，域名解析由本 Service 执行（`InetAddress.getByName()`），不转发给目标 DNS 服务器。`addrType == 0x01` 时直接连接 IP。

### 双向关闭语义

ForwardHandler 中，任一触发均调用 `onDirectionEnded()`：

- `channelInactive(ctx)` — 对端关闭了连接（收到 FIN）
- `exceptionCaught(ctx, cause)` — 连接异常
- `userEventTriggered(ctx, evt)` 且 `evt instanceof IdleStateEvent` — 30 秒空闲超时

`onDirectionEnded()`：
```java
if (endedCount.incrementAndGet() >= 2) {
    destroySession(context);  // 双向 close()，标记 record.ENDED，注销 context
}
```

### 线程池配置

```java
// NioEventLoopGroup 线程池，线程数可配置（默认 1）
NioEventLoopGroup eventLoopGroup = new NioEventLoopGroup(eventLoopThreads);
```

- 连接接受和数据转发均在 eventLoopGroup 线程内执行
- 不再为每个连接创建专属 Thread
- N 个并发连接 = eventLoopThreads 个线程（通常 1-2 个）

### 字节统计

ForwardHandler 在每次 `writeAndFlush()` 时更新字节计数，通过 `ChannelPromiseListener` 确保写入完成后更新 `record.bytesDown/bytesUp`（AtomicLong，线程安全）。

---

## 实现步骤

### Step 1：引入 Netty 依赖

**文件**：`molink-worker/app/build.gradle`

```groovy
dependencies {
    implementation 'io.netty:netty-all:4.1.100.Final'
}
```

### Step 2：新增 `SessionContext`

**文件**：`molink-worker/app/src/main/java/com/molink/worker/netty/SessionContext.java`

存储单个 proxy 会话的所有状态，线程安全。

### Step 3：新增 `ConnectionLifecycleManager`

**文件**：`molink-worker/app/src/main/java/com/molink/worker/netty/ConnectionLifecycleManager.java`

静态工具类，管理所有活跃 SessionContext：
- `register(clientChannel, sessionContext)`
- `findByChannel(channel)` → SessionContext
- `destroy(sessionContext)` → 双向 close() 两个 Channel，标记 record.ENDED，移除注册

内部使用 `ConcurrentHashMap<ChannelId, SessionContext>`。

### Step 4：新增 `ForwardHandler`

**文件**：`molink-worker/app/src/main/java/com/molink/worker/netty/ForwardHandler.java`

ChannelInboundHandlerAdapter，绑定到 clientChannel 和 targetChannel 的 pipeline：

- `channelRead(ctx, msg)`：将数据写入对端 channel，更新字节计数
- `channelInactive(ctx)`：`onDirectionEnded()`
- `exceptionCaught(ctx, cause)`：`onDirectionEnded()`
- `userEventTriggered(ctx, evt)`：处理 IdleStateEvent，调用 `onDirectionEnded()`
- `onDirectionEnded()`：`endedCount++`，当 `>= 2` 时调用 `ConnectionLifecycleManager.destroy()`

对端 channel 由构造参数传入（`otherChannel`）。

### Step 5：新增 `Socks5StateHandler`

**文件**：`molink-worker/app/src/main/java/com/molink/worker/netty/Socks5StateHandler.java`

状态机 Handler（ChannelInboundHandlerAdapter），状态枚举：

```
WAIT_AUTH_METHOD → handleAuthMethod()
WAIT_AUTH        → handleUserAuth()
WAIT_COMMAND     → handleConnect()
FORWARDING       → ForwardHandler 接管，不再进入此处
```

**SOCKS5H 域名解析**：
```java
if (addrType == 0x03) {  // 域名
    InetAddress addr = InetAddress.getByName(domain);  // 本地解析
    destAddr = addr.getHostAddress();
}
// 异步建立 targetChannel
Bootstrap bs = new Bootstrap();
bs.group(ctx.channel().eventLoop())
  .channel(NioSocketChannel.class)
  .handler(new ChannelInitializer<Channel>() {
      @Override
      protected void initChannel(Channel ch) {
          ch.pipeline().addLast(new IdleStateHandler(30, 30, 0, TimeUnit.SECONDS));
          ch.pipeline().addLast(new ForwardHandler(sessionContext.clientChannel, record, "s->c"));
      }
  });
Channel targetCh = bs.connect(destAddr, destPort).channel();
```

连接成功后：
1. 发送 SOCKS5 成功响应给客户端
2. 在 clientChannel.pipeline() 添加 `IdleStateHandler(30,30,0)` + `ForwardHandler`
3. 切换状态为 FORWARDING

### Step 6：重写 `Socks5ProxyService`

**文件**：`molink-worker/app/src/main/java/com/molink/worker/Socks5ProxyService.java`

移除：所有 Thread、Pipe 内部类、forward() 方法、readFully()、handleUserAuth()、setSoTimeout() 调用

新增：Netty 启动逻辑 + 线程池

```java
// 启动
eventLoopGroup = new NioEventLoopGroup(EVENT_LOOP_THREADS);  // 默认 1

ServerBootstrap bs = new ServerBootstrap();
bs.group(eventLoopGroup)
  .channel(NioServerSocketChannel.class)
  .childHandler(new ChannelInitializer<SocketChannel>() {
      @Override
      protected void initChannel(SocketChannel ch) {
          ch.pipeline().addLast(new Socks5StateHandler());
      }
  });

// 兼容 IPv4 + IPv6
bs.bind("127.0.0.1", SOCKS5_PORT).syncUninterruptibly();
bs.bind("::1", SOCKS5_PORT).syncUninterruptibly();
```

`onDestroy` 时调用 `eventLoopGroup.shutdownGracefully()`。

### Step 7：保持 UI 数据结构不变

- `ConnectionRecord` 不变（已有 `bytesDown`、`bytesUp`、`state`、`startTime` 等字段）
- `Socks5ProxyService.getConnectionSnapshot()` 不变
- `MainActivity` / `ConnectionLogAdapter` 不变
- `isEnded()` → 灰色；`!isEnded()` → 绿色

---

## 关键文件清单

| 操作 | 文件路径 |
|------|---------|
| 修改 | `molink-worker/app/build.gradle` |
| 重写 | `molink-worker/app/src/main/java/com/molink/worker/Socks5ProxyService.java` |
| 新增 | `molink-worker/app/src/main/java/com/molink/worker/netty/SessionContext.java` |
| 新增 | `molink-worker/app/src/main/java/com/molink/worker/netty/ConnectionLifecycleManager.java` |
| 新增 | `molink-worker/app/src/main/java/com/molink/worker/netty/Socks5StateHandler.java` |
| 新增 | `molink-worker/app/src/main/java/com/molink/worker/netty/ForwardHandler.java` |
| 无变更 | `MainActivity.java`、`ConnectionLogAdapter.java`、`ConnectionRecord.java` |

---

## 验证方案

1. **编译验证**：`./gradlew assembleDebug`，确认无编译错误
2. 功能验证由用户自行完成
