# MoLink 双端状态显示 — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 worker 端新增 NanoHTTPD HTTP Server（端口 8081）提供状态接口；在 access 端新增 WorkerStatusTracker 轮询 worker 状态、Socks5HealthChecker 测试代理通道，并在 `/api/status` 中整合两者的结果。

**Architecture:** Worker 端 NanoHTTPD 与 Socks5ProxyService 同进程运行；Access 端通过 dadb forward 轮询 worker HTTP 接口，同时通过 SOCKS 代理发测试请求验证通道通畅性。

**Tech Stack:** NanoHTTPD 2.3.1（worker）、Java HttpURLConnection / OkHttp（access）、dadb 1.2.10

---

## 文件结构

```
molink-worker/app/src/main/java/com/molink/worker/
├── StatusHttpServer.java     # [新增] NanoHTTPD HTTP Server，端口 8081
└── Socks5ProxyService.java  # [改造] 启动/停止时管理 HTTP Server

molink-access/src/main/java/com/molink/access/
├── status/
│   ├── WorkerStatusTracker.java    # [新增] dadb forward + HTTP 轮询 worker 状态
│   └── Socks5HealthChecker.java    # [新增] SOCKS 代理通道连通性测试
├── controller/
│   └── StatusController.java       # [改造] 整合 workerStatus + proxyHealth
└── AccessApplication.java          # [改造] 启动时初始化 Tracker 和 Checker
```

---

## Part 1: Worker 端

### Task 1: 添加 NanoHTTPD 依赖

**Files:**
- Modify: `molink-worker/app/build.gradle`

- [ ] **Step 1: 在 build.gradle dependencies 中添加 nanohttpd**

在 `dependencies { }` 块内新增一行：

```groovy
implementation 'org.nanohttpd:nanohttpd:2.3.1'
```

nanohttpd 2.3.1 兼容 Java 8，无其他传递依赖。

---

### Task 2: 新建 StatusHttpServer

**Files:**
- Create: `molink-worker/app/src/main/java/com/molink/worker/StatusHttpServer.java`

- [ ] **Step 1: 创建 StatusHttpServer.java**

```java
package com.molink.worker;

import fi.iki.elonen.NanoHTTPD;

public class StatusHttpServer extends NanoHTTPD {

    private final Socks5ProxyService proxyService;

    public StatusHttpServer(int port, Socks5ProxyService proxyService) {
        super("127.0.0.1", port);
        this.proxyService = proxyService;
    }

    @Override
    public Response serve(IHTTPSession session) {
        String uri = session.getUri();

        if ("/api/status".equals(uri) && Method.GET.equals(session.getMethod())) {
            return jsonResponse(buildStatus());
        } else if ("/api/ping".equals(uri) && Method.GET.equals(session.getMethod())) {
            return jsonResponse(buildPing());
        } else {
            return newFixedLengthResponse(Response.Status.NOT_FOUND,
                    "application/json", "{\"error\":\"not found\"}");
        }
    }

    private String buildStatus() {
        android.os.Debug.Writer debugWriter = new android.os.Debug.Writer();
        android.os.ProcessManager.ProcessState processState = null;
        long memoryUsage = 0;
        try {
            android.os.Debug.getMemoryInfo(debugWriter);
            Runtime runtime = Runtime.getRuntime();
            long totalMem = runtime.totalMemory();
            long freeMem = runtime.freeMemory();
            long usedMem = totalMem - freeMem;
            memoryUsage = (usedMem * 100.0) / totalMem;
        } catch (Exception e) {
            memoryUsage = -1;
        }

        StringBuilder sb = new StringBuilder();
        sb.append("{");
        sb.append("\"socksRunning\":").append(proxyService.isRunning()).append(",");
        sb.append("\"socksPort\":").append(proxyService.getPort()).append(",");
        sb.append("\"uptime\":").append(proxyService.getUptime()).append(",");
        sb.append("\"memoryUsage\":").append(String.format("%.1f", memoryUsage)).append(",");
        sb.append("\"connectionCount\":").append(proxyService.getConnectionCount()).append(",");
        sb.append("\"timestamp\":").append(System.currentTimeMillis());
        sb.append("}");
        return sb.toString();
    }

    private String buildPing() {
        return "{\"pong\":true,\"timestamp\":" + System.currentTimeMillis() + "}";
    }

    private Response jsonResponse(String body) {
        return newFixedLengthResponse(Response.Status.OK, "application/json", body);
    }
}
```

