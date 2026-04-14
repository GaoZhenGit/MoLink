# SOCKS5 Username/Password 认证实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Goal:** 在 Android Worker 端实现 RFC 1929 SOCKS5 用户名/密码认证，Access 端零改动完全透传
>
> **Architecture:** Worker 端（Socks5ProxyService）在 SOCKS5 握手阶段声明支持 User/Pass 认证（0x02），随后进行子协商验证。Access 端不做任何认证存储或处理，仅透传字节流。凭证通过 build.gradle 编译时注入 BuildConfig。
>
> **Tech Stack:** 纯 Java / Android（Socks5ProxyService），RFC 1929

---

## 文件变更概览

| 文件 | 动作 | 职责 |
|------|------|------|
| `molink-worker/app/src/main/java/com/molink/worker/Socks5ProxyService.java` | 修改 | 实现 RFC 1929 认证子协商逻辑 |
| `molink-worker/app/build.gradle` | 修改 | 新增 `SOCKS_USERNAME` / `SOCKS_PASSWORD` BuildConfig 字段 |

**molink-access 端无任何改动。**

---

## Task 1: build.gradle 新增凭证字段

**Files:**
- Modify: `molink-worker/app/build.gradle:14-15`

- [ ] 在 `defaultConfig` 块中，新增两个 `buildConfigField`：

```groovy
buildConfigField "String", "SOCKS_USERNAME", "\"admin\""
buildConfigField "String", "SOCKS_PASSWORD", "\"password\""
```

完整 `defaultConfig` 应为：

```groovy
defaultConfig {
    applicationId "com.molink.worker"
    minSdkVersion 27
    targetSdkVersion 31
    versionCode 1
    versionName "1.0"
    buildConfigField "int",    "SOCKS_PORT",           "1081"
    buildConfigField "int",    "STATUS_HTTP_PORT",     "8081"
    buildConfigField "String", "SOCKS_USERNAME",       "\"admin\""
    buildConfigField "String", "SOCKS_PASSWORD",      "\"password\""
}
```

- [ ] 验证：确认 `BuildConfig` 类中可访问 `SOCKS_USERNAME` 和 `SOCKS_PASSWORD`

---

## Task 2: Socks5ProxyService.java 实现 RFC 1929 认证

**Files:**
- Modify: `molink-worker/app/src/main/java/com/molink/worker/Socks5ProxyService.java:261-384`（handleClient 方法内部）

### 改动点 A：类顶部常量（新增 1 处）

在 `private static final int STATUS_HTTP_PORT = BuildConfig.STATUS_HTTP_PORT;` 后新增：

```java
private static final String SOCKS_USERNAME = BuildConfig.SOCKS_USERNAME;
private static final String SOCKS_PASSWORD = BuildConfig.SOCKS_PASSWORD;
```

### 改动点 B：握手阶段——声明支持 User/Pass 认证

找到 `handleClient` 方法中发送无认证响应的位置（第 282-285 行）：

```java
// 发送无认证响应
out.write(0x05);
out.write(0x00);
out.flush();
```

替换为：

```java
// 检查客户端是否支持用户名/密码认证（0x02）
boolean clientSupportsUserPass = false;
for (int i = 0; i < nMethods; i++) {
    if (methods[i] == 0x02) {
        clientSupportsUserPass = true;
        break;
    }
}

if (!clientSupportsUserPass) {
    // 客户端不支持认证，发送 no-auth 并拒绝连接
    Log.w(TAG, "Client does not support username/password auth");
    out.write(0x05);
    out.write(0xFF); // no acceptable methods
    out.flush();
    clientSocket.close();
    return;
}

// 发送 User/Pass 认证响应
out.write(0x05);
out.write(0x02); // METHOD_USER_PASSWORD
out.flush();
```

### 改动点 C：认证子协商阶段（新增方法 + 调用处）

在 `readFully` 方法之后、`forward` 方法之前，新增以下两个方法：

