# Worker 连接中断处理修复 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking>

**Plan 版本：** v3（整合用户反馈 v2 后的修订版）

**v3 新增需求（用户反馈）：**
- **v3 Req 1（核心）**：UI 每 0.5 秒自动全量刷新 record list，list 为 final 类型，连接逻辑只改 list
- **v3 Req 2（核心）**：两个方向的线程和 Socket 必须同步销毁，任一方向中断时，另一方向强制中断
- **v3 Req 3（确认）**：中断判定条件 = EOF 或异常或 15 秒无数据；使用 `setSoTimeout(15000)` 实现，无需额外定时器
- **v3 Req 4（确认）**：record 状态简化为"进行中/已结束"，对应绿色/灰色；去除红色状态；数据结构从 active+failed boolean 改为 state enum

**v2 遗留修复：**
- **v2 Fix 1（中等）**：forward() finally 块 + closeQuietly() + closePeer() 均使用 `shutdownInput() + shutdownOutput() + close()` 三步关闭，防止 read() 阻塞无法中断
- **v2 Fix 2（轻微）**：forward() 异常过滤列表加入 "Read timed out"，避免超时触发噪音日志
- **v2 Fix 3（确认）**：failed 逻辑迁移（已被 v3 Req 4 替代，数据结构改为 enum）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 molink-worker 的 Socks5ProxyService，当客户端主动断开（Ctrl-C）时，立即关闭与目标服务器的连接，节省流量并释放资源。

**Architecture:** 引入 `Pipe` 内部类封装双向转发的 Socket 对，实现级联关闭——任一方向检测到 EOF、异常或 15 秒无数据时，立即关闭对端 Socket 以中断另一个线程的阻塞 read()，并同步关闭本端 Socket。使用 AtomicBoolean 确保 Socket 只关闭一次避免竞态。record 使用 state enum 管理"进行中/已结束"状态，UI 每 0.5 秒全量刷新。

**Tech Stack:** Android Java (API 27+), SOCKS5 协议, Java Socket API

---

## File Structure

- **Modify:** `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\Socks5ProxyService.java`
  - 第 263-412 行：handleClient() 方法
  - 第 478-510 行：forward() 方法
  - 第 167 行附近：closeQuietly() 辅助方法位置
  - 第 38 行附近：类字段区域（ConnectionRecord 创建 + activeConnections）
  - onCreate()/onDestroy()：线程池生命周期管理（可选）
- **Modify:** `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\ConnectionRecord.java`
  - 新增 ConnectionState enum，替换 active + failed boolean
- **Modify:** `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\ConnectionLogAdapter.java`
  - 第 98-104 行：UI 颜色逻辑（绿色/灰色）
- **Modify:** `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\MainActivity.java`
  - UI 自动刷新 Handler（0.5 秒定时）

---

## Task 1: 添加 Pipe 内部类

**Files:**
- Modify: `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\Socks5ProxyService.java`

- [ ] **Step 1: 在 forward() 方法之前（第476行附近）添加 Pipe 内部类**

