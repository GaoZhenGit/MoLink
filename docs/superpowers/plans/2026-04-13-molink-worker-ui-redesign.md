# MoLink Worker UI 改版实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 worker Android 界面改造为信息密集实时状态面板，同时升级高清图标，代理逻辑零改动。

**Architecture:** 单页垂直布局（LinearLayout + ScrollView），顶部固定摘要区，下部恒高 RecyclerView，底部控制按钮。Socks5ProxyService 通过 StatusListener 回调向 MainActivity 推送状态更新，每 2 秒一次。

**Tech Stack:** Android SDK 27+，RecyclerView，Handler+Runnable 定时刷新，Vector Drawable，Adaptive Icon。

---

## 约束提醒

- **代理逻辑零改动**：Socks5ProxyService.handleClient() 和 forward() 方法仅新增统计字段，不得改动握手/SOCKS 命令处理/数据转发逻辑
- **Git 零提交**：所有改动仅保存在工作目录，不执行 git commit
- **自动化测试优先**：每次修改后优先运行 test/test.py 验证 proxy 功能正常

---

## 依赖关系

```
Task 1-3（图标准备）
    ↓
Task 4（数据模型 ConnectionRecord）
    ↓
Task 5（Service 新增字段和回调）
    ↓
Task 6（布局文件 activity_main.xml）
    ↓
Task 7（布局文件 item_connection_log.xml）
    ↓
Task 8（Adapter ConnectionLogAdapter）
    ↓
Task 9（MainActivity 改造）
    ↓
Task 10（E2E 自动化测试验证）
```

---

## Task 1: 创建通知栏图标 Vector Drawable

**Files:**
- Create: `molink-worker/app/src/main/res/drawable/ic_notification.xml`

- [ ] **Step 1: 创建 ic_notification.xml**

路径：`molink-worker/app/src/main/res/drawable/ic_notification.xml`

注意：Android 通知栏图标必须是**单色 alpha 通道图**（白色轮廓+透明背景），不能有实心填充圆。系统会自动将图标渲染为白色。

```xml
<?xml version="1.0" encoding="utf-8"?>
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    android:width="24dp"
    android:height="24dp"
    android:viewportWidth="24"
    android:viewportHeight="24">
    <!-- 向下箭头：c->s（下载），即客户端通过代理下载数据 -->
    <path
        android:fillColor="#FFFFFF"
        android:pathData="M12,4L12,14M7,9L12,14L17,9"
        android:strokeWidth="2"
        android:strokeColor="#FFFFFF"
        android:strokeLineCap="round"
        android:strokeLineJoin="round"/>
    <!-- 向上箭头：s->c（上传），即代理将数据返回客户端 -->
    <path
        android:fillColor="#FFFFFF"
        android:pathData="M12,20L12,10M7,15L12,10L17,15"
        android:strokeWidth="2"
        android:strokeColor="#FFFFFF"
        android:strokeLineCap="round"
        android:strokeLineJoin="round"/>
</vector>
```

- [ ] **Step 2: 验证文件存在**

检查 `molink-worker/app/src/main/res/drawable/ic_notification.xml` 已创建，文件内容正确。

---

## Task 2: 创建启动图标前景 Vector Drawable

**Files:**
- Create: `molink-worker/app/src/main/res/drawable/ic_launcher_foreground.xml`

- [ ] **Step 1: 创建 ic_launcher_foreground.xml**

路径：`molink-worker/app/src/main/res/drawable/ic_launcher_foreground.xml`

前景图标允许带颜色，与通知栏图标不同，采用渐变风格 + 圆形底座 + 双向箭头。

```xml
<?xml version="1.0" encoding="utf-8"?>
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    android:width="108dp"
    android:height="108dp"
    android:viewportWidth="108"
    android:viewportHeight="108">
    <!-- 圆形底座 -->
    <path
        android:fillColor="#2196F3"
        android:pathData="M54,54m-50,0a50,50 0,1 1,100 0a50,50 0,1 1,-100 0"/>
    <!-- 向下箭头（下载） -->
    <path
        android:fillColor="#FFFFFF"
        android:pathData="M54,30L54,62M36,46L54,62L72,46"
        android:strokeWidth="5"
        android:strokeColor="#FFFFFF"
        android:strokeLineCap="round"
        android:strokeLineJoin="round"/>
    <!-- 向上箭头（上传） -->
    <path
        android:fillColor="#FFFFFF"
        android:pathData="M54,78L54,46M36,62L54,46L72,62"
        android:strokeWidth="5"
        android:strokeColor="#FFFFFF"
        android:strokeLineCap="round"
        android:strokeLineJoin="round"/>
</vector>
```

