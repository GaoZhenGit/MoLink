package com.molink.access;

import com.molink.access.adb.AdbClientManager;
import com.molink.access.config.AppConfig;
import com.molink.access.forwarder.PortForwarder;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.boot.CommandLineRunner;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

@SpringBootApplication
@Configuration
public class AccessApplication {

    private static final Logger log = LoggerFactory.getLogger(AccessApplication.class);

    public static void main(String[] args) {
        SpringApplication.run(AccessApplication.class, args);
    }

    @Bean
    public AppConfig appConfig() {
        int localPort = Integer.parseInt(System.getProperty("molink.local.port", "1080"));
        int remotePort = Integer.parseInt(System.getProperty("molink.remote.port", "1080"));
        int apiPort = Integer.parseInt(System.getProperty("molink.api.port", "8080"));
        String configPath = System.getProperty("molink.config.path", "config.properties");

        return AppConfig.load(localPort, remotePort, apiPort, configPath);
    }

    @Bean
    public AdbClientManager adbClientManager() {
        return new AdbClientManager();
    }

    @Bean
    public PortForwarder portForwarder(AppConfig config, AdbClientManager adbClient) {
        return new PortForwarder(adbClient, config.getLocalPort(), config.getRemotePort());
    }

    @Bean
    public CommandLineRunner runner(AppConfig config, AdbClientManager adbClient, PortForwarder portForwarder) {
        return args -> {
            log.info("=== MoLink Access 启动 ===");
            log.info("本地端口: {}", config.getLocalPort());
            log.info("远端端口: {}", config.getRemotePort());
            log.info("API 端口: {}", config.getApiPort());

            // 连接 ADB
            if (!adbClient.connect()) {
                log.error("ADB 连接失败，程序退出");
                System.exit(1);
            }

            // 启动自动重连
            adbClient.startAutoReconnect();

            // 启动端口转发
            try {
                portForwarder.start();
            } catch (Exception e) {
                log.error("端口转发启动失败: {}", e.getMessage(), e);
                System.exit(1);
            }

            log.info("服务已启动，可通过 http://localhost:{}/api/status 查看状态", config.getApiPort());
        };
    }
}
