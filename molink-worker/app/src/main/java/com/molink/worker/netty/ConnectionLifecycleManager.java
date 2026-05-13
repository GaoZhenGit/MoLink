package com.molink.worker.netty;

import android.util.Log;
import com.molink.worker.ConnectionRecord;
import io.netty.channel.Channel;
import io.netty.channel.ChannelId;

import java.util.concurrent.ConcurrentHashMap;

/**
 * 全局会话生命周期管理器。
 * 管理所有活跃 SessionContext，提供双向关闭、标记结束等操作。
 * 线程安全。
 */
public final class ConnectionLifecycleManager {

    private static final String TAG = "ConnectionLifecycleManager";
    private static final ConnectionLifecycleManager INSTANCE = new ConnectionLifecycleManager();

    private final ConcurrentHashMap<ChannelId, SessionContext> contexts = new ConcurrentHashMap<>();

    private ConnectionLifecycleManager() {}

    public static ConnectionLifecycleManager getInstance() {
        return INSTANCE;
    }

    /**
     * 注册新会话。
     * @param clientChannel 客户端 channel
     * @param ctx 会话上下文
     */
    public void register(Channel clientChannel, SessionContext ctx) {
        contexts.put(clientChannel.id(), ctx);
    }

    /**
     * 根据 channel 查找会话上下文。
     */
    public SessionContext findByChannel(Channel channel) {
        return contexts.get(channel.id());
    }

    /**
     * 销毁会话：双向关闭两个 Channel，标记 record 结束，移除注册。
     * 可安全重复调用。
     *
     * 注意：必须同时关闭两个 channel，否则对端 read() 不会退出。
     */
    public void destroy(SessionContext sessionCtx) {
        if (sessionCtx == null || sessionCtx.isDestroyed()) {
            return;
        }
        sessionCtx.markDestroyed();

        // 计算并记录连接用时（精确到秒）
        ConnectionRecord r = sessionCtx.record;
        long secs = r.getDurationSec();
        long m = secs / 60;
        long s = secs % 60;
        String durationStr = m > 0 ? m + "m" + s + "s" : s + "s";
        Log.i(TAG, r.protocol + " " + r.targetHost + ":" + r.targetPort
                + " ended, duration=" + durationStr
                + " ↓" + formatBytes(r.getBytesUp()) + " ↑" + formatBytes(r.getBytesDown()));

        Channel client = sessionCtx.clientChannel;
        Channel target = sessionCtx.targetChannel;

        // 关闭客户端 channel（会向对端发送 FIN）
        if (client != null && client.isOpen()) {
            client.close();
        }
        // 关闭目标 channel（会向对端发送 FIN）
        if (target != null && target.isOpen()) {
            target.close();
        }

        // 标记 record 为已结束
        sessionCtx.record.setEnded();

        // 移除注册
        if (client != null) {
            contexts.remove(client.id());
        }
    }

    private static String formatBytes(long bytes) {
        if (bytes < 0) return "--";
        if (bytes < 1024) return bytes + "B";
        if (bytes < 1024 * 1024) return String.format("%.1fKB", bytes / 1024.0);
        return String.format("%.1fMB", bytes / (1024.0 * 1024));
    }

    /**
     * 任一方向结束时调用，立即关闭对端，触发双向同步关闭。
     * 幂等：已销毁则直接返回。
     *
     * 注意：不再等待另一方向结束（问题2&3修复）。
     * 立即关闭对端 channel → 对端收到 FIN → 触发 channelInactive → 双向关闭。
     *
     * @param sessionCtx 会话上下文
     */
    public void onDirectionEnded(SessionContext sessionCtx) {
        if (sessionCtx == null || sessionCtx.isDestroyed()) {
            return;
        }
        // 立即销毁：双向关闭两个 channel，标记 record 结束
        destroy(sessionCtx);
    }
}
