# MoLink 项目可行性分析报告

## 一、Context（背景与目标）

**项目目标**：开发一个允许内网电脑通过 USB 连接的 Android 手机代理上网的工具。

- **access 端**（Windows PC）：Spring Boot + 纯 Java + dadb 库，提供 TCP 端口转发
- **worker 端**（Android 8.1+）：原生 Android 应用，提供 SOCKS5 代理服务

**当前状态**：项目仅有需求文档，无任何代码。

---

## 二、可行性分析结论

### 2.1 总体评估：✅ 可行

| 组件 | 技术方案 | 可行性 | 关键依赖 |
|------|---------|--------|---------|
| **access USB通信** | dadb 库 | ✅ 可行 | 需 ADB Server |
| **access 端口转发** | Java NIO + Netty | ✅ 可行 | Netty 4.x |
| **worker SOCKS5** | Netty 内置 codec | ✅ 可行 | Android 8.1+ |
| **整体架构** | 多模块 Gradle | ✅ 可行 | Gradle 8.x |

### 2.2 技术风险与应对

| 风险 | 等级 | 应对 |
|------|------|------|
| dadb 库活跃度低 | 中 | 备用方案：直接调用 adb.exe 进程 |
| USB ADB 带宽受限 (~10MB/s) | 低 | ADB 转发已足够代理上网 |
| Android 设备 USB 权限 | 中 | 引导用户开启 USB 调试 |
| 多设备并行连接冲突 | 低 | 独立 adb forward 分配端口 |

### 2.3 dadb 库验证建议

在正式使用前，建议验证 dadb 库的关键能力：
1. 是否支持 `openStream()` 建立透明字节流隧道
2. 是否支持多设备并行连接
3. 断线重连机制

**备选方案**：如 dadb 不满足需求，可考虑：
- 直接调用 `adb forward` + `adb shell nc` 管道
- ddmlib（Android SDK 官方库，但体积较大）

---

## 三、项目结构设计

```
D:\project\MoLink\
├── build.gradle.kts              # 根级构建（Gradle Kotlin DSL）
├── settings.gradle.kts           # 包含 access 和 worker 模块
│
├── molink-access/               # Windows 端（Spring Boot）
│   └── src/main/java/com/molink/access/
│       ├── AccessApplication.java
│       ├── config/
│       │   └── AdbConfig.java            # ADB Server 生命周期管理
│       ├── service/
│       │   ├── AdbService.java           # dadb 封装层
│       │   └── ProxyService.java         # TCP 端口转发核心
│       └── controller/
│           └── DeviceController.java     # REST API（设备管理）
│
└── molink-worker/               # Android 端
    └── src/main/java/com/molink/worker/
        ├── WorkerApp.java               # Application 类
        ├── service/
        │   ├── Socks5ProxyService.java   # SOCKS5 代理（Netty）
        │   └── AdbReverseService.java    # ADB 反向端口转发
        └── ui/
            └── MainActivity.java        # UI 界面
```

---

## 四、核心实现方案

### 4.1 access 端（Windows）

**依赖**：
```groovy
// molink-access/build.gradle.kts
dependencies {
    implementation("org.springframework.boot:spring-boot-starter-web")
    implementation("io.netty:netty-all:4.1.108.Final")
    implementation("com.github.mobilelive:dadb:1.0.0")  // 待验证
}
```

**端口转发数据流**：
```
[内网应用] → localhost:10808 → [Spring Boot/Netty] → dadb隧道 → [Android SOCKS5:1080] → 互联网
```

### 4.2 worker 端（Android）

**依赖**：
```groovy
// molink-worker/build.gradle.kts
dependencies {
    implementation("io.netty:netty-all:4.1.108.Final")
    // AndroidX + Material UI
}
```

**SOCKS5 实现**：使用 Netty 内置 `io.netty.handler.codec.socks` codec，参考 Netty 官方 SOCKS5 示例。

---

## 五、实施计划（分阶段）

### Phase 1：项目骨架与 access 端核心
1. 初始化 Gradle 多模块项目
2. 搭建 access 端 Spring Boot 项目
3. 集成 dadb 库，验证 USB 设备连接
4. 实现 `AdbService`（dadb 封装层）
5. 实现 `ProxyService`（NIO 端口转发）

### Phase 2：worker 端 SOCKS5 实现
6. 搭建 Android 项目骨架
7. 使用 Netty 实现 SOCKS5 代理服务
8. 实现 `AdbReverseService`
9. 开发 Android UI（状态显示、开/关控制）

### Phase 3：集成测试与优化
10. 端到端集成测试
11. USB 断线重连机制
12. 打包与发布

---

## 六、验证方案

### 验证步骤：
1. **access 端**：启动 Spring Boot，验证能否通过 dadb 连接 Android 设备
2. **worker 端**：安装 APK，验证 SOCKS5 服务在指定端口监听
3. **端到端**：浏览器配置 SOCKS5 代理 → 访问网站，验证流量走 Android 网络
4. **性能测试**：测量实际吞吐量

### 测试命令（手动验证）：
```bash
# access 端启动后验证
curl http://localhost:8080/devices

# worker 端验证（Android shell）
adb shell nc localhost 1080
# 手动发送 SOCKS5 Greeting 包测试
```

---

## 七、关键文件清单

| 文件路径 | 用途 |
|---------|------|
| `molink-access/src/.../service/AdbService.java` | dadb 封装，USB 通信入口 |
| `molink-access/src/.../service/ProxyService.java` | TCP 端口转发核心逻辑 |
| `molink-worker/src/.../service/Socks5ProxyService.java` | SOCKS5 代理实现 |
| `molink-worker/src/.../service/AdbReverseService.java` | ADB 反向端口转发 |

---

## 八、待确认事项

在实施前需要确认：
1. **dadb 库具体版本和 API 稳定性**（建议 clone 源码验证）
2. **目标 Android 设备是否已开启 USB 调试模式**
3. **是否需要支持多设备并行连接**（影响 access 端架构）→ ✅ **已确认：单设备优先**
4. **是否需要用户认证机制**（SOCKS5 用户名/密码认证）