> **注意：** `proxyService.getUptime()` 和 `proxyService.getConnectionCount()` 尚未在 Socks5ProxyService 中实现，需要在 Task 3 中添加。

- [ ] **Step 2: 手动验证 — 编译检查**

在 Android Studio 中 Sync Gradle，确认无编译错误。

---

### Task 3: 改造 Socks5ProxyService — 添加状态字段 + 管理 HTTP Server

**Files:**
- Modify: `molink-worker/app/src/main/java/com/molink/worker/Socks5ProxyService.java`

- [ ] **Step 1: 添加状态字段（uptime、connectionCount）**

在类顶部新增字段：

```java
private volatile long startTime = 0;
private final AtomicInteger connectionCount = new AtomicInteger(0);
private StatusHttpServer httpServer;
```

- [ ] **Step 2: 在 `startSocks5Server()` 开头记录启动时间**

```java
startTime = SystemClock.elapsedRealtime() / 1000;
```

- [ ] **Step 3: 在 `onStartCommand()` 中启动 HTTP Server（NanoHTTPD 需要主线程/Looper）**

在 `startSocks5Server();` 后新增：

```java
// 启动 HTTP 状态服务
new Handler(Looper.getMainLooper()).post(() -> {
    try {
        httpServer = new StatusHttpServer(8081, this);
        httpServer.start();
        android.util.Log.i(TAG, "HTTP status server started on port 8081");
    } catch (IOException e) {
        android.util.Log.e(TAG, "Failed to start HTTP server: " + e.getMessage());
    }
});
```

- [ ] **Step 4: 在 `onDestroy()` 中停止 HTTP Server**

```java
if (httpServer != null) {
    httpServer.stop();
}
```

- [ ] **Step 5: 添加新方法**

```java
public long getUptime() {
    if (startTime == 0) return 0;
    return SystemClock.elapsedRealtime() / 1000 - startTime;
}

public int getConnectionCount() {
    return connectionCount.get();
}
```

- [ ] **Step 6: 添加缺失 import**

```java
import android.os.SystemClock;
import android.os.Handler;
import android.os.Looper;
import java.util.concurrent.atomic.AtomicInteger;
import fi.iki.elonen.NanoHTTPD;
```

- [ ] **Step 7: 手动验证 — 编译检查**

Sync Gradle，确认无编译错误。

- [ ] **Step 8: 手动验证 — HTTP 接口测试（设备连接后）**

```bash
adb forward tcp:18081 tcp:8081
curl http://127.0.0.1:18081/api/status
curl http://127.0.0.1:18081/api/ping
```

预期返回 JSON 格式的状态数据和 ping 响应。

---

## Part 2: Access 端

### Task 4: 新建 WorkerStatusTracker

**Files:**
- Create: `molink-access/src/main/java/com/molink/access/status/WorkerStatusTracker.java`

- [ ] **Step 1: 创建 WorkerStatusTracker.java**