- [ ] **Step 2: 验证文件存在**

检查 `molink-worker/app/src/main/res/drawable/ic_launcher_foreground.xml` 已创建。

---

## Task 3: 创建启动图标背景和 Adaptive Icon 配置

**Files:**
- Create: `molink-worker/app/src/main/res/values/ic_launcher_background.xml`
- Create: `molink-worker/app/src/main/res/mipmap-anydpi-v26/ic_launcher.xml`

- [ ] **Step 1: 创建 ic_launcher_background.xml**

路径：`molink-worker/app/src/main/res/values/ic_launcher_background.xml`

```xml
<?xml version="1.0" encoding="utf-8"?>
<resources>
    <color name="ic_launcher_background">#2196F3</color>
</resources>
```

- [ ] **Step 2: 创建 mipmap-anydpi-v26 目录**

路径：`molink-worker/app/src/main/res/mipmap-anydpi-v26/`

在 Windows 上使用 mkdir 创建目录：

```
mkdir "D:\project\MoLink\molink-worker\app\src\main\res\mipmap-anydpi-v26"
```

- [ ] **Step 3: 创建 ic_launcher.xml（adaptive icon 配置）**

路径：`molink-worker/app/src/main/res/mipmap-anydpi-v26/ic_launcher.xml`

```xml
<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@color/ic_launcher_background"/>
    <foreground android:drawable="@drawable/ic_launcher_foreground"/>
</adaptive-icon>
```

- [ ] **Step 4: 验证所有图标文件存在**

确认以下三个文件均已创建：
- `res/drawable/ic_notification.xml`
- `res/drawable/ic_launcher_foreground.xml`
- `res/mipmap-anydpi-v26/ic_launcher.xml`

---

## Task 4: 替换通知图标（验证构建）

**Files:**
- Modify: `molink-worker/app/src/main/java/com/molink/worker/Socks5ProxyService.java:132-138`

- [ ] **Step 1: 将通知图标从系统图标替换为自定义图标**

找到 `createNotification()` 方法中的：

```java
.setSmallIcon(android.R.drawable.ic_dialog_info)
```

替换为：

```java
.setSmallIcon(R.drawable.ic_notification)
```

在文件顶部（其他 import 之后）确认已存在：

```java
import com.molink.worker.R;
```

**如果不存在**，在现有 import 语句最后添加：

```java
import com.molink.worker.R;
```

- [ ] **Step 2: 验证构建（增量，不 clean）**

在 `molink-worker` 目录下运行：

```
gradlew.bat assembleDebug --no-daemon
```

预期：BUILD SUCCESSFUL，无资源引用错误。

---

## Task 5: 创建数据模型 ConnectionRecord

**Files:**
- Create: `molink-worker/app/src/main/java/com/molink/worker/ConnectionRecord.java`

- [ ] **Step 1: 创建 ConnectionRecord.java**

路径：`molink-worker/app/src/main/java/com/molink/worker/ConnectionRecord.java`

```java
package com.molink.worker;

import java.util.concurrent.atomic.AtomicLong;

/**
 * 连接记录数据模型。
 * bytesDown/bytesUp 使用 AtomicLong，支持 forward() 线程安全更新。
 */
public class ConnectionRecord {

    public final String clientIp;
    public final String targetHost;
    public final int targetPort;
    public final long startTime;
    public final AtomicLong bytesDown = new AtomicLong(0);
    public final AtomicLong bytesUp = new AtomicLong(0);
    public volatile boolean active = true;

    public ConnectionRecord(String clientIp, String targetHost, int targetPort, long startTime) {
        this.clientIp = clientIp;
        this.targetHost = targetHost;
        this.targetPort = targetPort;
        this.startTime = startTime;
    }

    public long getBytesDown() { return bytesDown.get(); }
    public long getBytesUp() { return bytesUp.get(); }

    public void addBytesDown(long n) { bytesDown.addAndGet(n); }
    public void addBytesUp(long n) { bytesUp.addAndGet(n); }

    public long getDurationSec() {
        return (System.currentTimeMillis() - startTime) / 1000;
    }

    public String getDisplayHost() {
        return targetHost + ":" + targetPort;
    }
}
```

