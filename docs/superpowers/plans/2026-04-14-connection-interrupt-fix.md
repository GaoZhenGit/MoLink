# Worker 连接中断处理修复 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking>

**Plan 版本：** v3（整合用户反馈 v2 后的修订版）

**v3 新增需求（用户反馈）：**
- **v3 Req 1（核心）**：UI 每 0.5 秒自动全量刷新 record list，list 为 final 类型，连接逻辑只改 list
- **v3 Req 2（核心）**：两个方向的线程和 Socket 必须同步销毁，任一方向中断时，另一方向强制中断
- **v3 Req 3（确认）**：中断判定条件 = EOF 或 IOException 或 55 秒无数据；使用 `setSoTimeout(55000)` 实现；FIN 延迟到达时 55 秒兜底检测
- **v3 Req 4（确认）**：record 状态简化为"进行中/已结束"，对应绿色/灰色；去除红色状态；数据结构从 active+failed boolean 改为 state enum

**v2 遗留修复：**
- **v2 Fix 1（中等）**：forward() finally 块 + closeQuietly() + closePeer() 均使用 `shutdownInput() + shutdownOutput() + close()` 三步关闭，防止 read() 阻塞无法中断
- **v2 Fix 2（轻微）**：forward() 异常过滤列表加入 "Read timed out"，避免超时触发噪音日志
- **v2 Fix 3（确认）**：failed 逻辑迁移（已被 v3 Req 4 替代，数据结构改为 enum）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 molink-worker 的 Socks5ProxyService，当客户端主动断开（Ctrl-C）时，立即关闭与目标服务器的连接，节省流量并释放资源。

**Architecture:** 三层完全解耦：
- **数据层**：`activeConnections` 为 `final List<ConnectionRecord> = Collections.synchronizedList(new ArrayList<>())`，两层共享同一引用，无复制开销
  - 单次 `add()` / `remove()`：内置 synchronized，无需额外加锁
  - check-then-act 复合操作（addConnectionRecord 中的 while+remove+add）：需 synchronized 包裹
  - `refreshAll()` 迭代：需 synchronized 块包裹
- **连接层（Socks5ProxyService）**：仅通过 `addConnectionRecord()` / `removeConnectionRecord()` 操作 list，不调用任何 UI 方法
- **Record 层（ConnectionRecord）**：通过 `ConnectionState enum` 管理状态，forward 线程更新 record 字段
- **UI 层（MainActivity + ConnectionLogAdapter）**：每 0.5 秒调用 `getConnectionSnapshot()` 读取同一份 list，refreshAll() 内部构建自己的 list，完全解耦

**Tech Stack:** Android Java (API 27+), SOCKS5 协议, Java Socket API

---

## File Structure

- **Modify:** `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\Socks5ProxyService.java`
  - 第 29 行附近：移除 `CopyOnWriteArrayList`，添加 `Collections`
  - 第 38 行附近：类字段区域（添加 MAX_HISTORY = 50）
  - 第 64 行：activeConnections 改为 `final List<ConnectionRecord> = Collections.synchronizedList(new ArrayList<>())`
  - 第 163 行附近：getConnectionCount() 改为 return activeConnections.size()
  - 第 256 行：移除该处 notifyStatusUpdate() 调用
  - 第 263-412 行：handleClient() 方法（仅操作 list，不调用任何 UI 方法）
  - 第 378-398 行：handleClient() 中的 list 操作改为 addRecord/removeRecord
  - 第 478-510 行：forward() 方法（仅更新 record 字段，不调用任何 UI 方法）
  - 第 82-92 行：notifyStatusUpdate() 方法（标记为 deprecated 或删除）
  - 第 167 行附近：closeQuietly() 辅助方法位置
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
> `setSoTimeout(55000)` 在 Socket 上设置后，以下任一条件触发即认为该方向中断：
> - `in.read()` 返回 -1（EOF，正常关闭）
> - `in.read()` 抛出 `IOException`（连接异常）
> - `in.read()` 抛出 `SocketTimeoutException`（55 秒无数据，兜底检测对端存活）
>
> **为何用 55 秒**：curl Ctrl-C 后 FIN 可能被 adb forward 隧道延迟，pipe1.read() 收不到 FIN 会一直阻塞，server 继续发送数据直到完成。55 秒覆盖大多数 HTTP Keep-Alive 场景。
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
// v3 Req 2: handleClient() 仅操作 record list，不调用任何 UI 方法

