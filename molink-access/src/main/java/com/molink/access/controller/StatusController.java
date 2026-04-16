package com.molink.access.controller;

import com.molink.access.adb.AdbClientManager;
import com.molink.access.config.MolinkProperties;
import com.molink.access.forwarder.ForwarderRunner;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.web.bind.annotation.*;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

@RestController
@RequestMapping("/api")
public class StatusController {

    private static final Logger log = LoggerFactory.getLogger(StatusController.class);

    private static final List<String> TEST_URLS = Arrays.asList(
            "https://www.douyin.com",
            "https://www.baidu.com"
    );

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

    /**
     * 通过 curl 子进程 + SOCKS5h 代理测试链路连通性。
     * curl 原生支持 SOCKS5 用户名/密码认证，胜过自行实现。
     */
    private Map<String, Object> checkProxyHealth() {
        Map<String, Object> result = new LinkedHashMap<>();
        result.put("reachable", false);
        result.put("latencyMs", -1L);
        result.put("testUrl", (String) null);
        result.put("error", (String) null);

        // 构建 SOCKS5h 代理 URL
        String proxyHost = "127.0.0.1:" + props.getLocalPort();
        String socksUser = props.getSocksUsername();
        String socksPass = props.getSocksPassword();
        if (socksUser != null && !socksUser.isEmpty()
                && socksPass != null && !socksPass.isEmpty()) {
            proxyHost = socksUser + ":" + socksPass + "@" + proxyHost;
        }
        String proxyUrl = "socks5h://" + proxyHost;

        // Windows 下用 curl.exe 避免被 PowerShell alias 解析为 Invoke-WebRequest
        String curlCmd = "curl.exe";
        // 输出设备: Windows 用 NUL，其他用 /dev/null
        String nullDevice = System.getProperty("os.name").toLowerCase().contains("windows") ? "NUL" : "/dev/null";

        for (String testUrl : TEST_URLS) {
            try {
                // curl: 获取 HTTP 状态码和耗时
                List<String> cmd = Arrays.asList(
                        curlCmd, "-s", "-o", nullDevice,
                        "-w", "%{http_code}|%{time_total}",
                        "--connect-timeout", "5",
                        "--max-time", "10",
                        "-x", proxyUrl,
                        testUrl
                );
                log.debug("Proxy health check cmd: {}", String.join(" ", cmd));

                ProcessBuilder pb = new ProcessBuilder(cmd);
                pb.redirectErrorStream(true);
                Process process = pb.start();

                StringBuilder output = new StringBuilder();
                // 加上进程超时保护，防止 waitFor() 永久阻塞
                boolean finished = process.waitFor(15, java.util.concurrent.TimeUnit.SECONDS);
                if (!finished) {
                    process.destroyForcibly();
                    log.warn("curl process timed out, destroyed");
                    result.put("error", "curl timeout");
                    continue;
                }

                try (BufferedReader r = new BufferedReader(new InputStreamReader(process.getInputStream()))) {
                    String line;
                    while ((line = r.readLine()) != null) {
                        output.append(line);
                    }
                }

                int exitCode = process.exitValue();
                String raw = output.toString().trim();
                log.debug("curl exit={}, output={}", exitCode, raw);

                if (exitCode == 0 && raw.contains("|")) {
                    String[] parts = raw.split("\\|", 2);
                    int httpCode = Integer.parseInt(parts[0]);
                    long latencyMs = (long) (Double.parseDouble(parts[1]) * 1000);
                    if (httpCode == 200 || httpCode == 204 || httpCode == 301 || httpCode == 302) {
                        result.put("reachable", true);
                        result.put("latencyMs", latencyMs);
                        result.put("testUrl", testUrl);
                        result.put("error", null);
                        log.debug("Proxy health OK: {} -> HTTP {}", testUrl, httpCode);
                        return result;
                    } else {
                        result.put("error", "HTTP " + httpCode);
                    }
                } else if (exitCode == 22) {
                    // curl exit 22 = server returned 4xx/5xx
                    if (raw.matches("\\d{3}.*")) {
                        result.put("error", "HTTP " + raw.split("\\|")[0]);
                    } else {
                        result.put("error", "Connection failed");
                    }
                } else if (exitCode == 28) {
                    result.put("error", "Connection timeout");
                } else if (exitCode == 7 || exitCode == 56) {
                    result.put("error", "Port unreachable");
                } else if (exitCode != 0) {
                    result.put("error", "curl exit " + exitCode);
                }
            } catch (Exception e) {
                result.put("error", e.getClass().getSimpleName());
            }
            // 当前 URL 失败，尝试下一个
            break;
        }

        log.debug("Proxy health FAIL: {}", result.get("error"));
        return result;
    }
}
