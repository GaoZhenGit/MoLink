# HTTP Proxy Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add HTTP CONNECT proxy support to the MoLink worker Android app, sharing the same port as SOCKS5, with a UI toggle switch to choose protocol.

**Architecture:** Single Netty ServerSocket with dynamic pipeline — `initChannel` branches based on current `ProxyProtocol` to build either SOCKS5 or HTTP handler chain. Service accepts protocol type via Intent Extra for testability.

**Tech Stack:** Android (Java 8), Netty 4.x, Gradle 6.9.4, ADB for testing

---

## File Map

| Responsibility | Action | File |
|---|---|---|
| Protocol enum | Create | `molink-worker/app/src/main/java/com/molink/worker/ProxyProtocol.java` |
| HTTP CONNECT state machine | Create | `molink-worker/app/src/main/java/com/molink/worker/netty/HttpProxyStateHandler.java` |
| Service: accept proxyType, dynamic pipeline | Modify | `molink-worker/app/src/main/java/com/molink/worker/Socks5ProxyService.java` |
| UI: add Toggle Switch | Modify | `molink-worker/app/src/main/res/layout/activity_main.xml` |
| Activity: toggle logic, state sync | Modify | `molink-worker/app/src/main/java/com/molink/worker/MainActivity.java` |
| E2E tests: add HTTP proxy tests | Modify | `test/test.py` |

All existing files (`ConnectionRecord`, `SessionContext`, `ConnectionLifecycleManager`, `ForwardHandler`, `Socks5StateHandler`) are reused without modification.

---

### Task 1: Create ProxyProtocol Enum

**Files:**
- Create: `molink-worker/app/src/main/java/com/molink/worker/ProxyProtocol.java`

- [ ] **Step 1.1: Write ProxyProtocol enum**

```java
package com.molink.worker;

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

- [ ] **Step 1.2: Commit**

```bash
git add molink-worker/app/src/main/java/com/molink/worker/ProxyProtocol.java
git commit -m "feat: add ProxyProtocol enum for HTTP/SOCKS5 selection"
```

---

### Task 2: Create HttpProxyStateHandler

**Files:**
- Create: `molink-worker/app/src/main/java/com/molink/worker/netty/HttpProxyStateHandler.java`

- [ ] **Step 2.1: Write HttpProxyStateHandler**

This handler implements HTTP CONNECT proxy protocol (RFC 7231). It uses Netty's `HttpRequestDecoder` to parse requests, then handles CONNECT + auth in one handler for simplicity.

Key design decisions:
- Uses `io.netty.handler.codec.http.HttpRequestDecoder` in pipeline before this handler
- Reads `FullHttpRequest` objects from `channelRead`
- Auth via `Proxy-Authorization: Basic <base64>` header, same credentials as SOCKS5 (BuildConfig)
- DNS resolution mirrors `Socks5StateHandler` behavior (local DNS, IPv4 only)
- After tunnel established, removes itself and `HttpRequestDecoder` from pipeline, adds `ForwardHandler` pair
- `SessionContext` is reused — the client channel is the HTTP channel, target channel is the upstream connection

```java
package com.molink.worker.netty;

import android.util.Log;
import com.molink.worker.BuildConfig;
import com.molink.worker.ConnectionRecord;
import com.molink.worker.Socks5ProxyService;
import io.netty.bootstrap.Bootstrap;
import io.netty.buffer.ByteBuf;
import io.netty.channel.Channel;
import io.netty.channel.ChannelFutureListener;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.socket.nio.NioSocketChannel;
import io.netty.handler.codec.http.DefaultFullHttpResponse;
import io.netty.handler.codec.http.FullHttpRequest;
import io.netty.handler.codec.http.HttpHeaderNames;
import io.netty.handler.codec.http.HttpResponseStatus;
import io.netty.handler.codec.http.HttpVersion;
import io.netty.handler.timeout.IdleStateHandler;

import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.concurrent.TimeUnit;

/**
 * HTTP CONNECT 代理协议状态机 Handler。
 * Pipeline: HttpRequestDecoder → HttpProxyStateHandler → ForwardHandler
 *
 * 仅支持 CONNECT 方法。认证成功后建立隧道，ForwardHandler 接管双向转发。
 */
public final class HttpProxyStateHandler extends ChannelInboundHandlerAdapter {

