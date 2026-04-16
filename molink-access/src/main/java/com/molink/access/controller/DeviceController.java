package com.molink.access.controller;

import com.molink.access.config.MolinkProperties;
import com.molink.access.health.OkHttpProxyHealthChecker;
import com.molink.access.manager.DeviceManager;
import com.molink.access.record.DeviceRecord;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.web.bind.annotation.*;

import java.util.*;

@RestController
@RequestMapping("/molink")
public class DeviceController {

    private static final Logger log = LoggerFactory.getLogger(DeviceController.class);

    private final DeviceManager deviceManager;
    private final MolinkProperties props;

    public DeviceController(DeviceManager deviceManager, MolinkProperties props) {
        this.deviceManager = deviceManager;
        this.props = props;
    }

    /**
     * GET /molink/devices
     * 返回当前所有已记录的设备。
     */
    @GetMapping("/devices")
    public Map<String, Object> getDevices() {
        List<DeviceRecord> records = deviceManager.getDevices();
        List<Map<String, Object>> deviceList = new ArrayList<>();
        for (DeviceRecord r : records) {
            Map<String, Object> d = new LinkedHashMap<>();
            d.put("deviceId", r.getDeviceId());
            d.put("serial", r.getSerial());
            d.put("state", r.getState().name());
            d.put("forwarderAlive", r.isForwarderAlive());
            d.put("localPort", r.getLocalPort());
            d.put("remotePort", r.getRemotePort());
            if (r.getForwardError() != null) {
                d.put("forwardError", r.getForwardError());
            }
            deviceList.add(d);
        }
        Map<String, Object> result = new LinkedHashMap<>();
        result.put("devices", deviceList);
        log.debug("GET /molink/devices -> {} devices", deviceList.size());
        return result;
    }

    /**
     * POST /molink/devices/{deviceId}/test
     * 对指定设备执行 SOCKS 代理测试。
     * 所有结果统一返回 HTTP 200，通过响应体区分类型。
     */
    @PostMapping("/devices/{deviceId}/test")
    public Map<String, Object> testDevice(@PathVariable String deviceId) {
        Map<String, Object> result = new LinkedHashMap<>();

        DeviceRecord record = deviceManager.getDevice(deviceId);
        if (record == null) {
            result.put("passed", false);
            result.put("error", "Device not found");
            result.put("deviceId", deviceId);
            log.debug("POST /molink/devices/{}/test -> device not found", deviceId);
            return result;
        }

        if (!record.isForwarderAlive()) {
            result.put("passed", false);
            result.put("error", "Forward not established");
            result.put("deviceId", deviceId);
            log.debug("POST /molink/devices/{}/test -> forward not established", deviceId);
            return result;
        }

        OkHttpProxyHealthChecker checker = new OkHttpProxyHealthChecker(
                "127.0.0.1",
                record.getLocalPort(),
                props.getSocksUsername(),
                props.getSocksPassword()
        );
        Map<String, Object> checkResult = checker.check();
        Boolean reachable = (Boolean) checkResult.get("reachable");
        String errMsg = (String) checkResult.get("error");

        result.put("passed", reachable != null && reachable);
        result.put("error", (reachable != null && reachable) ? null : errMsg);
        log.debug("POST /molink/devices/{}/test -> passed={}", deviceId, result.get("passed"));
        return result;
    }

    /**
     * GET /molink/config
     * 返回当前配置。
     */
    @GetMapping("/config")
    public Map<String, Object> getConfig() {
        Map<String, Object> cfg = new LinkedHashMap<>();
        cfg.put("localPort", props.getLocalPort());
        cfg.put("remotePort", props.getRemotePort());
        cfg.put("apiPort", props.getApiPort());
        return cfg;
    }
}
