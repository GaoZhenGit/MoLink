package com.molink.access.record;

import com.molink.access.adb.AdbClientManager.ConnectionState;

public class DeviceRecord {

    private final String deviceId;
    private final String serial;
    private ConnectionState state;
    private boolean forwarderAlive;
    private final int localPort;
    private final int remotePort;
    private long connectTime;
    private String forwardError;
    private AutoCloseable forwardHandle;

    public DeviceRecord(String deviceId, int localPort, int remotePort) {
        this.deviceId = deviceId;
        this.serial = deviceId;
        this.localPort = localPort;
        this.remotePort = remotePort;
        this.state = ConnectionState.CONNECTED;
        this.forwarderAlive = false;
        this.connectTime = System.currentTimeMillis();
    }

    // --- Getters ---
    public String getDeviceId() { return deviceId; }
    public String getSerial() { return serial; }
    public ConnectionState getState() { return state; }
    public boolean isForwarderAlive() { return forwarderAlive; }
    public int getLocalPort() { return localPort; }
    public int getRemotePort() { return remotePort; }
    public long getConnectTime() { return connectTime; }
    public String getForwardError() { return forwardError; }
    public AutoCloseable getForwardHandle() { return forwardHandle; }

    // --- Setters ---
    public void setState(ConnectionState state) { this.state = state; }
    public void setForwarderAlive(boolean alive) { this.forwarderAlive = alive; }
    public void setForwardError(String error) { this.forwardError = error; }
    public void setForwardHandle(AutoCloseable handle) { this.forwardHandle = handle; }
}
