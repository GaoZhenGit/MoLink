package com.molink.worker.netty;

import android.util.Log;
import com.molink.worker.BuildConfig;
import com.molink.worker.ConnectionRecord;
import com.molink.worker.Socks5ProxyService;
import io.netty.bootstrap.Bootstrap;
import io.netty.buffer.ByteBuf;
import io.netty.channel.Channel;
import io.netty.channel.ChannelFutureListener;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.socket.nio.NioSocketChannel;
import io.netty.handler.timeout.IdleStateHandler;

import java.net.InetAddress;
import java.util.concurrent.TimeUnit;

/**
 * SOCKS5 协议状态机 Handler。
 *
 * 状态流转：
 * WAIT_AUTH_METHOD → handleAuthMethod()
 * WAIT_AUTH        → handleUserAuth()
 * WAIT_COMMAND     → handleConnect()
 * FORWARDING       → ForwardHandler 接管，不再处理
 */
public final class Socks5StateHandler extends ChannelInboundHandlerAdapter {

    private static final String TAG = "Socks5StateHandler";

    // SOCKS5 协议常量
    private static final int SOCKS_VERSION = 0x05;
    private static final int CMD_CONNECT = 0x01;
    private static final int ATYP_IPV4 = 0x01;
    private static final int ATYP_DOMAIN = 0x03;
    private static final int AUTH_METHOD_USER_PASS = 0x02;
    private static final int AUTH_METHOD_NO_ACCEPTABLE = 0xFF;
    private static final int REPLY_SUCCESS = 0x00;
    private static final int REPLY_HOST_UNREACHABLE = 0x04;
    private static final int REPLY_COMMAND_NOT_SUPPORTED = 0x07;

    // 状态枚举
    private enum State { WAIT_AUTH_METHOD, WAIT_AUTH, WAIT_COMMAND, FORWARDING }
    private State state = State.WAIT_AUTH_METHOD;

    // 当前会话上下文（建立连接后赋值）
    private SessionContext sessionCtx;
    private String targetHost;
    private int targetPort;
    private String destAddr;  // IP 或解析后的 IP 地址

    // ===== SOCKS5 握手 =====

    @Override
    public void channelRead(ChannelHandlerContext ctx, Object msg) throws Exception {
        if (!(msg instanceof ByteBuf)) {
            ctx.fireChannelRead(msg);
            return;
        }
        ByteBuf in = (ByteBuf) msg;
        boolean consumed = true;
        try {
            switch (state) {
                case WAIT_AUTH_METHOD:
                    handleAuthMethod(ctx, in);
                    break;
                case WAIT_AUTH:
                    handleUserAuth(ctx, in);
                    break;
                case WAIT_COMMAND:
                    handleConnect(ctx, in);
                    break;
                case FORWARDING:
                    // msg 引用已转交下游，下游负责 release，本 Handler 不再 release
                    consumed = false;
                    ctx.fireChannelRead(msg);
                    break;
            }
        } finally {
            if (consumed) {
                in.release();  // Netty 4 ByteBuf 引用计数，必须显式 release
            }
        }
    }

    private void handleAuthMethod(ChannelHandlerContext ctx, ByteBuf in) throws Exception {
        if (in.readableBytes() < 2) {
            ctx.close();
            return;
        }
        int ver = in.readByte();
        if (ver != SOCKS_VERSION) {
            Log.w(TAG, "Unsupported SOCKS version: " + ver);
            ctx.close();
            return;
        }
        int nMethods = in.readByte();
        if (in.readableBytes() < nMethods) {
            ctx.close();
            return;
        }
        byte[] methods = new byte[nMethods];
        in.readBytes(methods);

        boolean supportsUserPass = false;
        for (int i = 0; i < nMethods; i++) {
            if (methods[i] == AUTH_METHOD_USER_PASS) {
                supportsUserPass = true;
                break;
            }
        }

        ByteBuf out = ctx.alloc().buffer(2);
        out.writeByte(SOCKS_VERSION);
        if (supportsUserPass) {
            out.writeByte(AUTH_METHOD_USER_PASS);
            state = State.WAIT_AUTH;
        } else {
            out.writeByte(AUTH_METHOD_NO_ACCEPTABLE);
            Log.w(TAG, "No acceptable auth method");
            ctx.writeAndFlush(out).addListener(ChannelFutureListener.CLOSE);
            return;
        }
        ctx.writeAndFlush(out);
    }

    // ===== RFC 1929 用户名/密码认证 =====

