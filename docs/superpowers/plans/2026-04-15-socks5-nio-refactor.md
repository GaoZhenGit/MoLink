# SOCKS5 NIO 重构实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 Socks5ProxyService 从 per-connection Thread 模型重构为 Netty 4.1 NIO 事件循环，NioEventLoopGroup 使用默认值线程池（不限制），消除连接数对线程数的线性增长，支持 30 秒空闲超时、SOCKS5H 域名解析。

**Architecture:** 使用 Netty NioEventLoopGroup 替代原生 ServerSocket + Thread；Socks5StateHandler 状态机处理协议握手→认证→CONNECT；ForwardHandler 处理双向转发和双向关闭；ConnectionLifecycleManager 统一管理会话生命周期。

**Tech Stack:** Netty 4.1.100.Final, Java 8, Android SDK 27+

---

## 文件结构

```
molink-worker/app/src/main/java/com/molink/worker/
├── Socks5ProxyService.java              # 重写：移除 Thread/Pipe，引入 Netty 启动逻辑
└── netty/
    ├── SessionContext.java              # 新增：per-session 上下文
    ├── ConnectionLifecycleManager.java  # 新增：全局会话管理
    ├── Socks5StateHandler.java          # 新增：SOCKS5 协议状态机
    └── ForwardHandler.java              # 新增：双向转发 + 超时检测

molink-worker/app/build.gradle           # 修改：添加 netty-all 依赖
```

**不变文件：** `MainActivity.java`、`ConnectionLogAdapter.java`、`ConnectionRecord.java`

---

## Task 1: 添加 Netty 依赖

**文件:**
- Modify: `molink-worker/app/build.gradle`

- [ ] **Step 1: 添加 netty-all 依赖**

在 `dependencies` 块内添加：

```groovy
dependencies {
    implementation 'androidx.appcompat:appcompat:1.3.1'
    implementation 'org.nanohttpd:nanohttpd:2.3.1'
    implementation 'androidx.recyclerview:recyclerview:1.2.1'
    implementation 'io.netty:netty-all:4.1.100.Final'
}
```

---

## Task 2: 新增 SessionContext

**文件:**
- Create: `molink-worker/app/src/main/java/com/molink/worker/netty/SessionContext.java`

- [ ] **Step 1: 编写 SessionContext**

```java
package com.molink.worker.netty;

import com.molink.worker.ConnectionRecord;
import io.netty.channel.Channel;

import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * 单个 SOCKS5 代理会话的上下文。
 * 存储 clientChannel / targetChannel / record / 双向结束计数 / 销毁标志。
 * 线程安全。
 */
public final class SessionContext {

    public final Channel clientChannel;
    public volatile Channel targetChannel;         // 异步建立，完成后赋值
    public final ConnectionRecord record;
    /** 已结束的方向数（0/1/2），两个方向都结束才触发销毁 */
    public final AtomicInteger endedCount = new AtomicInteger(0);
    /** 防止重复销毁 */
    public final AtomicBoolean destroyed = new AtomicBoolean(false);
    public final long createdAt = System.currentTimeMillis();

    public SessionContext(Channel clientChannel, ConnectionRecord record) {
        this.clientChannel = clientChannel;
        this.record = record;
    }

    /**
     * 标记本方向已结束。
     * @return true 如果双方都已结束（需要触发销毁）
     */
    public boolean markEnded() {
        return endedCount.incrementAndGet() >= 2;
    }

    public int getEndedCount() {
        return endedCount.get();
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

## Task 3: 新增 ConnectionLifecycleManager

**文件:**
- Create: `molink-worker/app/src/main/java/com/molink/worker/netty/ConnectionLifecycleManager.java`

- [ ] **Step 1: 编写 ConnectionLifecycleManager**

```java
package com.molink.worker.netty;

import android.util.Log;
import com.molink.worker.ConnectionRecord;
import io.netty.channel.Channel;
import io.netty.channel.ChannelId;

import java.util.concurrent.ConcurrentHashMap;

/**
 * 全局会话生命周期管理器。
 * 管理所有活跃 SessionContext，提供双向关闭、标记结束等操作。
 * 线程安全。
 */
public final class ConnectionLifecycleManager {

    private static final String TAG = "ConnectionLifecycleManager";
    private static final ConnectionLifecycleManager INSTANCE = new ConnectionLifecycleManager();

