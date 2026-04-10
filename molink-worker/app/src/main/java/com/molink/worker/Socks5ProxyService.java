package com.molink.worker;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.Build;
import android.os.IBinder;

import androidx.core.app.NotificationCompat;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;

public class Socks5ProxyService extends Service {
    private static final int SOCKS5_PORT = 1080;
    private static final String CHANNEL_ID = "Socks5ProxyChannel";
    private static final int NOTIFICATION_ID = 1;

    private ServerSocket serverSocket;
    private volatile boolean isRunning = false;
    private Thread serverThread;

    private final IBinder binder = new LocalBinder();

    public class LocalBinder extends Binder {
        Socks5ProxyService getService() {
            return Socks5ProxyService.this;
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startForeground(NOTIFICATION_ID, createNotification());
        startSocks5Server();
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        isRunning = false;
        if (serverSocket != null) {
            try {
                serverSocket.close();
            } catch (IOException e) {
                e.printStackTrace();
            }
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
                .setContentText("SOCKS5 代理服务运行中")
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .build();
    }

    private void startSocks5Server() {
        if (isRunning) {
            return; // 已运行
        }
        isRunning = true;
        serverThread = new Thread(() -> {
            try {
                serverSocket = new ServerSocket(SOCKS5_PORT);
                while (isRunning) {
                    Socket clientSocket = serverSocket.accept();
                    new Thread(() -> handleClient(clientSocket)).start();
                }
            } catch (IOException e) {
                if (isRunning) {
                    e.printStackTrace();
                }
            }
        });
        serverThread.start();
    }

    private void handleClient(Socket clientSocket) {
        try {
            InputStream in = clientSocket.getInputStream();
            OutputStream out = clientSocket.getOutputStream();

            // SOCKS5 握手
            int ver = in.read();
            if (ver != 0x05) {
                clientSocket.close();
                return;
            }

            int nMethods = in.read();
            byte[] methods = new byte[nMethods];
            in.read(methods);

            // 发送无认证响应
            out.write(0x05);
            out.write(0x00);

            // 读取连接请求
            ver = in.read();
            if (ver != 0x05) {
                clientSocket.close();
                return;
            }

            int cmd = in.read();
            int rsv = in.read();
            int addrType = in.read();

            if (cmd != 0x01) { // 只支持 CONNECT
                out.write(0x05);
                out.write(0x07); // Command not supported
                out.write(0x00);
                out.write(0x01);
                out.write(new byte[6]);
                clientSocket.close();
                return;
            }

            String destAddr;
            int destPort;

            if (addrType == 0x01) { // IPv4
                byte[] ip = new byte[4];
                in.read(ip);
                destAddr = String.format("%d.%d.%d.%d", ip[0] & 0xFF, ip[1] & 0xFF, ip[2] & 0xFF, ip[3] & 0xFF);
            } else if (addrType == 0x03) { // 域名
                int len = in.read();
                byte[] domain = new byte[len];
                in.read(domain);
                destAddr = new String(domain);
            } else {
                clientSocket.close();
                return;
            }

            destPort = (in.read() << 8) | in.read();

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

            // 双向转发数据
            Thread t1 = new Thread(() -> forward(clientSocket, targetSocket));
            Thread t2 = new Thread(() -> forward(targetSocket, clientSocket));
            t1.start();
            t2.start();

            t1.join();
            t2.join();

            targetSocket.close();
            clientSocket.close();
        } catch (Exception e) {
            e.printStackTrace();
            try {
                clientSocket.close();
            } catch (IOException ex) {
                ex.printStackTrace();
            }
        }
    }

    private void forward(Socket src, Socket dest) {
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
            // 连接关闭
        }
    }
}
