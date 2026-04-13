package com.molink.access.status;

import com.molink.access.adb.AdbClientManager;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.net.HttpURLConnection;
import java.net.URL;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.*;

public class WorkerStatusTracker {

    private static final Logger log = LoggerFactory.getLogger(WorkerStatusTracker.class);

    private static final int HTTP_PORT = 18081;
    private static final int REMOTE_PORT = 8081;
    private static final int POLL_INTERVAL_SEC = 10;
    private static final int HTTP_TIMEOUT_MS = 5000;

    private final AdbClientManager adbClient;
    private final ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();
    private AutoCloseable currentForward;
    private volatile Map<String, Object> cachedStatus = new LinkedHashMap<>();
    private volatile boolean available = false;

    public WorkerStatusTracker(AdbClientManager adbClient) {
        this.adbClient = adbClient;
        // 初始化缓存默认值
        this.cachedStatus = new LinkedHashMap<>();
        this.cachedStatus.put("appReachable", false);
        this.cachedStatus.put("socksRunning", false);
        this.cachedStatus.put("socksPort", -1);
        this.cachedStatus.put("uptime", 0L);
        this.cachedStatus.put("memoryUsage", -1.0);
        this.cachedStatus.put("connectionCount", -1);
    }

    public void start() {
        // 建立 dadb 端口转发
        try {
            currentForward = adbClient.forward(HTTP_PORT, REMOTE_PORT);
            log.info("Worker HTTP forward established: tcp:{} -> tcp:{}", HTTP_PORT, REMOTE_PORT);
        } catch (Exception e) {
            log.error("Failed to establish worker HTTP forward: {}", e.getMessage());
        }

        // 首次轮询异步执行，不阻塞调用线程
        new Thread(this::poll, "WorkerStatus-poll").start();
        scheduler.scheduleAtFixedRate(this::poll, POLL_INTERVAL_SEC, POLL_INTERVAL_SEC, TimeUnit.SECONDS);
    }

    public void stop() {
        scheduler.shutdown();
        if (currentForward != null) {
            try {
                currentForward.close();
                log.info("Worker HTTP forward closed");
            } catch (Exception e) {
                log.warn("Error closing worker HTTP forward: {}", e.getMessage());
            }
        }
    }

    /**
     * 获取 worker 状态。
     * available=false 时说明 worker app 不可达（app 未运行或 USB 断开）。
     * available=true 时根据 socksRunning 判断 service 是否在运行。
     */
    public Map<String, Object> getWorkerStatus() {
        Map<String, Object> result = new LinkedHashMap<>(cachedStatus);
        result.put("lastSeen", System.currentTimeMillis());
        result.put("available", available);
        return result;
    }

    private void poll() {
        if (!adbClient.isConnected()) {
            available = false;
            resetStatus("ADB disconnected");
            return;
        }
        try {
            URL url = new URL("http://127.0.0.1:" + HTTP_PORT + "/api/status");
            HttpURLConnection conn = (HttpURLConnection) url.openConnection();
            conn.setConnectTimeout(HTTP_TIMEOUT_MS);
            conn.setReadTimeout(HTTP_TIMEOUT_MS);
            conn.setRequestMethod("GET");

            int code = conn.getResponseCode();
            if (code == 200) {
                try (java.io.BufferedReader r = new java.io.BufferedReader(
                        new java.io.InputStreamReader(conn.getInputStream(), "UTF-8"))) {
                    StringBuilder sb = new StringBuilder();
                    String line;
                    while ((line = r.readLine()) != null) sb.append(line);
                    parseAndCache(sb.toString());
                    available = true;
                    log.debug("Worker status polled OK: {}", cachedStatus);
                }
            } else {
                available = false;
                resetStatus("HTTP " + code);
                log.warn("Worker HTTP responded code {}", code);
            }
            conn.disconnect();
        } catch (Exception e) {
            available = false;
            resetStatus(e.getClass().getSimpleName());
            log.warn("Worker status poll failed: {}", e.getMessage());
        }
    }

    private void resetStatus(String reason) {
        Map<String, Object> reset = new LinkedHashMap<>();
        reset.put("appReachable", false);
        reset.put("socksRunning", false);
        reset.put("socksPort", -1);
        reset.put("uptime", 0L);
        reset.put("memoryUsage", -1.0);
        reset.put("connectionCount", -1);
        reset.put("unavailableReason", reason);
        this.cachedStatus = reset;
    }

    private void parseAndCache(String json) {
        Map<String, Object> parsed = new LinkedHashMap<>();
        parsed.put("appReachable", true);
        parsed.put("socksRunning", extractBool(json, "socksRunning"));
        parsed.put("socksPort", extractInt(json, "socksPort"));
        parsed.put("uptime", extractLong(json, "uptime"));
        parsed.put("memoryUsage", extractDouble(json, "memoryUsage"));
        parsed.put("connectionCount", extractInt(json, "connectionCount"));
        this.cachedStatus = parsed;
    }

    private boolean extractBool(String json, String key) {
        return extractStr(json, key).equals("true");
    }

    private int extractInt(String json, String key) {
        try { return Integer.parseInt(extractStr(json, key)); } catch (Exception e) { return -1; }
    }

    private long extractLong(String json, String key) {
        try { return Long.parseLong(extractStr(json, key)); } catch (Exception e) { return -1; }
    }

    private double extractDouble(String json, String key) {
        try { return Double.parseDouble(extractStr(json, key)); } catch (Exception e) { return -1; }
    }

    private String extractStr(String json, String key) {
        String pattern = "\"" + key + "\":";
        int idx = json.indexOf(pattern);
        if (idx < 0) return "";
        int start = idx + pattern.length();
        while (start < json.length() && Character.isWhitespace(json.charAt(start))) start++;
        int end = start;
        boolean inString = false;
        while (end < json.length()) {
            char c = json.charAt(end);
            if (c == '"') { inString = !inString; end++; continue; }
            if (!inString && (c == ',' || c == '}')) break;
            end++;
        }
        return json.substring(start, end).trim();
    }
}