    private void handleUserAuth(ChannelHandlerContext ctx, ByteBuf in) throws Exception {
        if (in.readableBytes() < 2) {
            ctx.close();
            return;
        }
        int ver = in.readByte();
        if (ver != 0x01) {
            Log.w(TAG, "Unknown auth sub-negotiation version: " + ver);
            ctx.close();
            return;
        }
        int userLen = in.readByte();
        if (userLen < 0 || userLen > 255 || in.readableBytes() < userLen + 1) {
            ctx.close();
            return;
        }
        byte[] userBytes = new byte[userLen];
        in.readBytes(userBytes);
        String username = new String(userBytes);

        int passLen = in.readByte();
        if (passLen < 0 || passLen > 255 || in.readableBytes() < passLen) {
            ctx.close();
            return;
        }
        byte[] passBytes = new byte[passLen];
        in.readBytes(passBytes);
        String password = new String(passBytes);

        boolean success = true;
        if (!BuildConfig.SOCKS_USERNAME.isEmpty() && !BuildConfig.SOCKS_PASSWORD.isEmpty()) {
            success = BuildConfig.SOCKS_USERNAME.equals(username) && BuildConfig.SOCKS_PASSWORD.equals(password);
        }

        ByteBuf out = ctx.alloc().buffer(2);
        out.writeByte(0x01);
        out.writeByte(success ? 0x00 : 0x01);
        if (!success) {
            Log.w(TAG, "Auth failed for user: " + username);
            ctx.writeAndFlush(out).addListener(ChannelFutureListener.CLOSE);
            return;
        }
        ctx.writeAndFlush(out);
        state = State.WAIT_COMMAND;
        Log.i(TAG, "Auth success for user: " + username);
    }

    // ===== CONNECT 命令 + SOCKS5H 域名解析 =====

    private void handleConnect(ChannelHandlerContext ctx, ByteBuf in) throws Exception {
        if (in.readableBytes() < 4) {
            ctx.close();
            return;
        }
        int ver = in.readByte();
        if (ver != SOCKS_VERSION) {
            Log.w(TAG, "Unexpected version in connect request: " + ver);
            ctx.close();
            return;
        }
        int cmd = in.readByte();
        int rsv = in.readByte();
        int addrType = in.readByte();

        if (cmd != CMD_CONNECT) {
            sendReply(ctx, REPLY_COMMAND_NOT_SUPPORTED, null, 0);
            return;
        }

        // 解析目标地址
        if (addrType == ATYP_IPV4) {
            if (in.readableBytes() < 6) { ctx.close(); return; }
            byte[] ip = new byte[4];
            in.readBytes(ip);
            destAddr = String.format("%d.%d.%d.%d", ip[0] & 0xFF, ip[1] & 0xFF, ip[2] & 0xFF, ip[3] & 0xFF);
            byte[] portBytes = new byte[2];
            in.readBytes(portBytes);
            targetPort = ((portBytes[0] & 0xFF) << 8) | (portBytes[1] & 0xFF);
            targetHost = destAddr;
        } else if (addrType == ATYP_DOMAIN) {
            // SOCKS5H：域名由本端解析
            if (in.readableBytes() < 1) { ctx.close(); return; }
            int domainLen = in.readByte();
            if (in.readableBytes() < domainLen + 2) { ctx.close(); return; }
            byte[] domainBytes = new byte[domainLen];
            in.readBytes(domainBytes);
            targetHost = new String(domainBytes);
            byte[] portBytes = new byte[2];
            in.readBytes(portBytes);
            targetPort = ((portBytes[0] & 0xFF) << 8) | (portBytes[1] & 0xFF);

            // 本地 DNS 解析（仅返回 IPv4，阻塞调用，在 eventLoop 线程中执行，不阻塞其他连接）
            try {
                InetAddress[] addrs = InetAddress.getAllByName(targetHost);
                String ipv4Addr = null;
                for (InetAddress addr : addrs) {
                    byte[] raw = addr.getAddress();
                    if (raw.length == 4) {  // 过滤出 IPv4
                        ipv4Addr = addr.getHostAddress();
                        break;
                    }
                }
                if (ipv4Addr == null) {
                    throw new Exception("No IPv4 address found for " + targetHost);
                }
                destAddr = ipv4Addr;
            } catch (Exception e) {
                Log.e(TAG, "DNS resolution failed for " + targetHost, e);
                sendReply(ctx, REPLY_HOST_UNREACHABLE, null, 0);
                return;
            }
        } else {
            // 不支持的地址类型
            sendReply(ctx, REPLY_COMMAND_NOT_SUPPORTED, null, 0);
            return;
        }

        Log.i(TAG, "CONNECT: " + targetHost + ":" + targetPort + " (resolved: " + destAddr + ")");

        // 创建 ConnectionRecord 并注册到 Service 的历史列表
        String clientIp = ctx.channel().remoteAddress().toString();
        if (clientIp.startsWith("/")) clientIp = clientIp.substring(1);
        ConnectionRecord record = new ConnectionRecord(clientIp, targetHost, targetPort, System.currentTimeMillis());
        Socks5ProxyService.registerConnection(record);

        // 注册到全局管理器（提前注册，便于 ForwardHandler 查找）
        sessionCtx = new SessionContext(ctx.channel(), record);
        ConnectionLifecycleManager.getInstance().register(ctx.channel(), sessionCtx);

        // 异步建立目标连接
        connectToTarget(ctx, record, destAddr, targetPort);
    }

