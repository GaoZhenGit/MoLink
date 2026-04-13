package com.molink.access.controller;

import com.molink.access.adb.AdbClientManager;
import com.molink.access.config.AppConfig;
import com.molink.access.forwarder.PortForwarder;
import com.molink.access.status.WorkerStatusTracker;
import com.molink.access.status.Socks5HealthChecker;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.web.bind.annotation.*;

import java.util.HashMap;
import java.util.Map;

@RestController
@RequestMapping("/api")
public class StatusController {

    private static final Logger log = LoggerFactory.getLogger(StatusController.class);

    private final AppConfig config;
    private final AdbClientManager adbClient;
    private final PortForwarder portForwarder;
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

    @GetMapping("/status")
    public Map<String, Object> getStatus() {
        Map<String, Object> status = new HashMap<>();
        status.put("connected", adbClient.isConnected());
        status.put("deviceSerial", adbClient.getDeviceSerial());
        status.put("localPort", config.getLocalPort());
        status.put("remotePort", config.getRemotePort());
        status.put("reconnectCount", adbClient.getReconnectCount());
        status.put("uptime", adbClient.getUptime());
        // Worker 端状态（通过 dadb HTTP 轮询）
        if (workerStatusTracker != null) {
            status.put("workerStatus", workerStatusTracker.getWorkerStatus());
        }
        // SOCKS 代理通道健康状态
        if (socks5HealthChecker != null) {
            status.put("proxyHealth", socks5HealthChecker.getProxyHealth());
        }
        log.debug("GET /api/status -> connected={}", adbClient.isConnected());
        return status;
    }

    @GetMapping("/config")
    public Map<String, Object> getConfig() {
        Map<String, Object> cfg = new HashMap<>();
        cfg.put("localPort", config.getLocalPort());
        cfg.put("remotePort", config.getRemotePort());
        cfg.put("apiPort", config.getApiPort());
        return cfg;
    }

    @PutMapping("/config")
    public Map<String, Object> updateConfig(@RequestBody Map<String, Integer> updates) {
        if (updates.containsKey("apiPort")) {
            config.setApiPort(updates.get("apiPort"));
        }
        return getConfig();
    }
}
