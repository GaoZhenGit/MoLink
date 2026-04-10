package com.molink.access.adb;

import dadb.AdbStream;
import dadb.Dadb;
import dadb.forwarding.TcpForwarder;

import java.io.IOException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;

public class AdbClientManager {
    private Dadb dadb;
    private String deviceSerial;
    private ExecutorService executor;
    private volatile boolean connected = false;
    private volatile boolean running = true;

    private final AtomicInteger reconnectCount = new AtomicInteger(0);
    private volatile long startTime = 0;

    public AdbClientManager() {
        this.executor = Executors.newSingleThreadExecutor();
    }

    public boolean connect() {
        try {
            // 使用 Dadb.discover() 自动发现已连接的设备
            dadb = Dadb.discover();

            if (dadb != null) {
                connected = true;
                startTime = System.currentTimeMillis();
                System.out.println("ADB 连接成功");
                return true;
            } else {
                System.out.println("未发现已连接的 ADB 设备");
                return false;
            }
        } catch (Exception e) {
            System.err.println("ADB 连接失败: " + e.getMessage());
            return false;
        }
    }

    public void startAutoReconnect() {
        executor.submit(() -> {
            while (running) {
                if (!connected || !isDeviceConnected()) {
                    reconnectCount.incrementAndGet();
                    System.out.println("正在尝试重连... (第 " + reconnectCount.get() + " 次)");
                    connect();
                }
                try {
                    Thread.sleep(3000);
                } catch (InterruptedException e) {
                    break;
                }
            }
        });
    }

    public boolean isDeviceConnected() {
        if (dadb == null) return false;
        try {
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    public Dadb getDadb() {
        return dadb;
    }

    /**
     * 打开到设备指定地址的 ADB 流
     * @param address 目标地址，如 "tcp:localhost:1080"
     * @return ADB 流
     */
    public AdbStream openStream(String address) throws IOException {
        if (dadb == null) {
            throw new IOException("ADB 连接未建立");
        }
        return dadb.open(address);
    }

    /**
     * 建立 TCP 端口转发
     * @param localPort 本地端口
     * @param remotePort 远端端口（Android 设备上的端口）
     * @return AutoCloseable，关闭时取消转发
     */
    public AutoCloseable forward(int localPort, int remotePort) throws IOException, InterruptedException {
        if (dadb == null) {
            throw new IOException("ADB 连接未建立");
        }
        // 创建 TcpForwarder 并启动
        TcpForwarder forwarder = new TcpForwarder(dadb, localPort, remotePort);
        forwarder.start();
        return forwarder;
    }

    public void disconnect() {
        running = false;
        connected = false;
        if (dadb != null) {
            try {
                dadb.close();
            } catch (Exception e) {
                // 忽略
            }
        }
        executor.shutdown();
    }

    public boolean isConnected() {
        return connected;
    }

    public String getDeviceSerial() {
        return deviceSerial;
    }

    public int getReconnectCount() {
        return reconnectCount.get();
    }

    public long getUptime() {
        if (startTime == 0) return 0;
        return (System.currentTimeMillis() - startTime) / 1000;
    }
}