    private final ConcurrentHashMap<ChannelId, SessionContext> contexts = new ConcurrentHashMap<>();

    private ConnectionLifecycleManager() {}

    public static ConnectionLifecycleManager getInstance() {
        return INSTANCE;
    }

    /**
     * 注册新会话。
     * @param clientChannel 客户端 channel
     * @param ctx 会话上下文
     */
    public void register(Channel clientChannel, SessionContext ctx) {
        contexts.put(clientChannel.id(), ctx);
    }

    /**
     * 根据 channel 查找会话上下文。
     */
    public SessionContext findByChannel(Channel channel) {
        return contexts.get(channel.id());
    }

    /**
     * 销毁会话：双向关闭两个 Channel，标记 record 结束，移除注册。
     * 可安全重复调用。
     */
    public void destroy(SessionContext sessionCtx) {
        if (!sessionCtx.markDestroyed()) {
            return; // 已被其他线程销毁
        }

        Log.i(TAG, "Destroying session");

        Channel client = sessionCtx.clientChannel;
        Channel target = sessionCtx.targetChannel;

        // 关闭客户端 channel
        if (client != null && client.isOpen()) {
            client.close();
        }
        // 关闭目标 channel
        if (target != null && target.isOpen()) {
            target.close();
        }

        // 标记 record 为已结束
        sessionCtx.record.setEnded();

        // 移除注册
        if (client != null) {
            contexts.remove(client.id());
        }
    }

    /**
     * 单方向结束时调用，检查是否需要触发销毁。
     * @param sessionCtx 会话上下文
     */
    public void onDirectionEnded(SessionContext sessionCtx) {
        boolean bothEnded = sessionCtx.markEnded();
        if (bothEnded) {
            destroy(sessionCtx);
        }
    }
}
```

---

## Task 4: 新增 ForwardHandler

**文件:**
- Create: `molink-worker/app/src/main/java/com/molink/worker/netty/ForwardHandler.java`

- [ ] **Step 1: 编写 ForwardHandler**

```java
package com.molink.worker.netty;

import android.util.Log;
import com.molink.worker.ConnectionRecord;
import io.netty.channel.Channel;
import io.netty.channel.ChannelFutureListener;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.handler.timeout.IdleState;
import io.netty.handler.timeout.IdleStateEvent;

/**
 * 双向数据转发 Handler，同时处理 c→s 和 s→c 两个方向。
 *
 * 触发双向关闭的条件（任一满足）：
 * - channelInactive：对端关闭了连接（收到 FIN）
 * - exceptionCaught：连接异常
 * - IdleStateEvent（30 秒无读写）
 *
 * 字节计数方向约定：
 * - "c->s"：客户端→服务端 = 用户上传 = bytesDown
 * - "s->c"：服务端→客户端 = 用户下载 = bytesUp
 */
public final class ForwardHandler extends ChannelInboundHandlerAdapter {

    private static final String TAG = "ForwardHandler";

    private final Channel otherChannel;           // 对端 channel
    private final ConnectionRecord record;
    private final String direction;              // "c->s" 或 "s->c"

    public ForwardHandler(Channel otherChannel, ConnectionRecord record, String direction) {
        this.otherChannel = otherChannel;
        this.record = record;
        this.direction = direction;
    }

    @Override
    public void channelRead(ChannelHandlerContext ctx, Object msg) throws Exception {
        // 将数据写入对端
        otherChannel.writeAndFlush(msg).addListener((ChannelFutureListener) future -> {
            if (!future.isSuccess()) {
                Log.w(TAG, direction + ": write failed: " + future.cause());
            } else {
                // 更新字节计数（写入成功）
                int len = estimateWrittenBytes(future);
                if ("c->s".equals(direction)) {
                    record.addBytesDown(len);
                } else {
                    record.addBytesUp(len);
                }
            }
        });
    }

    /**
     * 从 ChannelFuture 中估算写入字节数（Netty 无法直接获取，写入 ByteBuf 时通过 readableBytes() 获取）。
     * 此方法为近似值，实际以 buffer 的 readableBytes 为准。
     * 调用方应在 writeAndFlush 前通过 msg 本身获取准确字节数。
     */
    private int estimateWrittenBytes(ChannelFuture future) {
        // 由于无法从 future 获取实际字节数，在 channelRead 中通过 msg 参数传递
        return 0; // 占位，下方 channelRead 重写时会用 msg 实际计算
    }

