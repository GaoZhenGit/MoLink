package com.molink.worker.netty;

import android.util.Log;
import com.molink.worker.ConnectionRecord;
import com.molink.worker.Socks5ProxyService;
import io.netty.channel.Channel;
import io.netty.channel.ChannelFutureListener;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.handler.timeout.IdleState;
import io.netty.handler.timeout.IdleStateEvent;

/**
 * 双向数据转发 Handler，同时处理 c→s 和 s→c 两个方向。
 *
 * 触发双向关闭的条件（任一满足）：
 * - channelInactive：对端关闭了连接（收到 FIN）
 * - exceptionCaught：连接异常
 * - write 失败：写入时发现对端已死（FIN/RST）
 * - IdleStateEvent（30 秒无读写）
 *
 * 字节计数方向约定：
 * - "c->s"：客户端→服务端 = 用户上传 = bytesDown
 * - "s->c"：服务端→客户端 = 用户下载 = bytesUp
 */
public final class ForwardHandler extends ChannelInboundHandlerAdapter {

    private static final String TAG = "ForwardHandler";

    private final Channel otherChannel;           // 对端 channel
    private final ConnectionRecord record;
    private final String direction;              // "c->s" 或 "s->c"

    public ForwardHandler(Channel otherChannel, ConnectionRecord record, String direction) {
        this.otherChannel = otherChannel;
        this.record = record;
        this.direction = direction;
    }

    @Override
    public void channelRead(ChannelHandlerContext ctx, Object msg) throws Exception {
        if (!(msg instanceof io.netty.buffer.ByteBuf)) {
            ctx.fireChannelRead(msg);
            return;
        }
        io.netty.buffer.ByteBuf buf = (io.netty.buffer.ByteBuf) msg;
        int len = buf.readableBytes();

        // 更新 ConnectionRecord 字节计数
        if ("c->s".equals(direction)) {
            record.addBytesDown(len);
            Socks5ProxyService.addBytesDown(len);   // 全局统计（问题1修复）
        } else {
            record.addBytesUp(len);
            Socks5ProxyService.addBytesUp(len);    // 全局统计（问题1修复）
        }

        // 将数据写入对端，写入失败立即触发双向关闭（问题3修复）
        otherChannel.writeAndFlush(msg).addListener((ChannelFutureListener) future -> {
            if (!future.isSuccess()) {
                Log.i(TAG, direction + ": write failed, triggering close: " + future.cause().getMessage());
                ConnectionLifecycleManager.getInstance()
                        .onDirectionEnded(ConnectionLifecycleManager.getInstance().findByChannel(ctx.channel()));
            }
        });
    }

    @Override
    public void channelInactive(ChannelHandlerContext ctx) throws Exception {
        Log.d(TAG, direction + ": channel inactive (FIN/RST received)");
        ConnectionLifecycleManager.getInstance()
                .onDirectionEnded(ConnectionLifecycleManager.getInstance().findByChannel(ctx.channel()));
        super.channelInactive(ctx);
    }

    @Override
    public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) throws Exception {
        String msg = cause.getMessage();
        // 忽略常见非关键异常日志
        if (msg == null || (!msg.contains("Connection reset")
                && !msg.contains("Broken pipe")
                && !msg.contains("Socket closed")
                && !msg.contains("EOF"))) {
            Log.w(TAG, direction + ": exception: " + msg);
        }
        ConnectionLifecycleManager.getInstance()
                .onDirectionEnded(ConnectionLifecycleManager.getInstance().findByChannel(ctx.channel()));
        super.exceptionCaught(ctx, cause);
    }

    @Override
    public void userEventTriggered(ChannelHandlerContext ctx, Object evt) throws Exception {
        if (evt instanceof IdleStateEvent) {
            IdleStateEvent idleEvt = (IdleStateEvent) evt;
            if (idleEvt.state() == IdleState.ALL_IDLE) {
                Log.i(TAG, direction + ": idle timeout (30s), closing both sides");
                ConnectionLifecycleManager.getInstance()
                        .onDirectionEnded(ConnectionLifecycleManager.getInstance().findByChannel(ctx.channel()));
            }
        }
        super.userEventTriggered(ctx, evt);
    }
}
