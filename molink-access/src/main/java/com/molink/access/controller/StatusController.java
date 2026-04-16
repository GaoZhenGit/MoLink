package com.molink.access.controller;

import com.molink.access.adb.AdbClientManager;
import com.molink.access.config.MolinkProperties;
import com.molink.access.forwarder.ForwarderRunner;
import com.molink.access.health.OkHttpProxyHealthChecker;
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

    /**
     * 通过 OkHttp + SOCKS5h 代理测试链路连通性。
     * 使用 {@link com.molink.access.health.OkHttpProxyHealthChecker} 替代 curl 子进程，
     * 通过 {@link java.net.InetSocketAddress#createUnresolved(String, int)} 实现 SOCKS5h 效果：
     * DNS 解析在代理端完成，而非本地。
     */
    private Map<String, Object> checkProxyHealth() {
        OkHttpProxyHealthChecker checker = new OkHttpProxyHealthChecker(
                "127.0.0.1",
                props.getLocalPort(),
                props.getSocksUsername(),
                props.getSocksPassword()
        );
        return checker.check();
    }
}
