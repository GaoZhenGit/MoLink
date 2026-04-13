# MoLink Worker UI 改版设计

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 worker Android 界面从简单的"标题+状态文字+按钮"改造为信息密集的实时状态面板，包含服务状态、流量统计、连接日志。

**Architecture:** 单页垂直布局。顶部为固定摘要区（LinearLayout），下方为连接日志列表（RecyclerView），底部为控制按钮。数据源为 Socks5ProxyService，通过广播/回调更新 UI。RecyclerView 最多保留 50 条日志，超出自动淘汰旧条目，保持内存恒定。

**Tech Stack:** Android SDK 27+，RecyclerView，Handler+Runnable 定时刷新（每 2 秒），Socks5ProxyService 现有 public API。

---

## 一、布局结构（activity_main.xml）

```
┌─────────────────────────────────────┐  ← ScrollView（可滚动）
│  [服务状态区]                        │  ← LinearLayout（固定）
│  ● 运行中    端口:1080  在线:02:34  │
├─────────────────────────────────────┤
│  [统计指标区]                        │  ← LinearLayout（固定）
│  当前连接:3   历史:47               │
│  ↓ 2.3 MB    ↑ 1.1 MB              │
├─────────────────────────────────────┤
│  [连接日志标题]  "最近连接"          │  ← TextView
├─────────────────────────────────────┤
│  [RecyclerView — 连接日志列表]       │  ← RecyclerView（恒定高度）
│  [连接条目] 10.0.0.xx → google.com  │
│  [连接条目] 192.168.x.x → baidu.com │
│  ...（最多50条，超出淘汰底部）       │
└─────────────────────────────────────┘
│  [ 停止服务 ]                       │  ← Button（固定底部）
└─────────────────────────────────────┘
```

**性能约束：**
- `RecyclerView` item count 上限 = 50，超出时移除最早的 item
- `RecyclerView` 高度设为固定 dp 值（如 300dp），而非 `match_parent`，防止内容无限扩展
- 整体外层使用 `ScrollView` 包裹，摘要区始终可见

---

## 二、数据模型

### ConnectionRecord（连接记录）

```java
public class ConnectionRecord {
    public final String clientIp;      // 客户端 IP，如 "192.168.1.100"
    public final String targetHost;    // 目标域名或 IP
    public final int targetPort;        // 目标端口
    public final long bytesDown;       // 下载字节数
    public final long bytesUp;         // 上传字节数
    public final long startTime;       // 连接建立时间（SystemClock.elapsedRealtime()）
    public final long durationMs;      // 持续时长
    public final boolean active;       // 是否仍活跃
}
```

### Socks5ProxyService 新增字段

| 字段/方法 | 类型 | 说明 |
|-----------|------|------|
| `historyCount` | `AtomicInteger` | 历史总连接数（启动以来） |
| `totalBytesDown` | `AtomicLong` | 累计下载字节数 |
| `totalBytesUp` | `AtomicLong` | 累计上传字节数 |
| `activeConnections` | `CopyOnWriteArrayList<ConnectionRecord>` | 当前活跃连接列表 |
| `getHistoryCount()` | `int` | 返回 historyCount |
| `getTotalBytesDown()` | `long` | 返回 totalBytesDown |
| `getTotalBytesUp()` | `long` | 返回 totalBytesUp |
| `getActiveConnections()` | `List<ConnectionRecord>` | 返回活跃连接快照（copy） |
| `addConnection(ConnectionRecord)` | `void` | 新连接接入时调用 |
| `removeConnection(ConnectionRecord)` | `void` | 连接断开时调用 |
| `updateBytes(long down, long up)` | `void` | 更新当前连接的流量计数 |

---

## 三、Activity 与 Service 通信

由于 Socks5ProxyService 是 `Started Service`（非 Bound Service），MainActivity 通过以下方式获取数据：

