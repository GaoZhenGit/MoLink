# MoLink Access 重构实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 重构 molink-access，直接使用 dadb TcpForwarder，删除冗余代码，更新 /api/status 接口

**Architecture:** 直接使用 dadb `TcpForwarder` 替代自定义 `AdbForwarder`，删除死代码 `PortForwarder`，新增 `ForwarderRunner` 封装生命周期，`StatusController` 自行通过 SOCKS5 代理发起 HTTPS 请求测试连通性，不再依赖 curl 子进程

**Tech Stack:** Java, Spring Boot 2.7, dadb 1.2.10, picocli

---

## 文件变更总览

| 操作 | 文件 |
|------|------|
| 删除 | `molink-access/src/main/java/com/molink/access/forwarder/AdbForwarder.java` |
| 删除 | `molink-access/src/main/java/com/molink/access/forwarder/PortForwarder.java` |
| 创建 | `molink-access/src/main/java/com/molink/access/forwarder/ForwarderRunner.java` |
| 创建 | `molink-access/src/main/java/com/molink/access/health/ProxyHealthChecker.java` |
| 修改 | `molink-access/src/main/java/com/molink/access/AccessApplication.java` |
| 修改 | `molink-access/src/main/java/com/molink/access/controller/StatusController.java` |
| 修改 | `test/test.py` |

---

## Task 1: 删除 AdbForwarder.java 和 PortForwarder.java

**Files:**
- 删除: `molink-access/src/main/java/com/molink/access/forwarder/AdbForwarder.java`
- 删除: `molink-access/src/main/java/com/molink/access/forwarder/PortForwarder.java`

- [ ] **Step 1: 删除 AdbForwarder.java**

使用 Bash 删除文件（Windows 下有效）:
```bash
rm "D:/project/MoLink/molink-access/src/main/java/com/molink/access/forwarder/AdbForwarder.java"
```

- [ ] **Step 2: 删除 PortForwarder.java**

```bash
rm "D:/project/MoLink/molink-access/src/main/java/com/molink/access/forwarder/PortForwarder.java"
```

- [ ] **Step 3: 验证 forwarder 目录是否为空，如为空则删除目录**

```bash
rmdir "D:/project/MoLink/molink-access/src/main/java/com/molink/access/forwarder" 2>/dev/null || true
```

---

## Task 2: 创建 ForwarderRunner.java

**Files:**
- 创建: `molink-access/src/main/java/com/molink/access/forwarder/ForwarderRunner.java`

**职责:** 封装 dadb `TcpForwarder` 生命周期，持有 `TcpForwarder` 实例供 `StatusController` 查询，并实现 `AutoCloseable` 便于 Spring 管理。

- [ ] **Step 1: 创建 ForwarderRunner.java**

目录不存在时先创建目录，然后写入文件:

```java
package com.molink.access.forwarder;

import com.molink.access.adb.AdbClientManager;
import dadb.Dadb;
import dadb.forwarding.TcpForwarder;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public class ForwarderRunner implements AutoCloseable {

    private static final Logger log = LoggerFactory.getLogger(ForwarderRunner.class);

    private final AdbClientManager adbClient;
    private final int localPort;
    private final int remotePort;
    private TcpForwarder tcpForwarder;

    public ForwarderRunner(AdbClientManager adbClient, int localPort, int remotePort) {
        this.adbClient = adbClient;
        this.localPort = localPort;
        this.remotePort = remotePort;
    }

    /**
     * 启动 TcpForwarder，开始监听本地端口。
     * 在 `start()` 时从 AdbClientManager 获取当前 Dadb 实例，
     * 因此应在设备连接后再调用。
     * 调用方应捕获异常并处理。
     */
    public synchronized void start() {
        if (tcpForwarder != null) {
            log.warn("TcpForwarder already started");
            return;
        }
        Dadb dadb = adbClient.getDadb();
        if (dadb == null) {
            throw new RuntimeException("AdbClientManager has no Dadb connection");
        }
        try {
            tcpForwarder = new TcpForwarder(dadb, localPort, remotePort);
            tcpForwarder.start();
            log.info("TcpForwarder started: localhost:{} -> device:{}", localPort, remotePort);
        } catch (Exception e) {
            tcpForwarder = null;
            throw new RuntimeException("Failed to start TcpForwarder", e);
        }
    }

    /**
     * 停止 TcpForwarder，关闭所有连接并释放资源。
     */
    public synchronized void stop() {
        if (tcpForwarder == null) {
            return;
        }
        try {
            tcpForwarder.close();
            log.info("TcpForwarder stopped");
        } catch (Exception e) {
            log.warn("Error closing TcpForwarder: {}", e.getMessage());
        } finally {
            tcpForwarder = null;
        }
    }

    /**
     * 查询 TcpForwarder 是否存活。
     * @return true if tcpForwarder != null (TcpForwarder 实例存在即视为存活)
     */
    public boolean isAlive() {
        return tcpForwarder != null;
    }

    public int getLocalPort() {
        return localPort;
    }

    public int getRemotePort() {
        return remotePort;
    }

    @Override
    public void close() {
        stop();
    }
}
```

