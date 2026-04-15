package com.molink.worker;

import com.molink.worker.BuildConfig;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;
import java.util.ArrayList;
import java.util.Collections;
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

    /** 全量更新（由 Handler 定时刷新时调用）。过滤掉 localhost/dadb tunnel 流量，超量时裁剪旧记录。最新连接显示在底部。 */
    public void refreshAll(List<ConnectionRecord> newItems) {
        items.clear();
        // activeConnections newest-first，所以先反转：oldest-first，再取前 MAX_ITEMS
        List<ConnectionRecord> filtered = new ArrayList<>();
        // v3 Req 1: synchronizedList 迭代需在 synchronized 块中，防止并发修改
        synchronized (newItems) {
            for (ConnectionRecord r : newItems) {
                if (r == null) continue;
                String host = r.targetHost;
                if (host == null) continue;
                boolean isLocalhost = host.startsWith("127.") || host.equals("::1") || host.equals("0:0:0:0:0:0:0:1");
                boolean isStatusPort = r.targetPort == BuildConfig.STATUS_HTTP_PORT;
                if (!isLocalhost && !isStatusPort) {
                    filtered.add(r);
                }
            }
        }
        // 反转： newest-first → oldest-first
        Collections.reverse(filtered);
        // 取最近的 MAX_ITEMS 条（反转后末尾是最新的）
        int start = Math.max(0, filtered.size() - MAX_ITEMS);
        for (int i = start; i < filtered.size(); i++) {
            items.add(filtered.get(i));
        }
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
            targetHost = itemView.findViewById(R.id.connTargetHost);
            traffic = itemView.findViewById(R.id.connTraffic);
            duration = itemView.findViewById(R.id.connDuration);
        }

        void bind(ConnectionRecord record) {
            targetHost.setText(record.getDisplayHost());

            // bytesDown = c->s = 用户上传 = ↑ ; bytesUp = s->c = 用户下载 = ↓
            long up = record.getBytesDown();
            long down = record.getBytesUp();
            traffic.setText("\u2193 " + formatBytes(down) + "  \u2191 " + formatBytes(up));

            long secs = record.getDurationSec();
            duration.setText(formatDuration(secs));

            if (record.isEnded()) {
                dot.setBackgroundResource(R.drawable.circle_gray);  // 已结束：灰色
            } else {
                dot.setBackgroundResource(R.drawable.circle_green);  // 进行中：绿色
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
            if (secs < 60) return "1\u5206\u5185";
            if (secs < 3600) return (secs / 60) + "m";
            return String.format("%.1fh", secs / 3600.0);
        }
    }
}
