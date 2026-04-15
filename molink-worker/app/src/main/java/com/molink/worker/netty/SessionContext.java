package com.molink.worker.netty;

import com.molink.worker.ConnectionRecord;
import io.netty.channel.Channel;

import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * 单个 SOCKS5 代理会话的上下文。
 * 存储 clientChannel / targetChannel / record / 双向结束计数 / 销毁标志。
 * 线程安全。
 */
public final class SessionContext {

    public final Channel clientChannel;
    public volatile Channel targetChannel;         // 异步建立，完成后赋值
    public final ConnectionRecord record;
    /** 已结束的方向数（0/1/2），两个方向都结束才触发销毁 */
    public final AtomicInteger endedCount = new AtomicInteger(0);
    /** 防止重复销毁 */
    public final AtomicBoolean destroyed = new AtomicBoolean(false);
    public final long createdAt = System.currentTimeMillis();

    public SessionContext(Channel clientChannel, ConnectionRecord record) {
        this.clientChannel = clientChannel;
        this.record = record;
    }

    /**
     * 标记本方向已结束。
     * @return true 如果双方都已结束（需要触发销毁）
     */
    public boolean markEnded() {
        return endedCount.incrementAndGet() >= 2;
    }

    public int getEndedCount() {
        return endedCount.get();
    }

    public boolean isDestroyed() {
        return destroyed.get();
    }

    public void markDestroyed() {
        destroyed.set(true);
    }
}
