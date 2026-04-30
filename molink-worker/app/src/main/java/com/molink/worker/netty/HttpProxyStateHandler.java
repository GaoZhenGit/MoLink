package com.molink.worker.netty;

import android.util.Base64;
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
import io.netty.handler.codec.http.DefaultFullHttpResponse;
import io.netty.handler.codec.http.FullHttpRequest;
import io.netty.handler.codec.http.HttpHeaderNames;
import io.netty.handler.codec.http.HttpResponseStatus;
import io.netty.handler.codec.http.HttpVersion;
import io.netty.handler.timeout.IdleStateHandler;

import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.TimeUnit;

import io.netty.handler.codec.ByteToMessageDecoder;

/**
 * HTTP CONNECT 代理协议状态机 Handler。
 * Pipeline: ProtocolDetectingHandler → HttpRequestDecoder → HttpObjectAggregator → HttpProxyStateHandler → ForwardHandler
 *
 * 仅支持 CONNECT 方法。认证成功后建立隧道，ForwardHandler 接管双向转发。
 */
public final class HttpProxyStateHandler extends ChannelInboundHandlerAdapter {

    private static final String TAG = "HttpProxyStateHandler";

    /**
     * 协议检测：在 HttpRequestDecoder 之前，检查第一个字节是否为 SOCKS5（0x05）。
     * 如果是，立即返回 HTTP 501 错误；否则移除此 handler，让 HttpRequestDecoder 处理。
     */
    public static class ProtocolDetectingHandler extends ByteToMessageDecoder {
        private static final String ERR_BODY = "HTTP/1.1 501 Not Implemented: this server only supports HTTP CONNECT\r\nConnection: close\r\n\r\n";

        @Override
        protected void decode(ChannelHandlerContext ctx, ByteBuf in, List<Object> out) {
            if (in.readableBytes() < 1) return;
            byte firstByte = in.getByte(in.readerIndex());
            if (firstByte == 0x05) {
                // SOCKS5 协议，不支持
                ByteBuf err = ctx.alloc().buffer();
                err.writeBytes(ERR_BODY.getBytes(StandardCharsets.US_ASCII));
                ctx.writeAndFlush(err).addListener(ChannelFutureListener.CLOSE);
            }
            // 不是 SOCKS5 → 移除此 handler，数据交给 HttpRequestDecoder
            ctx.pipeline().remove(this);
        }
    }

    private SessionContext sessionCtx;
    private String targetHost;
    private int targetPort;
    private String destAddr;

    @Override
    public void channelRead(ChannelHandlerContext ctx, Object msg) throws Exception {
        if (!(msg instanceof FullHttpRequest)) {
            ctx.fireChannelRead(msg);
            return;
        }
        FullHttpRequest req = (FullHttpRequest) msg;
        try {
            handleRequest(ctx, req);
        } finally {
            req.release();
        }
    }

    private void handleRequest(ChannelHandlerContext ctx, FullHttpRequest req) {
        String method = req.method().name();
        if (!"CONNECT".equalsIgnoreCase(method)) {
            sendResponse(ctx, HttpResponseStatus.METHOD_NOT_ALLOWED,
                    "Method Not Allowed: only CONNECT is supported");
            return;
        }

        // 解析目标地址 host:port
        String uri = req.uri();
        int colonIdx = uri.lastIndexOf(':');
        if (colonIdx < 0) {
            sendResponse(ctx, HttpResponseStatus.BAD_REQUEST, "Invalid target address");
            return;
        }
        targetHost = uri.substring(0, colonIdx);
        try {
            targetPort = Integer.parseInt(uri.substring(colonIdx + 1));
        } catch (NumberFormatException e) {
            sendResponse(ctx, HttpResponseStatus.BAD_REQUEST, "Invalid port");
            return;
        }

        // 认证检查
        if (requiresAuth()) {
            String authHeader = req.headers().get(HttpHeaderNames.PROXY_AUTHORIZATION);
            if (!validateAuth(authHeader)) {
                sendResponse(ctx, HttpResponseStatus.PROXY_AUTHENTICATION_REQUIRED,
                        "Proxy Authentication Required");
                return;
            }
        }

        connectToTarget(ctx);
    }