- [ ] **Step 2: 验证文件创建成功**

确认 `ConnectionRecord.java` 文件存在，编译无错误（通过 Gradle 构建验证）。

---

## Task 6: Socks5ProxyService 新增统计字段和回调接口

**Files:**
- Modify: `molink-worker/app/src/main/java/com/molink/worker/Socks5ProxyService.java`

**重要**：以下修改仅在现有类中**追加**字段和方法，handleClient() 的 SOCKS5 握手/命令处理逻辑和 forward() 的数据转发逻辑**完全不变**。

- [ ] **Step 1: 添加新的 import**

在现有 import 区（`java.util.concurrent.atomic` 之后）添加：

```java
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
```

- [ ] **Step 2: 添加统计字段（在现有字段区，即 connectionCount 附近）**

在 `private final AtomicInteger connectionCount = new AtomicInteger(0);` 之后添加：

```java
    // ========== UI 统计相关 ==========
    private final AtomicInteger historyCount = new AtomicInteger(0);
    private final AtomicLong totalBytesDown = new AtomicLong(0);
    private final AtomicLong totalBytesUp = new AtomicLong(0);
    private final CopyOnWriteArrayList<ConnectionRecord> activeConnections = new CopyOnWriteArrayList<>();

    /** 静态回调接口，MainActivity 通过 LocalBinder 注册 */
    public interface StatusListener {
        void onStatusUpdate(long uptime, int connectionCount, int historyCount,
                            long bytesDown, long bytesUp,
                            List<ConnectionRecord> activeConnections);
    }
    private final CopyOnWriteArrayList<StatusListener> statusListeners = new CopyOnWriteArrayList<>();

    public void addStatusListener(StatusListener listener) {
        if (listener != null) statusListeners.add(listener);
    }
    public void removeStatusListener(StatusListener listener) {
        statusListeners.remove(listener);
    }

    private void notifyStatusUpdate() {
        List<ConnectionRecord> snapshot = new ArrayList<>(activeConnections);
        for (StatusListener l : statusListeners) {
            try {
                l.onStatusUpdate(getUptime(), connectionCount.get(), historyCount.get(),
                        totalBytesDown.get(), totalBytesUp.get(), snapshot);
            } catch (Exception e) {
                // ignore
            }
        }
    }
    // ==================================
```

- [ ] **Step 3: 在 handleClient() 中注入连接记录逻辑（仅追加，不改现有代码）**

在 `handleClient()` 方法中，找到这行（连接建立成功响应发出之后，双向转发之前）：

```java
Log.i(TAG, "Connection established to " + destAddr + ":" + destPort);
```

在其**后**，在 `t1.start()` 和 `t2.start()` 之前添加：

```java
            // === UI 统计：记录新连接 ===
            historyCount.incrementAndGet();
            String clientIp = clientSocket.getRemoteSocketAddress().toString();
            // 去掉开头的 "/" prefix
            if (clientIp.startsWith("/")) clientIp = clientIp.substring(1);
            final ConnectionRecord record = new ConnectionRecord(clientIp, destAddr, destPort, System.currentTimeMillis());
            activeConnections.add(record);
            // === 统计注入结束 ===
```

- [ ] **Step 4: 在 forward() 中注入字节数统计（仅追加，不改现有 return 逻辑）**

在 `forward()` 方法中，找到这行：

```java
                out.write(buffer, 0, len);
                out.flush();
```

在其后添加：

```java
                // === UI 统计：字节计数 ===
                // c->s: 客户端到服务器 = 下载流量（totalBytesDown）
                // s->c: 服务器到客户端 = 上传流量（totalBytesUp）
                if ("c->s".equals(direction)) {
                    totalBytesDown.addAndGet(len);
                    if (record != null) record.bytesDown.addAndGet(len);
                } else {
                    totalBytesUp.addAndGet(len);
                    if (record != null) record.bytesUp.addAndGet(len);
                }
                // === 统计注入结束 ===
```