**说明:** 持有 `AdbClientManager` 而非 `Dadb`，在 `start()` 时才获取当前 Dadb 实例，规避了 bean 构造时 Dadb 尚未建立的时序问题。`isAlive()` 用 `tcpForwarder != null` 判断，无需反射。

- [ ] **Step 2: 验证编译**

```bash
cd "D:/project/MoLink/molink-access" && mvn.cmd compile -q
```

预期: BUILD SUCCESS（无输出或仅警告）

---

## Task 3: 创建 ProxyHealthChecker.java

**Files:**
- 创建: `molink-access/src/main/java/com/molink/access/health/ProxyHealthChecker.java`

**职责:** 通过 SOCKS5 代理发起 HTTPS 请求，返回连通性结果。

- [ ] **Step 1: 创建 health 目录并写入 ProxyHealthChecker.java**

```java
package com.molink.access.health;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.SocketAddress;
import java.net.URL;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * 通过 SOCKS5 代理发起 HTTPS GET 请求，检测链路连通性。
 */
public class ProxyHealthChecker {

    private static final Logger log = LoggerFactory.getLogger(ProxyHealthChecker.class);

    private static final String[] TEST_URLS = {
            "https://www.google.com/generate_204",
            "https://www.google.com",
            "https://www.baidu.com"
    };
    private static final int TIMEOUT_MS = 10_000;

    private final int socksPort;
    private final String username;
    private final String password;

    public ProxyHealthChecker(int socksPort, String username, String password) {
        this.socksPort = socksPort;
        this.username = username;
        this.password = password;
    }

    /**
     * 通过 SOCKS5 代理发起请求，返回结果 map。
     * @return 包含 reachable, latencyMs, testUrl, error 字段的 map
     */
    public Map<String, Object> check() {
        Map<String, Object> result = new LinkedHashMap<>();
        result.put("reachable", false);
        result.put("latencyMs", -1L);
        result.put("testUrl", (String) null);
        result.put("error", (String) null);

        SocketAddress proxyAddr = new InetSocketAddress("127.0.0.1", socksPort);
        Proxy proxy = new Proxy(Proxy.Type.SOCKS, proxyAddr);

        for (String testUrl : TEST_URLS) {
            try {
                URL url = new URL(testUrl);
                java.net.HttpURLConnection conn =
                        (java.net.HttpURLConnection) url.openConnection(proxy);
                conn.setConnectTimeout(TIMEOUT_MS);
                conn.setReadTimeout(TIMEOUT_MS);
                conn.setRequestMethod("GET");
                conn.setInstanceFollowRedirects(true);

                long start = System.currentTimeMillis();
                int responseCode = conn.getResponseCode();
                long latency = System.currentTimeMillis() - start;

                if (responseCode == 200 || responseCode == 204 || responseCode == 301 || responseCode == 302) {
                    result.put("reachable", true);
                    result.put("latencyMs", latency);
                    result.put("testUrl", testUrl);
                    result.put("error", null);
                    log.debug("Proxy health OK: {} -> HTTP {}", testUrl, responseCode);
                    return result;
                } else {
                    result.put("error", "HTTP " + responseCode);
                }
            } catch (java.net.SocketTimeoutException e) {
                result.put("error", "Connection timeout");
            } catch (java.net.ConnectException e) {
                result.put("error", "Port unreachable");
            } catch (java.net.ProtocolException e) {
                result.put("error", e.getMessage());
            } catch (Exception e) {
                result.put("error", e.getClass().getSimpleName() + ": " + e.getMessage());
            }
            // 当前 URL 失败，尝试下一个
            break;
        }

        log.debug("Proxy health FAIL: {}", result.get("error"));
        return result;
    }
}
```

