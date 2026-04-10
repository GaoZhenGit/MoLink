# MoLink Access

Windows 端端口转发工具，通过 USB 连接 Android 设备实现 SOCKS5 代理。

## 前置条件

- JDK 8+
- Maven 3.6+
- Android 设备通过 USB 连接，已开启 USB 调试

## 快速开始

### 1. 构建
```bash
build.bat
```
> 构建前会自动停止占用端口 8080 和 1080 的进程

### 2. 启动
```bash
start.bat
```

### 3. 测试
```bash
test.bat
```
> 确认服务已启动并可正常响应 API 请求

## 脚本说明

| 脚本 | 功能 |
|------|------|
| `build.bat` | 停止占用进程，清理并构建项目 |
| `start.bat` | 停止占用进程，启动 molink-access |
| `test.bat` | 检查端口监听，调用 API 测试 |

## 配置

### 配置文件 (config.properties)
```properties
local.port=1080
remote.port=1080
api.port=8080
```

### 环境变量
设置环境变量可覆盖配置文件和命令行参数：

```cmd
set MOLINK_LOCAL_PORT=1080
set MOLINK_REMOTE_PORT=1080
set MOLINK_API_PORT=8080
```

### 优先级
环境变量 > 配置文件 > 命令行参数

## API 接口

| 接口 | 方法 | 说明 |
|------|------|------|
| `/api/status` | GET | 获取连接状态、设备信息、重连次数等 |
| `/api/config` | GET | 获取当前配置 |
| `/api/config` | PUT | 更新配置（部分参数） |

### 状态响应示例
```json
{
  "connected": true,
  "deviceSerial": "1234567890",
  "localPort": 1080,
  "remotePort": 1080,
  "reconnectCount": 0,
  "uptime": 3600
}
```

## 手动运行

```bash
# 构建
mvn clean package -DskipTests

# 启动
java -Dmolink.local.port=1080 -Dmolink.remote.port=1080 -Dmolink.api.port=8080 -jar target\molink-access-1.0.0.jar
```
