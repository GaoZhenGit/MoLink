package com.molink.worker;

import android.util.Log;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;

import androidx.core.app.NotificationCompat;

import com.molink.worker.BuildConfig;
import com.molink.worker.R;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketException;
import java.net.SocketTimeoutException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

public class Socks5ProxyService extends Service {

    private static final String TAG = "Socks5ProxyService";
    private static final int SOCKS5_PORT = BuildConfig.SOCKS_PORT;
    private static final int STATUS_HTTP_PORT = BuildConfig.STATUS_HTTP_PORT;
    private static final String SOCKS_USERNAME = BuildConfig.SOCKS_USERNAME;
    private static final String SOCKS_PASSWORD = BuildConfig.SOCKS_PASSWORD;
    private static final String CHANNEL_ID = "Socks5ProxyChannel";
    private static final int NOTIFICATION_ID = 1;
    // v3 Req 1: 历史记录上限，超出时 FIFO 删除最旧记录
    private static final int MAX_HISTORY = 50;

    /** MainActivity 通过此静态引用直接访问服务，无需绑定 */
    private static volatile Socks5ProxyService instance;

    public static Socks5ProxyService getInstance() {
        return instance;
    }

    private ServerSocket serverSocketV4;
    private ServerSocket serverSocketV6;
    private volatile boolean isRunning = false;
    private volatile long startTime = 0;
    // v3 Req 2: 无需维护 connectionCount 字段，直接从 list 计算
    private Thread serverThreadV4;
    private Thread serverThreadV6;
    private StatusHttpServer httpServer;

    // ========== UI 统计相关 ==========
    private final AtomicInteger historyCount = new AtomicInteger(0);
    private final AtomicLong totalBytesDown = new AtomicLong(0);
    private final AtomicLong totalBytesUp = new AtomicLong(0);
    // v3 Req 1: static final synchronized list，两层共享同一引用，无复制开销
    // 写操作（add/remove）由 synchronized 保护；读操作（refreshAll 迭代）需在 synchronized 块中执行
    private static final List<ConnectionRecord> activeConnections = Collections.synchronizedList(new ArrayList<>());

    // v3 Req 1: synchronizedList 单次 add/remove 内部已加锁，无需额外同步
    // 但 check-then-act 复合操作（while + remove + add）需要 synchronized 包裹
    private void addConnectionRecord(ConnectionRecord record) {
        synchronized (activeConnections) {
            while (activeConnections.size() >= MAX_HISTORY) {
                activeConnections.remove(0);  // 删除最旧记录（FIFO）
            }
            activeConnections.add(record);
        }
    }

    public int getConnectionCount() {
        return activeConnections.size();
    }

    // v3 Req 1: 返回 shared reference，无需复制
    public static List<ConnectionRecord> getConnectionSnapshot() {
        return activeConnections;
    }

    public long getTotalBytesDown() { return totalBytesDown.get(); }
    public long getTotalBytesUp() { return totalBytesUp.get(); }
    public int getHistoryCount() { return historyCount.get(); }

    private void closeQuietly(ServerSocket ss) {
        if (ss != null) {
            try {
                ss.close();
            } catch (IOException e) {
                Log.w(TAG, "Error closing ServerSocket: " + e.getMessage());
            }
        }
    }

    /**
     * 安全关闭 Socket（v3 Req 2：shutdownInput + shutdownOutput + close 三步关闭确保 fd 释放）
     * 声明为 static，以便从 static 内部类 Pipe 调用
     */
    private static void closeQuietly(Socket socket) {
        if (socket == null) return;
        try {
            socket.shutdownInput();  // 发送 FIN，unblock 对端 read()
        } catch (IOException ignored) { /* ignore */ }
        try {
            socket.shutdownOutput();
        } catch (IOException ignored) { /* ignore */ }
        try {
            if (!socket.isClosed()) {
                socket.close();  // 释放 fd
            }
        } catch (IOException e) {
            Log.w(TAG, "Error closing Socket: " + e.getMessage());
        }
    }

