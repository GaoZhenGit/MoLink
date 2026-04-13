package com.molink.access.controller;

import com.molink.access.adb.AdbClientManager;
import com.molink.access.config.MolinkProperties;
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

    private final MolinkProperties props;
    private final AdbClientManager adbClient;
    private final Socks5HealthChecker socks5HealthChecker;

    public StatusController(MolinkProperties props, AdbClientManager adbClient,
            Socks5HealthChecker socks5HealthChecker) {
        this.props = props;
        this.adbClient = adbClient;
        this.socks5HealthChecker = socks5HealthChecker;
    }

    @GetMapping("/status")
    public Map<String, Object> getStatus() {
        Map<String, Object> status = new HashMap<>();
        status.put("connected", adbClient.isConnected());
        status.put("deviceSerial", adbClient.getDeviceSerial());
        status.put("localPort", props.getLocalPort());
        status.put("remotePort", props.getRemotePort());
        status.put("reconnectCount", adbClient.getReconnectCount());
        status.put("uptime", adbClient.getUptime());
        status.put("proxyHealth", socks5HealthChecker.getProxyHealth());
        log.debug("GET /api/status -> connected={}", adbClient.isConnected());
        return status;
    }

    /**
     * 同步触发一次 SOCKS 健康检查并返回结果。
     * 用于测试场景：停止 worker 后立即验证状态。
     */
    @PostMapping("/status/check")
    public Map<String, Object> triggerHealthCheck() {
        Map<String, Object> result = socks5HealthChecker.waitForNextCheck();
        log.info("Triggered health check: available={}, reason={}",
                result.get("available"), result.get("unavailableReason"));
        Map<String, Object> response = new HashMap<>();
        response.put("proxyHealth", result);
        return response;
    }

    @GetMapping("/config")
    public Map<String, Object> getConfig() {
        Map<String, Object> cfg = new HashMap<>();
        cfg.put("localPort", props.getLocalPort());
        cfg.put("remotePort", props.getRemotePort());
        cfg.put("apiPort", props.getApiPort());
        return cfg;
    }
}