**注意**：需要将 `forward()` 方法签名从：
`private void forward(Socket src, Socket dest, String direction)`
改为：
`private void forward(Socket src, Socket dest, String direction, ConnectionRecord record)`

同时在调用处（t1.start() 和 t2.start() 之前）找到：

```java
            Thread t1 = new Thread(() -> forward(clientSocket, targetSocket, "c->s"), "Forward-c->s");
            Thread t2 = new Thread(() -> forward(targetSocket, clientSocket, "s->c"), "Forward-s->c");
```

替换为：

```java
            final ConnectionRecord fwdRecord = record;
            Thread t1 = new Thread(() -> forward(clientSocket, targetSocket, "c->s", fwdRecord), "Forward-c->s");
            Thread t2 = new Thread(() -> forward(targetSocket, clientSocket, "s->c", fwdRecord), "Forward-s->c");
```

然后在 `forward()` 方法末尾（catch 块之后，`}` 之前）添加连接断开时的记录移除逻辑：

```java
            // === UI 统计：连接断开 ===
            if (record != null) {
                activeConnections.remove(record);
                connectionCount.decrementAndGet();
            }
            // === 统计注入结束 ===
```

- [ ] **Step 5: 在 handleClient() 末尾添加 t1/t2 join 后的清理调用**

找到 `t1.join()` 和 `t2.join()`

在两个 join 之后，`targetSocket.close()` 之前（现有代码顺序为 t1.join → t2.join → targetSocket.close → clientSocket.close），在 targetSocket.close 之前添加：

```java
            // 统计更新（forward 线程已结束，record 已标记 inactive）
            notifyStatusUpdate();
```

**注意**：`record.active = true` 已在 forward() 的 catch 块（SocketException/IOException）中通过 record 引用设置，forward() 方法结束时线程中 `record` 仍可见。

- [ ] **Step 6: 添加定时广播 Handler**

在 `startSocks5Server()` 方法末尾（在 `Log.i(TAG, "SOCKS5 server started...")` 之后）添加：

```java
        // 定时向 UI 推送状态更新（每 2 秒）
        final Handler uiHandler = new Handler(Looper.getMainLooper());
        uiHandler.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (isRunning) {
                    notifyStatusUpdate();
                    uiHandler.postDelayed(this, 2000);
                }
            }
        }, 2000);
```

- [ ] **Step 7: 验证 Gradle 构建**

```
gradlew.bat assembleDebug --no-daemon
```

预期：BUILD SUCCESSFUL。

---

## Task 7: 重写 activity_main.xml 布局文件

**Files:**
- Modify: `molink-worker/app/src/main/res/layout/activity_main.xml`

**重要**：必须先备份当前布局内容（读取后完全替换）。

- [ ] **Step 1: 读取当前 activity_main.xml 内容**

使用 Read 工具读取当前文件，确认内容后再操作。

- [ ] **Step 2: 完全重写布局**

路径：`molink-worker/app/src/main/res/layout/activity_main.xml`

