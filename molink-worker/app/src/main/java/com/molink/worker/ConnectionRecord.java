package com.molink.worker;

import java.util.concurrent.atomic.AtomicLong;

/**
 * 连接记录数据模型。
 * bytesDown/bytesUp 使用 AtomicLong，支持 forward() 线程安全更新。
 */
public class ConnectionRecord {

    public final String clientIp;
    public final String targetHost;
    public final int targetPort;
    public final long startTime;
    public final AtomicLong bytesDown = new AtomicLong(0);
    public final AtomicLong bytesUp = new AtomicLong(0);
    public volatile boolean active = true;

    public ConnectionRecord(String clientIp, String targetHost, int targetPort, long startTime) {
        this.clientIp = clientIp;
        this.targetHost = targetHost;
        this.targetPort = targetPort;
        this.startTime = startTime;
    }

    public long getBytesDown() { return bytesDown.get(); }
    public long getBytesUp() { return bytesUp.get(); }

    public void addBytesDown(long n) { bytesDown.addAndGet(n); }
    public void addBytesUp(long n) { bytesUp.addAndGet(n); }

    public long getDurationSec() {
        return (System.currentTimeMillis() - startTime) / 1000;
    }

    public String getDisplayHost() {
        return targetHost + ":" + targetPort;
    }
}