- **方式**：在 Socks5ProxyService 中注册一个静态回调接口 `StatusListener`
- **更新频率**：Service 每 2 秒通过 `Handler.post()` 向 UI 发消息
- **MainActivity**：持有 `ServiceConnection` + `IBinder`（即 LocalBinder），获取 service 引用后注册 listener
- **Fallback**：若 service 未启动，Activity 显示"服务未运行"状态

```java
// Socks5ProxyService 中
public interface StatusListener {
    void onStatusUpdate(long uptime, int connectionCount, int historyCount,
                        long bytesDown, long bytesUp,
                        List<ConnectionRecord> activeConnections);
}
private final CopyOnWriteArrayList<StatusListener> listeners = new CopyOnWriteArrayList<>();
```

---

## 四、连接日志 RecyclerView 设计

### item_connection_log.xml

```
┌────────────────────────────────────────────────────────┐
│ [●] 192.168.1.23 → api.github.com:443  ↓ 341 KB  12s  │
└────────────────────────────────────────────────────────┘
```

- 活跃连接：圆点为绿；断开连接：圆点为灰
- 时长用"Xs"（<60s）或"Xm"（<60m）或"Xh"（>=60m）显示
- 下载字节数超过 1MB 显示"X.X MB"，否则"X KB"

### 行为

- 新连接插入列表顶部（`adapter.notifyItemInserted(0)`，然后滚动到顶部）
- 活跃连接断开后：更新 item 样式（圆点变灰），不删除（保留在列表中最多 N 条）
- 列表上限 50 条；插入第 51 条时调用 `adapter.removeLast()`
- 点击条目：无操作（暂不实现）

---

## 五、字节数统计注入点

在 Socks5ProxyService 处理 SOCKS5 CONNECT 命令时：

```
收到 CONNECT 请求
  → historyCount.incrementAndGet()
  → 创建 ConnectionRecord（active=true）
  → activeConnections.add(record)
  → addConnection(record) → 通知所有 listener
```

在读写数据时（`forward()` 线程中，行 313）：

```
每次 in.read(buffer) 读到数据块（direction="c->s" 即下载，"s->c" 即上传）
  → totalBytesDown/totalBytesUp.addAndGet(n)
  → record.bytesDown/bytesUp += n
```

**实现细节**：forward() 方法中 `out.write(buffer, 0, len)` 即字节流出处，是字节数统计的精确注入点。handleClient() 在 t1.join() 和 t2.join() 后将对应 ConnectionRecord 从 activeConnections 移除并标记 active=false。

**注意**：forward() 中每次小块都更新 AtomicLong 会有锁竞争，建议在 ConnectionRecord 中用 AtomicLong，并在 forward() 中直接 update；UI 刷新由 Service 的 Handler 控制在每 2 秒批量推送。

---

## 六、界面刷新策略

| 更新内容 | 频率 | 方式 |
|----------|------|------|
| 服务状态（运行/停止） | 按钮操作时 | 立即刷新 |
| 在线时长（uptime） | 每 2 秒 | Handler.post |
| 当前连接数 | 每 2 秒 | Handler.post |
| 历史连接数 | 每次新连接 | 直接回调 |
| 累计流量 | 每 2 秒 | Handler.post |
| 连接日志列表 | 每次连接变化 | adapter.notifyItemInserted |

---

## 七、图标资源（高清）

### 7.1 通知栏图标 — `ic_notification.xml`（Vector Drawable）

路径：`molink-worker/app/src/main/res/drawable/ic_notification.xml`

通知栏图标必须为 **单色（白色/透明）** 且最大尺寸 24dp×24dp。采用 XML Vector Drawable，一个文件覆盖所有分辨率，不会模糊。

图标设计：圆形底座 + 双向箭头，表示数据双向流通（proxy 代理方向）。