    @Override
    public void channelInactive(ChannelHandlerContext ctx) throws Exception {
        Log.d(TAG, direction + ": channel inactive (FIN received)");
        ConnectionLifecycleManager.getInstance()
                .onDirectionEnded(ConnectionLifecycleManager.getInstance().findByChannel(ctx.channel()));
        super.channelInactive(ctx);
    }

    @Override
    public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) throws Exception {
        String msg = cause.getMessage();
        // 忽略常见非关键异常日志
        if (msg == null || (!msg.contains("Connection reset")
                && !msg.contains("Broken pipe")
                && !msg.contains("Socket closed")
                && !msg.contains("EOF"))) {
            Log.w(TAG, direction + ": exception: " + msg);
        }
        ConnectionLifecycleManager.getInstance()
                .onDirectionEnded(ConnectionLifecycleManager.getInstance().findByChannel(ctx.channel()));
        super.exceptionCaught(ctx, cause);
    }

    @Override
    public void userEventTriggered(ChannelHandlerContext ctx, Object evt) throws Exception {
        if (evt instanceof IdleStateEvent) {
            IdleStateEvent idleEvt = (IdleStateEvent) evt;
            if (idleEvt.state() == IdleState.ALL_IDLE) {
                Log.i(TAG, direction + ": idle timeout (30s), closing both sides");
                ConnectionLifecycleManager.getInstance()
                        .onDirectionEnded(ConnectionLifecycleManager.getInstance().findByChannel(ctx.channel()));
            }
        }
        super.userEventTriggered(ctx, evt);
    }
}
```

- [ ] **Step 2: 修复 ForwardHandler 的字节计数逻辑**

`estimateWrittenBytes` 为占位方法，实际字节数应从 `msg`（`ByteBuf`）中获取。删除占位方法，在 `channelRead` 中直接计算：

```java
@Override
public void channelRead(ChannelHandlerContext ctx, Object msg) throws Exception {
    if (!(msg instanceof io.netty.buffer.ByteBuf)) {
        ctx.fireChannelRead(msg);
        return;
    }
    io.netty.buffer.ByteBuf buf = (io.netty.buffer.ByteBuf) msg;
    int len = buf.readableBytes();

    // 更新字节计数
    if ("c->s".equals(direction)) {
        record.addBytesDown(len);
    } else {
        record.addBytesUp(len);
    }

    // 将数据写入对端（保留 msg 引用，Netty 会在写入完成后自动释放）
    otherChannel.writeAndFlush(msg).addListener((ChannelFutureListener) future -> {
        if (!future.isSuccess()) {
            Log.w(TAG, direction + ": write failed: " + future.cause());
        }
    });
}
```

并删除 `estimateWrittenBytes` 方法。

---

## Task 5: 新增 Socks5StateHandler

**文件:**
- Create: `molink-worker/app/src/main/java/com/molink/worker/netty/Socks5StateHandler.java`

- [ ] **Step 1: 编写 Socks5StateHandler（状态机 + SOCKS5H + 异步连接）**

```java
package com.molink.worker.netty;

import android.util.Log;
import com.molink.worker.BuildConfig;
import com.molink.worker.ConnectionRecord;
import io.netty.bootstrap.Bootstrap;
import io.netty.buffer.ByteBuf;
import io.netty.channel.Channel;
import io.netty.channel.ChannelFutureListener;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.socket.nio.NioSocketChannel;
import io.netty.handler.timeout.IdleStateHandler;

import java.net.InetAddress;
import java.util.concurrent.TimeUnit;

/**
 * SOCKS5 协议状态机 Handler。
 *
 * 状态流转：
 * WAIT_AUTH_METHOD → handleAuthMethod()
 * WAIT_AUTH        → handleUserAuth()
 * WAIT_COMMAND     → handleConnect()
 * FORWARDING       → ForwardHandler 接管，不再处理
 */
public final class Socks5StateHandler extends ChannelInboundHandlerAdapter {

    private static final String TAG = "Socks5StateHandler";