    private static final String TAG = "HttpProxyStateHandler";

    private SessionContext sessionCtx;
    private String targetHost;
    private int targetPort;
    private String destAddr;

    private enum State { WAIT_REQUEST, WAIT_AUTH, CONNECTING, FORWARDING }
    private State state = State.WAIT_REQUEST;

    @Override
    public void channelRead(ChannelHandlerContext ctx, Object msg) throws Exception {
        if (!(msg instanceof FullHttpRequest)) {
            ctx.fireChannelRead(msg);
            return;
        }
        FullHttpRequest req = (FullHttpRequest) msg;
        try {
            switch (state) {
                case WAIT_REQUEST:
                    handleConnect(ctx, req);
                    break;
                case WAIT_AUTH:
                    handleAuth(ctx, req);
                    break;
                default:
                    ctx.fireChannelRead(msg);
                    break;
            }
        } finally {
            req.release();
        }
    }

    private void handleConnect(ChannelHandlerContext ctx, FullHttpRequest req) throws Exception {
        // 仅支持 CONNECT 方法
        String method = req.method().name();
        if (!"CONNECT".equalsIgnoreCase(method)) {
            sendResponse(ctx, HttpResponseStatus.METHOD_NOT_ALLOWED,
                    "Method Not Allowed: only CONNECT is supported");
            return;
        }

        // 解析目标地址 host:port
        String uri = req.uri();
        int colonIdx = uri.lastIndexOf(':');
        if (colonIdx < 0) {
            sendResponse(ctx, HttpResponseStatus.BAD_REQUEST, "Invalid target address");
            return;
        }
        targetHost = uri.substring(0, colonIdx);
        try {
            targetPort = Integer.parseInt(uri.substring(colonIdx + 1));
        } catch (NumberFormatException e) {
            sendResponse(ctx, HttpResponseStatus.BAD_REQUEST, "Invalid port");
            return;
        }

        // 认证检查
        if (requiresAuth()) {
            String authHeader = req.headers().get(HttpHeaderNames.PROXY_AUTHORIZATION);
            if (!validateAuth(authHeader)) {
                sendResponse(ctx, HttpResponseStatus.PROXY_AUTHENTICATION_REQUIRED,
                        "Proxy Authentication Required");
                return;
            }
            // 认证成功，继续建立连接
            connectToTarget(ctx);
        } else {
            // 无需认证，直接建立连接
            connectToTarget(ctx);
        }
    }

    private boolean requiresAuth() {
        return !BuildConfig.SOCKS_USERNAME.isEmpty() && !BuildConfig.SOCKS_PASSWORD.isEmpty();
    }

    private boolean validateAuth(String authHeader) {
        if (authHeader == null || !authHeader.startsWith("Basic ")) {
            return false;
        }
        String encoded = authHeader.substring(6).trim();
        try {
            String decoded = new String(Base64.getDecoder().decode(encoded), StandardCharsets.UTF_8);
            int colonIdx = decoded.indexOf(':');
            if (colonIdx < 0) return false;
            String user = decoded.substring(0, colonIdx);
            String pass = decoded.substring(colonIdx + 1);
            return BuildConfig.SOCKS_USERNAME.equals(user) && BuildConfig.SOCKS_PASSWORD.equals(pass);
        } catch (IllegalArgumentException e) {
            return false;
        }
    }