    public class LocalBinder extends Binder {
        Socks5ProxyService getService() {
            return Socks5ProxyService.this;
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        Log.d(TAG, "onCreate");
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Log.d(TAG, "onStartCommand, starting foreground");
        startForeground(NOTIFICATION_ID, createNotification());
        startSocks5Server();
        // 启动 HTTP 状态服务
        new Handler(Looper.getMainLooper()).post(() -> {
            try {
                httpServer = new StatusHttpServer(STATUS_HTTP_PORT, this);
                httpServer.start();
                Log.i(TAG, "HTTP status server started on port " + STATUS_HTTP_PORT);
            } catch (IOException e) {
                Log.e(TAG, "Failed to start HTTP server: " + e.getMessage());
            }
        });
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Log.i(TAG, "onDestroy BEGIN, isRunning=" + isRunning);
        instance = null;
        isRunning = false;
        closeQuietly(serverSocketV4);
        closeQuietly(serverSocketV6);
        if (httpServer != null) {
            httpServer.stop();
        }
        // 强制移除前台通知（stopService 后 Android 会自动移除，此处显式调用确保可靠）
        stopForeground(STOP_FOREGROUND_REMOVE);
        Log.i(TAG, "onDestroy END");
    }

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }

    public boolean isRunning() {
        return isRunning;
    }

    public int getPort() {
        return SOCKS5_PORT;
    }

    public long getUptime() {
        if (startTime == 0) return 0;
        return SystemClock.elapsedRealtime() / 1000 - startTime;
    }

