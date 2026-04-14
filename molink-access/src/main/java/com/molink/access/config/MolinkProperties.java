package com.molink.access.config;

import org.springframework.boot.context.properties.ConfigurationProperties;
import org.springframework.stereotype.Component;

@Component
@ConfigurationProperties(prefix = "molink")
@org.springframework.context.annotation.Primary
public class MolinkProperties {

    private int localPort = 1080;
    private int remotePort = 1080;
    private int apiPort = 8080;
    private String configPath = "config.properties";
    private String socksUsername = "";
    private String socksPassword = "";

    public int getLocalPort() {
        return localPort;
    }

    public void setLocalPort(int localPort) {
        this.localPort = localPort;
    }

    public int getRemotePort() {
        return remotePort;
    }

    public void setRemotePort(int remotePort) {
        this.remotePort = remotePort;
    }

    public int getApiPort() {
        return apiPort;
    }

    public void setApiPort(int apiPort) {
        this.apiPort = apiPort;
    }

    public String getConfigPath() {
        return configPath;
    }

    public void setConfigPath(String configPath) {
        this.configPath = configPath;
    }

    public String getSocksUsername() {
        return socksUsername;
    }

    public void setSocksUsername(String socksUsername) {
        this.socksUsername = socksUsername;
    }

    public String getSocksPassword() {
        return socksPassword;
    }

    public void setSocksPassword(String socksPassword) {
        this.socksPassword = socksPassword;
    }
}