    private void connectToTarget(ChannelHandlerContext ctx) {
        // 创建 ConnectionRecord
        String clientIp = ctx.channel().remoteAddress().toString();
        if (clientIp.startsWith("/")) clientIp = clientIp.substring(1);
        ConnectionRecord record = new ConnectionRecord(clientIp, targetHost, targetPort, System.currentTimeMillis());
        Socks5ProxyService.registerConnection(record);

        // 本地 DNS 解析
        try {
            InetAddress[] addrs = InetAddress.getAllByName(targetHost);
            String ipv4Addr = null;
            for (InetAddress addr : addrs) {
                byte[] raw = addr.getAddress();
                if (raw.length == 4) {
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
            sendResponse(ctx, HttpResponseStatus.BAD_GATEWAY, "DNS resolution failed");
            return;
        }

        Log.i(TAG, "CONNECT: " + targetHost + ":" + targetPort + " (resolved: " + destAddr + ")");

        sessionCtx = new SessionContext(ctx.channel(), record);
        ConnectionLifecycleManager.getInstance().register(ctx.channel(), sessionCtx);

        Bootstrap bs = new Bootstrap();
        bs.group(ctx.channel().eventLoop())
          .channel(NioSocketChannel.class)
          .handler(new ChannelInitializer<Channel>() {
              @Override
              protected void initChannel(Channel ch) throws Exception {
                  ch.pipeline().addLast(new IdleStateHandler(0, 0, 30, TimeUnit.SECONDS));
                  ch.pipeline().addLast(new ForwardHandler(ctx.channel(), record, "s->c"));
              }
          });

        bs.connect(destAddr, targetPort).addListener((ChannelFutureListener) future -> {
            if (future.isSuccess()) {
                Channel targetChannel = future.channel();
                sessionCtx.targetChannel = targetChannel;
                ConnectionLifecycleManager.getInstance().register(targetChannel, sessionCtx);

                // 发送 200 Connection Established
                ByteBuf content = ctx.alloc().buffer();
                content.writeBytes("HTTP/1.1 200 Connection Established\r\n\r\n".getBytes(StandardCharsets.US_ASCII));
                ctx.writeAndFlush(content);

                // 在 clientChannel 添加 IdleStateHandler + ForwardHandler
                ctx.channel().pipeline().addLast(new IdleStateHandler(0, 0, 30, TimeUnit.SECONDS));
                ctx.channel().pipeline().addLast(new ForwardHandler(targetChannel, record, "c->s"));

                // 移除自身和 HttpRequestDecoder，让数据直接到 ForwardHandler
                ctx.channel().pipeline().remove(HttpProxyStateHandler.this);
                // 尝试移除 HttpRequestDecoder（如果存在）
                try {
                    ctx.channel().pipeline().remove("httpDecoder");
                } catch (Exception ignored) {}
            } else {
                Log.e(TAG, "Failed to connect to " + destAddr + ":" + targetPort, future.cause());
                sendResponse(ctx, HttpResponseStatus.BAD_GATEWAY, "Failed to connect to target");
                ctx.close();
            }
        });
    }

    private void handleAuth(ChannelHandlerContext ctx, FullHttpRequest req) {
        // 此状态理论上不会到达，因为认证在 handleConnect 中同步完成
        sendResponse(ctx, HttpResponseStatus.PROXY_AUTHENTICATION_REQUIRED,
                "Proxy Authentication Required");
    }

    private void sendResponse(ChannelHandlerContext ctx, HttpResponseStatus status, String body) {
        DefaultFullHttpResponse resp = new DefaultFullHttpResponse(
                HttpVersion.HTTP_1_1, status,
                ctx.alloc().buffer().writeBytes(body.getBytes(StandardCharsets.UTF_8)));
        resp.headers().set(HttpHeaderNames.CONTENT_LENGTH, body.length());
        resp.headers().set(HttpHeaderNames.CONNECTION, "close");
        ctx.writeAndFlush(resp).addListener(ChannelFutureListener.CLOSE);
    }

    @Override
    public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) throws Exception {
        Log.e(TAG, "HttpProxyStateHandler exception: " + cause.getMessage());
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

- [ ] **Step 2.2: Commit**

```bash
git add molink-worker/app/src/main/java/com/molink/worker/netty/HttpProxyStateHandler.java
git commit -m "feat: add HttpProxyStateHandler for HTTP CONNECT proxy protocol"
```

---

### Task 3: Modify Socks5ProxyService — Accept ProxyType, Dynamic Pipeline

**Files:**
- Modify: `molink-worker/app/src/main/java/com/molink/worker/Socks5ProxyService.java`

- [ ] **Step 3.1: Add imports and fields**

Add at top of class (after existing imports, before the class body):

```java
import io.netty.handler.codec.http.HttpObjectAggregator;
import io.netty.handler.codec.http.HttpRequestDecoder;
```

Add field after `private volatile long startTime = 0;`:

```java
private ProxyProtocol proxyType = ProxyProtocol.SOCKS5;
```

- [ ] **Step 3.2: Modify onStartCommand to read proxy_type extra**

Replace the existing `onStartCommand` method:

```java
@Override
public int onStartCommand(Intent intent, int flags, int startId) {
    // 读取 Intent 中的协议类型
    if (intent != null) {
        String typeStr = intent.getStringExtra("proxy_type");
        if (typeStr != null) {
            proxyType = ProxyProtocol.fromString(typeStr);
        }
    }
    Log.d(TAG, "onStartCommand, starting foreground, protocol=" + proxyType.getDisplayName());
    startForeground(NOTIFICATION_ID, createNotification());
    startServer();
    // 启动 HTTP 状态服务
    new Handler(Looper.getMainLooper()).post(() -> {
        try {
            httpServer = new StatusHttpServer(STATUS_HTTP_PORT, this);
            httpServer.start();
            Log.i(TAG, "HTTP status server started on port " + STATUS_HTTP_PORT);
        } catch (IOException e) {
            Log.e(TAG, "Failed to start HTTP server: " + e.getMessage());
        }
    });
    return START_STICKY;
}
```

- [ ] **Step 3.3: Rename startSocks5Server to startServer with dynamic pipeline**

Replace the entire `startSocks5Server()` method:

```java
private void startServer() {
    if (isRunning) {
        Log.w(TAG, "Server already running");
        return;
    }
    isRunning = true;
    startTime = SystemClock.elapsedRealtime() / 1000;

    eventLoopGroup = new NioEventLoopGroup();

    ServerBootstrap bs = new ServerBootstrap();
    bs.group(eventLoopGroup)
      .channel(NioServerSocketChannel.class)
      .childHandler(new ChannelInitializer<Channel>() {
          @Override
          protected void initChannel(Channel ch) throws Exception {
              if (proxyType == ProxyProtocol.HTTP) {
                  ch.pipeline().addLast("httpDecoder", new HttpRequestDecoder());
                  ch.pipeline().addLast("httpAggregator", new HttpObjectAggregator(8192));
                  ch.pipeline().addLast(new HttpProxyStateHandler());
              } else {
                  ch.pipeline().addLast(new Socks5StateHandler());
              }
          }
      });

    try {
        bs.bind("127.0.0.1", SOCKS5_PORT).syncUninterruptibly();
        Log.i(TAG, "ServerSocket listening on 127.0.0.1:" + SOCKS5_PORT + " (" + proxyType.getDisplayName() + ")");
    } catch (Exception e) {
        Log.e(TAG, "Failed to bind server socket", e);
        isRunning = false;
    }

    Log.i(TAG, proxyType.getDisplayName() + " proxy server started on port " + SOCKS5_PORT);
}
```

- [ ] **Step 3.4: Update createNotification to show protocol type**

Replace the `createNotification()` method:

```java
private Notification createNotification() {
    String protocol = proxyType.getDisplayName();
    return new NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("MoLink Worker")
            .setContentText(protocol + " 代理服务运行中，端口 " + SOCKS5_PORT)
            .setSmallIcon(R.drawable.ic_notification)
            .build();
}
```

- [ ] **Step 3.5: Add getProxyType() getter**

Add after `getPort()` method:

```java
public ProxyProtocol getProxyType() {
    return proxyType;
}
```

- [ ] **Step 3.6: Commit**

```bash
git add molink-worker/app/src/main/java/com/molink/worker/Socks5ProxyService.java
git commit -m "feat: support dynamic protocol selection in proxy service"
```

---

### Task 4: Add Toggle Switch to UI

**Files:**
- Modify: `molink-worker/app/src/main/res/layout/activity_main.xml`
- Modify: `molink-worker/app/src/main/java/com/molink/worker/MainActivity.java`

- [ ] **Step 4.1: Add SwitchCompat to layout**

In `activity_main.xml`, insert between the status bar section (the first LinearLayout ending with statusUptime) and the stats section (the LinearLayout with background #F5F5F5). Add this block:

```xml
    <!-- ===== 2b. Proxy Protocol Switch ===== -->
    <LinearLayout
        android:layout_width="match_parent"
        android:layout_height="wrap_content"
        android:orientation="horizontal"
        android:gravity="center_vertical"
        android:background="#F0F4F8"
        android:padding="8dp"
        android:layout_marginBottom="12dp"
        android:layout_marginTop="0dp">

        <TextView
            android:layout_width="0dp"
            android:layout_height="wrap_content"
            android:layout_weight="1"
            android:text="代理协议"
            android:textSize="14sp"
            android:textColor="#666666"/>

        <TextView
            android:id="@+id/protocolLabel"
            android:layout_width="wrap_content"
            android:layout_height="wrap_content"
            android:text="SOCKS5"
            android:textSize="13sp"
            android:textColor="#666666"
            android:layout_marginEnd="8dp"/>

        <androidx.appcompat.widget.SwitchCompat
            android:id="@+id/proxyProtocolSwitch"
            android:layout_width="wrap_content"
            android:layout_height="wrap_content"
            android:textOff="SOCKS"
            android:textOn="HTTP"
            android:showText="true"/>
    </LinearLayout>
```

Note: Adjust the stats section comment number from "2" to "3", and connection log title from "3" to "4", and RecyclerView from "4" to "5" if you want clean numbering. (Optional, cosmetic.)

- [ ] **Step 4.2: Update MainActivity — Add fields and initialization**

Add field after existing fields:

```java
private SwitchCompat protocolSwitch;
private TextView protocolLabel;
```

Add import:

```java
import androidx.appcompat.widget.SwitchCompat;
```

In `onCreate()`, after existing `findViewById` calls:

```java
protocolSwitch = findViewById(R.id.proxyProtocolSwitch);
protocolLabel = findViewById(R.id.protocolLabel);
```

- [ ] **Step 4.3: Update onToggleClick to pass proxy_type**

Replace `onToggleClick`:

```java
public void onToggleClick(View v) {
    Socks5ProxyService svc = Socks5ProxyService.getInstance();
    if (svc != null && svc.isRunning()) {
        stopService(new Intent(this, Socks5ProxyService.class));
        showStopped();
    } else {
        // 使用当前 Switch 状态决定协议类型
        Intent intent = new Intent(this, Socks5ProxyService.class);
        intent.putExtra("proxy_type", getCurrentProxyTypeString());
        startService(intent);
        showRunning(null);
    }
    uiHandler.removeCallbacks(uiPoller);
    uiHandler.postDelayed(uiPoller, 1500);
}
```

Add helper method:

```java
private String getCurrentProxyTypeString() {
    if (protocolSwitch != null && protocolSwitch.isChecked()) {
        return "http";
    }
    return "socks5";
}
```

- [ ] **Step 4.4: Update showStopped and showRunning for protocol-aware UI**

Replace `showStopped()`:

```java
private void showStopped() {
    toggleButton.setText("启动服务");
    String protocol = getCurrentProxyTypeString();
    statusRunning.setText(("http".equals(protocol) ? "HTTP" : "SOCKS5") + " 服务已停止");
    statusDot.setBackgroundResource(R.drawable.circle_gray);
    statusPort.setVisibility(View.GONE);
    statusUptime.setVisibility(View.GONE);
    connCount.setText("--");
    historyCount.setText("--");
    bytesDown.setText("--");
    bytesUp.setText("--");
    logAdapter.refreshAll(java.util.Collections.emptyList());
    protocolSwitch.setEnabled(false);
}
```

Replace `showRunning(Socks5ProxyService svc)`:

```java
private void showRunning(Socks5ProxyService svc) {
    toggleButton.setText("停止服务");
    ProxyProtocol proto = svc != null ? svc.getProxyType() :
            ("http".equals(getCurrentProxyTypeString()) ? ProxyProtocol.HTTP : ProxyProtocol.SOCKS5);
    statusRunning.setText(proto.getDisplayName() + " 运行中");
    statusDot.setBackgroundResource(R.drawable.circle_green);
    statusPort.setVisibility(View.VISIBLE);
    statusUptime.setVisibility(View.VISIBLE);
    if (svc != null) {
        statusPort.setText("端口:" + svc.getPort());
        statusUptime.setText("在线:" + formatUptime(svc.getUptime()));
        connCount.setText(String.valueOf(svc.getConnectionCount()));
        // 同步 Switch 状态
        protocolSwitch.setChecked(svc.getProxyType() == ProxyProtocol.HTTP);
        protocolLabel.setText(svc.getProxyType().getDisplayName());
    }
    protocolSwitch.setEnabled(true);
}
```

Add import:

```java
import com.molink.worker.ProxyProtocol;
```

- [ ] **Step 4.5: Add Switch onCheckedChangeListener for live protocol switching**

In `onCreate()`, after the findViewById calls for protocolSwitch:

```java
protocolSwitch.setOnCheckedChangeListener((buttonView, isChecked) -> {
    String newType = isChecked ? "http" : "socks5";
    Socks5ProxyService svc = Socks5ProxyService.getInstance();
    if (svc != null && svc.isRunning()) {
        // 服务正在运行，切换协议：停止 → 重启
        stopService(new Intent(this, Socks5ProxyService.class));
        Intent newIntent = new Intent(this, Socks5ProxyService.class);
        newIntent.putExtra("proxy_type", newType);
        uiHandler.postDelayed(() -> {
            startService(newIntent);
            uiHandler.removeCallbacks(uiPoller);
            uiHandler.postDelayed(uiPoller, 1500);
        }, 500);
    } else {
        // 服务未运行，仅更新标签
        protocolLabel.setText(isChecked ? "HTTP" : "SOCKS5");
    }
});
```

- [ ] **Step 4.6: Commit**

```bash
git add molink-worker/app/src/main/res/layout/activity_main.xml molink-worker/app/src/main/java/com/molink/worker/MainActivity.java
git commit -m "feat: add protocol toggle switch to main UI"
```

---

### Task 5: Add HTTP Proxy Tests to E2E Test Script

**Files:**
- Modify: `test/test.py`

- [ ] **Step 5.1: Add HTTP proxy test helper function**

After the existing `test_socks_proxy` function, add:

```python
def test_http_proxy(curl: str, urls: List[str],
                   username: Optional[str] = None,
                   password: Optional[str] = None,
                   port: Optional[int] = None) -> tuple:
    """Test HTTP via HTTP CONNECT proxy. Returns (passed, url, elapsed)."""
    target_port = port if port is not None else ADB_PORT
    if username and password:
        proxy_url = f"http://{username}:{password}@127.0.0.1:{target_port}"
    else:
        proxy_url = f"http://127.0.0.1:{target_port}"
    for url in urls:
        start = time.time()
        cmd = [curl, "-i", "-x", proxy_url, url, "--max-time", str(PROXY_TIMEOUT)]
        print(f"  $ {' '.join(cmd)}")
        r = run_cmd(cmd, timeout=PROXY_TIMEOUT + 10, print_output=True)
        elapsed = time.time() - start
        if r.returncode == 0 or "HTTP/" in r.stdout or '"origin"' in r.stdout:
            return (True, url, elapsed)
        fail(f"HTTP proxy FAIL: {url}")
    return (False, urls[-1], 0)
```

- [ ] **Step 5.2: Add helper for starting worker with HTTP protocol**

Add after `start_worker_service_with_retry`:

```python
def start_worker_service_http(device: str) -> bool:
    """Start worker service with HTTP proxy type."""
    for attempt in range(1, MAX_SERVICE_RETRIES + 1):
        info(f"Starting worker service with HTTP protocol (attempt {attempt}/{MAX_SERVICE_RETRIES})...")
        r = run_cmd(
            [
                "adb", "-s", device, "shell", "am", "startservice",
                "-n", "com.molink.worker/.Socks5ProxyService",
                "--es", "proxy_type", "http"
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

- [ ] **Step 5.3: Add HTTP proxy test phase**

After the SOCKS proxy tests (Step 15) and before the Degradation Test (Step 16), insert a new phase. This requires:
1. Restarting worker with HTTP protocol
2. Testing HTTP proxy with correct credentials
3. Testing HTTP proxy with wrong credentials
4. Testing HTTP proxy without credentials

In `main()`, after Step 15's `time.sleep(3)`, add:

```python
    # ================================================================
    # Phase 3b: HTTP Proxy Tests
    # ================================================================

    # --- Step 15b-1: Restart worker with HTTP protocol ---
    step = "15b-1"
    t = time.time()
    step_start(step, "Restart worker with HTTP protocol")
    run_cmd(
        ["adb", "-s", device, "shell", "am", "force-stop", "com.molink.worker"],
        timeout=STOP_TIMEOUT, print_output=True,
    )
    time.sleep(2)
    if not start_worker_service_http(device):
        step_fail(step, "Worker service HTTP start failed")
        results[step] = ("FAIL", "HTTP start failed")
        exit_code = 1
    else:
        time.sleep(SERVICE_WAIT)
        elapsed = int(time.time() - t)
        step_ok(step, "Worker running in HTTP mode", elapsed)
        results[step] = ("PASS", "HTTP mode started")
    print("")

    # --- Step 15b-2: HTTP wrong credentials ---
    step = "15b-2"
    t = time.time()
    step_start(step, "HTTP wrong credentials")
    wrong_passed, _, _ = test_http_proxy(
        curl, TEST_URLS, username="admin", password="wrongpassword")
    elapsed = int(time.time() - t)
    if not wrong_passed:
        step_ok(step, "Wrong credentials rejected", elapsed)
        results[step] = ("PASS", "Wrong credentials rejected")
    else:
        step_fail(step, "Wrong credentials accepted (should be rejected)")
        results[step] = ("FAIL", "Wrong credentials accepted")
        exit_code = 1
    print("")

    # --- Step 15b-3: HTTP no credentials ---
    step = "15b-3"
    t = time.time()
    step_start(step, "HTTP no credentials")
    noauth_passed, _, _ = test_http_proxy(curl, TEST_URLS)
    elapsed = int(time.time() - t)
    if not noauth_passed:
        step_ok(step, "No credentials rejected", elapsed)
        results[step] = ("PASS", "No credentials rejected")
    else:
        step_fail(step, "No credentials accepted (should be rejected)")
        results[step] = ("FAIL", "No credentials accepted")
        exit_code = 1
    print("")

    # --- Step 15b-4: HTTP correct credentials ---
    step = "15b-4"
    t = time.time()
    step_start(step, "HTTP correct credentials")
    try:
        import json
        alive = [d for d in devices if d.get("forwarderAlive")]
        proxy_port = alive[0].get("localPort", 1080) if alive else 1080
    except Exception:
        proxy_port = 1080
    http_proxy_passed, http_proxy_url, http_proxy_elapsed = test_http_proxy(
        curl, TEST_URLS, username=SOCKS_AUTH_USER, password=SOCKS_AUTH_PASS,
        port=proxy_port)
    elapsed = int(time.time() - t)
    if http_proxy_passed:
        step_ok(step, f"OK via {http_proxy_url} ({http_proxy_elapsed}s)", elapsed)
        results[step] = ("PASS", f"HTTP Proxy OK via {http_proxy_url}")
    else:
        step_fail(step, f"Proxy unreachable with correct credentials")
        results[step] = ("FAIL", "HTTP Proxy unreachable")
        exit_code = 1
    print("")
    time.sleep(3)

    # --- Step 15b-5: Switch back to SOCKS5 and verify ---
    step = "15b-5"
    t = time.time()
    step_start(step, "Switch back to SOCKS5 and verify")
    run_cmd(
        ["adb", "-s", device, "shell", "am", "force-stop", "com.molink.worker"],
        timeout=STOP_TIMEOUT, print_output=True,
    )
    time.sleep(2)
    if not start_worker_service_with_retry(device):
        step_fail(step, "Worker service SOCKS5 restart failed")
        results[step] = ("FAIL", "SOCKS5 restart failed")
        exit_code = 1
    else:
        time.sleep(SERVICE_WAIT)
        elapsed = int(time.time() - t)
        # 验证 SOCKS5 代理仍然正常工作
        socks_reverify, _, _ = test_socks_proxy(
            curl, TEST_URLS, username=SOCKS_AUTH_USER, password=SOCKS_AUTH_PASS,
            port=proxy_port)
        if socks_reverify:
            step_ok(step, "SOCKS5 re-verified after protocol switch", elapsed)
            results[step] = ("PASS", "SOCKS5 re-verified")
        else:
            step_fail(step, "SOCKS5 failed after protocol switch")
            results[step] = ("FAIL", "SOCKS5 re-verify failed")
            exit_code = 1
    print("")
```

- [ ] **Step 5.4: Commit**

```bash
git add test/test.py
git commit -m "test: add HTTP proxy E2E test cases"
```

---

### Task 6: Final Review and Build Verification

**Files:**
- All modified files

- [ ] **Step 6.1: Verify the build compiles**

```bash
cd molink-worker && gradlew.bat assembleDebug --no-daemon
```

Expected: BUILD SUCCESSFUL

- [ ] **Step 6.2: Run the full E2E test suite**

```bash
python test/test.py
```

Expected: All steps pass (including new HTTP proxy tests)

- [ ] **Step 6.3: Final commit if any fixes needed**

```bash
git add -A
git commit -m "fix: address review findings for HTTP proxy support"
```
