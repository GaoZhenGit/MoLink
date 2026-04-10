package com.molink.access.forwarder;

import com.molink.access.adb.AdbClientManager;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.util.concurrent.atomic.AtomicInteger;

public class PortForwarder {

    private static final Logger log = LoggerFactory.getLogger(PortForwarder.class);

    private final AdbClientManager adbClient;
    private final int localPort;
    private final int remotePort;
    private volatile boolean running = true;
    private AutoCloseable currentForward;
    private final AtomicInteger connectionCount = new AtomicInteger(0);

    public PortForwarder(AdbClientManager adbClient, int localPort, int remotePort) {
        this.adbClient = adbClient;
        this.localPort = localPort;
        this.remotePort = remotePort;
    }

    public void start() throws IOException, InterruptedException {
        // 建立端口转发
        // TcpForwarder 会在本地监听 localPort，接受连接后通过 ADB 隧道转发到设备
        currentForward = adbClient.forward(localPort, remotePort);
        log.info("端口转发已启动: localhost:{} -> Android:{}", localPort, remotePort);
    }

    public void stop() {
        running = false;
        if (currentForward != null) {
            try {
                currentForward.close();
                log.info("端口转发已停止");
            } catch (Exception e) {
                log.warn("停止端口转发时出错: {}", e.getMessage());
            }
        }
    }

    public int getConnectionCount() {
        return connectionCount.get();
    }
}