```xml
<?xml version="1.0" encoding="utf-8"?>
<LinearLayout xmlns:android="http://schemas.android.com/apk/res/android"
    android:layout_width="match_parent"
    android:layout_height="match_parent"
    android:orientation="vertical"
    android:padding="16dp">

    <!-- ===== 1. 服务状态区 ===== -->
    <LinearLayout
        android:layout_width="match_parent"
        android:layout_height="wrap_content"
        android:orientation="horizontal"
        android:gravity="center_vertical"
        android:layout_marginBottom="12dp">

        <View
            android:id="@+id/statusDot"
            android:layout_width="12dp"
            android:layout_height="12dp"
            android:background="@drawable/circle_gray"/>

        <TextView
            android:id="@+id/statusRunning"
            android:layout_width="wrap_content"
            android:layout_height="wrap_content"
            android:text="SOCKS5 服务已停止"
            android:textSize="16sp"
            android:textStyle="bold"
            android:layout_marginStart="8dp"/>

        <TextView
            android:id="@+id/statusPort"
            android:layout_width="wrap_content"
            android:layout_height="wrap_content"
            android:text=""
            android:textSize="14sp"
            android:layout_marginStart="12dp"
            android:visibility="gone"/>

        <TextView
            android:id="@+id/statusUptime"
            android:layout_width="wrap_content"
            android:layout_height="wrap_content"
            android:text=""
            android:textSize="14sp"
            android:layout_marginStart="12dp"
            android:visibility="gone"/>
    </LinearLayout>

    <!-- ===== 2. 统计指标区 ===== -->
    <LinearLayout
        android:layout_width="match_parent"
        android:layout_height="wrap_content"
        android:orientation="vertical"
        android:background="#F5F5F5"
        android:padding="12dp"
        android:layout_marginBottom="12dp">

        <!-- 第一行：连接数 -->
        <LinearLayout
            android:layout_width="match_parent"
            android:layout_height="wrap_content"
            android:orientation="horizontal"
            android:layout_marginBottom="6dp">

            <TextView
                android:layout_width="0dp"
                android:layout_height="wrap_content"
                android:layout_weight="1"
                android:text="当前连接"
                android:textSize="13sp"
                android:textColor="#666666"/>

            <TextView
                android:id="@+id/connCount"
                android:layout_width="wrap_content"
                android:layout_height="wrap_content"
                android:text="--"
                android:textSize="13sp"
                android:textStyle="bold"/>
        </LinearLayout>

        <LinearLayout
            android:layout_width="match_parent"
            android:layout_height="wrap_content"
            android:orientation="horizontal"
            android:layout_marginBottom="6dp">

            <TextView
                android:layout_width="0dp"
                android:layout_height="wrap_content"
                android:layout_weight="1"
                android:text="历史连接"
                android:textSize="13sp"
                android:textColor="#666666"/>

            <TextView
                android:id="@+id/historyCount"
                android:layout_width="wrap_content"
                android:layout_height="wrap_content"
                android:text="--"
                android:textSize="13sp"
                android:textStyle="bold"/>
        </LinearLayout>

        <!-- 第二行：流量 -->
        <LinearLayout
            android:layout_width="match_parent"
            android:layout_height="wrap_content"
            android:orientation="horizontal">

            <TextView
                android:layout_width="0dp"
                android:layout_height="wrap_content"
                android:layout_weight="1"
                android:text="下载流量"
                android:textSize="13sp"
                android:textColor="#666666"/>

            <TextView
                android:id="@+id/bytesDown"
                android:layout_width="wrap_content"
                android:layout_height="wrap_content"
                android:text="--"
                android:textSize="13sp"
                android:textStyle="bold"
                android:textColor="#4CAF50"/>
        </LinearLayout>

        <LinearLayout
            android:layout_width="match_parent"
            android:layout_height="wrap_content"
            android:orientation="horizontal">

            <TextView
                android:layout_width="0dp"
                android:layout_height="wrap_content"
                android:layout_weight="1"
                android:text="上传流量"
                android:textSize="13sp"
                android:textColor="#666666"/>

            <TextView
                android:id="@+id/bytesUp"
                android:layout_width="wrap_content"
                android:layout_height="wrap_content"
                android:text="--"
                android:textSize="13sp"
                android:textStyle="bold"
                android:textColor="#2196F3"/>
        </LinearLayout>
    </LinearLayout>

    <!-- ===== 3. 连接日志标题 ===== -->
    <TextView
        android:layout_width="wrap_content"
        android:layout_height="wrap_content"
        android:text="最近连接"
        android:textSize="14sp"
        android:textStyle="bold"
        android:layout_marginBottom="8dp"/>

    <!-- ===== 4. 连接日志 RecyclerView（恒定高度 280dp）===== -->
    <androidx.recyclerview.widget.RecyclerView
        android:id="@+id/connectionLogList"
        android:layout_width="match_parent"
        android:layout_height="280dp"
        android:layout_marginBottom="12dp"
        android:background="#FAFAFA"
        android:padding="4dp"/>

    <!-- ===== 5. 控制按钮 ===== -->
    <Button
        android:id="@+id/toggleButton"
        android:layout_width="match_parent"
        android:layout_height="wrap_content"
        android:text="启动服务"
        android:onClick="onToggleClick"/>
</LinearLayout>
```

- [ ] **Step 3: 创建圆点背景 drawable**

路径：`molink-worker/app/src/main/res/drawable/circle_gray.xml`

