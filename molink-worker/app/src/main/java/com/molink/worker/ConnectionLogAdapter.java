package com.molink.worker;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;
import java.util.ArrayList;
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

    /** 全量更新（由 Handler 定时刷新时调用）。过滤掉 localhost/dadb tunnel 流量。 */
    public void refreshAll(List<ConnectionRecord> newItems) {
        items.clear();
        for (ConnectionRecord r : newItems) {
            // 过滤掉 localhost 和 HTTP status server 端口（dadb tunnel / /api/status 请求）
            if (r == null) continue;
            String host = r.targetHost;
            if (host == null) continue;
            boolean isLocalhost = host.startsWith("127.") || host.equals("::1") || host.equals("0:0:0:0:0:0:0:1");
            boolean isStatusPort = r.targetPort == 8081;
            if (!isLocalhost && !isStatusPort) {
                items.add(r);
            }
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
            targetHost = itemView.findViewById(R.id.targetHost);
            traffic = itemView.findViewById(R.id.connTraffic);
            duration = itemView.findViewById(R.id.connDuration);
        }

        void bind(ConnectionRecord record) {
            targetHost.setText(record.getDisplayHost());

            long down = record.getBytesDown();
            long up = record.getBytesUp();
            traffic.setText("↓" + formatBytes(down) + " ↑" + formatBytes(up));

            long secs = record.getDurationSec();
            duration.setText(formatDuration(secs));

            if (record.active) {
                dot.setBackgroundResource(R.drawable.circle_green);
            } else {
                dot.setBackgroundResource(R.drawable.circle_gray);
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
            if (secs < 60) return secs + "s";
            if (secs < 3600) return (secs / 60) + "m";
            return String.format("%.1fh", secs / 3600.0);
        }
    }
}
