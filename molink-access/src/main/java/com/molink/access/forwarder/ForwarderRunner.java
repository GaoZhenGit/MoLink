package com.molink.access.forwarder;

import com.molink.access.adb.AdbClientManager;
import dadb.Dadb;
import dadb.forwarding.TcpForwarder;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public class ForwarderRunner implements AutoCloseable {

    private static final Logger log = LoggerFactory.getLogger(ForwarderRunner.class);

    private final AdbClientManager adbClient;
    private final int localPort;
    private final int remotePort;
    private TcpForwarder tcpForwarder;

    public ForwarderRunner(AdbClientManager adbClient, int localPort, int remotePort) {
        this.adbClient = adbClient;
        this.localPort = localPort;
        this.remotePort = remotePort;
    }

    /**
     * 启动 TcpForwarder，开始监听本地端口。
     * 在 `start()` 时从 AdbClientManager 获取当前 Dadb 实例，
     * 因此应在设备连接后再调用。
     * 调用方应捕获异常并处理。
     */
    public synchronized void start() {
        if (tcpForwarder != null) {
            log.warn("TcpForwarder already started");
            return;
        }
        Dadb dadb = adbClient.getDadb();
        if (dadb == null) {
            throw new RuntimeException("AdbClientManager has no Dadb connection");
        }
        try {
            tcpForwarder = new TcpForwarder(dadb, localPort, remotePort);
            tcpForwarder.start();
            log.info("TcpForwarder started: localhost:{} -> device:{}", localPort, remotePort);
        } catch (Exception e) {
            tcpForwarder = null;
            throw new RuntimeException("Failed to start TcpForwarder", e);
        }
    }

    /**
     * 停止 TcpForwarder，关闭所有连接并释放资源。
     */
    public synchronized void stop() {
        if (tcpForwarder == null) {
            return;
        }
        try {
            tcpForwarder.close();
            log.info("TcpForwarder stopped");
        } catch (Exception e) {
            log.warn("Error closing TcpForwarder: {}", e.getMessage());
        } finally {
            tcpForwarder = null;
        }
    }

    /**
     * 查询 TcpForwarder 是否存活。
     * @return true if tcpForwarder != null (TcpForwarder 实例存在即视为存活)
     */
    public boolean isAlive() {
        return tcpForwarder != null;
    }

    public int getLocalPort() {
        return localPort;
    }

    public int getRemotePort() {
        return remotePort;
    }

    @Override
    public void close() {
        stop();
    }
}
