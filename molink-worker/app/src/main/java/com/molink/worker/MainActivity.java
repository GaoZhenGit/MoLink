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

    // v3 Req 1: 0.5 秒全量轮询，完全替代 StatusListener 回调
    private static final long UI_REFRESH_INTERVAL_MS = 500;

    private final Runnable uiPoller = new Runnable() {
        @Override
        public void run() {
            if (!isFinishing()) {
                Socks5ProxyService svc = Socks5ProxyService.getInstance();
                if (svc != null && svc.isRunning()) {
                    showRunning(svc);
                    logAdapter.refreshAll(Socks5ProxyService.getConnectionSnapshot());  // 全量刷新
                }
                uiHandler.postDelayed(this, UI_REFRESH_INTERVAL_MS);
            }
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
        uiHandler.removeCallbacks(uiPoller);
        uiHandler.postDelayed(uiPoller, 300);
    }

    @Override
    protected void onPause() {
        super.onPause();
        uiHandler.removeCallbacks(uiPoller);
    }

    /** 显式启动 / 显式停止 */
    public void onToggleClick(View v) {
        Socks5ProxyService svc = Socks5ProxyService.getInstance();
        if (svc != null && svc.isRunning()) {
            // ===== 显式停止 =====
            stopService(new Intent(this, Socks5ProxyService.class));
            // 立即刷新 UI
            showStopped();
        } else {
            // ===== 显式启动 =====
            startService(new Intent(this, Socks5ProxyService.class));
            // 乐观更新 UI
            showRunning(null);
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