// 第378行附近：连接建立后，添加到 list（超出 MAX_HISTORY 时从队首删除旧记录）
historyCount.incrementAndGet();  // 历史计数，不变
String clientIp = clientSocket.getRemoteSocketAddress().toString();
if (clientIp.startsWith("/")) clientIp = clientIp.substring(1);
final ConnectionRecord record = new ConnectionRecord(clientIp, destAddr, destPort, System.currentTimeMillis());
addConnectionRecord(record);  // ← 新增，超过 MAX_HISTORY 时 FIFO 删除旧记录

// 双向转发（Pipe 逻辑不变，同 Task 1-2）
Pipe pipe1 = new Pipe(clientSocket, targetSocket, "c->s", record);
Pipe pipe2 = new Pipe(targetSocket, clientSocket, "s->c", record);
Thread t1 = new Thread(() -> forward(pipe1), "Forward-c->s");
Thread t2 = new Thread(() -> forward(pipe2), "Forward-s->c");
t1.start();
t2.start();

// 设置超时等待，避免永久阻塞
t1.join(60_000);
t2.join(60_000);

// v3 Req 2: 超时强制中断时，closeAll() 关闭连接（不涉及 UI）
if (t1.isAlive() || t2.isAlive()) {
    Log.w(TAG, "Forward threads still alive after 60s, forcing shutdown");
    t1.interrupt();
    t2.interrupt();
    pipe1.closeAll();
    pipe2.closeAll();
}

// === 记录结束状态（v3 Req 4）===
pipe1.isEnded();   // 触发 Pipe1 的 ended 状态读取
pipe2.isEnded();   // 触发 Pipe2 的 ended 状态读取
record.setEnded(); // 两个方向都结束了，标记 record 为已结束

