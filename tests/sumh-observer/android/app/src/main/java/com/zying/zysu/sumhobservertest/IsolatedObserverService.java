package com.zying.zysu.sumhobservertest;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public final class IsolatedObserverService extends Service {
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final AtomicBoolean running = new AtomicBoolean();
    private final IBinder binder = new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
            if (code != ObserverWire.COLLECT) return false;
            ParcelFileDescriptor incoming = null;
            try {
                data.enforceInterface(ObserverWire.SERVICE);
                String token = data.readString();
                if (token == null || token.length() > 128) throw new IllegalArgumentException("Invalid session token");
                byte[] bytes = ObserverWire.readBytes(data, RequestData.MAX_REQUEST);
                int fdMode = data.readInt(); if (fdMode != 0 && fdMode != 1) throw new IllegalArgumentException("Invalid descriptor mode");
                incoming = fdMode == 1 ? ParcelFileDescriptor.CREATOR.createFromParcel(data) : null;
                IBinder callback = data.readStrongBinder(); ObserverWire.end(data);
                if (callback == null) throw new IllegalArgumentException("Missing callback");
                if (!running.compareAndSet(false, true)) { ObserverWire.respond(callback, token, null, "An isolated collection is still running"); return true; }
                ParcelFileDescriptor fd = incoming; incoming = null;
                OwnedCollection task = new OwnedCollection(fd, () -> run(token, bytes, fd, callback), () -> running.set(false));
                try { worker.execute(task); }
                catch (RuntimeException e) { task.cancel(); throw e; }
            } catch (Exception ignored) {
                // Malformed one-way transport produces no success; caller's bounded timeout remains authoritative.
            } finally { if (incoming != null) try { incoming.close(); } catch (Exception ignored) {} }
            return true;
        }
    };
    private void run(String token, byte[] bytes, ParcelFileDescriptor fd, IBinder callback) {
        try {
            RequestData request = RequestData.parse(bytes);
            byte[] result = NativeBridge.nCollect(request.text, "isolated", fd == null ? -1 : fd.getFd()).getBytes(StandardCharsets.UTF_8);
            if (result.length > RequestData.MAX_RESULT) throw new IllegalArgumentException("Result exceeds byte limit");
            ObserverWire.respond(callback, token, result, null);
        } catch (Exception | LinkageError e) {
            try { ObserverWire.respond(callback, token, null, e.getClass().getSimpleName() + ": " + String.valueOf(e.getMessage())); } catch (Exception ignored) {}
        }
    }
    @Override public IBinder onBind(Intent intent) { return binder; }
    @Override public void onDestroy() {
        for (Runnable queued : worker.shutdownNow()) if (queued instanceof OwnedCollection task) task.cancel();
        super.onDestroy();
    }
    /** Shutdown may discard a queued task. Only its winner owns descriptor cleanup. */
    static final class OwnedCollection implements Runnable {
        private final AtomicBoolean claimed = new AtomicBoolean();
        private final ParcelFileDescriptor fd;
        private final Runnable collect, finished;
        OwnedCollection(ParcelFileDescriptor fd, Runnable collect, Runnable finished) { this.fd=fd; this.collect=collect; this.finished=finished; }
        @Override public void run() {
            if (!claimed.compareAndSet(false,true)) return;
            try { collect.run(); } finally { release(); }
        }
        void cancel() { if (claimed.compareAndSet(false,true)) release(); }
        private void release() { try { if (fd!=null) fd.close(); } catch (Exception ignored) {} finally { finished.run(); } }
    }
}
