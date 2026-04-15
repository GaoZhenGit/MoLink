# Access Netty NIO 重构计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 molink-access 改造为 Netty NIO 模式，替换现有的阻塞式 AdbForwarder，支持快速中断（Ctrl-C 等场景）。

**Architecture:** 使用 Netty NioEventLoopGroup 实现端口转发服务器，ADB 数据流通过 Okio + 专属后台线程处理，不封装为 Netty Channel。SessionContext + ConnectionLifecycleManager 模式管理连接生命周期。原有 Spring Boot HTTP API 和 AdbClientManager 保持不变。

**Tech Stack:** Netty 4.1.x, Spring Boot 2.7.18, dadb 1.2.10

---

## 最终设计（方案 B）

### 数据流架构

```
[curl/应用]
    ↕ NioSocketChannel
[Netty Pipeline]
    ↕
[AdbStreamHandler] (InboundHandler)
    ↕
[dadb AdbStream] ←→ USB/ADB ←→ [Android SOCKS5]
```

### Pipeline 结构

```
NioSocketChannel (local client)
  └─ IdleStateHandler (30s idle)
  └─ AdbStreamHandler (InboundHandlerAdapter)
       ├─ channelRead()        → AdbStream.getSink().write()  [curl → ADB]
       ├─ channelInactive()    → destroy()  [FIN 到达，触发双向关闭]
       ├─ exceptionCaught()   → destroy()  [异常，触发双向关闭]
       └─ write() 失败          → ChannelFutureListener → destroy()
```

### 双向中断链路

| 触发源 | 事件 | 响应 |
|--------|------|------|
| curl Ctrl-C / socket FIN | `channelInactive()` | `destroy()` → 关闭 adbStream + localChannel |
| ADB stream 关闭 | 后台 read 返回 -1 | `adbStream.close()` + `localChannel.close()` |
| 写入 ADB 失败 | `ChannelFutureListener` | `destroy()` |
| 30s 无数据 | `IdleStateEvent` | `destroy()` |

### SessionContext 查找

- 按 `localChannel.id()` 索引到 `ConcurrentHashMap`
- 后台线程读 ADB 时，通过 `localChannel` 查找 sessionCtx

---

## 文件结构

```
molink-access/src/main/java/com/molink/access/
├── forwarder/
│   ├── AdbForwarderNetty.java       # 新增：Netty 版端口转发器
│   ├── AdbStreamHandler.java         # 新增：InboundHandler，处理 curl→ADB
│   ├── SessionContext.java           # 新增：会话上下文
│   └── ConnectionLifecycleManager.java  # 新增：全局生命周期管理
├── AccessApplication.java           # 修改：Bean 初始化
└── config/MolinkProperties.java     # 参考：配置属性
```

---

## Task 1: 添加 Netty 依赖

**Files:**
- Modify: `molink-access/pom.xml`

- [ ] **Step 1: 在 pom.xml 中添加 Netty 依赖**

在 `<dependencies>` 中添加：

```xml
<!-- Netty NIO -->
<dependency>
    <groupId>io.netty</groupId>
    <artifactId>netty-all</artifactId>
    <version>4.1.100.Final</version>
</dependency>
```

---

## Task 2: 创建 SessionContext

**Files:**
- Create: `molink-access/src/main/java/com/molink/access/forwarder/SessionContext.java`

- [ ] **Step 1: 编写 SessionContext 类**

```java
package com.molink.access.forwarder;

import io.netty.channel.Channel;
import dadb.AdbStream;

import java.util.concurrent.atomic.AtomicBoolean;

/**
 * 单个转发会话的上下文。
 * 存储 localChannel（NioSocketChannel） 和 adbStream（dadb）。
 * 按 localChannel.id() 索引到 ConnectionLifecycleManager。
 */
public final class SessionContext {

    public final Channel localChannel;   // 本地客户端连接（NioSocketChannel）
    public final AdbStream adbStream;    // ADB stream（非 Channel）

    /** 防止重复销毁 */
    public final AtomicBoolean destroyed = new AtomicBoolean(false);
    public final long createdAt = System.currentTimeMillis();

    public SessionContext(Channel localChannel, AdbStream adbStream) {
        this.localChannel = localChannel;
        this.adbStream = adbStream;
    }

    public boolean isDestroyed() {
        return destroyed.get();
    }

    public void markDestroyed() {
        destroyed.set(true);
    }
}
```

---

## Task 3: 创建 ConnectionLifecycleManager

**Files:**
- Create: `molink-access/src/main/java/com/molink/access/forwarder/ConnectionLifecycleManager.java`

- [ ] **Step 1: 编写 ConnectionLifecycleManager 类**

