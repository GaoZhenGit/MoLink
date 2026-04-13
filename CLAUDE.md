# MoLink

## Context

内网电脑需要通过 Android 设备代理上网。项目包含两个独立端：worker（Android）和 access（Windows）。通过 USB 连接，利用 dadb 库实现 ADB 协议通信，建立 SOCKS5 代理通道。

## 项目结构

两个独立项目目录：

```
D:/project/MoLink/
├── molink-worker/     # Android App
└── molink-access/     # Windows CLI + HTTP API
```

## 通用说明

- **dadb 库**：用于通过 ADB 协议进行 USB 通信，两端均依赖此库
- **配置优先级**：环境变量 > 配置文件 > 默认值
- **开发环境**：Windows 10，有 Android SDK

## 测试
- 本工程提供自动化测试脚本test\test.py，包含清理日志、停止服务、构建、运行服务、测试、停止服务

## 一、molink-worker（Android 端）

### 技术栈
- Android Studio + Gradle
- 纯 Java 开发
- **Gradle 版本：6.9.4，不要修改**
- **AGP 版本：不要修改**
- **Android 框架版本：不要修改**
- Android SDK：D:\AndroidSdk
- 最低支持 Android 8.1（API 27）

### 核心组件
- **Socks5ProxyService**：后台 Service，实现 SOCKS5 协议，仅支持 CONNECT 命令，默认端口 1080
- **MainActivity**：主界面，显示连接状态、本地端口、可开启/停止服务
- **AdbConnectionManager**：通过 dadb 库管理 ADB 连接（预留接口）

### 数据流
```
[access] → [USB/ADB] → [Socks5ProxyService] → [互联网]
```

## 二、molink-access（Windows 端）

### 技术栈
- Spring Boot（Java 8）
- 纯 Java 开发
- dadb 库实现 ADB 协议

### 运行条件
- 运行在 Windows 10 系统上
- 通过 USB 连接 Android 设备，该移动设备**无法被识别为 U 盘**，且**无法连接互联网**

### 核心组件
- **PortForwarder**：端口转发器，将本地端口转发到 worker 的 SOCKS5 端口，类似 `adb forward` 命令
- **AdbClient**：通过 dadb 库管理 USB 连接，支持自动重连
- **CliApplication**：CLI 入口，支持命令行参数
- **StatusController**：REST API 控制器（预留 Web 前端接口）

### REST API 接口

| 接口 | 方法 | 说明 |
|------|------|------|
| `/api/status` | GET | 获取连接状态、设备信息、重连次数等 |
| `/api/config` | GET | 获取当前配置 |