    private boolean requiresAuth() {
        return !BuildConfig.SOCKS_USERNAME.isEmpty() && !BuildConfig.SOCKS_PASSWORD.isEmpty();
    }

    private boolean validateAuth(String authHeader) {
        if (authHeader == null || !authHeader.startsWith("Basic ")) {
            return false;
        }
        String encoded = authHeader.substring(6).trim();
        try {
            byte[] decoded = Base64.decode(encoded, Base64.DEFAULT);
            String decodedStr = new String(decoded, StandardCharsets.UTF_8);
            int colonIdx = decodedStr.indexOf(':');
            if (colonIdx < 0) return false;
            String user = decodedStr.substring(0, colonIdx);
            String pass = decodedStr.substring(colonIdx + 1);
            return BuildConfig.SOCKS_USERNAME.equals(user) && BuildConfig.SOCKS_PASSWORD.equals(pass);
        } catch (Exception e) {
            return false;
        }
    }

    private void connectToTarget(ChannelHandlerContext ctx) {
        // 创建 ConnectionRecord
        String clientIp = ctx.channel().remoteAddress().toString();
        if (clientIp.startsWith("/")) clientIp = clientIp.substring(1);
        ConnectionRecord record = new ConnectionRecord(clientIp, targetHost, targetPort,
                System.currentTimeMillis(), "HTTP");
        Socks5ProxyService.registerConnection(record);

        // 本地 DNS 解析
        try {
            InetAddress[] addrs = InetAddress.getAllByName(targetHost);
            String ipv4Addr = null;
            for (InetAddress addr : addrs) {
                byte[] raw = addr.getAddress();
                if (raw.length == 4) {
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
            sendResponse(ctx, HttpResponseStatus.BAD_GATEWAY, "DNS resolution failed");
            return;
        }

        Log.i(TAG, "CONNECT: " + targetHost + ":" + targetPort + " (resolved: " + destAddr + ")");

        sessionCtx = new SessionContext(ctx.channel(), record);
        ConnectionLifecycleManager.getInstance().register(ctx.channel(), sessionCtx);

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

        bs.connect(destAddr, targetPort).addListener((ChannelFutureListener) future -> {
            if (future.isSuccess()) {
                Channel targetChannel = future.channel();
                sessionCtx.targetChannel = targetChannel;
                ConnectionLifecycleManager.getInstance().register(targetChannel, sessionCtx);

                // 发送 200 Connection Established（纯文本，不走 HTTP codec）
                ByteBuf content = ctx.alloc().buffer();
                content.writeBytes("HTTP/1.1 200 Connection Established\r\n\r\n".getBytes(StandardCharsets.US_ASCII));
                ctx.writeAndFlush(content);

                // 在 clientChannel 添加 IdleStateHandler + ForwardHandler
                ctx.channel().pipeline().addLast(new IdleStateHandler(0, 0, 30, TimeUnit.SECONDS));
                ctx.channel().pipeline().addLast(new ForwardHandler(targetChannel, record, "c->s"));

                // 移除自身和 HTTP codec handlers，让数据直接到 ForwardHandler
                ctx.channel().pipeline().remove(HttpProxyStateHandler.this);
                try { ctx.channel().pipeline().remove("httpDecoder"); } catch (Exception ignored) {}
                try { ctx.channel().pipeline().remove("httpAggregator"); } catch (Exception ignored) {}
            } else {
                Log.e(TAG, "Failed to connect to " + destAddr + ":" + targetPort, future.cause());
                sendResponse(ctx, HttpResponseStatus.BAD_GATEWAY, "Failed to connect to target");
                ctx.close();
            }
        });
    }

    private void sendResponse(ChannelHandlerContext ctx, HttpResponseStatus status, String body) {
        DefaultFullHttpResponse resp = new DefaultFullHttpResponse(
                HttpVersion.HTTP_1_1, status,
                ctx.alloc().buffer().writeBytes(body.getBytes(StandardCharsets.UTF_8)));
        resp.headers().set(HttpHeaderNames.CONTENT_LENGTH, body.length());
        resp.headers().set(HttpHeaderNames.CONNECTION, "close");
        ctx.writeAndFlush(resp).addListener(ChannelFutureListener.CLOSE);
    }

    @Override
    public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) throws Exception {
        Log.e(TAG, "HttpProxyStateHandler exception: " + cause.getMessage());
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
