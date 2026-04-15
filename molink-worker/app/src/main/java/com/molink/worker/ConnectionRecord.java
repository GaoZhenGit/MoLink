package com.molink.worker;

import java.util.concurrent.atomic.AtomicLong;

/**
 * 连接记录数据模型。
 * bytesDown/bytesUp 使用 AtomicLong，支持 forward() 线程安全更新。
 */
public class ConnectionRecord {

    /**
     * 连接状态（v3 Req 4）：
     * - ACTIVE：连接进行中 → UI 显示绿色
     * - ENDED：连接已结束（无论正常/异常）→ UI 显示灰色
     */
    public enum ConnectionState {
        ACTIVE,
        ENDED
    }

    public final String clientIp;
    public final String targetHost;
    public final int targetPort;
    public final long startTime;
    public final AtomicLong bytesDown = new AtomicLong(0);
    public final AtomicLong bytesUp = new AtomicLong(0);
    public volatile ConnectionState state = ConnectionState.ACTIVE;  // 原 active + failed

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

    /** 标记连接已结束（由 forward() 在任一方向结束时调用） */
    public synchronized void setEnded() {
        if (this.state == ConnectionState.ACTIVE) {
            this.state = ConnectionState.ENDED;
        }
    }

    /** 两个方向都结束时调用，同步更新 record 状态（v3 Req 4） */
    public void checkEnded() {
        setEnded();
    }

    public boolean isEnded() {
        return state == ConnectionState.ENDED;
    }

    public long getDurationSec() {
        return (System.currentTimeMillis() - startTime) / 1000;
    }

    public String getDisplayHost() {
        return targetHost + ":" + targetPort;
    }
}