```xml
<?xml version="1.0" encoding="utf-8"?>
<shape xmlns:android="http://schemas.android.com/apk/res/android"
    android:shape="oval">
    <solid android:color="#9E9E9E"/>
    <size android:width="12dp" android:height="12dp"/>
</shape>
```

路径：`molink-worker/app/src/main/res/drawable/circle_green.xml`

```xml
<?xml version="1.0" encoding="utf-8"?>
<shape xmlns:android="http://schemas.android.com/apk/res/android"
    android:shape="oval">
    <solid android:color="#4CAF50"/>
    <size android:width="12dp" android:height="12dp"/>
</shape>
```

路径：`molink-worker/app/src/main/res/drawable/circle_red.xml`

```xml
<?xml version="1.0" encoding="utf-8"?>
<shape xmlns:android="http://schemas.android.com/apk/res/android"
    android:shape="oval">
    <solid android:color="#F44336"/>
    <size android:width="12dp" android:height="12dp"/>
</shape>
```

---

## Task 8: 创建连接日志 item 布局

**Files:**
- Create: `molink-worker/app/src/main/res/layout/item_connection_log.xml`

- [ ] **Step 1: 创建 item_connection_log.xml**

路径：`molink-worker/app/src/main/res/layout/item_connection_log.xml`

```xml
<?xml version="1.0" encoding="utf-8"?>
<LinearLayout xmlns:android="http://schemas.android.com/apk/res/android"
    android:layout_width="match_parent"
    android:layout_height="wrap_content"
    android:orientation="horizontal"
    android:gravity="center_vertical"
    android:paddingVertical="6dp"
    android:paddingHorizontal="4dp">

    <!-- 状态指示圆点 -->
    <View
        android:id="@+id/connDot"
        android:layout_width="8dp"
        android:layout_height="8dp"
        android:background="@drawable/circle_gray"/>

    <!-- 目标地址 -->
    <TextView
        android:id="@+id/targetHost"
        android:layout_width="0dp"
        android:layout_height="wrap_content"
        android:layout_weight="1"
        android:layout_marginStart="8dp"
        android:text="example.com:443"
        android:textSize="12sp"
        android:ellipsize="end"
        android:maxLines="1"/>

    <!-- 流量 -->
    <TextView
        android:id="@+id/connTraffic"
        android:layout_width="wrap_content"
        android:layout_height="wrap_content"
        android:text="↓ --"
        android:textSize="11sp"
        android:textColor="#4CAF50"
        android:layout_marginStart="6dp"/>

    <!-- 在线时长 -->
    <TextView
        android:id="@+id/connDuration"
        android:layout_width="wrap_content"
        android:layout_height="wrap_content"
        android:text="0s"
        android:textSize="11sp"
        android:textColor="#888888"
        android:layout_marginStart="6dp"/>
</LinearLayout>
```

- [ ] **Step 2: 验证文件创建**

确认 `item_connection_log.xml` 文件存在。

---

## Task 9: 创建 ConnectionLogAdapter

**Files:**
- Create: `molink-worker/app/src/main/java/com/molink/worker/ConnectionLogAdapter.java`

- [ ] **Step 1: 创建 ConnectionLogAdapter.java**

路径：`molink-worker/app/src/main/java/com/molink/worker/ConnectionLogAdapter.java`