**说明:** 使用 Java 标准 `HttpURLConnection` + `Proxy.SOCKS`，无需引入额外依赖。SOCKS5 认证（用户名/密码）由 SOCKS5 协议层自动处理，但 `HttpURLConnection` 不直接支持带认证的 SOCKS5——这里使用无认证方式（access 作为透明隧道，认证在 worker 端处理），所以 `username`/`password` 字段保留但暂不使用。

- [ ] **Step 2: 验证编译**

```bash
cd "D:/project/MoLink/molink-access" && mvn.cmd compile -q
```

预期: BUILD SUCCESS

---

## Task 4: 修改 AccessApplication.java

**Files:**
- 修改: `molink-access/src/main/java/com/molink/access/AccessApplication.java`

**变更:** 删除 `AdbForwarder` bean，添加 `ForwarderRunner` bean，重写 `CommandLineRunner` 启动逻辑。

- [ ] **Step 1: 写入新的 AccessApplication.java**

```java
package com.molink.access;

import com.molink.access.adb.AdbClientManager;
import com.molink.access.config.MolinkProperties;
import com.molink.access.forwarder.ForwarderRunner;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.boot.CommandLineRunner;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

@SpringBootApplication
@Configuration
@EnableConfigurationProperties(MolinkProperties.class)
public class AccessApplication {

    private static final Logger log = LoggerFactory.getLogger(AccessApplication.class);

    public static void main(String[] args) {
        SpringApplication.run(AccessApplication.class, args);
    }

    @Bean
    public AdbClientManager adbClientManager() {
        return new AdbClientManager();
    }

    @Bean
    public ForwarderRunner forwarderRunner(AdbClientManager adbClient, MolinkProperties props) {
        return new ForwarderRunner(adbClient, props.getLocalPort(), props.getRemotePort());
    }

    @Bean
    public CommandLineRunner runner(MolinkProperties props, AdbClientManager adbClient,
            ForwarderRunner forwarderRunner) {
        return args -> {
            log.info("=== MoLink Access Starting ===");
            log.info("Local proxy port: {}", props.getLocalPort());
            log.info("Remote port: {}", props.getRemotePort());
            log.info("API port: {}", props.getApiPort());

            // Set up callbacks first (avoid race where background thread connects before callback is registered)
            adbClient.setOnConnected(dadb -> {
                try {
                    forwarderRunner.start();
                    log.info("ForwarderRunner ready, proxy available -> localhost:{}", props.getLocalPort());
                } catch (Exception e) {
                    log.error("ForwarderRunner start failed: {}", e.getMessage(), e);
                }
            });

            adbClient.setOnDisconnected(() -> {
                forwarderRunner.stop();
                log.warn("Device disconnected, waiting for reconnect...");
            });

            // Start background reconnect thread (continuously monitors for device)
            adbClient.startAutoReconnect();

            // Wait for device connection (blocks until device is connected)
            log.info("Waiting for Android device...");
            adbClient.waitForConnection();

            // Device is now connected; manually trigger onConnected to ensure ForwarderRunner is started
            if (adbClient.isConnected()) {
                try {
                    forwarderRunner.start();
                    log.info("ForwarderRunner ready, proxy available -> localhost:{}", props.getLocalPort());
                } catch (Exception e) {
                    log.error("ForwarderRunner start failed: {}", e.getMessage(), e);
                }
            }

            log.info("Service ready, check status at http://localhost:{}/api/status", props.getApiPort());
        };
    }
}
```

**说明:** `ForwarderRunner` 持有 `AdbClientManager`，在 `start()` 时获取 Dadb，规避了 bean 构造时 Dadb 未建立的时序问题。

- [ ] **Step 2: 验证编译**

```bash
cd "D:/project/MoLink/molink-access" && mvn.cmd compile -q
```

预期: BUILD SUCCESS

---

## Task 5: 修改 StatusController.java

**Files:**
- 修改: `molink-access/src/main/java/com/molink/access/controller/StatusController.java`

**变更:** 移除 curl 子进程逻辑，注入 `ForwarderRunner`，使用 `ProxyHealthChecker` 自测连通性，更新响应格式。

- [ ] **Step 1: 写入新的 StatusController.java**

