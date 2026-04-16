package com.molink.access.health;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.net.Authenticator;
import java.net.InetSocketAddress;
import java.net.PasswordAuthentication;
import java.net.Proxy;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;

public class OkHttpProxyHealthChecker {

    private static final Logger log = LoggerFactory.getLogger(OkHttpProxyHealthChecker.class);

    private static final List<String> TEST_URLS = Arrays.asList(
            "https://www.baidu.com"
    );

    private static final int CONNECT_TIMEOUT_SECONDS = 5;
    private static final int READ_TIMEOUT_SECONDS = 10;

    private final String proxyHost;
    private final int proxyPort;
    private final String socksUsername;
    private final String socksPassword;

    public OkHttpProxyHealthChecker(String proxyHost, int proxyPort, String socksUsername, String socksPassword) {
        this.proxyHost = proxyHost;
        this.proxyPort = proxyPort;
        this.socksUsername = socksUsername;
        this.socksPassword = socksPassword;

        // 设置全局 SOCKS 认证
        Authenticator.setDefault(new Authenticator() {
            @Override
            protected PasswordAuthentication getPasswordAuthentication() {
                log.debug("using {}, auth:[{}:{}]", getRequestorType(), socksUsername, socksPassword);
                return new PasswordAuthentication(socksUsername, socksPassword.toCharArray());
            }
        });
    }

    public Map<String, Object> check() {
        Map<String, Object> result = new LinkedHashMap<>();
        result.put("reachable", false);
        result.put("latencyMs", -1L);
        result.put("testUrl", null);
        result.put("error", null);

        // 使用 createUnresolved 实现 SOCKS5h
        Proxy proxy = new Proxy(Proxy.Type.SOCKS,
                InetSocketAddress.createUnresolved(proxyHost, proxyPort));

        OkHttpClient client = buildClient(proxy);

        for (String testUrl : TEST_URLS) {
            try {
                long start = System.currentTimeMillis();
                Request request = new Request.Builder()
                        .url(testUrl)
                        .header("Connection", "close") // 添加此头，告诉服务器本次请求后关闭连接
                        .build();

                try (Response response = client.newCall(request).execute()) {
                    long latencyMs = System.currentTimeMillis() - start;
                    int code = response.code();

                    log.debug("Proxy health check: {} -> HTTP {}", testUrl, code);

                    if (code == 200 || code == 204 || code == 301 || code == 302) {
                        result.put("reachable", true);
                        result.put("latencyMs", latencyMs);
                        result.put("testUrl", testUrl);
                        result.put("error", null);
                        log.debug("Proxy health OK: {} -> HTTP {}, {}ms", testUrl, code, latencyMs);
                        return result;
                    } else {
                        result.put("error", "HTTP " + code);
                    }
                }
            } catch (IOException e) {
                String msg = e.getMessage();
                if (msg != null && msg.contains("timeout")) {
                    result.put("error", "Connection timeout");
                } else if (msg != null && (msg.contains("closed") || msg.contains("reset"))) {
                    result.put("error", "Connection closed");
                } else {
                    result.put("error", e.getClass().getSimpleName() + ": " + msg);
                }
                log.debug("Proxy health check failed for {}: {}", testUrl, msg);
            } catch (Exception e) {
                result.put("error", e.getClass().getSimpleName());
                log.debug("Proxy health check exception for {}: {}", testUrl, e.getMessage());
            }
            // 当前 URL 失败，尝试下一个
        }

        log.debug("Proxy health FAIL: {}", result.get("error"));
        return result;
    }

    private OkHttpClient buildClient(Proxy proxy) {
        return new OkHttpClient.Builder()
                .proxy(proxy)
                .connectTimeout(CONNECT_TIMEOUT_SECONDS, TimeUnit.SECONDS)
                .readTimeout(READ_TIMEOUT_SECONDS, TimeUnit.SECONDS)
                .followRedirects(true)
                .followSslRedirects(true)
                .build();
    }
}