package com.molink.access.status;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.URL;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;

public class Socks5HealthChecker {

    private static final Logger log = LoggerFactory.getLogger(Socks5HealthChecker.class);

    private static final int POLL_INTERVAL_SEC = 3;
    private static final int HTTP_TIMEOUT_MS = 10000;

    private final int socksPort;
    private final List<String> testUrls = Arrays.asList(
            "https://www.baidu.com",
            "https://httpbin.org/ip",
            "https://myip.ipip.net"
    );
    private final ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();
    private final AtomicReference<Map<String, Object>> cachedHealth = new AtomicReference<>();

    public Socks5HealthChecker(int socksPort) {
        this.socksPort = socksPort;
        cachedHealth.set(newInitialHealth(false, -1L, ""));
    }

    private static Map<String, Object> newInitialHealth(boolean available, long latencyMs, String reason) {
        Map<String, Object> h = new LinkedHashMap<>();
        h.put("available", available);
        h.put("latencyMs", latencyMs);
        h.put("lastCheck", 0L);
        h.put("unavailableReason", reason);
        return h;
    }

    public void start() {
        new Thread(this::check, "Socks5Health-check").start();
        scheduler.scheduleAtFixedRate(this::check, POLL_INTERVAL_SEC, POLL_INTERVAL_SEC, TimeUnit.SECONDS);
    }

    public void stop() {
        scheduler.shutdown();
    }

    public Map<String, Object> getProxyHealth() {
        return new LinkedHashMap<>(cachedHealth.get());
    }

    /**
     * 触发一次同步健康检查（在新线程中执行），等待完成并返回结果。
     * 最长阻塞 10s。
     */
    public Map<String, Object> waitForNextCheck() {
        try {
            return CompletableFuture.supplyAsync(this::check, scheduler).get(10, TimeUnit.SECONDS);
        } catch (Exception e) {
            log.warn("waitForNextCheck interrupted: {}", e.getMessage());
            Thread.currentThread().interrupt();
            return new LinkedHashMap<>(cachedHealth.get());
        }
    }

    private Map<String, Object> check() {
        long start = System.currentTimeMillis();
        String reason = "";

        for (String testUrl : testUrls) {
            try {
                Proxy proxy = new Proxy(Proxy.Type.SOCKS,
                        new InetSocketAddress("127.0.0.1", socksPort));
                URL url = new URL(testUrl);
                HttpURLConnection conn = (HttpURLConnection) url.openConnection(proxy);
                conn.setConnectTimeout(HTTP_TIMEOUT_MS);
                conn.setReadTimeout(HTTP_TIMEOUT_MS);
                conn.setRequestMethod("GET");

                int code = conn.getResponseCode();
                if (code == 200 || code == 301 || code == 302) {
                    long latencyMs = System.currentTimeMillis() - start;
                    Map<String, Object> result = new LinkedHashMap<>();
                    result.put("available", true);
                    result.put("latencyMs", latencyMs);
                    result.put("lastCheck", System.currentTimeMillis());
                    result.put("unavailableReason", "");
                    cachedHealth.set(result);
                    log.debug("SOCKS health: available=true, latencyMs={}", latencyMs);
                    return result;
                } else {
                    reason = String.format("HTTP %d", code);
                }
                conn.disconnect();
            } catch (java.net.ConnectException e) {
                reason = "端口不可达";
            } catch (java.net.SocketTimeoutException e) {
                reason = "连接超时";
            } catch (java.net.NoRouteToHostException e) {
                reason = "无路由到主机";
            } catch (Exception e) {
                reason = e.getClass().getSimpleName();
            }
            break;
        }

        Map<String, Object> result = new LinkedHashMap<>();
        result.put("available", false);
        result.put("latencyMs", -1L);
        result.put("lastCheck", System.currentTimeMillis());
        result.put("unavailableReason", reason);
        cachedHealth.set(result);
        log.debug("SOCKS health: available=false, reason={}", reason);
        return result;
    }
}
