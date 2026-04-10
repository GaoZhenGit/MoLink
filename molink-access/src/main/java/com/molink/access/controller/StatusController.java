package com.molink.access.controller;

import com.molink.access.adb.AdbClientManager;
import com.molink.access.config.AppConfig;
import com.molink.access.forwarder.PortForwarder;
import org.springframework.web.bind.annotation.*;

import java.util.HashMap;
import java.util.Map;

@RestController
@RequestMapping("/api")
public class StatusController {

    private final AppConfig config;
    private final AdbClientManager adbClient;
    private final PortForwarder portForwarder;

    public StatusController(AppConfig config, AdbClientManager adbClient, PortForwarder portForwarder) {
        this.config = config;
        this.adbClient = adbClient;
        this.portForwarder = portForwarder;
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
