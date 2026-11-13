package dev.zysu.visibilityprobe;

import android.app.Service;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.RemoteException;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** No inherited app permissions or storage access; results travel over Binder only. */
public final class IsolatedProbeService extends Service {
    static final int CAPTURE = 1;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Messenger messenger = new Messenger(new Handler(Looper.getMainLooper(), msg -> {
        if (msg.what != CAPTURE || msg.replyTo == null) return false;
        Messenger reply = msg.replyTo;
        int generation = msg.arg1;
        int[] targets = msg.getData().getIntArray("pids");
        worker.execute(() -> {
            String json;
            try {
                json = ProbeSnapshot.capture("isolated", targets).toString();
            } catch (Exception error) {
                json = "{\"outcome\":\"capture_failed\"}";
            }
            Message response = Message.obtain(null, CAPTURE);
            response.arg1 = generation;
            Bundle data = new Bundle();
            data.putString("snapshot", json);
            response.setData(data);
            try { reply.send(response); } catch (RemoteException ignored) {}
        });
        return true;
    }));

    @Override public IBinder onBind(Intent intent) { return messenger.getBinder(); }

    @Override public void onDestroy() {
        worker.shutdownNow();
        super.onDestroy();
    }
}
