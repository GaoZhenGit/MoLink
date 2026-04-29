package com.molink.worker;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.RadioButton;
import android.widget.RadioGroup;
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
    private RadioGroup protocolRadioGroup;
    private RadioButton socks5Radio;
    private RadioButton httpRadio;
    private boolean isSwitching = false;

    private final Handler uiHandler = new Handler(Looper.getMainLooper());

    private static final long UI_REFRESH_INTERVAL_MS = 500;

    private final Runnable uiPoller = new Runnable() {
        @Override
        public void run() {
            if (!isFinishing()) {
                Socks5ProxyService svc = Socks5ProxyService.getInstance();
                if (svc != null && svc.isRunning()) {
                    showRunning(svc);
                    List<ConnectionRecord> snapshot = Socks5ProxyService.getConnectionSnapshot();
                    connCount.setText(String.valueOf(snapshot.size()));
                    historyCount.setText(String.valueOf(svc.getHistoryCount()));
                    bytesDown.setText(formatBytes(svc.getTotalBytesDown()));
                    bytesUp.setText(formatBytes(svc.getTotalBytesUp()));
                    statusUptime.setText("在线:" + formatUptime(svc.getUptime()));
                    logAdapter.refreshAll(snapshot);
                }
                uiHandler.postDelayed(this, UI_REFRESH_INTERVAL_MS);
            }
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

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

        protocolRadioGroup = findViewById(R.id.protocolRadioGroup);
        socks5Radio = findViewById(R.id.socks5Radio);
        httpRadio = findViewById(R.id.httpRadio);

        // 协议切换监听：停止旧服务 → 启动新协议服务 → 轮询自动同步 UI
        protocolRadioGroup.setOnCheckedChangeListener((group, checkedId) -> {
            if (isSwitching) return;
            String newType = checkedId == R.id.socks5Radio ? "socks5" : "http";
            Socks5ProxyService svc = Socks5ProxyService.getInstance();

            isSwitching = true;
            socks5Radio.setEnabled(false);
            httpRadio.setEnabled(false);

            if (svc != null && svc.isRunning()) {
                stopService(new Intent(this, Socks5ProxyService.class));
                Intent newIntent = new Intent(this, Socks5ProxyService.class);
                newIntent.putExtra("proxy_type", newType);
                uiHandler.postDelayed(() -> {
                    startService(newIntent);
                }, 500);
            } else {
                Intent newIntent = new Intent(this, Socks5ProxyService.class);
                newIntent.putExtra("proxy_type", newType);
                startService(newIntent);
            }
            uiHandler.removeCallbacks(uiPoller);
            uiHandler.postDelayed(uiPoller, 2000);
        });
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

    public void onToggleClick(View v) {
        Socks5ProxyService svc = Socks5ProxyService.getInstance();
        if (svc != null && svc.isRunning()) {
            stopService(new Intent(this, Socks5ProxyService.class));
            showStopped();
        } else {
            Intent intent = new Intent(this, Socks5ProxyService.class);
            intent.putExtra("proxy_type", getCurrentProxyTypeString());
            startService(intent);
            showRunning(null);
        }
        uiHandler.removeCallbacks(uiPoller);
        uiHandler.postDelayed(uiPoller, 1500);
    }

    private String getCurrentProxyTypeString() {
        if (protocolRadioGroup.getCheckedRadioButtonId() == R.id.httpRadio) {
            return "http";
        }
        return "socks5";
    }

    private void showStopped() {
        toggleButton.setText("启动服务");
        String protocol = getCurrentProxyTypeString();
        statusRunning.setText(("http".equals(protocol) ? "HTTP" : "SOCKS5") + " 服务已停止");
        statusDot.setBackgroundResource(R.drawable.circle_gray);
        statusPort.setVisibility(View.GONE);
        statusUptime.setVisibility(View.GONE);
        connCount.setText("--");
        historyCount.setText("--");
        bytesDown.setText("--");
        bytesUp.setText("--");
        logAdapter.refreshAll(java.util.Collections.emptyList());
        socks5Radio.setEnabled(true);
        httpRadio.setEnabled(true);
        isSwitching = false;
    }

    private void showRunning(Socks5ProxyService svc) {
        toggleButton.setText("停止服务");
        ProxyProtocol proto = svc != null ? svc.getProxyType() :
                ("http".equals(getCurrentProxyTypeString()) ? ProxyProtocol.HTTP : ProxyProtocol.SOCKS5);
        statusRunning.setText(proto.getDisplayName() + " 运行中");
        statusDot.setBackgroundResource(R.drawable.circle_green);
        statusPort.setVisibility(View.VISIBLE);
        statusUptime.setVisibility(View.VISIBLE);
        if (svc != null) {
            statusPort.setText("端口:" + svc.getPort());
            statusUptime.setText("在线:" + formatUptime(svc.getUptime()));
            connCount.setText(String.valueOf(svc.getConnectionCount()));
            // 根据实际协议同步 RadioGroup 选中状态
            protocolRadioGroup.check(svc.getProxyType() == ProxyProtocol.HTTP ? R.id.httpRadio : R.id.socks5Radio);
        }
        socks5Radio.setEnabled(true);
        httpRadio.setEnabled(true);
        isSwitching = false;
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