```xml
<?xml version="1.0" encoding="utf-8"?>
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    android:width="24dp"
    android:height="24dp"
    android:viewportWidth="24"
    android:viewportHeight="24"
    android:tint="#FFFFFF">
    <!-- 圆形底座 -->
    <path
        android:fillColor="#FFFFFF"
        android:pathData="M12,2C6.48,2 2,6.48 2,12s4.48,10 10,10 10,-4.48 10,-10S17.52,2 12,2z"/>
    <!-- 向下箭头（下载） -->
    <path
        android:fillColor="#333333"
        android:pathData="M12,5v10M8,11l4,4 4,-4"/>
    <!-- 向上箭头（上传，较小） -->
    <path
        android:fillColor="#333333"
        android:pathData="M12,16v2M10,17.5l2,-1.5 2,1.5"/>
</vector>
```

**说明**：NotificationCompat 默认会将 vector drawable 渲染为白色（tint），需在 `setSmallIcon()` 时保证 drawable 中填充色为非透明，Android 自动 tint 为白色。通知栏图标路径用 `R.drawable.ic_notification` 替换原 `android.R.drawable.ic_dialog_info`。

### 7.2 应用启动图标（mipmap）

路径：`molink-worker/app/src/main/res/mipmap-anydpi-v26/ic_launcher.xml`（adaptive icon）
前景：`molink-worker/app/src/main/res/drawable/ic_launcher_foreground.xml`
背景：`molink-worker/app/src/main/res/values/ic_launcher_background.xml`

设计风格与通知栏图标一致：圆形底座 + 双向箭头。

**重要 — 测试约束**：实现期间不得修改 Socks5ProxyService.java 中的代理逻辑（握手、数据转发 forward() 等），仅允许新增字段、回调接口和通知图标替换。自动化测试脚本 test/test.py 优先通过，任何影响代理功能的改动都必须可逆。

### 7.3 Git 提交约束

本项目明确禁止在未经用户明确授权的情况下进行 git 提交。所有代码改动仅保存在工作目录，待用户确认后再统一提交。

---

## 八、兼容性处理

- **服务未启动时**：Activity 显示所有字段为"--"或"0"，按钮文字为"启动服务"
- **服务已启动时**：按钮文字为"停止服务"
- **Activity 与 Service 绑定失败**：`isServiceBound = false`，按未启动处理
- **无网络权限**：`ConnectionRecord.targetHost` 显示为 IP（跳过 DNS 解析）

---

## 九、测试验证

**核心约束**：Socks5ProxyService 的代理逻辑（握手、SOCKS 命令处理、forward 转发）不得改动。所有测试基于现有 proxy 功能，UI 改动不影响代理正确性。

1. **启动服务**：点击按钮，服务状态变为"运行中"，端口/在线时长刷新
2. **E2E 代理**：通过 curl 经过 access → worker 访问外网，验证流量计数增加
3. **连接日志**：建立 3 个并发连接，验证 3 条日志均出现在列表顶部
4. **断开连接**：关闭 curl，验证日志条目圆点变为灰色
5. **历史计数**：多次连接后，历史计数单调递增
6. **日志上限**：建立超过 50 个连接，验证底部旧日志被移除
7. **停止服务**：点击按钮，服务停止，所有字段恢复默认值

---

## 十、文件清单

| 操作 | 路径 |
|------|------|
| 新建 | `molink-worker/app/src/main/res/drawable/ic_notification.xml` |
| 新建 | `molink-worker/app/src/main/res/drawable/ic_launcher_foreground.xml` |
| 新建 | `molink-worker/app/src/main/res/values/ic_launcher_background.xml` |
| 新建 | `molink-worker/app/src/main/res/mipmap-anydpi-v26/ic_launcher.xml` |
| 新建 | `molink-worker/app/src/main/res/layout/activity_main.xml`（重写） |
| 新建 | `molink-worker/app/src/main/res/layout/item_connection_log.xml` |
| 新建 | `molink-worker/app/src/main/java/com/molink/worker/ConnectionRecord.java` |
| 新建 | `molink-worker/app/src/main/java/com/molink/worker/ConnectionLogAdapter.java` |
| 修改 | `molink-worker/app/src/main/java/com/molink/worker/MainActivity.java` |
| 修改 | `molink-worker/app/src/main/java/com/molink/worker/Socks5ProxyService.java` |