```java
package com.molink.access.controller;

import com.molink.access.adb.AdbClientManager;
import com.molink.access.config.MolinkProperties;
import com.molink.access.forwarder.ForwarderRunner;
import com.molink.access.health.ProxyHealthChecker;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.web.bind.annotation.*;

import java.util.LinkedHashMap;
import java.util.Map;

@RestController
@RequestMapping("/api")
public class StatusController {

    private static final Logger log = LoggerFactory.getLogger(StatusController.class);

    private final MolinkProperties props;
    private final AdbClientManager adbClient;
    private final ForwarderRunner forwarderRunner;

    public StatusController(MolinkProperties props, AdbClientManager adbClient,
            ForwarderRunner forwarderRunner) {
        this.props = props;
        this.adbClient = adbClient;
        this.forwarderRunner = forwarderRunner;
    }

    @GetMapping("/status")
    public Map<String, Object> getStatus() {
        Map<String, Object> status = new LinkedHashMap<>();
        status.put("forwarderAlive", forwarderRunner.isAlive());
        status.put("connectionState", adbClient.getState().name());
        status.put("deviceSerial", adbClient.getDeviceSerial());
        status.put("proxyHealth", checkProxyHealth());
        status.put("localPort", props.getLocalPort());
        status.put("remotePort", props.getRemotePort());
        status.put("uptime", adbClient.getUptime());
        log.debug("GET /api/status -> forwarderAlive={}, state={}",
                forwarderRunner.isAlive(), adbClient.getState());
        return status;
    }

    @GetMapping("/config")
    public Map<String, Object> getConfig() {
        Map<String, Object> cfg = new LinkedHashMap<>();
        cfg.put("localPort", props.getLocalPort());
        cfg.put("remotePort", props.getRemotePort());
        cfg.put("apiPort", props.getApiPort());
        return cfg;
    }

    private Map<String, Object> checkProxyHealth() {
        ProxyHealthChecker checker = new ProxyHealthChecker(
                props.getLocalPort(),
                props.getSocksUsername(),
                props.getSocksPassword()
        );
        return checker.check();
    }
}
```

- [ ] **Step 2: 验证编译**

```bash
cd "D:/project/MoLink/molink-access" && mvn.cmd compile -q
```

预期: BUILD SUCCESS

---

## Task 6: 更新 test/test.py 的 /api/status 验证逻辑

**Files:**
- 修改: `test/test.py`

**变更:** Step 8e 和 Step 9 中对 `/api/status` 响应字段的断言从 `proxyHealth.available` 改为 `proxyHealth.reachable`，同时支持新旧两种格式（向后兼容）。

- [ ] **Step 1: 找到 Step 8e 的断言逻辑并更新**

当前代码（大约在 853-868 行）:
```python
    ph = api_data.get("proxyHealth", {})
    ph_available = ph.get("available", None)
```

替换为:
```python
    ph = api_data.get("proxyHealth", {})
    # 支持新格式 (reachable) 和旧格式 (available)
    ph_reachable = ph.get("reachable", ph.get("available", None))
```

对应的判断逻辑也改为 `ph_reachable is True`。

- [ ] **Step 2: 找到 Step 9 的断言逻辑并更新**

当前代码（大约在 889-900 行）:
```python
    ph = api_data_after_stop.get("proxyHealth", {})
    ph_available = ph.get("available", None)
    unavailable_reason = ph.get("unavailableReason", "")
```

替换为:
```python
    ph = api_data_after_stop.get("proxyHealth", {})
    # 支持新格式 (reachable) 和旧格式 (available)
    ph_reachable = ph.get("reachable", ph.get("available", None))
    unavailable_reason = ph.get("error", ph.get("unavailableReason", ""))
```

对应的判断逻辑也改为 `ph_reachable is False`。

- [ ] **Step 3: 验证 test.py 语法正确**

```bash
python -m py_compile "D:/project/MoLink/test/test.py"
```

预期: 无输出（无语法错误）

---

## Task 7: 完整编译并打包

- [ ] **Step 1: 完整编译打包**

```bash
cd "D:/project/MoLink/molink-access" && mvn.cmd clean package -DskipTests -q
```

预期: BUILD SUCCESS，生成 `target/molink-access-1.0.0.jar`

---

## Task 8: 运行自动化测试

- [ ] **Step 1: 运行 test/test.py**

```bash
cd "D:/project/MoLink" && python test/test.py
```

预期: 全部 PASS（或允许 WARN 但不能 FAIL）

---

## 验收标准检查

- [ ] `mvn clean package -DskipTests` 通过
- [ ] `AdbForwarder.java` 和 `PortForwarder.java` 已删除
- [ ] `ForwarderRunner` 正确启动 `dadb TcpForwarder`
- [ ] `StatusController` `/api/status` 返回 `forwarderAlive`、`connectionState`、`proxyHealth.reachable` 字段
- [ ] `test/test.py` Step 8e 和 Step 9 断言通过