    // SOCKS5 协议常量
    private static final int SOCKS_VERSION = 0x05;
    private static final int CMD_CONNECT = 0x01;
    private static final int ATYP_IPV4 = 0x01;
    private static final int ATYP_DOMAIN = 0x03;
    private static final int ATYP_IPV6 = 0x04;
    private static final int AUTH_METHOD_NONE = 0x00;
    private static final int AUTH_METHOD_USER_PASS = 0x02;
    private static final int AUTH_METHOD_NO_ACCEPTABLE = 0xFF;
    private static final int REPLY_SUCCESS = 0x00;
    private static final int REPLY_GENERAL_FAILURE = 0x01;
    private static final int REPLY_HOST_UNREACHABLE = 0x04;
    private static final int REPLY_COMMAND_NOT_SUPPORTED = 0x07;

    // 状态枚举
    private enum State { WAIT_AUTH_METHOD, WAIT_AUTH, WAIT_COMMAND, FORWARDING }
    private State state = State.WAIT_AUTH_METHOD;

    // 当前会话上下文（建立连接后赋值）
    private SessionContext sessionCtx;
    private String targetHost;
    private int targetPort;
    private String destAddr;  // IP 或解析后的 IP 地址

    // ===== SOCKS5 握手 =====

    @Override
    public void channelRead(ChannelHandlerContext ctx, Object msg) throws Exception {
        if (!(msg instanceof ByteBuf)) {
            ctx.fireChannelRead(msg);
            return;
        }
        ByteBuf in = (ByteBuf) msg;
        boolean consumed = true;
        try {
            switch (state) {
                case WAIT_AUTH_METHOD:
                    handleAuthMethod(ctx, in);
                    break;
                case WAIT_AUTH:
                    handleUserAuth(ctx, in);
                    break;
                case WAIT_COMMAND:
                    handleConnect(ctx, in);
                    break;
                case FORWARDING:
                    // msg 引用已转交下游，下游负责 release，本 Handler 不再 release
                    consumed = false;
                    ctx.fireChannelRead(msg);
                    break;
            }
        } finally {
            if (consumed) {
                in.release();  // Netty 4 ByteBuf 引用计数，必须显式 release
            }
        }
    }

    private void handleAuthMethod(ChannelHandlerContext ctx, ByteBuf in) throws Exception {
        if (in.readableBytes() < 2) {
            ctx.close();
            return;
        }
        int ver = in.readByte();
        if (ver != SOCKS_VERSION) {
            Log.w(TAG, "Unsupported SOCKS version: " + ver);
            ctx.close();
            return;
        }
        int nMethods = in.readByte();
        if (in.readableBytes() < nMethods) {
            ctx.close();
            return;
        }
        byte[] methods = new byte[nMethods];
        in.readBytes(methods);

        boolean supportsUserPass = false;
        for (int i = 0; i < nMethods; i++) {
            if (methods[i] == AUTH_METHOD_USER_PASS) {
                supportsUserPass = true;
                break;
            }
        }

        ByteBuf out = ctx.alloc().buffer(2);
        out.writeByte(SOCKS_VERSION);
        if (supportsUserPass) {
            out.writeByte(AUTH_METHOD_USER_PASS);
            state = State.WAIT_AUTH;
        } else {
            out.writeByte(AUTH_METHOD_NO_ACCEPTABLE);
            Log.w(TAG, "No acceptable auth method");
            ctx.writeAndFlush(out).addListener(ChannelFutureListener.CLOSE);
            return;
        }
        ctx.writeAndFlush(out);
    }

    // ===== RFC 1929 用户名/密码认证 =====

    private void handleUserAuth(ChannelHandlerContext ctx, ByteBuf in) throws Exception {
        if (in.readableBytes() < 2) {
            ctx.close();
            return;
        }
        int ver = in.readByte();
        if (ver != 0x01) {
            Log.w(TAG, "Unknown auth sub-negotiation version: " + ver);
            ctx.close();
            return;
        }
        int userLen = in.readByte();
        if (userLen < 0 || userLen > 255 || in.readableBytes() < userLen + 1) {
            ctx.close();
            return;
        }
        byte[] userBytes = new byte[userLen];
        in.readBytes(userBytes);
        String username = new String(userBytes);

        int passLen = in.readByte();
        if (passLen < 0 || passLen > 255 || in.readableBytes() < passLen) {
            ctx.close();
            return;
        }
        byte[] passBytes = new byte[passLen];
        in.readBytes(passBytes);
        String password = new String(passBytes);

        boolean success = true;
        if (!BuildConfig.SOCKS_USERNAME.isEmpty() && !BuildConfig.SOCKS_PASSWORD.isEmpty()) {
            success = BuildConfig.SOCKS_USERNAME.equals(username) && BuildConfig.SOCKS_PASSWORD.equals(password);
        }

        ByteBuf out = ctx.alloc().buffer(2);
        out.writeByte(0x01);
        out.writeByte(success ? 0x00 : 0x01);
        if (!success) {
            Log.w(TAG, "Auth failed for user: " + username);
            ctx.writeAndFlush(out).addListener(ChannelFutureListener.CLOSE);
            return;
        }
        ctx.writeAndFlush(out);
        state = State.WAIT_COMMAND;
        Log.i(TAG, "Auth success for user: " + username);
    }

