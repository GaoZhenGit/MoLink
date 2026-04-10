package com.molink.worker;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.IBinder;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;

public class MainActivity extends Activity {
    private TextView statusText;
    private Button toggleButton;
    private boolean isServiceBound = false;
    private Socks5ProxyService proxyService;

    private ServiceConnection serviceConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            Socks5ProxyService.LocalBinder binder = (Socks5ProxyService.LocalBinder) service;
            proxyService = binder.getService();
            isServiceBound = true;
            updateStatus();
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            isServiceBound = false;
            updateStatus();
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        statusText = findViewById(R.id.statusText);
        toggleButton = findViewById(R.id.toggleButton);

        // 绑定服务检查状态
        Intent intent = new Intent(this, Socks5ProxyService.class);
        bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE);
    }

    @Override
    protected void onResume() {
        super.onResume();
        updateStatus();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (isServiceBound) {
            unbindService(serviceConnection);
            isServiceBound = false;
        }
    }

    private void updateStatus() {
        runOnUiThread(() -> {
            if (proxyService != null && proxyService.isRunning()) {
                statusText.setText("SOCKS5 服务运行中\n端口: " + proxyService.getPort());
                toggleButton.setText("停止服务");
            } else {
                statusText.setText("SOCKS5 服务已停止");
                toggleButton.setText("启动服务");
            }
        });
    }

    public void onToggleClick(View v) {
        Intent intent = new Intent(this, Socks5ProxyService.class);

        if (proxyService != null && proxyService.isRunning()) {
            stopService(intent);
        } else {
            startService(intent);
        }

        // 延迟更新状态
        new android.os.Handler().postDelayed(this::updateStatus, 500);
    }
}
