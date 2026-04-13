package com.molink.access.status;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.URL;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.*;

public class Socks5HealthChecker {

    private static final Logger log = LoggerFactory.getLogger(Socks5HealthChecker.class);

    private static final String TEST_URL = "http://httpbin.org/ip";
    private static final int SOCKS_PORT = 1080;
    private static final int POLL_INTERVAL_SEC = 10;
    private static final int HTTP_TIMEOUT_MS = 15000;

    private final ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();
    private volatile Map<String, Object> cachedHealth = new LinkedHashMap<>();

    public Socks5HealthChecker() {
        // 初始化缓存，确保 getProxyHealth() 在首次 check() 完成前也有数据
        cachedHealth = new LinkedHashMap<>();
        cachedHealth.put("available", false);
        cachedHealth.put("latencyMs", -1L);
        cachedHealth.put("lastCheck", 0L);
    }

    public void start() {
        // 首次检查异步执行，不阻塞调用线程
        new Thread(this::check, "Socks5Health-check").start();
        scheduler.scheduleAtFixedRate(this::check, POLL_INTERVAL_SEC, POLL_INTERVAL_SEC, TimeUnit.SECONDS);
    }

    public void stop() {
        scheduler.shutdown();
    }

    public Map<String, Object> getProxyHealth() {
        return new LinkedHashMap<>(cachedHealth);
    }

    private void check() {
        long start = System.currentTimeMillis();
        boolean ok = false;
        try {
            Proxy proxy = new Proxy(Proxy.Type.SOCKS,
                    new InetSocketAddress("127.0.0.1", SOCKS_PORT));
            URL url = new URL(TEST_URL);
            HttpURLConnection conn = (HttpURLConnection) url.openConnection(proxy);
            conn.setConnectTimeout(HTTP_TIMEOUT_MS);
            conn.setReadTimeout(HTTP_TIMEOUT_MS);
            conn.setRequestMethod("GET");

            int code = conn.getResponseCode();
            // httpbin.org/ip 返回 200 即为正常
            ok = (code == 200);
            conn.disconnect();
        } catch (Exception e) {
            log.warn("SOCKS health check failed: {}", e.getMessage());
        }

        long latencyMs = System.currentTimeMillis() - start;
        Map<String, Object> result = new LinkedHashMap<>();
        result.put("available", ok);
        result.put("latencyMs", ok ? latencyMs : -1);
        result.put("lastCheck", System.currentTimeMillis());
        this.cachedHealth = result;
        log.debug("SOCKS health: available={}, latencyMs={}", ok, latencyMs);
    }
}