```java
package com.molink.worker;

import android.content.res.Resources;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;
import java.util.ArrayList;
import java.util.List;

public class ConnectionLogAdapter extends RecyclerView.Adapter<ConnectionLogAdapter.ViewHolder> {

    private static final int MAX_ITEMS = 50;
    private final List<ConnectionRecord> items = new ArrayList<>();

    @NonNull
    @Override
    public ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View view = LayoutInflater.from(parent.getContext())
                .inflate(R.layout.item_connection_log, parent, false);
        return new ViewHolder(view);
    }

    @Override
    public void onBindViewHolder(@NonNull ViewHolder holder, int position) {
        ConnectionRecord record = items.get(position);
        holder.bind(record);
    }

    @Override
    public int getItemCount() {
        return items.size();
    }

    /** 在顶部插入新记录，超量时移除最后一条 */
    public void addTop(ConnectionRecord record) {
        if (items.size() >= MAX_ITEMS) {
            items.remove(items.size() - 1);
            notifyItemRemoved(items.size());
        }
        items.add(0, record);
        notifyItemInserted(0);
    }

    /** 全量更新（由 Handler 定时刷新时调用） */
    public void refreshAll(List<ConnectionRecord> newItems) {
        // 按现有顺序更新字节数（不去除旧记录，保持用户可见的断开连接）
        // 如果新列表中不包含某条记录，说明已断开
        items.clear();
        items.addAll(newItems);
        notifyDataSetChanged();
    }

    static class ViewHolder extends RecyclerView.ViewHolder {
        private final View dot;
        private final TextView targetHost;
        private final TextView traffic;
        private final TextView duration;

        ViewHolder(@NonNull View itemView) {
            super(itemView);
            dot = itemView.findViewById(R.id.connDot);
            targetHost = itemView.findViewById(R.id.targetHost);
            traffic = itemView.findViewById(R.id.connTraffic);
            duration = itemView.findViewById(R.id.connDuration);
        }

        void bind(ConnectionRecord record) {
            targetHost.setText(record.getDisplayHost());

            long down = record.getBytesDown();
            long up = record.getBytesUp();
            traffic.setText("↓" + formatBytes(down) + " ↑" + formatBytes(up));

            long secs = record.getDurationSec();
            duration.setText(formatDuration(secs));

            // 活跃连接=绿点，断开=灰点
            if (record.active) {
                dot.setBackgroundResource(R.drawable.circle_green);
            } else {
                dot.setBackgroundResource(R.drawable.circle_gray);
            }
        }

        private static String formatBytes(long bytes) {
            if (bytes < 0) return "--";
            if (bytes < 1024) return bytes + " B";
            if (bytes < 1024 * 1024) return String.format("%.1f KB", bytes / 1024.0);
            return String.format("%.1f MB", bytes / (1024.0 * 1024));
        }

        private static String formatDuration(long secs) {
            if (secs < 0) return "0s";
            if (secs < 60) return secs + "s";
            if (secs < 3600) return (secs / 60) + "m";
            return String.format("%.1fh", secs / 3600.0);
        }
    }
}
```

- [ ] **Step 2: 验证文件创建成功**

确认文件存在，编译无错误（通过 Gradle 构建验证）。

---

## Task 10: 改造 MainActivity

**Files:**
- Modify: `molink-worker/app/src/main/java/com/molink/worker/MainActivity.java`

**重要**：读取现有文件后再修改，仅改动指定区域。

- [ ] **Step 1: 读取当前 MainActivity.java**

使用 Read 工具完整读取文件。

- [ ] **Step 2: 添加 RecyclerView 相关 import**

在 import 区（`import android.os.Binder;` 等附近）添加：

```java
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import android.widget.View;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Button;
import android.graphics.drawable.Drawable;
```

- [ ] **Step 3: 添加 View 字段声明**

在现有字段区（`private TextView statusText;` 附近）添加：

```java
    private View statusDot;
    private TextView statusRunning;
    private TextView statusPort;
    private TextView statusUptime;
    private TextView connCount;
    private TextView historyCount;
    private TextView bytesDown;
    private TextView bytesUp;
    private RecyclerView connectionLogList;
    private ConnectionLogAdapter logAdapter;
```

- [ ] **Step 4: 更新 onCreate() 中的 findViewById**

将现有的 `statusText = findViewById(R.id.statusText);` 替换为：

```java
        statusDot = findViewById(R.id.statusDot);
        statusRunning = findViewById(R.id.statusRunning);
        statusPort = findViewById(R.id.statusPort);
        statusUptime = findViewById(R.id.statusUptime);
        connCount = findViewById(R.id.connCount);
        historyCount = findViewById(R.id.historyCount);
        bytesDown = findViewById(R.id.bytesDown);
        bytesUp = findViewById(R.id.bytesUp);
        connectionLogList = findViewById(R.id.connectionLogList);

        logAdapter = new ConnectionLogAdapter();
        connectionLogList.setLayoutManager(new LinearLayoutManager(this));
        connectionLogList.setAdapter(logAdapter);
```

同时删除旧的 `statusText` 相关引用。

- [ ] **Step 5: 注册 StatusListener 并更新 UI**

在 `onResume()` 中（bindService 之后）添加：

```java
        if (proxyService != null) {
            proxyService.addStatusListener(statusListener);
            updateUIFromService(proxyService);
        }
```

