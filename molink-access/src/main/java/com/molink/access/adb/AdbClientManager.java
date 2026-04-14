package com.molink.access.adb;

import dadb.AdbStream;
import dadb.Dadb;
import dadb.forwarding.TcpForwarder;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;

public class AdbClientManager {

    private static final Logger log = LoggerFactory.getLogger(AdbClientManager.class);

    public enum ConnectionState { WAITING, CONNECTED, DISCONNECTED }

    private Dadb dadb;
    private String deviceSerial;
    private ExecutorService executor;
    private volatile boolean connected = false;
    private volatile boolean running = true;

    private final CountDownLatch connectedLatch = new CountDownLatch(1);
    private volatile ConnectionState state = ConnectionState.DISCONNECTED;

    private Consumer<Dadb> onConnectedCallback;
    private Runnable onDisconnectedCallback;

    private final AtomicInteger reconnectCount = new AtomicInteger(0);
    private volatile long startTime = 0;

    public AdbClientManager() {
        this.executor = Executors.newSingleThreadExecutor();
    }

    public boolean connect() {
        try {
            // Use Dadb.discover() to auto-detect connected devices
            dadb = Dadb.discover();

            if (dadb != null) {
                connected = true;
                state = ConnectionState.CONNECTED;
                connectedLatch.countDown();
                startTime = System.currentTimeMillis();
                // Get device serial via Dadb.list()
                try {
                    java.util.List<dadb.Dadb> devices = Dadb.list();
                    for (dadb.Dadb d : devices) {
                        // Find the currently connected device
                        this.deviceSerial = d.toString();
                        break;
                    }
                } catch (Exception e) {
                    log.debug("Failed to get device serial: {}", e.getMessage());
                }
                log.info("ADB connected, device: {}", this.deviceSerial);
                if (onConnectedCallback != null) {
                    onConnectedCallback.accept(dadb);
                }
                return true;
            } else {
                log.warn("No ADB device found, entering wait mode...");
                connected = false;
                state = ConnectionState.WAITING;
                return false;
            }
        } catch (Exception e) {
            log.error("ADB connection failed: {}", e.getMessage(), e);
            connected = false;
            state = ConnectionState.WAITING;
            return false;
        }
    }

    public void startAutoReconnect() {
        executor.submit(() -> {
            while (running) {
                if (!connected || !isDeviceConnected()) {
                    if (connected && !isDeviceConnected()) {
                        // Device unexpectedly disconnected
                        connected = false;
                        state = ConnectionState.DISCONNECTED;
                        log.warn("ADB device disconnected");
                        if (onDisconnectedCallback != null) {
                            onDisconnectedCallback.run();
                        }
                    }
                    int count = reconnectCount.incrementAndGet();
                    log.info("Attempting to connect device... (attempt #{})", count);
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

    public void waitForConnection() {
        if (connected) return;
        try {
            connectedLatch.await();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    public void setOnConnected(Consumer<Dadb> callback) {
        this.onConnectedCallback = callback;
    }

    public void setOnDisconnected(Runnable callback) {
        this.onDisconnectedCallback = callback;
    }

    public boolean isDeviceConnected() {
        if (dadb == null) return false;
        try {
            return true;
        } catch (Exception e) {
            log.warn("ADB device connection check failed: {}", e.getMessage());
            return false;
        }
    }

    public Dadb getDadb() {
        return dadb;
    }

    /**
     * Open an ADB stream to the device at the given address
     * @param address target address, e.g. "tcp:localhost:1080"
     * @return ADB stream
     */
    public AdbStream openStream(String address) throws IOException {
        if (dadb == null) {
            throw new IOException("ADB not connected");
        }
        return dadb.open(address);
    }

    /**
     * Establish TCP port forwarding
     * @param localPort local port
     * @param remotePort remote port (port on Android device)
     * @return AutoCloseable, close to cancel the forward
     */
    public AutoCloseable forward(int localPort, int remotePort) throws IOException, InterruptedException {
        if (dadb == null) {
            throw new IOException("ADB not connected");
        }
        // Create and start TcpForwarder
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
                log.warn("Error closing ADB connection: {}", e.getMessage());
            }
        }
        executor.shutdown();
    }

    public boolean isConnected() {
        return connected;
    }

    public ConnectionState getState() {
        return state;
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
