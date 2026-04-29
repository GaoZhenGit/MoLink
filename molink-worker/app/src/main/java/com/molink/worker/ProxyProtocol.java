package com.molink.worker;

public enum ProxyProtocol {
    SOCKS5("SOCKS5"),
    HTTP("HTTP");

    private final String displayName;

    ProxyProtocol(String displayName) {
        this.displayName = displayName;
    }

    public String getDisplayName() { return displayName; }

    public static ProxyProtocol fromString(String value) {
        if ("http".equalsIgnoreCase(value)) return HTTP;
        return SOCKS5;
    }
}
