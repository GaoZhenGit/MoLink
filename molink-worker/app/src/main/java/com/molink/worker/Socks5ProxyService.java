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

import com.molink.worker.netty.ConnectionLifecycleManager;
import com.molink.worker.netty.HttpProxyStateHandler;
import com.molink.worker.netty.MixProtocolDetector;
import com.molink.worker.netty.Socks5StateHandler;

import io.netty.bootstrap.ServerBootstrap;
import io.netty.channel.Channel;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.nio.NioServerSocketChannel;
import io.netty.handler.codec.http.HttpObjectAggregator;
import io.netty.handler.codec.http.HttpRequestDecoder;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

public class Socks5ProxyService extends Service {

    private static final String TAG = "Socks5ProxyService";
    private static final int SOCKS5_PORT = BuildConfig.SOCKS_PORT;
    private static final int STATUS_HTTP_PORT = BuildConfig.STATUS_HTTP_PORT;
    private static final String CHANNEL_ID = "Socks5ProxyChannel";
    private static final int NOTIFICATION_ID = 1;
    // 历史记录上限，超出时 FIFO 删除最旧记录
    private static final int MAX_HISTORY = 50;

    /** MainActivity 通过此静态引用直接访问服务，无需绑定 */
    private static volatile Socks5ProxyService instance;

    public static Socks5ProxyService getInstance() {
        return instance;
    }

    private NioEventLoopGroup eventLoopGroup;
    private volatile boolean isRunning = false;
    private volatile long startTime = 0;
    private ProxyProtocol proxyType = ProxyProtocol.MIX;
    private StatusHttpServer httpServer;

    // ========== UI 统计相关（static 供静态方法直接访问）==========
    private static final AtomicInteger historyCount = new AtomicInteger(0);
    private static final AtomicLong totalBytesDown = new AtomicLong(0);
    private static final AtomicLong totalBytesUp = new AtomicLong(0);
    // static final synchronized list，两层共享同一引用，无复制开销
    // 写操作（add/remove）由 synchronized 保护
    private static final List<ConnectionRecord> activeConnections = Collections.synchronizedList(new ArrayList<>());

    public void addConnectionRecord(ConnectionRecord record) {
        synchronized (activeConnections) {
            while (activeConnections.size() >= MAX_HISTORY) {
                activeConnections.remove(0);  // 删除最旧记录（FIFO）
            }
            activeConnections.add(record);
            historyCount.incrementAndGet();
        }
    }

    public static void addBytesDown(long len) {
        totalBytesDown.addAndGet(len);
    }

    public static void addBytesUp(long len) {
        totalBytesUp.addAndGet(len);
    }

    public int getConnectionCount() {
        return activeConnections.size();
    }

    public static void registerConnection(ConnectionRecord record) {
        if (instance != null) {
            instance.addConnectionRecord(record);
        }
    }

    public static List<ConnectionRecord> getConnectionSnapshot() {
        return activeConnections;
    }

    public long getTotalBytesDown() { return totalBytesDown.get(); }
    public long getTotalBytesUp() { return totalBytesUp.get(); }
    public int getHistoryCount() { return historyCount.get(); }

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
        // 读取 Intent 中的协议类型
        if (intent != null) {
            String typeStr = intent.getStringExtra("proxy_type");
            if (typeStr != null) {
                proxyType = ProxyProtocol.fromString(typeStr);
            }
        }
        Log.d(TAG, "onStartCommand, starting foreground, protocol=" + proxyType.getDisplayName());
        startForeground(NOTIFICATION_ID, createNotification());
        startServer();
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
        if (eventLoopGroup != null) {
            eventLoopGroup.shutdownGracefully();
        }
        if (httpServer != null) {
            httpServer.stop();
        }
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

    public ProxyProtocol getProxyType() {
        return proxyType;
    }

    public void setProxyType(ProxyProtocol type) {
        this.proxyType = type;
        updateNotification();
    }

    private void updateNotification() {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) {
            manager.notify(NOTIFICATION_ID, createNotification());
        }
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
        String protocol = proxyType.getDisplayName();
        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle("MoLink Worker")
                .setContentText(protocol + " 代理服务运行中，端口 " + SOCKS5_PORT)
                .setSmallIcon(R.drawable.ic_notification)
                .build();
    }

    private void startServer() {
        if (isRunning) {
            Log.w(TAG, "Server already running");
            return;
        }
        isRunning = true;
        startTime = SystemClock.elapsedRealtime() / 1000;

        eventLoopGroup = new NioEventLoopGroup();

        ServerBootstrap bs = new ServerBootstrap();
        bs.group(eventLoopGroup)
          .channel(NioServerSocketChannel.class)
          .childHandler(new ChannelInitializer<Channel>() {
              @Override
              protected void initChannel(Channel ch) throws Exception {
                  if (proxyType == ProxyProtocol.MIX) {
                      ch.pipeline().addLast("readTimeoutHandler",
                              new io.netty.handler.timeout.ReadTimeoutHandler(5, java.util.concurrent.TimeUnit.SECONDS));
                      ch.pipeline().addLast(new MixProtocolDetector());
                  } else if (proxyType == ProxyProtocol.HTTP) {
                      ch.pipeline().addLast(new HttpProxyStateHandler.ProtocolDetectingHandler());
                      ch.pipeline().addLast("httpDecoder", new HttpRequestDecoder());
                      ch.pipeline().addLast("httpAggregator", new HttpObjectAggregator(8192));
                      ch.pipeline().addLast(new HttpProxyStateHandler());
                  } else {
                      ch.pipeline().addLast(new Socks5StateHandler());
                  }
              }
          });

        try {
            bs.bind("127.0.0.1", SOCKS5_PORT).syncUninterruptibly();
            Log.i(TAG, "ServerSocket listening on 127.0.0.1:" + SOCKS5_PORT + " (" + proxyType.getDisplayName() + ")");
        } catch (Exception e) {
            Log.e(TAG, "Failed to bind server socket", e);
            isRunning = false;
        }

        Log.i(TAG, proxyType.getDisplayName() + " proxy server started on port " + SOCKS5_PORT);
    }
}