```java
package com.molink.access.forwarder;

import io.netty.channel.Channel;
import io.netty.channel.ChannelId;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.concurrent.ConcurrentHashMap;

/**
 * 全局会话生命周期管理器。
 * 管理所有活跃 SessionContext，提供双向关闭、标记结束等操作。
 * 线程安全。
 */
public final class ConnectionLifecycleManager {

    private static final Logger log = LoggerFactory.getLogger(ConnectionLifecycleManager.class);
    private static final ConnectionLifecycleManager INSTANCE = new ConnectionLifecycleManager();

    private final ConcurrentHashMap<ChannelId, SessionContext> contexts = new ConcurrentHashMap<>();

    private ConnectionLifecycleManager() {}

    public static ConnectionLifecycleManager getInstance() {
        return INSTANCE;
    }

    public void register(Channel localChannel, SessionContext ctx) {
        contexts.put(localChannel.id(), ctx);
    }

    public SessionContext findByChannel(Channel channel) {
        return contexts.get(channel.id());
    }

    /**
     * 销毁会话：双向关闭 localChannel 和 adbStream。
     * 幂等：已销毁则直接返回，safeCloseAdbStream 用 try-catch 防止重复关闭。
     */
    public void destroy(SessionContext sessionCtx) {
        if (sessionCtx == null || sessionCtx.isDestroyed()) {
            return;
        }
        sessionCtx.markDestroyed();

        log.debug("Destroying session");

        Channel local = sessionCtx.localChannel;

        if (local != null && local.isOpen()) {
            local.close();
        }

        safeCloseAdbStream(sessionCtx.adbStream);

        contexts.remove(local.id());
    }

    private void safeCloseAdbStream(dadb.AdbStream stream) {
        if (stream == null) return;
        try {
            stream.close();
        } catch (Exception ignored) {}
    }

    /**
     * 任一方向结束时调用，立即关闭对端，触发双向同步关闭。
     * 幂等：已销毁则直接返回。
     */
    public void onDirectionEnded(SessionContext sessionCtx) {
        if (sessionCtx == null || sessionCtx.isDestroyed()) {
            return;
        }
        destroy(sessionCtx);
    }
}
```

---

## Task 4: 创建 AdbStreamHandler

**Files:**
- Create: `molink-access/src/main/java/com/molink/access/forwarder/AdbStreamHandler.java`

- [ ] **Step 1: 编写 AdbStreamHandler 类**

```java
package com.molink.access.forwarder;

import io.netty.buffer.ByteBuf;
import io.netty.channel.ChannelFutureListener;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * 处理 curl→ADB 方向的 InboundHandler。
 *
 * 触发双向关闭的条件（任一满足）：
 * - channelInactive：对端关闭了连接（收到 FIN）
 * - exceptionCaught：连接异常
 * - write 失败：写入 ADB stream 时发现对端已死
 */
public final class AdbStreamHandler extends ChannelInboundHandlerAdapter {

    private static final Logger log = LoggerFactory.getLogger(AdbStreamHandler.class);

    private final SessionContext sessionCtx;

    public AdbStreamHandler(SessionContext sessionCtx) {
        this.sessionCtx = sessionCtx;
    }

    @Override
    public void channelRead(ChannelHandlerContext ctx, Object msg) throws Exception {
        if (!(msg instanceof ByteBuf)) {
            ctx.fireChannelRead(msg);
            return;
        }
        ByteBuf buf = (ByteBuf) msg;
        int len = buf.readableBytes();

        // 将数据写入 ADB stream
        // AdbStream.getSink().write() 内部会 buffer，不阻塞 EventLoop
        try {
            byte[] bytes = new byte[len];
            buf.readBytes(bytes);
            sessionCtx.adbStream.getSink().write(bytes);
            sessionCtx.adbStream.getSink().flush();
        } finally {
            buf.release();
        }

        // 写入失败监听（通过 flush 触发的 future）
        // 注：dadb 的 write 是同步的，如果失败会抛异常，这里用 try-catch 处理
    }

    @Override
    public void channelInactive(ChannelHandlerContext ctx) throws Exception {
        log.debug("local channel inactive (FIN/RST received)");
        // 收到 FIN → 关闭 ADB stream，触发双向关闭
        ConnectionLifecycleManager.getInstance().onDirectionEnded(sessionCtx);
        super.channelInactive(ctx);
    }

    @Override
    public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) throws Exception {
        String msg = cause.getMessage();
        if (msg == null || (!msg.contains("Connection reset")
                && !msg.contains("Broken pipe")
                && !msg.contains("Socket closed")
                && !msg.contains("EOF"))) {
            log.debug("exception: {}", msg);
        }
        ConnectionLifecycleManager.getInstance().onDirectionEnded(sessionCtx);
        super.exceptionCaught(ctx, cause);
    }

    @Override
    public void userEventTriggered(ChannelHandlerContext ctx, Object evt) throws Exception {
        if (evt instanceof io.netty.handler.timeout.IdleStateEvent) {
            log.debug("idle timeout (30s), closing both sides");
            ConnectionLifecycleManager.getInstance().onDirectionEnded(sessionCtx);
        }
        super.userEventTriggered(ctx, evt);
    }
}
```

