# MoLink 自动化测试脚本

## 使用方式

```cmd
test-all.bat
```

## 测试流程

| 步骤 | 操作 | 检查项 |
|------|------|--------|
| 1 | 构建 molink-worker | Gradle 构建成功 |
| 2 | 安装 molink-worker | APK 安装成功 |
| 3 | 启动 molink-worker | 应用启动成功 |
| 4 | 构建 molink-access | Maven 构建成功 |
| 5 | 启动 molink-access | 端口监听正常 |
| 6 | 测试 API | 接口响应正常 |

## 前提条件

- Android SDK (D:\AndroidSdk)
- Maven 3.6+
- JDK 8+
- ADB (Android SDK Platform Tools)
- Android 设备已连接并开启 USB 调试
