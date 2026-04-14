package com.molink.worker;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import java.util.List;

public class MainActivity extends Activity {

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
    private Button toggleButton;

    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private final Runnable uiPoller = new Runnable() {
        @Override
        public void run() {
            if (!isFinishing()) {
                updateUI(Socks5ProxyService.getInstance());
                uiHandler.postDelayed(this, 2000);
            }
        }
    };

    // StatusListener：通过静态实例注册，服务销毁后自动失效
    private final Socks5ProxyService.StatusListener statusListener =
            new Socks5ProxyService.StatusListener() {
                @Override
                public void onStatusUpdate(long uptime, int connectionCountVal, int historyCountVal,
                                           long bytesDownVal, long bytesUpVal,
                                           List<ConnectionRecord> activeConnections) {
                    runOnUiThread(() -> {
                        connCount.setText(String.valueOf(connectionCountVal));
                        historyCount.setText(String.valueOf(historyCountVal));
                        bytesDown.setText(formatBytes(bytesUpVal));
                        bytesUp.setText(formatBytes(bytesDownVal));
                        statusUptime.setText("在线:" + formatUptime(uptime));
                        logAdapter.refreshAll(activeConnections);
                    });
                }
            };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        statusDot = findViewById(R.id.statusDot);
        statusRunning = findViewById(R.id.statusRunning);
        statusPort = findViewById(R.id.statusPort);
        statusUptime = findViewById(R.id.statusUptime);
        connCount = findViewById(R.id.connCount);
        historyCount = findViewById(R.id.historyCount);
        bytesDown = findViewById(R.id.bytesDown);
        bytesUp = findViewById(R.id.bytesUp);
        connectionLogList = findViewById(R.id.connectionLogList);
        toggleButton = findViewById(R.id.toggleButton);

        logAdapter = new ConnectionLogAdapter();
        connectionLogList.setLayoutManager(new LinearLayoutManager(this));
        connectionLogList.setAdapter(logAdapter);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // 注册 StatusListener（如果服务正在运行）
        Socks5ProxyService svc = Socks5ProxyService.getInstance();
        if (svc != null) svc.addStatusListener(statusListener);

        uiHandler.removeCallbacks(uiPoller);
        uiHandler.postDelayed(uiPoller, 300);
    }

    @Override
    protected void onPause() {
        super.onPause();
        uiHandler.removeCallbacks(uiPoller);
        Socks5ProxyService svc = Socks5ProxyService.getInstance();
        if (svc != null) svc.removeStatusListener(statusListener);
    }

    /** 显式启动 / 显式停止 */
    public void onToggleClick(View v) {
        Socks5ProxyService svc = Socks5ProxyService.getInstance();
        if (svc != null && svc.isRunning()) {
            // ===== 显式停止 =====
            svc.removeStatusListener(statusListener);
            stopService(new Intent(this, Socks5ProxyService.class));
            // 立即刷新 UI
            showStopped();
        } else {
            // ===== 显式启动 =====
            startService(new Intent(this, Socks5ProxyService.class));
            // 服务启动需要短暂时间，乐观更新 UI
            showRunning(null);
            // 500ms 后注册 StatusListener（此时服务通常已 onCreate）
            uiHandler.postDelayed(() -> {
                Socks5ProxyService started = Socks5ProxyService.getInstance();
                if (started != null) started.addStatusListener(statusListener);
            }, 500);
        }
        // 重新触发轮询，确保状态最终一致
        uiHandler.removeCallbacks(uiPoller);
        uiHandler.postDelayed(uiPoller, 1500);
    }

    private void updateUI(Socks5ProxyService svc) {
        if (svc == null || !svc.isRunning()) {
            showStopped();
        } else {
            showRunning(svc);
            // 确保 StatusListener 已注册
            svc.addStatusListener(statusListener);
        }
    }

    private void showStopped() {
        toggleButton.setText("启动服务");
        statusRunning.setText("SOCKS5 服务已停止");
        statusDot.setBackgroundResource(R.drawable.circle_gray);
        statusPort.setVisibility(View.GONE);
        statusUptime.setVisibility(View.GONE);
        connCount.setText("--");
        historyCount.setText("--");
        bytesDown.setText("--");
        bytesUp.setText("--");
        logAdapter.refreshAll(java.util.Collections.emptyList());
    }

    private void showRunning(Socks5ProxyService svc) {
        toggleButton.setText("停止服务");
        statusRunning.setText("SOCKS5 运行中");
        statusDot.setBackgroundResource(R.drawable.circle_green);
        statusPort.setVisibility(View.VISIBLE);
        statusUptime.setVisibility(View.VISIBLE);
        if (svc != null) {
            statusPort.setText("端口:" + svc.getPort());
            statusUptime.setText("在线:" + formatUptime(svc.getUptime()));
            connCount.setText(String.valueOf(svc.getConnectionCount()));
        }
    }

    private String formatBytes(long bytes) {
        if (bytes < 0) return "--";
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return String.format("%.1f KB", bytes / 1024.0);
        return String.format("%.1f MB", bytes / (1024.0 * 1024));
    }

    private String formatUptime(long secs) {
        if (secs < 0) return "0s";
        if (secs < 60) return secs + "s";
        if (secs < 3600) return (secs / 60) + "m";
        return String.format("%.1fh", secs / 3600.0);
    }
}