---

## Task 5: 创建 AdbForwarderNetty（核心组件）

**Files:**
- Create: `molink-access/src/main/java/com/molink/access/forwarder/AdbForwarderNetty.java`

- [ ] **Step 1: 编写 AdbForwarderNetty 类**

```java
package com.molink.access.forwarder;

import com.molink.access.adb.AdbClientManager;
import io.netty.bootstrap.ServerBootstrap;
import io.netty.channel.*;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.nio.NioServerSocketChannel;
import io.netty.handler.timeout.IdleStateHandler;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.Closeable;
import java.util.concurrent.TimeUnit;

/**
 * Netty NIO 版 ADB 端口转发器，替代原有的 AdbForwarder。
 *
 * 数据流：
 * [本地客户端] → [NioServerSocketChannel:localPort] → [AdbStream] → [Android SOCKS5 服务]
 *
 * 关键设计：
 * - NioEventLoopGroup 处理所有连接
 * - 本地连接用 Pipeline：IdleStateHandler + AdbStreamHandler
 * - ADB 数据读在专属后台线程（不阻塞 EventLoop），通过 localChannel.writeAndFlush() 写回
 * - SessionContext + ConnectionLifecycleManager 管理生命周期
 * - 支持快速中断：channel.close() → FIN → adbStream.close() → Android 端立即收到
 */
public final class AdbForwarderNetty implements Closeable {

    private static final Logger log = LoggerFactory.getLogger(AdbForwarderNetty.class);

    private final AdbClientManager adbClient;
    private final int localPort;
    private final int remotePort;

    private EventLoopGroup bossGroup;
    private EventLoopGroup workerGroup;
    private volatile boolean running = true;
    private Channel serverChannel;

    public AdbForwarderNetty(AdbClientManager adbClient, int localPort, int remotePort) {
        this.adbClient = adbClient;
        this.localPort = localPort;
        this.remotePort = remotePort;
    }

    public void start() {
        if (running) return;
        running = true;

        bossGroup = new NioEventLoopGroup(1);
        workerGroup = new NioEventLoopGroup();

        ServerBootstrap sb = new ServerBootstrap();
        sb.group(bossGroup, workerGroup)
          .channel(NioServerSocketChannel.class)
          .childHandler(new ChannelInitializer<Channel>() {
              @Override
              protected void initChannel(Channel ch) throws Exception {
                  handleLocalChannel(ch);
              }
          })
          .bind(localPort)
          .addListener((ChannelFutureListener) future -> {
              if (future.isSuccess()) {
                  serverChannel = future.channel();
                  log.info("AdbForwarderNetty started: localhost:{} -> Android:{}", localPort, remotePort);
              } else {
                  log.error("Failed to bind port {}: {}", localPort, future.cause().getMessage());
              }
          });
    }

    private void handleLocalChannel(Channel localChannel) {
        dadb.Dadb dadbConn = adbClient.getDadb();
        if (dadbConn == null) {
            log.warn("ADB not connected");
            localChannel.close();
            return;
        }

        try {
            // 打开 ADB stream
            String remoteAddr = "tcp:localhost:" + remotePort;
            dadb.AdbStream adbStream = dadbConn.open(remoteAddr);
            log.debug("ADB stream opened for local: {}", localChannel.remoteAddress());

            // 创建 SessionContext 并注册
            SessionContext sessionCtx = new SessionContext(localChannel, adbStream);
            ConnectionLifecycleManager.getInstance().register(localChannel, sessionCtx);

            // 添加 Pipeline
            localChannel.pipeline().addLast(new IdleStateHandler(0, 0, 30, TimeUnit.SECONDS));
            localChannel.pipeline().addLast(new AdbStreamHandler(sessionCtx));

            // 启动后台线程读取 ADB stream 数据
            new Thread(() -> readFromAdb(adbStream, localChannel), "adb-reader").start();

        } catch (Exception e) {
            log.debug("Failed to open ADB stream: {}", e.getMessage());
            localChannel.close();
        }
    }

    private void readFromAdb(dadb.AdbStream adbStream, Channel localChannel) {
        try {
            byte[] buf = new byte[8192];
            while (running && localChannel.isOpen()) {
                int len = adbStream.read(buf);
                if (len > 0) {
                    // 将 ADB 数据写入本地 socket
                    ByteBuf nettyBuf = localChannel.alloc().buffer(len);
                    nettyBuf.writeBytes(buf, 0, len);
                    localChannel.writeAndFlush(nettyBuf);
                } else if (len == -1) {
                    // ADB stream 关闭（EOF）
                    log.debug("ADB stream EOF");
                    ConnectionLifecycleManager.getInstance()
                            .onDirectionEnded(ConnectionLifecycleManager.getInstance().findByChannel(localChannel));
                    break;
                }
            }
        } catch (Exception e) {
            log.debug("readFromAdb error: {}", e.getMessage());
        }
    }

    public void stop() {
        if (!running) return;
        running = false;

        if (serverChannel != null) {
            serverChannel.close();
        }
        if (bossGroup != null) {
            bossGroup.shutdownGracefully();
        }
        if (workerGroup != null) {
            workerGroup.shutdownGracefully();
        }
        log.info("AdbForwarderNetty stopped");
    }

    @Override
    public void close() {
        stop();
    }
}
```

