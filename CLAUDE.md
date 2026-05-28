# MoLink

## Context

内网电脑需要通过 Android 设备代理上网。项目包含两个独立端：worker（Android）和 access（Windows C++）。通过 USB 连接，实现 ADB 协议通信，建立 SOCKS5 代理通道。

## 项目结构

两个独立项目目录：

```
D:/project/MoLink/
├── molink-worker/         # Android App
└── molink-access/     # Windows CLI (C++, 自实现 ADB 协议栈)
```

## 通用说明

- **自实现 ADB 协议栈**：libusb + BCrypt RSA，零第三方 DLL 依赖，静态编译
- **开发环境**：Windows 10，MinGW-w64 64-bit，CMake 3.14+
- **构建脚本**：`molink-access/clean_build.ps1`（停服→清理→编译→恢复 key）
- **配置优先级**：环境变量 > 配置文件 > 默认值

## 技术文档

> 架构、命令参考、日志规范、密钥管理等详细技术文档见 `docs/project.md`。
