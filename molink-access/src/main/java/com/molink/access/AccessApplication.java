package com.molink.access;

import com.molink.access.adb.AdbClientManager;
import com.molink.access.config.MolinkProperties;
import com.molink.access.forwarder.AdbForwarder;
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
    public AdbForwarder adbForwarder(MolinkProperties props, AdbClientManager adbClient) {
        return new AdbForwarder(adbClient, props.getLocalPort(), props.getRemotePort());
    }

    @Bean
    public CommandLineRunner runner(MolinkProperties props, AdbClientManager adbClient,
            AdbForwarder adbForwarder) {
        return args -> {
            log.info("=== MoLink Access Starting ===");
            log.info("Local proxy port: {}", props.getLocalPort());
            log.info("Remote port: {}", props.getRemotePort());
            log.info("API port: {}", props.getApiPort());

            // Set up callbacks first (avoid race where background thread connects before callback is registered)
            adbClient.setOnConnected(dadb -> {
                try {
                    adbForwarder.start();
                    log.info("AdbForwarder ready, proxy available -> localhost:{}", props.getLocalPort());
                } catch (Exception e) {
                    log.error("AdbForwarder start failed: {}", e.getMessage(), e);
                }
            });

            adbClient.setOnDisconnected(() -> {
                adbForwarder.stop();
                log.warn("Device disconnected, waiting for reconnect...");
            });

            // Start background reconnect thread (continuously monitors for device)
            adbClient.startAutoReconnect();

            // Wait for device connection (blocks until device is connected)
            log.info("Waiting for Android device...");
            adbClient.waitForConnection();

            // Device is now connected; manually trigger onConnected to ensure AdbForwarder is started
            if (adbClient.isConnected()) {
                try {
                    adbForwarder.start();
                    log.info("AdbForwarder ready, proxy available -> localhost:{}", props.getLocalPort());
                } catch (Exception e) {
                    log.error("AdbForwarder start failed: {}", e.getMessage(), e);
                }
            }

            log.info("Service ready, check status at http://localhost:{}/api/status", props.getApiPort());
        };
    }
}
