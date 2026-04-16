package com.molink.access;

import com.molink.access.adb.AdbClientManager;
import com.molink.access.config.MolinkProperties;
import com.molink.access.manager.DeviceManager;
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
    public DeviceManager deviceManager(AdbClientManager adbClient, MolinkProperties props) {
        return new DeviceManager(adbClient, props.getRemotePort(),
                props.getSocksUsername(), props.getSocksPassword());
    }

    @Bean
    public CommandLineRunner runner(MolinkProperties props, DeviceManager deviceManager) {
        return args -> {
            log.info("=== MoLink Access Starting ===");
            log.info("Remote port: {}", props.getRemotePort());
            log.info("API port: {}", props.getApiPort());

            deviceManager.start();

            log.info("Service ready, check devices at http://localhost:{}/molink/devices", props.getApiPort());
        };
    }
}
