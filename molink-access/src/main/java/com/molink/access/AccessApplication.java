package com.molink.access;

import com.molink.access.adb.AdbClientManager;
import com.molink.access.config.AppConfig;
import com.molink.access.forwarder.PortForwarder;
import org.springframework.boot.CommandLineRunner;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

@SpringBootApplication
@Configuration
public class AccessApplication {

    public static void main(String[] args) {
        SpringApplication.run(AccessApplication.class, args);
    }

    @Bean
    public AppConfig appConfig() {
        // 从系统属性获取参数（由启动脚本设置）
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
            System.out.println("=== MoLink Access ===");
            System.out.println("本地端口: " + config.getLocalPort());
            System.out.println("远端端口: " + config.getRemotePort());
            System.out.println("API 端口: " + config.getApiPort());
            System.out.println();

            // 连接 ADB
            if (!adbClient.connect()) {
                System.err.println("ADB 连接失败，程序退出");
                System.exit(1);
            }

            // 启动自动重连
            adbClient.startAutoReconnect();

            // 启动端口转发
            try {
                portForwarder.start();  // 建立 ADB 端口转发并监听
            } catch (Exception e) {
                System.err.println("端口转发启动失败: " + e.getMessage());
                e.printStackTrace();
                System.exit(1);
            }

            System.out.println();
            System.out.println("服务已启动，可通过 http://localhost:" + config.getApiPort() + "/api/status 查看状态");
        };
    }
}