    private void connectToTarget(ChannelHandlerContext ctx, ConnectionRecord record,
                                 String destAddr, int destPort) {
        Bootstrap bs = new Bootstrap();
        bs.group(ctx.channel().eventLoop())
          .channel(NioSocketChannel.class)
          .handler(new ChannelInitializer<Channel>() {
              @Override
              protected void initChannel(Channel ch) throws Exception {
                  ch.pipeline().addLast(new IdleStateHandler(0, 0, 30, TimeUnit.SECONDS));
                  ch.pipeline().addLast(new ForwardHandler(ctx.channel(), record, "s->c"));
              }
          });

        bs.connect(destAddr, destPort).addListener((ChannelFutureListener) future -> {
            if (future.isSuccess()) {
                Channel targetChannel = future.channel();
                sessionCtx.targetChannel = targetChannel;
                // 注册 target channel 到 lifecycle manager
                ConnectionLifecycleManager.getInstance().register(targetChannel, sessionCtx);

                // 发送成功响应
                sendReplySuccess(ctx, targetChannel);

                // 在 clientChannel.pipeline() 添加 IdleStateHandler + ForwardHandler
                ctx.channel().pipeline().addLast(new IdleStateHandler(0, 0, 30, TimeUnit.SECONDS));
                ctx.channel().pipeline().addLast(new ForwardHandler(targetChannel, record, "c->s"));

                // 切换状态，移除自身（让数据直接到 ForwardHandler）
                ctx.channel().pipeline().remove(Socks5StateHandler.this);
            } else {
                Log.e(TAG, "Failed to connect to " + destAddr + ":" + destPort, future.cause());
                sendReply(ctx, REPLY_HOST_UNREACHABLE, null, 0);
                ctx.close();
            }
        });
    }

    private void sendReply(ChannelHandlerContext ctx, int reply, byte[] bindAddr, int bindPort) {
        ByteBuf out = ctx.alloc().buffer(10);
        out.writeByte(SOCKS_VERSION);
        out.writeByte(reply);
        out.writeByte(0x00); // rsv
        out.writeByte(ATYP_IPV4);
        if (bindAddr != null && bindAddr.length == 4) {
            out.writeBytes(bindAddr);
        } else {
            out.writeBytes(new byte[]{0, 0, 0, 0});
        }
        out.writeShort(bindPort);
        ctx.writeAndFlush(out).addListener(ChannelFutureListener.CLOSE);
    }

    private void sendReplySuccess(ChannelHandlerContext ctx, Channel targetChannel) {
        ByteBuf out = ctx.alloc().buffer(10);
        out.writeByte(SOCKS_VERSION);
        out.writeByte(REPLY_SUCCESS);
        out.writeByte(0x00);
        out.writeByte(ATYP_IPV4);
        // 固定 4 字节，避免 IPv6 时 getAddress() 返回 16 字节导致协议格式错误
        byte[] localIp = new byte[4];
        java.net.InetSocketAddress localSockAddr = (java.net.InetSocketAddress) targetChannel.localAddress();
        java.net.InetAddress localAddr = localSockAddr.getAddress();
        byte[] addrBytes = localAddr.getAddress();
        if (addrBytes.length == 4) {
            localIp = addrBytes;
        }
        int localPort = localSockAddr.getPort();
        out.writeBytes(localIp);
        out.writeShort(localPort);
        ctx.writeAndFlush(out);
    }

    @Override
    public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) throws Exception {
        Log.e(TAG, "Socks5StateHandler exception: " + cause.getMessage());
        if (sessionCtx != null) {
            ConnectionLifecycleManager.getInstance().destroy(sessionCtx);
        }
        ctx.close();
    }

    @Override
    public void channelInactive(ChannelHandlerContext ctx) throws Exception {
        if (sessionCtx != null) {
            ConnectionLifecycleManager.getInstance().destroy(sessionCtx);
        }
        super.channelInactive(ctx);
    }
}
