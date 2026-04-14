package com.molink.access.controller;

import com.molink.access.adb.AdbClientManager;
import com.molink.access.config.MolinkProperties;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.web.bind.annotation.*;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.*;

@RestController
@RequestMapping("/api")
public class StatusController {

    private static final Logger log = LoggerFactory.getLogger(StatusController.class);

    /** Test URLs, tried in order of priority */
    private static final List<String> TEST_URLS = Arrays.asList(
            "http://httpbin.org/ip",
            "http://myip.ipip.net",
            "http://ip.sb"
    );

    private final MolinkProperties props;
    private final AdbClientManager adbClient;

    public StatusController(MolinkProperties props, AdbClientManager adbClient) {
        this.props = props;
        this.adbClient = adbClient;
    }

    @GetMapping("/status")
    public Map<String, Object> getStatus() {
        Map<String, Object> status = new LinkedHashMap<>();
        status.put("connected", adbClient.isConnected());
        status.put("deviceSerial", adbClient.getDeviceSerial());
        status.put("localPort", props.getLocalPort());
        status.put("remotePort", props.getRemotePort());
        status.put("reconnectCount", adbClient.getReconnectCount());
        status.put("uptime", adbClient.getUptime());
        status.put("proxyHealth", testProxy());
        log.debug("GET /api/status -> connected={}", adbClient.isConnected());
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
     * Test proxy connectivity via system curl with SOCKS5h, return health status.
     * Tries multiple URLs; one success means proxy is available.
     */
    private Map<String, Object> testProxy() {
        int socksPort = props.getLocalPort();
        String socksUser = props.getSocksUsername();
        String socksPass = props.getSocksPassword();
        String proxyHost = "127.0.0.1:" + socksPort;
        if (!socksUser.isEmpty() && !socksPass.isEmpty()) {
            proxyHost = socksUser + ":" + socksPass + "@" + proxyHost;
        }
        String reason = "";
        long latencyMs = -1;

        for (String testUrl : TEST_URLS) {
            try {
                List<String> cmd = Arrays.asList(
                        "curl", "-s", "-o", "NUL",
                        "-w", "%{http_code}|%{time_total}",
                        "--connect-timeout", "5",
                        "--max-time", "10",
                        "-x", "socks5h://" + proxyHost,
                        testUrl
                );
                ProcessBuilder pb = new ProcessBuilder(cmd);
                pb.redirectErrorStream(true);
                Process process = pb.start();

                StringBuilder output = new StringBuilder();
                try (BufferedReader r = new BufferedReader(new InputStreamReader(process.getInputStream()))) {
                    String line;
                    while ((line = r.readLine()) != null) {
                        output.append(line);
                    }
                }

                int exitCode = process.waitFor();
                String result = output.toString().trim();

                if (exitCode == 0 && result.contains("|")) {
                    String[] parts = result.split("\\|", 2);
                    int httpCode = Integer.parseInt(parts[0]);
                    latencyMs = (long) (Double.parseDouble(parts[1]) * 1000);
                    if (httpCode == 200 || httpCode == 301 || httpCode == 302) {
                        log.debug("Proxy test OK via {}: HTTP {}", testUrl, httpCode);
                        Map<String, Object> health = new LinkedHashMap<>();
                        health.put("available", true);
                        health.put("latencyMs", latencyMs);
                        health.put("lastCheck", System.currentTimeMillis());
                        health.put("unavailableReason", "");
                        return health;
                    } else {
                        reason = "HTTP " + httpCode;
                    }
                } else if (exitCode == 22) {
                    // curl exit 22 means server returned error (4xx/5xx) in --fail mode
                    if (result.matches("\\d{3}.*")) {
                        reason = "HTTP " + result.split("\\|")[0];
                    } else {
                        reason = "Connection failed";
                    }
                } else if (exitCode == 28) {
                    reason = "Connection timeout";
                } else if (exitCode == 7 || exitCode == 56) {
                    reason = "Port unreachable";
                } else if (exitCode != 0) {
                    reason = "curl exit " + exitCode;
                }
            } catch (NumberFormatException e) {
                reason = "Response parse failed";
            } catch (Exception e) {
                reason = e.getClass().getSimpleName();
            }
            // Current URL failed, try next one
            break;
        }

        log.debug("Proxy test FAIL: {}", reason);
        Map<String, Object> health = new LinkedHashMap<>();
        health.put("available", false);
        health.put("latencyMs", -1L);
        health.put("lastCheck", System.currentTimeMillis());
        health.put("unavailableReason", reason);
        return health;
    }
}