```java
/**
 * 连接管道：管理双向转发的生命周期
 *
 * 中断判定（v3 Req 3）：任一条件满足即认为本方向已中断，关闭对端 Socket：
 * - 读到 EOF（正常结束）
 * - 抛出 IOException（异常结束）
 * - 15 秒无数据（setSoTimeout 触发 SocketTimeoutException）
 *
 * 双向同步（v3 Req 2）：closePeer() 关闭对端后，对端 forward() 的 read() 会抛出
 * IOException 从而也调用 closePeer()，最终两个方向都会被正确销毁。
 *
 * 状态语义（v3 Req 4，用于 UI）：
 * - markEnded() 被调用 → ended = true → 灰色（连接已结束）
 * - 未调用 markEnded() → ended = false → 绿色（正在进行中）
 * - 不再区分正常/异常，结束即灰色
 */
private static class Pipe {
    private final Socket source;  // 本方向的读端
    private final Socket sink;    // 本方向的写端（即对端的读端）
    private final AtomicBoolean closed = new AtomicBoolean(false);
    /** 标记本方向是否已结束。EOF/异常/超时均调用 markEnded()。 */
    private final AtomicBoolean ended = new AtomicBoolean(false);
    private final String direction;
    private final ConnectionRecord record;

    Pipe(Socket source, Socket sink, String direction, ConnectionRecord record) {
        this.source = source;
        this.sink = sink;
        this.direction = direction;
        this.record = record;
    }

    /**
     * 关闭对端 Socket，触发另一个 forward 线程的 read() 退出（v3 Req 2）
     *
     * 关键：必须先用 shutdownInput() 发送 TCP FIN 包，再 close()。
     * 直接 close() 在某些 OS/JVM 下不会可靠地中断另一个线程阻塞的 read()。
     * shutdownInput() 让对端的 read() 立即返回 -1（EOF），从而退出 while 循环。
     */
    void closePeer() {
        if (closed.compareAndSet(false, true)) {
            try {
                sink.shutdownInput();   // 发送 TCP FIN → 对端 read() 立即返回 -1
            } catch (IOException ignored) { /* socket 已关闭则忽略 */ }
            try {
                sink.shutdownOutput();  // 同时关闭输出方向，更干净
            } catch (IOException ignored) { /* socket 已关闭则忽略 */ }
            closeQuietly(sink);        // 最后释放 socket fd
            Log.d(TAG, direction + ": closed peer socket to stop opposite thread");
        }
    }

    /**
     * 标记本方向已结束（EOF/异常/超时），用于 UI 状态判断（v3 Req 4）
     */
    void markEnded() {
        ended.set(true);
    }

    boolean isEnded() {
        return ended.get();
    }

    /**
     * 关闭本方向的所有 Socket（v3 Req 2：线程销毁时同步关闭连接）
     */
    void closeAll() {
        closePeer();  // 关闭对端
        closeQuietly(source);  // 关闭本端（v2 Fix 1：防止 fd 泄漏）
    }
}
```

- [ ] **Step 2: 验证编译**

Run: `cd molink-worker && ./gradlew assembleDebug 2>&1 | tail -20`
Expected: BUILD SUCCESSFUL（新增代码不影响编译）

**注意：** 不允许提交 git，仅修改本地文件

---

## Task 2: 重构 forward() 方法

**Files:**
- Modify: `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\Socks5ProxyService.java:478-510`

> **Behavior Note（v3 Req 4）：** 所有退出路径（EOF/超时/异常）均调用 `markEnded()`，UI 只区分"进行中（绿色）"和"已结束（灰色）"。不再区分正常/异常结束。

- [ ] **Step 1: 将 forward() 方法签名从 `forward(Socket src, Socket dest, String direction, ConnectionRecord record)` 改为 `forward(Pipe pipe)`，替换方法体**

删除原有 478-510 行的 forward() 方法，用以下代码替换：

```java
private void forward(Pipe pipe) {
    try {
        InputStream in = pipe.source.getInputStream();
        OutputStream out = pipe.sink.getOutputStream();
        byte[] buffer = new byte[8192];
        int len;
        while ((len = in.read(buffer)) != -1) {
            out.write(buffer, 0, len);
            out.flush();

            // === UI 统计：字节计数 ===
            if ("c->s".equals(pipe.direction)) {
                totalBytesDown.addAndGet(len);
                pipe.record.bytesDown.addAndGet(len);
            } else {
                totalBytesUp.addAndGet(len);
                pipe.record.bytesUp.addAndGet(len);
            }
        }

        // 正常 EOF → 标记结束，关闭对端（v3 Req 3 & Req 4）
        pipe.markEnded();
        Log.d(TAG, pipe.direction + ": EOF reached, ended");
        pipe.closePeer();

    } catch (IOException e) {
        String msg = e.getMessage();
        // v2 Fix 2 + v3 Req 3: "Read timed out" = SocketTimeoutException = 15秒无数据中断
        if (msg == null || (!msg.contains("Socket closed")
                && !msg.contains("Connection reset")
                && !msg.contains("Broken pipe")
                && !msg.contains("EOF")
                && !msg.contains("Read timed out")
                && !msg.contains("Socket timed out"))) {
            Log.w(TAG, pipe.direction + ": unexpected IOException: " + msg);
        }
        // 异常 → 标记结束，关闭对端（v3 Req 3 & Req 4）
        pipe.markEnded();
        pipe.closePeer();
    } finally {
        // v2 Fix 1 + v3 Req 2: closeQuietly() 内部调用 shutdownInput+close，
        // 确保 source fd 释放，且对端 read() 立即退出
        closeQuietly(pipe.source);
        pipe.record.checkEnded();  // v3 Req 4: 同步 record 状态
    }
}
```