    private final IBinder binder = new LocalBinder();

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID,
                    "SOCKS5 Proxy Service",
                    NotificationManager.IMPORTANCE_LOW
            );
            NotificationManager manager = getSystemService(NotificationManager.class);
            manager.createNotificationChannel(channel);
        }
    }

    private Notification createNotification() {
        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle("MoLink Worker")
                .setContentText("SOCKS5 代理服务运行中，端口 " + SOCKS5_PORT)
                .setSmallIcon(R.drawable.ic_notification)
                .build();
    }

    private void startSocks5Server() {
        if (isRunning) {
            Log.w(TAG, "Server already running");
            return;
        }
        isRunning = true;
        startTime = SystemClock.elapsedRealtime() / 1000;

        // 同时监听 IPv4 (127.0.0.1) 和 IPv6 (::1)，确保 ADB 端口转发过来的连接都能接收
        serverThreadV4 = new Thread(() -> {
            try {
                serverSocketV4 = new ServerSocket();
                serverSocketV4.setReuseAddress(true);
                serverSocketV4.bind(new InetSocketAddress("127.0.0.1", SOCKS5_PORT));
                Log.i(TAG, "ServerSocket IPv4 listening on 127.0.0.1:" + SOCKS5_PORT);
                while (isRunning) {
                    try {
                        Socket clientSocket = serverSocketV4.accept();
                        Log.d(TAG, "IPv4: client connected from " + clientSocket.getRemoteSocketAddress());
                        new Thread(() -> handleClient(clientSocket), "Socks5Handler-IPv4").start();
                    } catch (SocketException e) {
                        if (isRunning) Log.w(TAG, "IPv4 accept error: " + e.getMessage());
                    }
                }
            } catch (IOException e) {
                if (isRunning) Log.e(TAG, "IPv4 ServerSocket error: " + e.getMessage(), e);
            }
        }, "Socks5Server-IPv4");

        serverThreadV6 = new Thread(() -> {
            try {
                serverSocketV6 = new ServerSocket();
                serverSocketV6.setReuseAddress(true);
                serverSocketV6.bind(new InetSocketAddress("::1", SOCKS5_PORT));
                Log.i(TAG, "ServerSocket IPv6 listening on [::1]:" + SOCKS5_PORT);
                while (isRunning) {
                    try {
                        Socket clientSocket = serverSocketV6.accept();
                        Log.d(TAG, "IPv6: client connected from " + clientSocket.getRemoteSocketAddress());
                        new Thread(() -> handleClient(clientSocket), "Socks5Handler-IPv6").start();
                    } catch (SocketException e) {
                        if (isRunning) Log.w(TAG, "IPv6 accept error: " + e.getMessage());
                    }
                }
            } catch (IOException e) {
                if (isRunning) Log.e(TAG, "IPv6 ServerSocket error: " + e.getMessage(), e);
            }
        }, "Socks5Server-IPv6");

        serverThreadV4.start();
        serverThreadV6.start();
        Log.i(TAG, "SOCKS5 server started on port " + SOCKS5_PORT);

        // v3 Req 2: 定时 UI 推送已移除（由 MainActivity 0.5s 轮询替代）
    }

    private void handleClient(Socket clientSocket) {
        // v3 Req 3: 55 秒无数据即认为对端已断开（触发 SocketTimeoutException）
        // 原因：curl Ctrl-C 后 FIN 可能被 adb forward 隧道延迟，
        // pipe1.read() 收不到 FIN 会一直阻塞，导致 proxy 无法感知客户端已死，
        // server 继续发送数据直到完成。55 秒兜底检测可覆盖大多数 HTTP Keep-Alive 场景。
        try {
            clientSocket.setSoTimeout(55_000);
            clientSocket.setKeepAlive(true);
        } catch (SocketException e) {
            Log.w(TAG, "Failed to set client socket options: " + e.getMessage());
        }

        ConnectionRecord record = null;
        Socket targetSocket = null;
        try {
            InputStream in = clientSocket.getInputStream();
            OutputStream out = clientSocket.getOutputStream();

            // SOCKS5 握手
            int ver = in.read();
            if (ver != 0x05) {
                Log.w(TAG, "Unsupported SOCKS version: " + ver);
                clientSocket.close();
                return;
            }

            int nMethods = in.read();
            byte[] methods = new byte[nMethods];
            int read = in.read(methods);
            if (read < nMethods) {
                clientSocket.close();
                return;
            }

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

            // RFC 1929 用户名/密码认证子协商
            if (!handleUserAuth(in, out)) {
                // 认证失败，handleUserAuth 已发送失败响应，关闭连接
                clientSocket.close();
                return;
            }

            // 读取连接请求
            ver = in.read();
            if (ver != 0x05) {
                Log.w(TAG, "Unexpected version in connect request: " + ver);
                clientSocket.close();
                return;
            }

            int cmd = in.read();
            int rsv = in.read();
            int addrType = in.read();

            if (cmd != 0x01) { // 只支持 CONNECT
                Log.w(TAG, "Unsupported CMD: " + cmd);
                out.write(0x05);
                out.write(0x07); // Command not supported
                out.write(0x00);
                out.write(0x01);
                out.write(new byte[6]);
                out.flush();
                clientSocket.close();
                return;
            }

            String destAddr;
            int destPort;

            if (addrType == 0x01) { // IPv4
                byte[] ip = new byte[4];
                readFully(in, ip);
                destAddr = String.format("%d.%d.%d.%d", ip[0] & 0xFF, ip[1] & 0xFF, ip[2] & 0xFF, ip[3] & 0xFF);
            } else if (addrType == 0x03) { // 域名
                int len = in.read();
                byte[] domain = new byte[len];
                readFully(in, domain);
                destAddr = new String(domain);
            } else {
                Log.w(TAG, "Unsupported ATYP: " + addrType);
                clientSocket.close();
                return;
            }

            destPort = (in.read() << 8) | in.read();
            Log.i(TAG, "CONNECT request: " + destAddr + ":" + destPort);

            // 建立目标连接（使用无参构造 + connect 以设置超时）
            targetSocket = new Socket();
            try {
                targetSocket.connect(
                    new InetSocketAddress(destAddr, destPort),
                    10_000  // 连接超时 10 秒
                );
                // v3 Req 3: 55 秒无数据即认为中断（触发 SocketTimeoutException）
                targetSocket.setSoTimeout(55_000);
                targetSocket.setKeepAlive(true);
                targetSocket.setTcpNoDelay(true);
            } catch (IOException e) {
                Log.e(TAG, "Failed to connect to " + destAddr + ":" + destPort, e);
                // 发送失败响应
                out.write(0x05);
                out.write(0x04); // Host unreachable
                out.write(0x00);
                out.write(0x01);
                out.write(new byte[6]);
                out.flush();
                clientSocket.close();
                return;
            }

            // 发送成功响应
            out.write(0x05);
            out.write(0x00); // success
            out.write(0x00);
            out.write(0x01);
            byte[] localIp = targetSocket.getLocalAddress().getAddress();
            int localPort = targetSocket.getLocalPort();
            out.write(localIp);
            out.write((localPort >> 8) & 0xFF);
            out.write(localPort & 0xFF);
            out.flush();

            Log.i(TAG, "Connection established to " + destAddr + ":" + destPort);

            // === v3 Req 1: 记录新连接 ===
            historyCount.incrementAndGet();
            String clientIp = clientSocket.getRemoteSocketAddress().toString();
            if (clientIp.startsWith("/")) clientIp = clientIp.substring(1);
            record = new ConnectionRecord(clientIp, destAddr, destPort, System.currentTimeMillis());
            addConnectionRecord(record);  // 超出 MAX_HISTORY 时 FIFO 删除旧记录

            // 双向转发（Pipe 逻辑，v3 Req 2）
            Pipe pipe1 = new Pipe(clientSocket, targetSocket, "c->s", record);
            Pipe pipe2 = new Pipe(targetSocket, clientSocket, "s->c", record);
            Thread t1 = new Thread(() -> forward(pipe1), "Forward-c->s");
            Thread t2 = new Thread(() -> forward(pipe2), "Forward-s->c");
            t1.start();
            t2.start();

            // 设置超时等待，避免永久阻塞
            t1.join(60_000);
            t2.join(60_000);

            // v3 Req 2: 超时强制中断时，closeAll() 关闭连接（不涉及 UI）
            if (t1.isAlive() || t2.isAlive()) {
                Log.w(TAG, "Forward threads still alive after 60s, forcing shutdown");
                t1.interrupt();
                t2.interrupt();
                pipe1.closeAll();
                pipe2.closeAll();
            }

            // === 记录结束状态（v3 Req 4）===
            pipe1.isEnded();   // 触发 Pipe1 的 ended 状态读取
            pipe2.isEnded();   // 触发 Pipe2 的 ended 状态读取
            record.setEnded(); // 两个方向都结束了，标记 record 为已结束

            // v3 Req 1: 记录保留在 list 中作为历史（MAX_HISTORY 内），UI 0.5s 轮询读取
            // 不再从 list 删除，由 addConnectionRecord() 的 FIFO 逻辑统一管理上限
            Log.i(TAG, "Client session closed");
        } catch (Exception e) {
            Log.e(TAG, "handleClient error: " + e.getMessage(), e);

            // v3 Req 2: 仅关闭 Socket，不调用任何 UI 方法
            closeQuietly(targetSocket);
            closeQuietly(clientSocket);

            // v3 Req 4: 如果 record 已创建，标记为已结束（保留在 list 中作为历史）
            if (record != null) {
                record.setEnded();
                // v3 Req 1: 不再从 list 删除，由 addConnectionRecord() 的 FIFO 统一管理
            }
        }
    }

    private void readFully(InputStream in, byte[] b) throws IOException {
        int off = 0;
        while (off < b.length) {
            int read = in.read(b, off, b.length - off);
            if (read == -1) throw new IOException("Unexpected end of stream");
            off += read;
        }
    }

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

    /**
     * 连接管道：管理双向转发的生命周期
     *
     * 中断判定（v3 Req 3）：任一条件满足即认为本方向已中断，关闭对端 Socket：
     * 中断判定（v3 Req 3 修订）：以下任一条件触发即认为本方向已中断，关闭对端 Socket：
     * - 读到 EOF（正常结束，对端已关闭连接）
     * - 抛出 IOException（连接异常，如连接重置、对端崩溃）
     * - 55 秒无数据（setSoTimeout 触发 SocketTimeoutException，兜底检测对端存活）
     *
     * 双向同步（v3 Req 2）：closePeer() 关闭对端后，对端 forward() 的 read() 会抛出
     * IOException 从而也调用 closePeer()，最终两个方向都会被正确销毁。
     *
     * 状态语义（v3 Req 4，用于 UI）：
     * - markEnded() 被调用 → ended = true → 灰色（连接已结束）
     * - 未调用 markEnded() → ended = false → 绿色（正在进行中）
     * - 不再区分正常/异常，结束即灰色
     */
    private static class Pipe {
        private final Socket source;  // 本方向的读端
        private final Socket sink;    // 本方向的写端（即对端的读端）
        private final AtomicBoolean closed = new AtomicBoolean(false);
        /** 标记本方向是否已结束。EOF/异常/55秒超时均调用 markEnded()。 */
        private final AtomicBoolean ended = new AtomicBoolean(false);
        private final String direction;
        private final ConnectionRecord record;

        Pipe(Socket source, Socket sink, String direction, ConnectionRecord record) {
            this.source = source;
            this.sink = sink;
            this.direction = direction;
            this.record = record;
        }

        /**
         * 关闭对端 Socket，触发另一个 forward 线程的 read() 退出（v3 Req 2）
         *
         * 关键：必须先用 shutdownInput() 发送 TCP FIN 包，再 close()。
         * 直接 close() 在某些 OS/JVM 下不会可靠地中断另一个线程阻塞的 read()。
         * shutdownInput() 让对端的 read() 立即返回 -1（EOF），从而退出 while 循环。
         */
        void closePeer() {
            if (closed.compareAndSet(false, true)) {
                try {
                    sink.shutdownInput();   // 发送 TCP FIN → 对端 read() 立即返回 -1
                } catch (IOException ignored) { /* socket 已关闭则忽略 */ }
                try {
                    sink.shutdownOutput();  // 同时关闭输出方向，更干净
                } catch (IOException ignored) { /* socket 已关闭则忽略 */ }
                closeQuietly(sink);        // 最后释放 socket fd
                Log.d(TAG, direction + ": closed peer socket to stop opposite thread");
            }
        }

        /**
         * 标记本方向已结束（EOF/异常/超时），用于 UI 状态判断（v3 Req 4）
         */
        void markEnded() {
            ended.set(true);
        }

        boolean isEnded() {
            return ended.get();
        }

        /**
         * 关闭本方向的所有 Socket（v3 Req 2：线程销毁时同步关闭连接）
         */
        void closeAll() {
            closePeer();  // 关闭对端
            closeQuietly(source);  // 关闭本端（v2 Fix 1：防止 fd 泄漏）
        }
    }

    /**
     * 数据转发（v3 Req 2 & Req 3 & Req 4）
     *
     * 中断判定（v3 Req 3）：
     * - in.read() 返回 -1（EOF，正常关闭）
     * - in.read() 抛出 IOException（连接异常）
     * - 55 秒无数据（setSoTimeout 触发 SocketTimeoutException）
     *
     * 双向同步：任一方向中断 → closePeer() 关闭对端 → 对端 read() 也会抛异常 → 双向同步关闭。
     */
    private void forward(Pipe pipe) {
        try {
            InputStream in = pipe.source.getInputStream();
            OutputStream out = pipe.sink.getOutputStream();
            byte[] buffer = new byte[8192];
            int len;
            while ((len = in.read(buffer)) != -1) {
                out.write(buffer, 0, len);
                out.flush();

                // === UI 统计：字节计数 ===
                if ("c->s".equals(pipe.direction)) {
                    totalBytesDown.addAndGet(len);
                    pipe.record.bytesDown.addAndGet(len);
                } else {
                    totalBytesUp.addAndGet(len);
                    pipe.record.bytesUp.addAndGet(len);
                }
            }

            // 正常 EOF → 标记结束，关闭对端（v3 Req 3 & Req 4）
            pipe.markEnded();
            Log.d(TAG, pipe.direction + ": EOF reached, ended");
            pipe.closePeer();

        } catch (IOException e) {
            String msg = e.getMessage();
            // v2 Fix 2 + v3 Req 3: "Read timed out" = SocketTimeoutException = 55秒无数据中断
            if (msg == null || (!msg.contains("Socket closed")
                    && !msg.contains("Connection reset")
                    && !msg.contains("Broken pipe")
                    && !msg.contains("EOF")
                    && !msg.contains("Read timed out")
                    && !msg.contains("Socket timed out"))) {
                Log.w(TAG, pipe.direction + ": unexpected IOException: " + msg);
            }
            // 异常 → 标记结束，关闭对端（v3 Req 3 & Req 4）
            pipe.markEnded();
            pipe.closePeer();
        } finally {
            // v2 Fix 1 + v3 Req 2: closeQuietly() 内部调用 shutdownInput+close，
            // 确保 source fd 释放，且对端 read() 立即退出
            closeQuietly(pipe.source);
            pipe.record.checkEnded();  // v3 Req 4: 同步 record 状态
        }
    }
}