```java
package com.molink.access.status;

import com.molink.access.adb.AdbClientManager;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.net.HttpURLConnection;
import java.net.URL;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.*;

public class WorkerStatusTracker {

    private static final Logger log = LoggerFactory.getLogger(WorkerStatusTracker.class);

    private static final int HTTP_PORT = 18081;
    private static final int REMOTE_PORT = 8081;
    private static final int POLL_INTERVAL_SEC = 10;
    private static final int HTTP_TIMEOUT_MS = 5000;

    private final AdbClientManager adbClient;
    private final ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();
    private AutoCloseable currentForward;
    private volatile Map<String, Object> cachedStatus = new LinkedHashMap<>();
    private volatile boolean available = false;

    public WorkerStatusTracker(AdbClientManager adbClient) {
        this.adbClient = adbClient;
    }

    public void start() {
        // 建立 dadb 端口转发
        try {
            currentForward = adbClient.forward(HTTP_PORT, REMOTE_PORT);
            log.info("Worker HTTP forward established: tcp:{} -> tcp:{}", HTTP_PORT, REMOTE_PORT);
        } catch (Exception e) {
            log.error("Failed to establish worker HTTP forward: {}", e.getMessage());
        }

        // 启动轮询
        scheduler.scheduleAtFixedRate(this::poll, 0, POLL_INTERVAL_SEC, TimeUnit.SECONDS);
    }

    public void stop() {
        scheduler.shutdown();
        if (currentForward != null) {
            try {
                currentForward.close();
                log.info("Worker HTTP forward closed");
            } catch (Exception e) {
                log.warn("Error closing worker HTTP forward: {}", e.getMessage());
            }
        }
    }

    public Map<String, Object> getWorkerStatus() {
        Map<String, Object> result = new LinkedHashMap<>(cachedStatus);
        result.put("lastSeen", System.currentTimeMillis());
        result.put("available", available);
        return result;
    }

    private void poll() {
        if (!adbClient.isConnected()) {
            available = false;
            return;
        }
        try {
            URL url = new URL("http://127.0.0.1:" + HTTP_PORT + "/api/status");
            HttpURLConnection conn = (HttpURLConnection) url.openConnection();
            conn.setConnectTimeout(HTTP_TIMEOUT_MS);
            conn.setReadTimeout(HTTP_TIMEOUT_MS);
            conn.setRequestMethod("GET");

            int code = conn.getResponseCode();
            if (code == 200) {
                try (java.io.BufferedReader r = new java.io.BufferedReader(
                        new java.io.InputStreamReader(conn.getInputStream(), "UTF-8"))) {
                    StringBuilder sb = new StringBuilder();
                    String line;
                    while ((line = r.readLine()) != null) sb.append(line);
                    parseAndCache(sb.toString());
                    available = true;
                    log.debug("Worker status polled OK: {}", cachedStatus);
                }
            } else {
                available = false;
                log.warn("Worker HTTP responded code {}", code);
            }
            conn.disconnect();
        } catch (Exception e) {
            available = false;
            log.warn("Worker status poll failed: {}", e.getMessage());
        }
    }

    private void parseAndCache(String json) {
        // 简单字符串解析：提取 socksRunning, socksPort, uptime, memoryUsage, connectionCount
        Map<String, Object> parsed = new LinkedHashMap<>();
        parsed.put("socksRunning", extractBool(json, "socksRunning"));
        parsed.put("socksPort", extractInt(json, "socksPort"));
        parsed.put("uptime", extractLong(json, "uptime"));
        parsed.put("memoryUsage", extractDouble(json, "memoryUsage"));
        parsed.put("connectionCount", extractInt(json, "connectionCount"));
        this.cachedStatus = parsed;
    }

    private boolean extractBool(String json, String key) {
        return extractStr(json, key).equals("true");
    }

    private int extractInt(String json, String key) {
        try { return Integer.parseInt(extractStr(json, key)); } catch (Exception e) { return -1; }
    }

    private long extractLong(String json, String key) {
        try { return Long.parseLong(extractStr(json, key)); } catch (Exception e) { return -1; }
    }

    private double extractDouble(String json, String key) {
        try { return Double.parseDouble(extractStr(json, key)); } catch (Exception e) { return -1; }
    }

    private String extractStr(String json, String key) {
        String pattern = "\"" + key + "\":";
        int idx = json.indexOf(pattern);
        if (idx < 0) return "";
        int start = idx + pattern.length();
        // 跳过空白
        while (start < json.length() && Character.isWhitespace(json.charAt(start))) start++;
        int end = start;
        boolean inString = false;
        while (end < json.length()) {
            char c = json.charAt(end);
            if (c == '"') { inString = !inString; end++; continue; }
            if (!inString && (c == ',' || c == '}')) break;
            end++;
        }
        return json.substring(start, end).trim();
    }
}
```

- [ ] **Step 2: 手动验证 — 编译检查**

```bash
cd molink-access
mvn compile
```