> **中断判定逻辑（v3 Req 3）：**
> `setSoTimeout(15000)` 在 Socket 上设置后，以下任一条件触发即认为该方向中断：
> - `in.read()` 返回 -1（EOF，正常关闭）
> - `in.read()` 抛出 `SocketTimeoutException`（15 秒无数据，异常关闭）
> - `in.read()` 抛出其他 `IOException`（连接异常）
>
> 任一方向中断 → `closePeer()` 关闭对端 → 对端 read() 也会抛异常 → 双向同步关闭。

- [ ] **Step 2: 验证编译**

Run: `cd molink-worker && ./gradlew assembleDebug 2>&1 | tail -20`
Expected: BUILD SUCCESSFUL

---

## Task 3: 修改 handleClient() 创建 Pipe 对象

**Files:**
- Modify: `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\Socks5ProxyService.java:386-411`

- [ ] **Step 1: 修改 handleClient() 中双向转发部分的代码（第386-393行）**

修改前：
```java
Thread t1 = new Thread(() -> forward(clientSocket, targetSocket, "c->s", record), "Forward-c->s");
Thread t2 = new Thread(() -> forward(targetSocket, clientSocket, "s->c", record), "Forward-s->c");
t1.start();
t2.start();

t1.join();
t2.join();
```

修改后：
```java
// 创建管道对象
Pipe pipe1 = new Pipe(clientSocket, targetSocket, "c->s", record);
Pipe pipe2 = new Pipe(targetSocket, clientSocket, "s->c", record);

Thread t1 = new Thread(() -> forward(pipe1), "Forward-c->s");
Thread t2 = new Thread(() -> forward(pipe2), "Forward-s->c");
t1.start();
t2.start();

// 设置超时等待，避免永久阻塞
t1.join(60_000);
t2.join(60_000);

// v3 Req 2: 如果超时后仍存活，强制中断并销毁 Socket
if (t1.isAlive() || t2.isAlive()) {
    Log.w(TAG, "Forward threads still alive after 60s, forcing shutdown");
    t1.interrupt();
    t2.interrupt();
    pipe1.closeAll();
    pipe2.closeAll();
}

// === UI 状态判断（v3 Req 4）：任一方向结束即灰色 ===
// 不再区分正常/异常，只要任一方向中断即为已结束
boolean pipe1Ended = pipe1.isEnded();
boolean pipe2Ended = pipe2.isEnded();
if (pipe1Ended && pipe2Ended) {
    // 两个方向都结束了，record 状态在 checkEnded() 中已更新
    Log.d(TAG, "Connection ended (gray dot)");
} else {
    // 理论上 join() 已等待完毕，不会走到这里，记录以防万一
    Log.w(TAG, "Connection join() returned but threads not both ended: c->s=" + pipe1Ended + " s->c=" + pipe2Ended);
}
```

- [ ] **Step 2: 验证编译**

Run: `cd molink-worker && ./gradlew assembleDebug 2>&1 | tail -20`
Expected: BUILD SUCCESSFUL

---

## Task 4: 修复异常处理中的 targetSocket 泄漏

**Files:**
- Modify: `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\Socks5ProxyService.java:404-411`

- [ ] **Step 1: 修改 handleClient() 异常处理块（第404-411行）**

修改前：
```java
} catch (Exception e) {
    Log.e(TAG, "handleClient error: " + e.getMessage(), e);
    try {
        clientSocket.close();
    } catch (IOException ex) {
        // ignore
    }
}
```

修改后：
```java
} catch (Exception e) {
    Log.e(TAG, "handleClient error: " + e.getMessage(), e);

    // v3 Req 2: 确保所有 Socket 都被关闭
    closeQuietly(targetSocket);
    closeQuietly(clientSocket);

    // 更新统计（v3 Req 4：如果 record 已创建，标记为已结束）
    if (record != null) {
        record.setEnded();  // 替代 active=false + failed=true
        activeConnections.remove(record);
        connectionCount.decrementAndGet();
        notifyStatusUpdate();
    }
}
```

- [ ] **Step 2: 验证编译**

Run: `cd molink-worker && ./gradlew assembleDebug 2>&1 | tail -20`
Expected: BUILD SUCCESSFUL

---

## Task 5: 添加 closeQuietly(Socket) 辅助方法

**Files:**
- Modify: `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\Socks5ProxyService.java`

- [ ] **Step 1: 在现有的 closeQuietly(ServerSocket) 辅助方法附近添加 Socket 版本**

找到现有的 closeQuietly(ServerSocket) 方法（约第167行），在其后添加：

