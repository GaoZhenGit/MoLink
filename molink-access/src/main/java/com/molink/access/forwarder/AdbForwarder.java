package com.molink.access.forwarder;

import com.molink.access.adb.AdbClientManager;
import okio.BufferedSink;
import okio.BufferedSource;
import okio.Okio;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * 自定义 ADB 端口转发器，解决 dadb TcpForwarder 的 FIN 不传递问题。
 *
 * 问题：dadb TcpForwarder 在 client socket EOF 时不关闭 AdbStream，
 * 导致远程端永远收不到 FIN，连接无法释放。
 *
 * 修复：读写两端均监听对方关闭事件，任一端关闭时同时关闭 client socket 和 adb stream，
 * 确保 FIN 能正确传递到远程。
 *
 * 使用 Okio 2.x API（dadb 1.2.10 依赖 Okio 2.10.0）。
 */
public class AdbForwarder implements AutoCloseable {

    private static final Logger log = LoggerFactory.getLogger(AdbForwarder.class);

    private final AdbClientManager adbClient;
    private final int localPort;
    private final int remotePort;
    private final AtomicBoolean started = new AtomicBoolean(false);
    private volatile boolean running = true;
    private ServerSocket server;
    private ExecutorService executor;

    public AdbForwarder(AdbClientManager adbClient, int localPort, int remotePort) {
        this.adbClient = adbClient;
        this.localPort = localPort;
        this.remotePort = remotePort;
    }

    public void start() throws IOException {
        if (!started.getAndSet(true)) {
            running = true;
            executor = Executors.newCachedThreadPool(r -> {
                Thread t = new Thread(r, "AdbForwarder");
                t.setDaemon(true);
                return t;
            });
            executor.submit(this::runServer);
            log.info("AdbForwarder started: localhost:{} -> Android:{}", localPort, remotePort);
        }
    }

    private void runServer() {
        try {
            server = new ServerSocket(localPort);
            server.setReuseAddress(true);
            while (running) {
                try {
                    Socket clientSocket = server.accept();
                    executor.submit(() -> handleClient(clientSocket));
                } catch (Exception e) {
                    if (running) log.warn("Server accept error: {}", e.getMessage());
                }
            }
        } catch (Exception e) {
            log.error("Server error: {}", e.getMessage(), e);
        }
    }

    private void handleClient(Socket clientSocket) {
        dadb.AdbStream adbStream = null;
        try {
            clientSocket.setSoTimeout(30000);
            clientSocket.setKeepAlive(true);

            dadb.Dadb dadbConn = adbClient.getDadb();
            if (dadbConn == null) {
                clientSocket.close();
                return;
            }

            // Open ADB stream to remote port
            final dadb.AdbStream stream = dadbConn.open("tcp:localhost:" + remotePort);
            adbStream = stream;
            log.debug("ADB stream opened for client: {}", clientSocket.getRemoteSocketAddress());

            // Bidirectional copy: client <-> adb stream
            // Each direction runs in its own thread so they can independently detect close
            Thread c2s = new Thread(() -> copyStreamToAdb(clientSocket, stream), "c2s");
            Thread s2c = new Thread(() -> copyAdbToStream(stream, clientSocket), "s2c");
            c2s.start();
            s2c.start();

            // Wait for either direction to finish
            c2s.join();
            s2c.join();

        } catch (Exception e) {
            log.debug("Client handler error: {}", e.getMessage());
        } finally {
            // Key fix: always close both sides when done
            safeCloseSocket(clientSocket);
            safeCloseAdbStream(adbStream);
            log.debug("Client connection cleaned up");
        }
    }

    private void copyStreamToAdb(Socket clientSocket, dadb.AdbStream adbStream) {
        try {
            BufferedSource in = Okio.buffer(Okio.source(clientSocket));
            BufferedSink out = adbStream.getSink();
            byte[] buf = new byte[8192];
            int len;
            while ((len = in.read(buf)) != -1) {
                out.write(buf, 0, len);
                out.flush();
            }
            // EOF from client → close the write end of adb stream (sends FIN to remote)
            adbStream.getSink().close();
            log.debug("c->s: EOF reached");
        } catch (Exception e) {
            log.debug("c->s: error: {}", e.getMessage());
        } finally {
            // Also close the adb stream on error to propagate close to remote
            safeCloseAdbStream(adbStream);
            safeCloseSocket(clientSocket);
        }
    }

    private void copyAdbToStream(dadb.AdbStream adbStream, Socket clientSocket) {
        try {
            BufferedSource in = adbStream.getSource();
            BufferedSink out = Okio.buffer(Okio.sink(clientSocket));
            byte[] buf = new byte[8192];
            int len;
            while ((len = in.read(buf)) != -1) {
                out.write(buf, 0, len);
                out.flush();
            }
            // EOF from remote → close the write end of client socket (sends FIN to client)
            clientSocket.shutdownOutput();
            log.debug("s->c: EOF reached");
        } catch (Exception e) {
            log.debug("s->c: error: {}", e.getMessage());
        } finally {
            // Also close the adb stream on error to stop reading
            safeCloseAdbStream(adbStream);
            safeCloseSocket(clientSocket);
        }
    }

    private void safeCloseSocket(Socket sock) {
        if (sock == null) return;
        try {
            sock.shutdownInput();
        } catch (Exception ignored) {}
        try {
            sock.shutdownOutput();
        } catch (Exception ignored) {}
        try {
            sock.close();
        } catch (Exception ignored) {}
    }

    private void safeCloseAdbStream(dadb.AdbStream stream) {
        if (stream == null) return;
        try {
            stream.close();
        } catch (Exception ignored) {}
    }

    public void stop() {
        if (!started.getAndSet(false)) return;
        running = false;
        if (server != null) {
            try { server.close(); } catch (Exception ignored) {}
        }
        if (executor != null) {
            executor.shutdownNow();
            try { executor.awaitTermination(2, java.util.concurrent.TimeUnit.SECONDS); } catch (Exception ignored) {}
        }
        log.info("AdbForwarder stopped");
    }

    @Override
    public void close() {
        stop();
    }
}