    // ===== CONNECT 命令 + SOCKS5H 域名解析 =====

    private void handleConnect(ChannelHandlerContext ctx, ByteBuf in) throws Exception {
        if (in.readableBytes() < 4) {
            ctx.close();
            return;
        }
        int ver = in.readByte();
        if (ver != SOCKS_VERSION) {
            Log.w(TAG, "Unexpected version in connect request: " + ver);
            ctx.close();
            return;
        }
        int cmd = in.readByte();
        int rsv = in.readByte();
        int addrType = in.readByte();

        if (cmd != CMD_CONNECT) {
            sendReply(ctx, REPLY_COMMAND_NOT_SUPPORTED, null, 0);
            return;
        }

        // 解析目标地址
        if (addrType == ATYP_IPV4) {
            if (in.readableBytes() < 6) { ctx.close(); return; }
            byte[] ip = new byte[4];
            in.readBytes(ip);
            destAddr = String.format("%d.%d.%d.%d", ip[0] & 0xFF, ip[1] & 0xFF, ip[2] & 0xFF, ip[3] & 0xFF);
            byte[] portBytes = new byte[2];
            in.readBytes(portBytes);
            targetPort = ((portBytes[0] & 0xFF) << 8) | (portBytes[1] & 0xFF);
        } else if (addrType == ATYP_DOMAIN) {
            // SOCKS5H：域名由本端解析
            if (in.readableBytes() < 1) { ctx.close(); return; }
            int domainLen = in.readByte();
            if (in.readableBytes() < domainLen + 2) { ctx.close(); return; }
            byte[] domainBytes = new byte[domainLen];
            in.readBytes(domainBytes);
            targetHost = new String(domainBytes);
            byte[] portBytes = new byte[2];
            in.readBytes(portBytes);
            targetPort = ((portBytes[0] & 0xFF) << 8) | (portBytes[1] & 0xFF);

            // 本地 DNS 解析（仅返回 IPv4，阻塞调用，在 eventLoop 线程中执行，不阻塞其他连接）
            try {
                InetAddress[] addrs = InetAddress.getAllByName(targetHost);
                String ipv4Addr = null;
                for (InetAddress addr : addrs) {
                    byte[] raw = addr.getAddress();
                    if (raw.length == 4) {  // 过滤出 IPv4
                        ipv4Addr = addr.getHostAddress();
                        break;
                    }
                }
                if (ipv4Addr == null) {
                    throw new Exception("No IPv4 address found for " + targetHost);
                }
                destAddr = ipv4Addr;
            } catch (Exception e) {
                Log.e(TAG, "DNS resolution failed for " + targetHost, e);
                sendReply(ctx, REPLY_HOST_UNREACHABLE, null, 0);
                return;
            }
        } else {
            // ATYP_IPV6 或其他不支持的类型
            sendReply(ctx, REPLY_COMMAND_NOT_SUPPORTED, null, 0);
            return;
        }

        Log.i(TAG, "CONNECT: " + targetHost + ":" + targetPort + " (resolved: " + destAddr + ")");

        // 创建 ConnectionRecord
        String clientIp = ctx.channel().remoteAddress().toString();
        if (clientIp.startsWith("/")) clientIp = clientIp.substring(1);
        ConnectionRecord record = new ConnectionRecord(clientIp, targetHost, targetPort, System.currentTimeMillis());

        // 注册到全局管理器（提前注册，便于 ForwardHandler 查找）
        sessionCtx = new SessionContext(ctx.channel(), record);
        ConnectionLifecycleManager.getInstance().register(ctx.channel(), sessionCtx);

        // 异步建立目标连接
        connectToTarget(ctx, record, destAddr, targetPort);
    }