```java
/**
 * 安全关闭 Socket（v3 Req 2 补充：shutdownInput + close 确保 fd 释放）
 */
private void closeQuietly(Socket socket) {
    if (socket == null) return;
    try {
        socket.shutdownInput();  // 发送 FIN，unblock 对端 read()
    } catch (IOException ignored) { /* ignore */ }
    try {
        socket.shutdownOutput();
    } catch (IOException ignored) { /* ignore */ }
    try {
        if (!socket.isClosed()) {
            socket.close();  // 释放 fd
        }
    } catch (IOException e) {
        Log.w(TAG, "Error closing Socket: " + e.getMessage());
    }
}
```

- [ ] **Step 2: 验证编译**

Run: `cd molink-worker && ./gradlew assembleDebug 2>&1 | tail -20`
Expected: BUILD SUCCESSFUL

---

## Task 6: 添加超时控制

**Files:**
- Modify: `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\Socks5ProxyService.java`

- [ ] **Step 1: 修改 targetSocket 创建逻辑（第361行附近）**

修改前（推测）：
```java
Socket targetSocket = new Socket(destAddr, destPort);
```

修改后：
```java
// 建立目标连接（使用无参构造 + connect 以设置超时）
Socket targetSocket = new Socket();
try {
    targetSocket.connect(
        new InetSocketAddress(destAddr, destPort),
        10_000  // 连接超时 10 秒
    );
    // v3 Req 3: 15 秒无数据即认为中断（触发 SocketTimeoutException）
    targetSocket.setSoTimeout(15_000);
    targetSocket.setKeepAlive(true);
    targetSocket.setTcpNoDelay(true);
} catch (IOException e) {
    Log.e(TAG, "Failed to connect to " + destAddr + ":" + destPort, e);
    // 发送失败响应
    out.write(0x05);
    out.write(0x04); // Host unreachable
    out.write(0x00);
    out.write(0x01);
    out.write(new byte[6]);
    out.flush();
    clientSocket.close();
    return;
}
```

- [ ] **Step 2: 在 handleClient() 方法开始处添加客户端 Socket 超时设置**

在 handleClient() 方法开始处（约第264行），try 块之后添加：
```java
// v3 Req 3: 客户端 15 秒无数据即中断
clientSocket.setSoTimeout(15_000);
clientSocket.setKeepAlive(true);
```

- [ ] **Step 3: 验证编译**

Run: `cd molink-worker && ./gradlew assembleDebug 2>&1 | tail -20`
Expected: BUILD SUCCESSFUL

---

## Task 7 (Optional): 添加线程池管理

**Priority:** P1 — 核心修复验证后执行

**Files:**
- Modify: `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\Socks5ProxyService.java`

- [ ] **Step 1: 添加线程池字段（约第38行附近）**

```java
private ExecutorService connectionPool;
private ExecutorService forwardPool;
private static final int MAX_CONNECTIONS = 50;
private static final int MAX_FORWARD_THREADS = 100;
```

- [ ] **Step 2: 在 onCreate() 中初始化线程池**

```java
connectionPool = Executors.newFixedThreadPool(
    MAX_CONNECTIONS,
    r -> {
        Thread t = new Thread(r, "Socks5Handler");
        t.setDaemon(true);
        return t;
    }
);

forwardPool = Executors.newFixedThreadPool(
    MAX_FORWARD_THREADS,
    r -> {
        Thread t = new Thread(r, "Forward");
        t.setDaemon(true);
        return t;
    }
);
```

- [ ] **Step 3: 修改第216、236行的连接接受逻辑**

修改前：
```java
new Thread(() -> handleClient(clientSocket), "Socks5Handler-IPv4").start();
```

修改后：
```java
connectionPool.submit(() -> handleClient(clientSocket));
```

- [ ] **Step 4: 在 onDestroy() 中清理线程池**

```java
if (connectionPool != null) {
    connectionPool.shutdownNow();
    try {
        if (!connectionPool.awaitTermination(5, TimeUnit.SECONDS)) {
            Log.w(TAG, "Connection pool did not terminate");
        }
    } catch (InterruptedException e) {
        Thread.currentThread().interrupt();
    }
}

if (forwardPool != null) {
    forwardPool.shutdownNow();
    try {
        if (!forwardPool.awaitTermination(5, TimeUnit.SECONDS)) {
            Log.w(TAG, "Forward pool did not terminate");
        }
    } catch (InterruptedException e) {
        Thread.currentThread().interrupt();
    }
}
```

