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

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketException;
import java.util.concurrent.atomic.AtomicInteger;

public class Socks5ProxyService extends Service {

    private static final String TAG = "Socks5ProxyService";
    private static final int SOCKS5_PORT = 1080;
    private static final String CHANNEL_ID = "Socks5ProxyChannel";
    private static final int NOTIFICATION_ID = 1;

    private ServerSocket serverSocketV4;
    private ServerSocket serverSocketV6;
    private volatile boolean isRunning = false;
    private volatile long startTime = 0;
    private final AtomicInteger connectionCount = new AtomicInteger(0);
    private Thread serverThreadV4;
    private Thread serverThreadV6;
    private StatusHttpServer httpServer;

    private final IBinder binder = new LocalBinder();

    public class LocalBinder extends Binder {
        Socks5ProxyService getService() {
            return Socks5ProxyService.this;
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
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
                httpServer = new StatusHttpServer(8081, this);
                httpServer.start();
                Log.i(TAG, "HTTP status server started on port 8081");
            } catch (IOException e) {
                Log.e(TAG, "Failed to start HTTP server: " + e.getMessage());
            }
        });
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Log.d(TAG, "onDestroy, stopping server");
        isRunning = false;
        closeQuietly(serverSocketV4);
        closeQuietly(serverSocketV6);
        if (httpServer != null) {
            httpServer.stop();
        }
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
                .setSmallIcon(android.R.drawable.ic_dialog_info)
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

            // 双向转发数据
            Thread t1 = new Thread(() -> forward(clientSocket, targetSocket, "c->s"), "Forward-c->s");
            Thread t2 = new Thread(() -> forward(targetSocket, clientSocket, "s->c"), "Forward-s->c");
            t1.start();
            t2.start();

            t1.join();
            t2.join();

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

    private void forward(Socket src, Socket dest, String direction) {
        try {
            InputStream in = src.getInputStream();
            OutputStream out = dest.getOutputStream();
            byte[] buffer = new byte[8192];
            int len;
            while ((len = in.read(buffer)) != -1) {
                out.write(buffer, 0, len);
                out.flush();
            }
        } catch (IOException e) {
            // 连接正常关闭
        }
    }
}