```java
/**
 * 处理 RFC 1929 用户名/密码认证子协商。
 * @param in  客户端输入流
 * @param out 客户端输出流
 * @return true=认证成功，false=认证失败（方法外部负责关闭连接）
 */
private boolean handleUserAuth(InputStream in, OutputStream out) throws IOException {
    int version = in.read();
    if (version != 0x01) {
        Log.w(TAG, "Unknown auth sub-negotiation version: " + version);
        return false;
    }

    int userLen = in.read();
    if (userLen < 0 || userLen > 255) {
        Log.w(TAG, "Invalid username length: " + userLen);
        return false;
    }

    byte[] userBytes = new byte[userLen];
    readFully(in, userBytes);
    String username = new String(userBytes);

    int passLen = in.read();
    if (passLen < 0 || passLen > 255) {
        Log.w(TAG, "Invalid password length: " + passLen);
        return false;
    }

    byte[] passBytes = new byte[passLen];
    readFully(in, passBytes);
    String password = new String(passBytes);

    // 凭证验证：空字符串视为未配置，跳过认证
    if (SOCKS_USERNAME.isEmpty() || SOCKS_PASSWORD.isEmpty()) {
        Log.i(TAG, "Auth not configured, skipping verification");
        out.write(0x01);
        out.write(0x00);
        out.flush();
        return true;
    }

    boolean success = SOCKS_USERNAME.equals(username) && SOCKS_PASSWORD.equals(password);
    out.write(0x01);
    out.write(success ? 0x00 : 0x01); // 0x00=成功，0x01=失败
    out.flush();

    if (success) {
        Log.i(TAG, "Auth success for user: " + username);
    } else {
        Log.w(TAG, "Auth failed for user: " + username);
    }
    return success;
}
```

### 改动点 D：在 handleClient 中调用认证方法

在"读取连接请求"代码块之前（第 288 行 `ver = in.read();` 之前），插入：

```java
// RFC 1929 用户名/密码认证子协商
if (!handleUserAuth(in, out)) {
    // 认证失败，handleUserAuth 已发送失败响应，关闭连接
    clientSocket.close();
    return;
}
```

### 改动点 E：SOCKS5H 兼容性确认

CONNECT 阶段的 `addrType == 0x03` 域名处理（第 318-322 行）**保持不变**，DNS 解析仍在 Worker 端。认证子协商和地址类型是两个独立的 SOCKS5 机制，互不影响。

### 改动后 handleClient 完整流程（骨架）

```
handleClient(clientSocket)
  → read VER(0x05) + NMETHODS + METHODS
  → if (methods包含0x02) send 0x05|0x02 else send 0x05|0xFF → close
  → handleUserAuth(in, out)  ← 新增
    → read 0x01|ULEN|UNAME|PLEN|PASSWD
    → compare → send 0x01|STATUS(0x00/0x01)
    → return true/false
  → if (!authSuccess) → close
  → read CONNECT request (VER|CMD|RSV|ATYP|DESTADDR|DESTPORT)  ← 原逻辑不变
  → ...
```

---

## Task 3: 验证方案

### 手动测试步骤（Android 模拟器或真机）

1. **重新编译 Worker**（Android Studio Build → Rebuild Project）

2. **安装并启动 Worker App，开启 SOCKS5 服务**

3. **正确凭证测试**
   ```bash
   curl --socks5 username:password@localhost:1080 http://www.example.com
   # 期望：正常返回 HTML
   ```

4. **错误凭证测试**
   ```bash
   curl --socks5 wronguser:wrongpass@localhost:1080 http://www.example.com
   # 期望：连接被拒，无响应
   ```

5. **无认证请求测试**（旧版客户端）
   ```bash
   curl --socks5@localhost:1080 http://www.example.com
   # 期望：连接被拒（SOCKS5H + 无认证被 Worker 拒绝）
   ```

6. **SOCKS5H 域名解析验证**
   - 在 Worker 端抓包：`adb shell tcpdump -i any port 1081 -n`
   - 确认目的地址为域名（如 `www.example.com`），DNS 解析发生在 Worker 而非 Access 端

### 回归测试

- 不带凭证的请求在**未配置凭证**（空字符串）时应通过——用于临时禁用认证的场景

---

## 边界情况处理汇总

| 场景 | 处理方式 |
|------|----------|
| 客户端只发送 0x00（无认证） | 发送 0x05\|0xFF，关闭连接 |
| 认证子协商 version != 0x01 | 发送失败，关闭连接 |
| ULEN 或 PLEN 超出 0-255 | 发送失败，关闭连接 |
| 用户名密码不匹配 | 发送 0x01\|0x01，关闭连接 |
| 凭证未配置（空字符串） | 回退无认证，发送 0x01\|0x00（成功），**慎用** |

---

## 自查清单

- [ ] spec coverage：Worker 端认证 ✓，Access 零改动 ✓，SOCKS5H 兼容 ✓，配置文件新增字段 ✓
- [ ] 无 placeholder：所有代码段均为完整可运行代码
- [ ] 类型一致性：BuildConfig 字段名 `SOCKS_USERNAME` / `SOCKS_PASSWORD` 在两处使用一致
- [ ] git commit：**禁止提交**，由用户自行决定提交时机