- [ ] **Step 5: 验证编译**

Run: `cd molink-worker && ./gradlew assembleDebug 2>&1 | tail -20`
Expected: BUILD SUCCESSFUL

**注意：** Task 7 为可选（P1），根据实际负载情况决定是否实施

---

## Task 8 (P0): 重构 ConnectionRecord 状态（v3 Req 4）

**Files:**
- Modify: `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\ConnectionRecord.java`

- [ ] **Step 1: 替换 active + failed boolean 为 ConnectionState enum**

删除原有的 `active` 和 `failed` 字段，替换为：

```java
/**
 * 连接状态（v3 Req 4）：
 * - ACTIVE：连接进行中 → UI 显示绿色
 * - ENDED：连接已结束（无论正常/异常）→ UI 显示灰色
 */
public enum ConnectionState {
    ACTIVE,
    ENDED
}

public class ConnectionRecord {
    public final String clientIp;
    public final String targetHost;
    public final int targetPort;
    public final long startTime;
    public final AtomicLong bytesDown = new AtomicLong(0);
    public final AtomicLong bytesUp = new AtomicLong(0);
    public volatile ConnectionState state = ConnectionState.ACTIVE;  // 原 active + failed

    public ConnectionRecord(...) { ... }

    /** 标记连接已结束（由 forward() 在任一方向结束时调用） */
    public synchronized void setEnded() {
        if (this.state == ConnectionState.ACTIVE) {
            this.state = ConnectionState.ENDED;
        }
    }

    /** 两个方向都结束时调用，同步更新 record 状态（v3 Req 4） */
    public void checkEnded() {
        // 由 handleClient() 在两个 pipe.isEnded() 都为 true 时调用
        setEnded();
    }

    public boolean isEnded() {
        return state == ConnectionState.ENDED;
    }
}
```

- [ ] **Step 2: 验证编译**

Run: `cd molink-worker && ./gradlew assembleDebug 2>&1 | tail -20`
Expected: BUILD SUCCESSFUL

---

## Task 9 (P0): 更新 ConnectionLogAdapter UI 颜色（v3 Req 4）

**Files:**
- Modify: `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\ConnectionLogAdapter.java:98-104`

- [ ] **Step 1: 替换 UI 颜色逻辑**

修改前：
```java
if (record.failed) {
    dot.setBackgroundResource(R.drawable.circle_red);
} else if (record.active) {
    dot.setBackgroundResource(R.drawable.circle_yellow);
} else {
    dot.setBackgroundResource(R.drawable.circle_green);
}
```

修改后（v3 Req 4：绿色=进行中，灰色=已结束，无红色）：
```java
if (record.isEnded()) {
    dot.setBackgroundResource(R.drawable.circle_gray);  // 原 circle_green
} else {
    dot.setBackgroundResource(R.drawable.circle_green);  // 原 circle_yellow
}
```

- [ ] **Step 2: 创建 circle_gray.xml drawable（如果没有）**

在 `res/drawable/` 中创建 `circle_gray.xml`：
```xml
<?xml version="1.0" encoding="utf-8"?>
<shape xmlns:android="http://schemas.android.com/apk/res/android"
    android:shape="oval">
    <solid android:color="#9E9E9E" />
    <size android:width="12dp" android:height="12dp" />
</shape>
```

- [ ] **Step 3: 验证编译**

Run: `cd molink-worker && ./gradlew assembleDebug 2>&1 | tail -20`
Expected: BUILD SUCCESSFUL

---

## Task 10 (P0): UI 每 0.5 秒自动全量刷新（v3 Req 1）

**Files:**
- Modify: `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\MainActivity.java`

**背景：** 当前 UI 依赖 `notifyStatusUpdate()` 主动刷新，耦合紧。改为 UI 定时读取 final list，实现完全解耦。

- [ ] **Step 1: 在 MainActivity 中查找现有的 UI 刷新逻辑**

找到 `Handler` 或 `Runnable` 相关的刷新代码。

- [ ] **Step 2: 添加 0.5 秒定时刷新 Handler**

