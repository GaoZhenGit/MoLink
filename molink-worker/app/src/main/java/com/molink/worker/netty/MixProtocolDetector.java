package com.molink.worker.netty;

import java.util.List;

import io.netty.buffer.ByteBuf;
import io.netty.channel.ChannelHandlerContext;
import io.netty.handler.codec.ByteToMessageDecoder;
import io.netty.handler.codec.http.HttpObjectAggregator;
import io.netty.handler.codec.http.HttpRequestDecoder;

/**
 * MIX 模式协议检测器。peek 首个字节：
 * 0x05 → SOCKS5 pipeline
 * 其他 → HTTP pipeline
 * 检测完成后移除自身和 readTimeoutHandler。
 */
public class MixProtocolDetector extends ByteToMessageDecoder {

    @Override
    protected void decode(ChannelHandlerContext ctx, ByteBuf in, List<Object> out) {
        if (in.readableBytes() < 1) return;

        byte firstByte = in.getByte(in.readerIndex());

        if (firstByte == 0x05) {
            ctx.pipeline().addLast(new Socks5StateHandler());
        } else {
            ctx.pipeline().addLast("httpDecoder", new HttpRequestDecoder());
            ctx.pipeline().addLast("httpAggregator", new HttpObjectAggregator(8192));
            ctx.pipeline().addLast(new HttpProxyStateHandler());
        }

        ctx.pipeline().remove("readTimeoutHandler");
        ctx.pipeline().remove(this);
    }
}
