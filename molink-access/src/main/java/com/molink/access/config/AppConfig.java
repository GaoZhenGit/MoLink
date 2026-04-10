package com.molink.access.config;

import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Properties;

public class AppConfig {
    private int localPort;
    private int remotePort;
    private int apiPort;

    public AppConfig(int localPort, int remotePort, int apiPort) {
        this.localPort = localPort;
        this.remotePort = remotePort;
        this.apiPort = apiPort;
    }

    public static AppConfig load(int cliLocalPort, int cliRemotePort, int cliApiPort, String configPath) {
        Properties props = new Properties();
        try (InputStream in = new FileInputStream(configPath)) {
            props.load(in);
        } catch (IOException e) {
            // 配置文件不存在，使用默认值
        }

        // 优先级：环境变量 > 配置文件 > 命令行参数 > 默认值
        String localPortEnv = System.getenv("MOLINK_LOCAL_PORT");
        String remotePortEnv = System.getenv("MOLINK_REMOTE_PORT");
        String apiPortEnv = System.getenv("MOLINK_API_PORT");

        int localPort = localPortEnv != null ? Integer.parseInt(localPortEnv)
                : (props.getProperty("local.port") != null ? Integer.parseInt(props.getProperty("local.port"))
                : cliLocalPort);

        int remotePort = remotePortEnv != null ? Integer.parseInt(remotePortEnv)
                : (props.getProperty("remote.port") != null ? Integer.parseInt(props.getProperty("remote.port"))
                : cliRemotePort);

        int apiPort = apiPortEnv != null ? Integer.parseInt(apiPortEnv)
                : (props.getProperty("api.port") != null ? Integer.parseInt(props.getProperty("api.port"))
                : cliApiPort);

        return new AppConfig(localPort, remotePort, apiPort);
    }

    public int getLocalPort() {
        return localPort;
    }

    public int getRemotePort() {
        return remotePort;
    }

    public int getApiPort() {
        return apiPort;
    }

    public void setLocalPort(int localPort) {
        this.localPort = localPort;
    }

    public void setRemotePort(int remotePort) {
        this.remotePort = remotePort;
    }

    public void setApiPort(int apiPort) {
        this.apiPort = apiPort;
    }
}