```java
// v3 Req 1: UI 每 0.5 秒自动全量刷新，数据源为 final list
private static final long UI_REFRESH_INTERVAL_MS = 500;

private final Handler uiRefreshHandler = new Handler(Looper.getMainLooper());
private final Runnable uiRefreshRunnable = new Runnable() {
    @Override
    public void run() {
        adapter.refreshAll(Socks5ProxyService.getConnectionSnapshot());
        uiRefreshHandler.postDelayed(this, UI_REFRESH_INTERVAL_MS);
    }
};

// 在 onResume() 中启动：
@Override
protected void onResume() {
    super.onResume();
    uiRefreshHandler.post(uiRefreshRunnable);  // 立即刷新一次，然后每 0.5s
}

// 在 onPause() 中停止（节省资源）：
@Override
protected void onPause() {
    super.onPause();
    uiRefreshHandler.removeCallbacks(uiRefreshRunnable);
}
```

- [ ] **Step 3: 在 Socks5ProxyService 中添加快照方法**

在 `Socks5ProxyService.java` 类字段区域添加：
```java
// v3 Req 1: 提供只读快照供 UI 定时读取，调用方获得瞬时副本，不持有锁
public static List<ConnectionRecord> getConnectionSnapshot() {
    return new ArrayList<>(activeConnections);
}
```

- [ ] **Step 4: 移除 Socks5ProxyService 中的 notifyStatusUpdate() 调用（不再需要主动通知）**

搜索 `notifyStatusUpdate()` 的所有调用位置，从 `handleClient()` 和连接管理代码中移除。Service 的 notifyStatusUpdate() 方法本身可保留（如果其他模块还在用），否则删除。

**注意：** 此改动涉及多处搜索，建议用 Grep 工具确认所有调用位置后再修改。

- [ ] **Step 5: 验证编译**

Run: `cd molink-worker && ./gradlew assembleDebug 2>&1 | tail -20`
Expected: BUILD SUCCESSFUL

### 测试场景 1：客户端主动断开（必须验证）

```bash
# 终端1：启动 worker 并查看日志
adb logcat -s Socks5ProxyService

# 终端2：通过 proxy 开始下载大文件
curl --socks5 127.0.0.1:1080 http://ipv4.download.thinkbroadband.com/10MB.zip -o test.zip

# 下载过程中按 Ctrl-C 中断

# 预期：日志立即显示 "closed peer socket"，线程快速退出，无资源泄漏
```

### 测试场景 2：连接超时

```bash
# 连接到不存在的服务器（10秒应超时）
curl --socks5 127.0.0.1:1080 http://10.255.255.1:9999

# 预期：约10秒后收到 SOCKS5 失败响应，而非无限等待
```

### 测试场景 3：正常完成

```bash
# 正常访问
curl --socks5 127.0.0.1:1080 http://example.com

# 预期：正常返回，连接正常关闭，统计数据正确
```

### 测试场景 4：资源泄漏检查

```bash
# 创建100个短连接
for i in {1..100}; do
    curl --socks5 127.0.0.1:1080 http://example.com &
done
wait

# 检查线程数
adb shell ps -T | grep molink-worker | wc -l

# 预期：线程数不应持续增长
```

---

## 修改总结（v3）

- **修改文件数：** 4 个（Socks5ProxyService.java、ConnectionRecord.java、ConnectionLogAdapter.java、MainActivity.java）
- **新增代码：** 约 50 行（Pipe 类 + 辅助方法 + 线程池）
- **修改代码：** 约 60 行（forward + handleClient + ConnectionRecord + UI）
- **Task 数：** 10 个（Task 1-6 + 8-10 为必须 P0；Task 7 为可选 P1）
- **v3 修订：** Task 1 closePeer() 新增 shutdownInput+shutdownOutput；Task 2 finally 块说明更新；Task 5 closeQuietly() 三步关闭；Task 8-10 新增

---

## 预期效果

| 场景 | 修改前 | 修改后 |
|------|--------|--------|
| 客户端 Ctrl-C | targetSocket 保持打开 60+ 秒 | targetSocket < 1 秒关闭，fd 无泄漏，双向同步 |
| 异常路径 | targetSocket 泄漏 | 全部正确关闭（finally + closeAll 保证） |
| 连接失败 | 无限等待 | 10 秒超时返回 |
| 15 秒无数据 | 无感知 | 自动中断，双向同步关闭，UI 变灰色 |
| 高并发 | 无线程数上限 | 线程池限制（Task 7 可选） |
| UI 刷新 | 主动通知触发 | 0.5 秒定时全量刷新，list 为 final，完全解耦 |
| UI 状态 | 红/黄/绿三色 | 绿（进行中）/灰（已结束），无红色 |
