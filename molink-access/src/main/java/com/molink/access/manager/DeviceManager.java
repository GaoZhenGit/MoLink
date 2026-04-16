package com.molink.access.manager;

import com.molink.access.adb.AdbClientManager;
import com.molink.access.health.OkHttpProxyHealthChecker;
import com.molink.access.record.DeviceRecord;
import dadb.Dadb;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.*;

public class DeviceManager {

    private static final Logger log = LoggerFactory.getLogger(DeviceManager.class);
    private static final int POLL_INTERVAL_SECONDS = 3;
    private static final int LOCAL_PORT_BASE = 1080;

    private final AdbClientManager adbClient;
    private final int remotePort;
    private final String socksUsername;
    private final String socksPassword;

    private final Map<String, DeviceRecord> devices = new ConcurrentHashMap<>();
    private final ExecutorService pollExecutor = Executors.newSingleThreadExecutor();
    private final ExecutorService testExecutor = Executors.newCachedThreadPool();
    private volatile boolean running = true;
    private int localPortCounter = 0;
    private static final int TEST_TIMEOUT_SECONDS = 8;

    public DeviceManager(AdbClientManager adbClient, int remotePort,
                         String socksUsername, String socksPassword) {
        this.adbClient = adbClient;
        this.remotePort = remotePort;
        this.socksUsername = socksUsername;
        this.socksPassword = socksPassword;
    }

    /**
     * 启动设备轮询线程，立即触发一次扫描。
     */
    public void start() {
        pollDevicesOnce();
        pollExecutor.submit(this::pollLoop);
        log.info("DeviceManager started, polling every {}s", POLL_INTERVAL_SECONDS);
    }

    /**
     * 停止轮询并关闭所有 forward。
     */
    public void stop() {
        running = false;
        pollExecutor.shutdown();
        for (DeviceRecord record : devices.values()) {
            closeForward(record);
        }
        devices.clear();
        log.info("DeviceManager stopped");
    }

    private void pollLoop() {
        while (running) {
            try {
                Thread.sleep(POLL_INTERVAL_SECONDS * 1000L);
                pollDevicesOnce();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }
    }

    private void pollDevicesOnce() {
        List<Dadb> currentDevices;
        try {
            currentDevices = adbClient.getAllDevices();
        } catch (Exception e) {
            log.warn("pollDevices: failed to get device list: {}", e.getMessage());
            return;
        }

        Set<String> currentSerials = new HashSet<>();
        for (Dadb dadb : currentDevices) {
            String serial = dadb.toString();
            currentSerials.add(serial);

            if (!devices.containsKey(serial)) {
                int localPort = LOCAL_PORT_BASE + localPortCounter++;
                DeviceRecord record = new DeviceRecord(serial, localPort, remotePort);
                devices.put(serial, record);
                log.info("New device detected: {}", serial);
                tryEstablishForward(record, dadb);
            } else {
                DeviceRecord record = devices.get(serial);
                if (!record.isForwarderAlive() && record.getForwardError() != null) {
                    tryEstablishForward(record, dadb);
                }
            }
        }

        // 移除已断开的设备
        Set<String> knownSerials = new HashSet<>(devices.keySet());
        for (String serial : knownSerials) {
            if (!currentSerials.contains(serial)) {
                DeviceRecord record = devices.remove(serial);
                if (record != null) {
                    closeForward(record);
                    log.info("Device removed: {}", serial);
                }
            }
        }
    }

    private void tryEstablishForward(DeviceRecord record, Dadb dadb) {
        try {
            AutoCloseable forwardHandle = dadb.tcpForward(record.getLocalPort(), record.getRemotePort());
            record.setForwardHandle(forwardHandle);
            record.setForwarderAlive(true);
            record.setForwardError(null);
            log.info("Forward established: localhost:{} -> device:{} (serial={})",
                    record.getLocalPort(), record.getRemotePort(), record.getDeviceId());
        } catch (Exception e) {
            record.setForwarderAlive(false);
            record.setForwardError(e.getClass().getSimpleName() + ": " + e.getMessage());
            log.warn("Forward failed for {}: {}", record.getDeviceId(), e.getMessage());
        }
    }

    private void closeForward(DeviceRecord record) {
        if (record.getForwardHandle() != null) {
            try {
                record.getForwardHandle().close();
                log.info("Forward closed for {}", record.getDeviceId());
            } catch (Exception e) {
                log.warn("Error closing forward for {}: {}", record.getDeviceId(), e.getMessage());
            }
            record.setForwardHandle(null);
            record.setForwarderAlive(false);
        }
    }

    // --- Public API ---

    /**
     * 返回当前所有已记录的设备。
     */
    public List<DeviceRecord> getDevices() {
        return new ArrayList<>(devices.values());
    }

    /**
     * 根据 deviceId 查找设备记录。
     */
    public DeviceRecord getDevice(String deviceId) {
        return devices.get(deviceId);
    }

    /**
     * 对指定设备执行 SOCKS 代理测试。
     * @return true = 代理可达，false = 不可达或设备不存在或 forward 未建立
     */
    public boolean testForwarder(String deviceId) {
        DeviceRecord record = devices.get(deviceId);
        if (record == null) {
            return false;
        }
        if (!record.isForwarderAlive()) {
            return false;
        }
        OkHttpProxyHealthChecker checker = new OkHttpProxyHealthChecker(
                "127.0.0.1",
                record.getLocalPort(),
                socksUsername,
                socksPassword
        );
        Future<Boolean> future = testExecutor.submit(() -> {
            Map<String, Object> result = checker.check();
            Boolean reachable = (Boolean) result.get("reachable");
            return reachable != null && reachable;
        });
        try {
            return future.get(TEST_TIMEOUT_SECONDS, TimeUnit.SECONDS);
        } catch (TimeoutException e) {
            future.cancel(true);
            log.warn("testForwarder timed out for device {}", deviceId);
            return false;
        } catch (Exception e) {
            log.warn("testForwarder failed for device {}: {}", deviceId, e.getMessage());
            return false;
        }
    }
}