// 第396行附近：连接关闭后，从 list 移除（不再调用 notifyStatusUpdate）
removeConnectionRecord(record);  // ← 新增
Log.i(TAG, "Client session closed");
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

    // v3 Req 2: 仅关闭 Socket，不调用任何 UI 方法
    closeQuietly(targetSocket);
    closeQuietly(clientSocket);

    // v3 Req 4: 如果 record 已创建，标记为已结束并从 list 移除
    if (record != null) {
        record.setEnded();
        removeConnectionRecord(record);  // ← 替代 notifyStatusUpdate()
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
    // v3 Req 3: 55 秒无数据即认为中断（触发 SocketTimeoutException）
    targetSocket.setSoTimeout(55_000);
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
// v3 Req 3: 55 秒无数据即中断，兜底检测客户端存活
try {
    clientSocket.setSoTimeout(55_000);
    clientSocket.setKeepAlive(true);
} catch (SocketException e) {
    Log.w(TAG, "Failed to set client socket options: " + e.getMessage());
}
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

## Task 8 (P0): 记录列表管理 + 控制层完全解耦（v3 Req 1 & Req 2）

**Files:**
- Modify: `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\Socks5ProxyService.java`

**设计原则（v3 Req 2）：连接处理逻辑仅操作 record list，不调用任何 UI 方法。UI 通过定时轮询 getConnectionSnapshot() 获取数据，完全解耦。**

- [ ] **Step 1: 添加 MAX_HISTORY 常量，与 ConnectionLogAdapter.MAX_ITEMS 保持一致（第38行附近）**

```java
// v3 Req 1: 历史记录上限，超出时 FIFO 删除最旧记录
private static final int MAX_HISTORY = 50;
```

- [ ] **Step 2: 将 activeConnections 改为 final**

```java
// v3 Req 1: final synchronized list，两层共享同一引用，无复制开销
// 写操作（add/remove）由 synchronized 保护；读操作（refreshAll 迭代）需在 synchronized 块中执行
private final List<ConnectionRecord> activeConnections = Collections.synchronizedList(new ArrayList<>());
```

- [ ] **Step 3: 添加 addConnectionRecord() 方法（记录上限 FIFO 管理）**

在 activeConnections 字段下方添加：

```java
/**
 * 添加连接记录，超出 MAX_HISTORY 时从队首删除最旧记录（v3 Req 1）
 * synchronizedList 单次 add/remove 内部已加锁，无需额外同步
 * 但 check-then-act 复合操作（while + remove + add）需要 synchronized 包裹
 */
private void addConnectionRecord(ConnectionRecord record) {
    synchronized (activeConnections) {
        while (activeConnections.size() >= MAX_HISTORY) {
            activeConnections.remove(0);  // 删除最旧记录（FIFO）
        }
        activeConnections.add(record);
    }
}

/**
 * 移除连接记录（v3 Req 1）
 * synchronizedList 单次 remove 内部已加锁，无需额外同步
 */
private void removeConnectionRecord(ConnectionRecord record) {
    activeConnections.remove(record);
}
```

- [ ] **Step 4: 修改 getConnectionCount()，直接返回 list size**

约第163行，修改 `getConnectionCount()` 方法：

```java
// v3 Req 2: 无需维护 connectionCount 字段，直接从 list 计算
public int getConnectionCount() {
    return activeConnections.size();
}
```

- [ ] **Step 5: 添加 getConnectionSnapshot() 方法（供 UI 轮询）**

在 getConnectionCount() 附近添加：

```java
// v3 Req 1: 返回 shared reference，无需复制
// CopyOnWriteArrayList 本身线程安全，refreshAll() 内部会构建自己的 list
public static List<ConnectionRecord> getConnectionSnapshot() {
    return activeConnections;
}
```

- [ ] **Step 6: 移除所有 notifyStatusUpdate() 调用（共2处）**

使用 Grep 找到以下位置，逐一移除：
- `handleClient()` 第398行：移除 `notifyStatusUpdate()` 调用（已被 Step 3/Task 4 替换）
- SOCKS5 服务启动处约第256行：移除该处 `notifyStatusUpdate()` 调用

notifyStatusUpdate() 方法本身可以保留（不影响功能，若确认无其他调用方可删除）。

- [ ] **Step 7: 验证编译**

Run: `cd molink-worker && ./gradlew assembleDebug 2>&1 | tail -20`
Expected: BUILD SUCCESSFUL

---

## Task 9 (P0): 重构 ConnectionRecord 状态（v3 Req 4）

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

## Task 10 (P0): 更新 ConnectionLogAdapter UI 颜色（v3 Req 4）

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

- [ ] **Step 2: 修改 refreshAll() 迭代逻辑（使用 synchronized 块）**

在 `refreshAll()` 方法中，迭代 shared synchronized list 时需要 synchronized 块：

```java
public void refreshAll(List<ConnectionRecord> newItems) {
    items.clear();
    List<ConnectionRecord> filtered = new ArrayList<>();
    // v3 Req 1: synchronizedList 迭代需在 synchronized 块中，防止并发修改
    synchronized (newItems) {
        for (ConnectionRecord r : newItems) {
            if (r == null) continue;
            String host = r.targetHost;
            if (host == null) continue;
            boolean isLocalhost = host.startsWith("127.") || host.equals("::1") || host.equals("0:0:0:0:0:0:0:1");
            boolean isStatusPort = r.targetPort == BuildConfig.STATUS_HTTP_PORT;
            if (!isLocalhost && !isStatusPort) {
                filtered.add(r);
            }
        }
    }
    // 后续逻辑不变：反转 + 取前 MAX_ITEMS 条 ...
}
```

- [ ] **Step 3: 创建 circle_gray.xml drawable（如果没有）**

在 `res/drawable/` 中创建 `circle_gray.xml`：
```xml
<?xml version="1.0" encoding="utf-8"?>
<shape xmlns:android="http://schemas.android.com/apk/res/android"
    android:shape="oval">
    <solid android:color="#9E9E9E" />
    <size android:width="12dp" android:height="12dp" />
</shape>
```

- [ ] **Step 4: 验证编译**

Run: `cd molink-worker && ./gradlew assembleDebug 2>&1 | tail -20`
Expected: BUILD SUCCESSFUL

---

## Task 11 (P0): UI 每 0.5 秒自动全量刷新（v3 Req 1）

**Files:**
- Modify: `D:\project\MoLink\molink-worker\app\src\main\java\com\molink\worker\MainActivity.java`

**目标：** 移除 StatusListener 主动回调，用 0.5 秒定时轮询 getConnectionSnapshot() 替代，实现 UI 与控制层完全解耦。getConnectionSnapshot() 在 Task 8 中已添加（返回 shared reference，无复制）。**

- [ ] **Step 1: 读取 MainActivity 现有 uiPoller 和 StatusListener 代码**

确认现有的 `uiHandler` 和 `uiPoller`（每 2 秒）的具体实现位置，以及 StatusListener 的注册/注销位置。

- [ ] **Step 2: 简化 uiPoller 为 0.5 秒轮询**

修改现有的 `uiPoller`，改为每 0.5 秒读取快照并刷新全部 UI：

```java
// v3 Req 1: 0.5 秒全量轮询，完全替代 StatusListener 回调
private static final long UI_REFRESH_INTERVAL_MS = 500;

private final Runnable uiPoller = new Runnable() {
    @Override
    public void run() {
        if (!isFinishing()) {
            Socks5ProxyService svc = Socks5ProxyService.getInstance();
            if (svc != null && svc.isRunning()) {
                // 读取 shared list，refreshAll() 内部构建自己的 list
                List<ConnectionRecord> snapshot = Socks5ProxyService.getConnectionSnapshot();
                connCount.setText(String.valueOf(snapshot.size()));  // 活动连接数
                historyCount.setText(String.valueOf(svc.getHistoryCount()));  // 历史总数
                bytesDown.setText(formatBytes(svc.getTotalBytesDown()));
                bytesUp.setText(formatBytes(svc.getTotalBytesUp()));
                statusUptime.setText("在线:" + formatUptime(svc.getUptime()));
                logAdapter.refreshAll(snapshot);  // 全量刷新（snapshot 内部构建自己的 list）
            }
            uiHandler.postDelayed(this, UI_REFRESH_INTERVAL_MS);
        }
    }
};
```

- [ ] **Step 3: 移除 StatusListener 机制**

在 MainActivity 中：
1. 删除 `statusListener` 字段（整个匿名类）
2. 从 `onCreate()`/`onResume()`/`onPause()`/`onToggleClick()` 中移除所有 `addStatusListener()` 和 `removeStatusListener()` 调用

```java
// onCreate 中：删除以下代码
// svc.addStatusListener(statusListener);

// onResume 中：删除以下代码
// svc.addStatusListener(statusListener);

// onPause 中：删除以下代码
// svc.removeStatusListener(statusListener);

// onToggleClick 中：删除以下代码
// svc.removeStatusListener(statusListener);
```

- [ ] **Step 4: 添加 totalBytesDown/totalBytesUp 访问方法**

在 Socks5ProxyService 中（若不存在）添加：

```java
public long getTotalBytesDown() { return totalBytesDown.get(); }
public long getTotalBytesUp() { return totalBytesUp.get(); }
public int getHistoryCount() { return historyCount.get(); }
```

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
- **Task 数：** 11 个（Task 1-6 + 8-11 为必须 P0；Task 7 为可选 P1）
- **v3 修订：** Task 1 closePeer() 新增 shutdownInput+shutdownOutput；Task 2 finally 块说明更新；Task 5 closeQuietly() 三步关闭；Task 8-10 新增

---

## 预期效果

| 场景 | 修改前 | 修改后 |
|------|--------|--------|
| 客户端 Ctrl-C | targetSocket 保持打开 60+ 秒 | <1 秒关闭，fd 无泄漏，双向同步 |
| 异常路径 | targetSocket 泄漏 | 全部正确关闭（finally + closeAll 保证） |
| 连接失败 | 无限等待 | 10 秒超时返回 |
| 55 秒无数据（curl 崩溃/隧道断） | 无感知 | 自动中断，双向同步关闭，UI 变灰色 |
| 高并发 | 无线程数上限 | 线程池限制（Task 7 可选） |
| UI 刷新 | 主动通知触发 | 0.5 秒定时全量刷新，list 为 final，完全解耦 |
| UI 状态 | 红/黄/绿三色 | 绿（进行中）/灰（已结束），无红色 |
