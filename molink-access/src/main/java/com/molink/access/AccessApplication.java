package com.molink.access;

import com.molink.access.adb.AdbClientManager;
import com.molink.access.config.MolinkProperties;
import com.molink.access.forwarder.PortForwarder;
import com.molink.access.status.WorkerStatusTracker;
import com.molink.access.status.Socks5HealthChecker;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.boot.CommandLineRunner;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

@SpringBootApplication
@Configuration
@EnableConfigurationProperties(MolinkProperties.class)
public class AccessApplication {

    private static final Logger log = LoggerFactory.getLogger(AccessApplication.class);

    public static void main(String[] args) {
        SpringApplication.run(AccessApplication.class, args);
    }

    @Bean
    public AdbClientManager adbClientManager() {
        return new AdbClientManager();
    }

    @Bean
    public PortForwarder portForwarder(MolinkProperties props, AdbClientManager adbClient) {
        return new PortForwarder(adbClient, props.getLocalPort(), props.getRemotePort());
    }

    @Bean
    public WorkerStatusTracker workerStatusTracker(AdbClientManager adbClient) {
        return new WorkerStatusTracker(adbClient);
    }

    @Bean
    public Socks5HealthChecker socks5HealthChecker(MolinkProperties props) {
        return new Socks5HealthChecker(props.getLocalPort());
    }

    @Bean
    public CommandLineRunner runner(MolinkProperties props, AdbClientManager adbClient,
            PortForwarder portForwarder,
            WorkerStatusTracker workerStatusTracker,
            Socks5HealthChecker socks5HealthChecker) {
        return args -> {
            log.info("=== MoLink Access 启动 ===");
            log.info("本地端口: {}", props.getLocalPort());
            log.info("远端端口: {}", props.getRemotePort());
            log.info("API 端口: {}", props.getApiPort());

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

            // 启动 worker 状态轮询和 SOCKS 健康检查
            workerStatusTracker.start();
            socks5HealthChecker.start();
            log.info("Worker status tracker and SOCKS health checker started");

            log.info("服务已启动，可通过 http://localhost:{}/api/status 查看状态", props.getApiPort());
        };
    }
}
