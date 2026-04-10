package com.molink.access.forwarder;

import com.molink.access.adb.AdbClientManager;

import java.io.IOException;
import java.util.concurrent.atomic.AtomicInteger;

public class PortForwarder {
    private final AdbClientManager adbClient;
    private final int localPort;
    private final int remotePort;
    private volatile boolean running = true;
    private AutoCloseable currentForward;

    public PortForwarder(AdbClientManager adbClient, int localPort, int remotePort) {
        this.adbClient = adbClient;
        this.localPort = localPort;
        this.remotePort = remotePort;
    }

    public void start() throws IOException, InterruptedException {
        // 建立端口转发
        // TcpForwarder 会在本地监听 localPort，接受连接后通过 ADB 隧道转发到设备
        currentForward = adbClient.forward(localPort, remotePort);
        System.out.println("端口转发已启动: localhost:" + localPort + " -> Android:" + remotePort);
    }

    public void stop() {
        running = false;
        if (currentForward != null) {
            try {
                currentForward.close();
            } catch (Exception e) {
                // 忽略
            }
        }
    }
}
