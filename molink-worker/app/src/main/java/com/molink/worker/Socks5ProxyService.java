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
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

public class Socks5ProxyService extends Service {

    private static final String TAG = "Socks5ProxyService";
    private static final int SOCKS5_PORT = BuildConfig.SOCKS_PORT;
    private static final int STATUS_HTTP_PORT = BuildConfig.STATUS_HTTP_PORT;
    private static final String CHANNEL_ID = "Socks5ProxyChannel";
    private static final int NOTIFICATION_ID = 1;

    /** MainActivity 通过此静态引用直接访问服务，无需绑定 */
    private static volatile Socks5ProxyService instance;

    public static Socks5ProxyService getInstance() {
        return instance;
    }

    private ServerSocket serverSocketV4;
    private ServerSocket serverSocketV6;
    private volatile boolean isRunning = false;
    private volatile long startTime = 0;
    private final AtomicInteger connectionCount = new AtomicInteger(0);
    private Thread serverThreadV4;
    private Thread serverThreadV6;
    private StatusHttpServer httpServer;

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
        if (listener != null) statusListeners.addIfAbsent(listener);
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

    private final IBinder binder = new LocalBinder();

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

    public int getConnectionCount() {
        return connectionCount.get();
    }

    private void closeQuietly(ServerSocket ss) {
        if (ss != null) {
            try {
                ss.close();
            } catch (IOException e) {
                Log.w(TAG, "Error closing ServerSocket: " + e.getMessage());
            }
        }
    }

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
    }

    private void handleClient(Socket clientSocket) {
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

            // 发送无认证响应
            out.write(0x05);
            out.write(0x00);
            out.flush();

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

            // 建立目标连接
            Socket targetSocket = new Socket(destAddr, destPort);

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

            // === UI 统计：记录新连接 ===
            historyCount.incrementAndGet();
            connectionCount.incrementAndGet();
            String clientIp = clientSocket.getRemoteSocketAddress().toString();
            if (clientIp.startsWith("/")) clientIp = clientIp.substring(1);
            final ConnectionRecord record = new ConnectionRecord(clientIp, destAddr, destPort, System.currentTimeMillis());
            activeConnections.add(record);
            // === 统计注入结束 ===

            // 双向转发数据
            Thread t1 = new Thread(() -> forward(clientSocket, targetSocket, "c->s", record), "Forward-c->s");
            Thread t2 = new Thread(() -> forward(targetSocket, clientSocket, "s->c", record), "Forward-s->c");
            t1.start();
            t2.start();

            t1.join();
            t2.join();

            // === UI 统计：连接断开，移除记录 ===
            activeConnections.remove(record);
            connectionCount.decrementAndGet();
            notifyStatusUpdate();
            // === 统计注入结束 ===

            targetSocket.close();
            clientSocket.close();
            Log.i(TAG, "Client session closed");
        } catch (Exception e) {
            Log.e(TAG, "handleClient error: " + e.getMessage(), e);
            try {
                clientSocket.close();
            } catch (IOException ex) {
                // ignore
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

    private void forward(Socket src, Socket dest, String direction, ConnectionRecord record) {
        try {
            InputStream in = src.getInputStream();
            OutputStream out = dest.getOutputStream();
            byte[] buffer = new byte[8192];
            int len;
            while ((len = in.read(buffer)) != -1) {
                out.write(buffer, 0, len);
                out.flush();
                // === UI 统计：字节计数 ===
                // c->s: 客户端到服务器 = 下载流量
                // s->c: 服务器到客户端 = 上传流量
                if ("c->s".equals(direction)) {
                    totalBytesDown.addAndGet(len);
                    record.bytesDown.addAndGet(len);
                } else {
                    totalBytesUp.addAndGet(len);
                    record.bytesUp.addAndGet(len);
                }
                // === 统计注入结束 ===
            }
        } catch (IOException e) {
            // 连接正常关闭
        }
    }
}