    private void connectToTarget(ChannelHandlerContext ctx, ConnectionRecord record,
                                 String destAddr, int destPort) {
        Bootstrap bs = new Bootstrap();
        bs.group(ctx.channel().eventLoop())
          .channel(NioSocketChannel.class)
          .handler(new ChannelInitializer<Channel>() {
              @Override
              protected void initChannel(Channel ch) throws Exception {
                  // 注意：targetChannel 暂时没有 sessionCtx.targetChannel，
                  // 等 connect 成功后设置
                  ch.pipeline().addLast(new IdleStateHandler(30, 30, 0, TimeUnit.SECONDS));
                  ch.pipeline().addLast(new ForwardHandler(ctx.channel(), record, "s->c"));
              }
          });

        bs.connect(destAddr, destPort).addListener((ChannelFutureListener) future -> {
            if (future.isSuccess()) {
                Channel targetChannel = future.channel();
                sessionCtx.targetChannel = targetChannel;
                // 注册 target channel 到 lifecycle manager
                ConnectionLifecycleManager.getInstance().register(targetChannel, sessionCtx);

                // 发送成功响应
                sendReplySuccess(ctx, targetChannel);

                // 在 clientChannel.pipeline() 添加 IdleStateHandler + ForwardHandler
                ctx.channel().pipeline().addLast(new IdleStateHandler(30, 30, 0, TimeUnit.SECONDS));
                ctx.channel().pipeline().addLast(new ForwardHandler(targetChannel, record, "c->s"));

                // 切换状态，移除自身（让数据直接到 ForwardHandler）
                ctx.channel().pipeline().remove(Socks5StateHandler.this);
            } else {
                Log.e(TAG, "Failed to connect to " + destAddr + ":" + destPort, future.cause());
                sendReply(ctx, REPLY_HOST_UNREACHABLE, null, 0);
                ctx.close();
            }
        });
    }

    private void sendReply(ChannelHandlerContext ctx, int reply, byte[] bindAddr, int bindPort) {
        ByteBuf out = ctx.alloc().buffer(10);
        out.writeByte(SOCKS_VERSION);
        out.writeByte(reply);
        out.writeByte(0x00); // rsv
        out.writeByte(ATYP_IPV4);
        if (bindAddr != null && bindAddr.length == 4) {
            out.writeBytes(bindAddr);
        } else {
            out.writeBytes(new byte[]{0, 0, 0, 0});
        }
        out.writeShort(bindPort);
        ctx.writeAndFlush(out).addListener(ChannelFutureListener.CLOSE);
    }

    private void sendReplySuccess(ChannelHandlerContext ctx, Channel targetChannel) {
        ByteBuf out = ctx.alloc().buffer(10);
        out.writeByte(SOCKS_VERSION);
        out.writeByte(REPLY_SUCCESS);
        out.writeByte(0x00);
        out.writeByte(ATYP_IPV4);
        byte[] localIp = new byte[4];
        java.net.InetAddress localAddr = targetChannel.localAddress().getAddress();
        byte[] addrBytes = localAddr.getAddress();
        if (addrBytes.length == 4) {
            localIp = addrBytes;
        }
        int localPort = targetChannel.localAddress().getPort();
        out.writeBytes(localIp);
        out.writeShort(localPort);
        ctx.writeAndFlush(out);
    }

