package com.molink.access.forwarder;

import com.molink.access.adb.AdbClientManager;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

public class PortForwarder {

    private static final Logger log = LoggerFactory.getLogger(PortForwarder.class);

    private final AdbClientManager adbClient;
    private final int localPort;
    private final int remotePort;
    private volatile boolean running = true;
    private AutoCloseable currentForward;
    private final AtomicBoolean started = new AtomicBoolean(false);
    private final AtomicInteger connectionCount = new AtomicInteger(0);

    public PortForwarder(AdbClientManager adbClient, int localPort, int remotePort) {
        this.adbClient = adbClient;
        this.localPort = localPort;
        this.remotePort = remotePort;
    }

    public void start() throws IOException, InterruptedException {
        // Stop existing forward if any
        if (started.get()) {
            stop();
        }
        // Establish port forwarding
        // TcpForwarder listens on localPort and relays connections to the device via ADB tunnel
        currentForward = adbClient.forward(localPort, remotePort);
        started.set(true);
        log.info("Port forward started: localhost:{} -> Android:{}", localPort, remotePort);
    }

    public void stop() {
        if (!started.getAndSet(false)) {
            return;
        }
        running = false;
        if (currentForward != null) {
            try {
                currentForward.close();
                log.info("Port forward stopped");
            } catch (Exception e) {
                log.warn("Error stopping port forward: {}", e.getMessage());
            }
            currentForward = null;
        }
    }

    public int getConnectionCount() {
        return connectionCount.get();
    }
}