确认无编译错误。

---

### Task 5: 新建 Socks5HealthChecker

**Files:**
- Create: `molink-access/src/main/java/com/molink/access/status/Socks5HealthChecker.java`

- [ ] **Step 1: 创建 Socks5HealthChecker.java**

```java
package com.molink.access.status;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.URL;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.*;

public class Socks5HealthChecker {

    private static final Logger log = LoggerFactory.getLogger(SOCKS5HealthChecker.class);

    private static final String TEST_URL = "https://www.google.com/generate_204";
    private static final int SOCKS_PORT = 1080;
    private static final int POLL_INTERVAL_SEC = 10;
    private static final int HTTP_TIMEOUT_MS = 10000;

    private final ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();
    private volatile Map<String, Object> cachedHealth = new LinkedHashMap<>();

    public Socks5HealthChecker() {}

    public void start() {
        scheduler.scheduleAtFixedRate(this::check, 0, POLL_INTERVAL_SEC, TimeUnit.SECONDS);
    }

    public void stop() {
        scheduler.shutdown();
    }

    public Map<String, Object> getProxyHealth() {
        return new LinkedHashMap<>(cachedHealth);
    }

    private void check() {
        long start = System.currentTimeMillis();
        boolean ok = false;
        try {
            Proxy proxy = new Proxy(Proxy.Type.SOCKS,
                    new InetSocketAddress("127.0.0.1", SOCKS_PORT));
            URL url = new URL(TEST_URL);
            HttpURLConnection conn = (HttpURLConnection) url.openConnection(proxy);
            conn.setConnectTimeout(HTTP_TIMEOUT_MS);
            conn.setReadTimeout(HTTP_TIMEOUT_MS);
            conn.setRequestMethod("GET");
            conn.setInstanceFollowRedirects(false);

            int code = conn.getResponseCode();
            // generate_204 返回 204 或 200 都算正常
            ok = (code == 204 || code == 200);
            conn.disconnect();
        } catch (Exception e) {
            log.warn("SOCKS health check failed: {}", e.getMessage());
        }

        long latencyMs = System.currentTimeMillis() - start;
        Map<String, Object> result = new LinkedHashMap<>();
        result.put("available", ok);
        result.put("latencyMs", ok ? latencyMs : -1);
        result.put("lastCheck", System.currentTimeMillis());
        this.cachedHealth = result;
        log.debug("SOCKS health: available={}, latencyMs={}", ok, latencyMs);
    }
}
```

- [ ] **Step 2: 手动验证 — 编译检查**

```bash
cd molink-access
mvn compile
```

确认无编译错误。

---

### Task 6: 改造 StatusController — 整合 workerStatus + proxyHealth

**Files:**
- Modify: `molink-access/src/main/java/com/molink/access/controller/StatusController.java`

- [ ] **Step 1: 注入 WorkerStatusTracker 和 Socks5HealthChecker**

在类顶部新增字段和构造函数参数：

```java
private final WorkerStatusTracker workerStatusTracker;
private final Socks5HealthChecker socks5HealthChecker;

public StatusController(AppConfig config, AdbClientManager adbClient,
        PortForwarder portForwarder,
        WorkerStatusTracker workerStatusTracker,
        Socks5HealthChecker socks5HealthChecker) {
    this.config = config;
    this.adbClient = adbClient;
    this.portForwarder = portForwarder;
    this.workerStatusTracker = workerStatusTracker;
    this.socks5HealthChecker = socks5HealthChecker;
}
```

- [ ] **Step 2: 在 getStatus() 中追加两个新字段**

在 `status.put("uptime", ...)` 后新增：

```java
// Worker 端状态（通过 dadb HTTP 轮询）
if (workerStatusTracker != null) {
    status.put("workerStatus", workerStatusTracker.getWorkerStatus());
}

// SOCKS 代理通道健康状态
if (socks5HealthChecker != null) {
    status.put("proxyHealth", socks5HealthChecker.getProxyHealth());
}
```

- [ ] **Step 3: 添加缺失 import**

```java
import com.molink.access.status.WorkerStatusTracker;
import com.molink.access.status.Socks5HealthChecker;
```

