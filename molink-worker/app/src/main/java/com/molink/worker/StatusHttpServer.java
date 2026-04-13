package com.molink.worker;

import fi.iki.elonen.NanoHTTPD;
import fi.iki.elonen.NanoHTTPD.IHTTPSession;
import fi.iki.elonen.NanoHTTPD.Response;
import fi.iki.elonen.NanoHTTPD.Method;

import java.io.IOException;

public class StatusHttpServer extends NanoHTTPD {

    private final Socks5ProxyService proxyService;

    public StatusHttpServer(int port, Socks5ProxyService proxyService) {
        super("127.0.0.1", port);
        this.proxyService = proxyService;
    }

    @Override
    public Response serve(IHTTPSession session) {
        String uri = session.getUri();

        if ("/api/status".equals(uri) && Method.GET.equals(session.getMethod())) {
            return newFixedLengthResponse(Response.Status.OK, "application/json", buildStatus());
        } else if ("/api/ping".equals(uri) && Method.GET.equals(session.getMethod())) {
            return newFixedLengthResponse(Response.Status.OK, "application/json", buildPing());
        } else {
            return newFixedLengthResponse(Response.Status.NOT_FOUND, "application/json", "{\"error\":\"not found\"}");
        }
    }

    private String buildStatus() {
        double memoryUsage = 0;
        try {
            Runtime runtime = Runtime.getRuntime();
            long totalMem = runtime.totalMemory();
            long freeMem = runtime.freeMemory();
            if (totalMem > 0) {
                memoryUsage = ((totalMem - freeMem) * 100.0) / totalMem;
            }
        } catch (Exception e) {
            memoryUsage = -1;
        }

        StringBuilder sb = new StringBuilder();
        sb.append("{");
        sb.append("\"socksRunning\":").append(proxyService.isRunning()).append(",");
        sb.append("\"socksPort\":").append(proxyService.getPort()).append(",");
        sb.append("\"uptime\":").append(proxyService.getUptime()).append(",");
        sb.append("\"memoryUsage\":").append(String.format("%.1f", memoryUsage)).append(",");
        sb.append("\"connectionCount\":").append(proxyService.getConnectionCount()).append(",");
        sb.append("\"timestamp\":").append(System.currentTimeMillis());
        sb.append("}");
        return sb.toString();
    }

    private String buildPing() {
        return "{\"pong\":true,\"timestamp\":" + System.currentTimeMillis() + "}";
    }
}