---

## Task 6: 修改 AccessApplication 集成 AdbForwarderNetty

**Files:**
- Modify: `molink-access/src/main/java/com/molink/access/AccessApplication.java`
- Modify: `molink-access/src/main/java/com/molink/access/forwarder/PortForwarder.java`（或保留兼容）

- [ ] **Step 1: 读取 AccessApplication.java**

Run: `Read molink-access/src/main/java/com/molink/access/AccessApplication.java`

- [ ] **Step 2: 添加 AdbForwarderNetty Bean**

在 AccessApplication 类中添加：

```java
@Bean
public AdbForwarderNetty adbForwarderNetty(AdbClientManager adbClient, MolinkProperties properties) {
    AdbForwarderNetty forwarder = new AdbForwarderNetty(adbClient,
            properties.getLocalPort(), properties.getRemotePort());
    forwarder.start();
    return forwarder;
}
```

- [ ] **Step 3: 原有 AdbForwarder 保留或删除**

保留作为兼容层，或删除（取决于是否还有其他地方引用）。

---

## Task 7: 快速中断功能验证

**Files:**
- 无新增文件

- [ ] **Step 1: 确认 Ctrl-C 场景下的中断链路**

验证链路：
1. Windows 端收到 Ctrl-C → JVM 关闭信号
2. Netty bossGroup/workerGroup 收到关闭事件 → serverChannel.close()
3. 本地 NioSocketChannel.close() → FIN 写入本地 TCP 连接
4. FIN 传递到 AdbStreamHandler.channelInactive()
5. → ConnectionLifecycleManager.destroy()
6. → safeCloseAdbStream(adbStream) → dadb AdbStream.close()
7. → ADB 协议关闭包发送到 USB
8. → Android 端 dadb 检测到 stream 关闭 → Socks5ProxyService 检测到 channelInactive
9. → 双向关闭触发

- [ ] **Step 2: 使用 curl 测试快速中断场景**

```bash
curl --max-time 30 http://example.com
# 在传输过程中 Ctrl-C 中断
# 验证：进程立即退出，Android 端连接立即关闭（无残留）
```

---

## Task 8: 构建验证

**Files:**
- 无修改

- [ ] **Step 1: 执行 Maven 构建**

Run: `cd D:\MyProjects\MoLink\molink-access && mvn clean compile -q`
Expected: 编译成功，无错误

- [ ] **Step 2: 检查是否有警告**

关注 Netty 和 dadb 版本兼容性警告。

---

## 验证清单

- [ ] AdbForwarderNetty 能启动并绑定本地端口
- [ ] curl 能通过 SOCKS5 代理访问互联网
- [ ] Ctrl-C 中断时，Android 端连接立即关闭
- [ ] 30 秒空闲超时能正常触发关闭
- [ ] 原有 REST API (/api/status, /api/config) 仍正常

---

## 依赖版本

| 库 | 版本 |
|----|------|
| Netty | 4.1.100.Final |
| Spring Boot | 2.7.18 |
| dadb | 1.2.10 |
| Java | 8+ |

---

## 与 worker 的对称设计

| 组件 | molink-access | molink-worker |
|------|---------------|---------------|
| 连接管理 | `ConnectionLifecycleManager` | `ConnectionLifecycleManager` |
| 会话上下文 | `SessionContext(localChannel, adbStream)` | `SessionContext(clientChannel, targetChannel)` |
| 数据转发 | `AdbStreamHandler` + 后台线程读 | `ForwardHandler` (InboundHandler) |
| 空闲检测 | `IdleStateHandler` (30s) | `IdleStateHandler` (30s) |
| 快速中断 | `channelInactive()` → `destroy()` | `channelInactive()` → `destroy()` |