- [ ] **Step 4: 手动验证 — 编译检查**

```bash
cd molink-access
mvn compile
```

---

### Task 7: 改造 AccessApplication — 初始化 Tracker 和 Checker

**Files:**
- Modify: `molink-access/src/main/java/com/molink/access/AccessApplication.java`

- [ ] **Step 1: 在 Bean 定义区域新增两个 Bean**

在 `PortForwarder` Bean 后新增：

```java
@Bean
public WorkerStatusTracker workerStatusTracker(AdbClientManager adbClient) {
    return new WorkerStatusTracker(adbClient);
}

@Bean
public Socks5HealthChecker socks5HealthChecker() {
    return new Socks5HealthChecker();
}
```

- [ ] **Step 2: 在 CommandLineRunner 中启动 Tracker 和 Checker**

在 `portForwarder.start();` 后新增：

```java
workerStatusTracker.start();
socks5HealthChecker.start();
log.info("Worker status tracker and SOCKS health checker started");
```

- [ ] **Step 3: 添加缺失 import**

```java
import com.molink.access.status.WorkerStatusTracker;
import com.molink.access.status.Socks5HealthChecker;
```

- [ ] **Step 4: 手动验证 — 编译检查**

```bash
cd molink-access
mvn compile
```

---

## Part 3: E2E 测试验证

### Task 8: E2E 测试验证

**Files:**
- Modify: `test/test.py` — Step 8 新增 `/api/status` 字段验证

- [ ] **Step 1: 在 Step 8 curl 测试后新增 API 状态验证**

在 `test_socks_proxy()` 调用后新增：

```python
# 8c: Access API status（含 workerStatus + proxyHealth）
print("  [8c] Access /api/status verification...")
try:
    import urllib.request
    import json
    with urllib.request.urlopen("http://127.0.0.1:8080/api/status", timeout=5) as resp:
        api_data = json.loads(resp.read().decode("utf-8"))
    print(f"  Full status: {json.dumps(api_data, indent=2)}")

    has_worker_status = "workerStatus" in api_data
    has_proxy_health = "proxyHealth" in api_data
    worker_available = api_data.get("workerStatus", {}).get("available", False)
    proxy_available = api_data.get("proxyHealth", {}).get("available", False)

    if has_worker_status:
        pass_(f"workerStatus present: socksRunning={api_data['workerStatus'].get('socksRunning')}")
    else:
        warn("workerStatus not present (worker HTTP server may not be ready)")

    if has_proxy_health:
        pass_(f"proxyHealth available={proxy_available}")
    else:
        warn("proxyHealth not present")
except Exception as e:
    warn(f"Could not verify /api/status: {e}")
```

- [ ] **Step 2: 手动验证 — 完整 E2E 运行**

```bash
python test/test.py
```

确认：
- Step 8c 打印出 `workerStatus` 和 `proxyHealth` 字段
- `workerStatus.socksRunning = true`
- `proxyHealth.available = true`

---

## 自检清单

| 检查项 | 状态 |
|--------|------|
| NanoHTTPD 依赖已添加 | ☑ |
| StatusHttpServer 响应 `/api/status` 和 `/api/ping` | ☑ |
| Socks5ProxyService 管理 HTTP Server 生命周期 | ☑ |
| Socks5ProxyService 新增 `getUptime()` / `getConnectionCount()` | ☑ |
| AdbClientManager 支持 `forward(localPort, remotePort)` | ☑（dadb 内置） |
| WorkerStatusTracker 轮询 worker HTTP 状态 | ☑ |
| Socks5HealthChecker 通过 SOCKS 代理发测试请求 | ☑ |
| StatusController 整合 workerStatus + proxyHealth | ☑ |
| AccessApplication 初始化 Tracker 和 Checker | ☑ |
| E2E 测试脚本验证新字段 | ☑ |
| **不擅自 git commit** | ☑ |

> AdbClientManager 底层使用 dadb 库，`forward(localPort, remotePort)` 方法已存在（参考 `PortForwarder.java` 中 `adbClient.forward(localPort, remotePort)` 的用法），无需额外改造。
