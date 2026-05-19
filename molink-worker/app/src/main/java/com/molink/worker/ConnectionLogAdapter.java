package com.molink.worker;

import com.molink.worker.BuildConfig;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.List;
import java.util.Locale;

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

    public void addTop(ConnectionRecord record) {
        if (items.size() >= MAX_ITEMS) {
            items.remove(items.size() - 1);
            notifyItemRemoved(items.size());
        }
        items.add(0, record);
        notifyItemInserted(0);
    }

    public void refreshAll(List<ConnectionRecord> newItems) {
        items.clear();
        List<ConnectionRecord> filtered = new ArrayList<>();
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
        Collections.reverse(filtered);
        int start = Math.max(0, filtered.size() - MAX_ITEMS);
        for (int i = start; i < filtered.size(); i++) {
            items.add(filtered.get(i));
        }
        notifyDataSetChanged();
    }

    static class ViewHolder extends RecyclerView.ViewHolder {
        private final View dot;
        private final TextView targetHost;
        private final TextView protocol;
        private final TextView startTime;
        private final TextView traffic;
        private final TextView duration;
        private final SimpleDateFormat timeFmt = new SimpleDateFormat("HH:mm:ss", Locale.getDefault());

        ViewHolder(@NonNull View itemView) {
            super(itemView);
            dot = itemView.findViewById(R.id.connDot);
            targetHost = itemView.findViewById(R.id.connTargetHost);
            protocol = itemView.findViewById(R.id.connProtocol);
            startTime = itemView.findViewById(R.id.connStartTime);
            traffic = itemView.findViewById(R.id.connTraffic);
            duration = itemView.findViewById(R.id.connDuration);
        }

        void bind(ConnectionRecord record) {
            targetHost.setText(record.getDisplayHost());

            protocol.setText(record.protocol);
            if ("SOCKS5".equals(record.protocol)) {
                protocol.setBackgroundColor(0xFF2196F3);
            } else {
                protocol.setBackgroundColor(0xFFE91E63);
            }

            startTime.setText(timeFmt.format(new Date(record.startTime)));

            long up = record.getBytesDown();
            long down = record.getBytesUp();
            traffic.setText("↓ " + formatBytes(down) + "  ↑ " + formatBytes(up));

            long secs = record.getDurationSec();
            duration.setText(formatDuration(secs));

            if (record.isEnded()) {
                dot.setBackgroundResource(R.drawable.circle_gray);
            } else {
                dot.setBackgroundResource(R.drawable.circle_green);
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
            long m = secs / 60;
            long s = secs % 60;
            if (m == 0) return s + "s";
            if (secs < 3600) return m + "m" + s + "s";
            long h = secs / 3600;
            long hm = (secs % 3600) / 60;
            return h + "h" + hm + "m";
        }
    }
}