在 `onDestroy()` 中（unbindService 之前）添加：

```java
        if (proxyService != null) {
            proxyService.removeStatusListener(statusListener);
        }
```

- [ ] **Step 6: 添加 updateUIFromService() 方法**

在 MainActivity 类末尾（最后一个方法之后，`}` 之前）添加：

```java
    private void updateUIFromService(Socks5ProxyService svc) {
        if (svc == null || !svc.isRunning()) {
            statusRunning.setText("SOCKS5 服务已停止");
            statusPort.setVisibility(View.GONE);
            statusUptime.setVisibility(View.GONE);
            statusDot.setBackgroundResource(R.drawable.circle_gray);
            toggleButton.setText("启动服务");
            connCount.setText("--");
            historyCount.setText("--");
            bytesDown.setText("--");
            bytesUp.setText("--");
            return;
        }

        statusRunning.setText("SOCKS5 运行中");
        statusDot.setBackgroundResource(R.drawable.circle_green);
        statusPort.setVisibility(View.VISIBLE);
        statusPort.setText("端口:" + svc.getPort());
        statusUptime.setVisibility(View.VISIBLE);
        toggleButton.setText("停止服务");
    }

    private Socks5ProxyService.StatusListener statusListener = new Socks5ProxyService.StatusListener() {
        @Override
        public void onStatusUpdate(long uptime, int connectionCountVal, int historyCountVal,
                                   long bytesDownVal, long bytesUpVal,
                                   List<ConnectionRecord> activeConnections) {
            runOnUiThread(() -> {
                connCount.setText(String.valueOf(connectionCountVal));
                historyCount.setText(String.valueOf(historyCountVal));
                bytesDown.setText(formatBytes(bytesDownVal));
                bytesUp.setText(formatBytes(bytesUpVal));
                statusUptime.setText("在线:" + formatUptime(uptime));
                logAdapter.refreshAll(activeConnections);
            });
        }
    };

    private static String formatBytes(long bytes) {
        if (bytes < 0) return "--";
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return String.format("%.1f KB", bytes / 1024.0);
        return String.format("%.1f MB", bytes / (1024.0 * 1024));
    }

    private static String formatUptime(long secs) {
        if (secs < 0) return "0s";
        if (secs < 60) return secs + "s";
        if (secs < 3600) return (secs / 60) + "m";
        return String.format("%.1fh", secs / 3600.0);
    }
```

- [ ] **Step 7: 更新 onToggleClick() 中的状态更新逻辑**

找到 `onToggleClick()` 中的 `updateStatus()` 调用，在按钮文字切换后，改为调用新的 updateUIFromService()。

如果旧的 `updateStatus()` 方法仍然存在且只设置 statusText，则整个 `updateStatus()` 方法可以删除（因为新的 updateUIFromService 替代了它）。

- [ ] **Step 8: 验证 Gradle 构建**

```
gradlew.bat assembleDebug --no-daemon
```

预期：BUILD SUCCESSFUL。

---

## Task 11: E2E 自动化测试验证

**Files:**
- 验证: `test/test.py`

- [ ] **Step 1: 运行自动化测试**

在 `D:\project\MoLink` 目录下运行：

```
python test/test.py
```

**预期结果**：ALL PASS，特别是 Step 7（SOCKS 代理）必须通过，证明 UI 改动未影响代理功能。

**如果测试失败**：
1. 首先确认是否由 UI 改动引起（检查 Socks5ProxyService 的 handleClient/forward 是否被意外修改）
2. 如果 proxy 逻辑完好，修复测试脚本
3. 如果 proxy 逻辑被意外修改，恢复原有代码并重新添加统计注入

---

## 验证清单

- [ ] `ic_notification.xml` — 通知栏图标正常显示
- [ ] `ic_launcher.xml` — 应用启动图标正常显示
- [ ] 服务启动后，状态区显示"● SOCKS5 运行中 端口:1080 在线:Xs"
- [ ] 统计区显示当前连接数、历史连接数、下载/上传流量
- [ ] RecyclerView 连接日志正常显示新连接
- [ ] 断开连接后，日志条目圆点变为灰色
- [ ] `python test/test.py` ALL PASS
- [ ] BUILD SUCCESSFUL，无资源引用错误