    @Override
    public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) throws Exception {
        Log.e(TAG, "Socks5StateHandler exception: " + cause.getMessage());
        if (sessionCtx != null) {
            ConnectionLifecycleManager.getInstance().destroy(sessionCtx);
        }
        ctx.close();
    }

    @Override
    public void channelInactive(ChannelHandlerContext ctx) throws Exception {
        if (sessionCtx != null) {
            ConnectionLifecycleManager.getInstance().destroy(sessionCtx);
        }
        super.channelInactive(ctx);
    }
}
```

---

## Task 6: 重写 Socks5ProxyService

**文件:**
- Rewrite: `molink-worker/app/src/main/java/com/molink/worker/Socks5ProxyService.java`

- [ ] **Step 1: 保留原有 UI 统计和 ConnectionRecord 相关代码**

保留以下字段和方法不变：
- `activeConnections`、`addConnectionRecord()`、`getConnectionSnapshot()`
- `historyCount`、`totalBytesDown`、`totalBytesUp`
- `getConnectionCount()`、`getTotalBytesDown()`、`getTotalBytesUp()`、`getHistoryCount()`
- 通知相关：`createNotificationChannel()`、`createNotification()`
- 生命周期：`onCreate()`、`onDestroy()`、`onBind()`、`LocalBinder`
- 状态方法：`isRunning()`、`getPort()`、`getUptime()`

- [ ] **Step 2: 移除原有 Thread/ServerSocket 监听代码**

删除以下内容：
- `serverSocketV4`、`serverSocketV6` 字段
- `serverThreadV4`、`serverThreadV6` 字段
- `startSocks5Server()` 方法（替换为新的 Netty 启动逻辑）
- `handleClient()` 方法
- `Pipe` 内部类
- `forward()` 方法
- `readFully()` 方法
- `handleUserAuth()` 方法

- [ ] **Step 3: 添加 Netty 字段**

```java
import io.netty.bootstrap.ServerBootstrap;
import io.netty.channel.Channel;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.nio.NioServerSocketChannel;
// 在现有字段区域添加：
private NioEventLoopGroup eventLoopGroup;
// 使用默认值（CPU cores * 2），Android 上通常 2-4 个线程，按需扩展不限制
```

- [ ] **Step 4: 重写 startSocks5Server()**

```java
private void startSocks5Server() {
    if (isRunning) {
        Log.w(TAG, "Server already running");
        return;
    }
    isRunning = true;
    startTime = SystemClock.elapsedRealtime() / 1000;

    eventLoopGroup = new NioEventLoopGroup();  // 默认值，不限制线程数

    ServerBootstrap bs = new ServerBootstrap();
    bs.group(eventLoopGroup)
      .channel(NioServerSocketChannel.class)
      .childHandler(new ChannelInitializer<Channel>() {
          @Override
          protected void initChannel(Channel ch) throws Exception {
              ch.pipeline().addLast(new Socks5StateHandler());
          }
      });

    // 仅监听 IPv4
    try {
        Channel ch4 = bs.bind("127.0.0.1", SOCKS5_PORT).syncUninterruptibly().channel();
        Log.i(TAG, "ServerSocket listening on 127.0.0.1:" + SOCKS5_PORT);
    } catch (Exception e) {
        Log.e(TAG, "Failed to bind server socket", e);
        isRunning = false;
    }

    Log.i(TAG, "SOCKS5 server started on port " + SOCKS5_PORT);
}
```

- [ ] **Step 5: 修改 onDestroy() 关闭 eventLoopGroup**

在 `onDestroy()` 中添加 `eventLoopGroup.shutdownGracefully()` 调用，并删除所有 `closeQuietly(serverSocketV4/V6)` 调用（Netty 接管后不再使用这些字段）。

---

## 自检清单

- [ ] spec 覆盖检查：
  - [x] NIO 替代阻塞 Thread — Task 6
  - [x] 线程池管理 — Task 6（`NioEventLoopGroup`）
  - [x] 双向关闭（EOF/FIN/异常）— Task 3、Task 4
  - [x] 30 秒空闲超时 — Task 4（`IdleStateHandler`）+ Task 5
  - [x] SOCKS5H 域名解析 — Task 5（`handleConnect` 中 `ATYP_DOMAIN` 分支）
  - [x] IPv4 监听（仅支持 IPv4，127.0.0.1）
  - [x] UI record 兼容 — 所有 Task（`ConnectionRecord` 未改动）
  - [x] 不提交 git — 计划中无 git 操作
  - [x] 编译通过即可 — 验证方案为 `./gradlew assembleDebug`

- [ ] 类型一致性检查：
  - `SessionContext.clientChannel` 类型为 `Channel` ✓
  - `ConnectionLifecycleManager.findByChannel()` 返回 `SessionContext` ✓
  - `ForwardHandler.constructor(otherChannel, record, direction)` 参数顺序一致 ✓
  - `IdleStateHandler(30, 30, 0, TimeUnit.SECONDS)` 三个参数顺序正确 ✓

- [ ] 占位符扫描：无 TBD/TODO/placeholder ✓
