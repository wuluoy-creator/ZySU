package dev.zysu.visibilityprobe;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class ProbeActivity extends Activity {
    private static final int EXPORT = 1;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private EditText scenario;
    private EditText targets;
    private TextView status;
    private Button capture;
    private Button export;
    private JSONObject report;
    private String completedReport;
    private ServiceConnection connection;
    private boolean collecting;
    private boolean bound;
    private int generation;
    private int[] requestedPids;
    private final Runnable timeout = () -> {
        if (collecting) finishIsolated("{\"outcome\":\"timeout\"}");
    };
    private final Messenger replies = new Messenger(new Handler(Looper.getMainLooper(), msg -> {
        if (msg.what == IsolatedProbeService.CAPTURE && collecting && msg.arg1 == generation) {
            finishIsolated(msg.getData().getString("snapshot", "{}"));
        }
        return true;
    }));

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        int padding = (int) (20 * getResources().getDisplayMetrics().density);
        content.setPadding(padding, padding, padding, padding);
        content.setOnApplyWindowInsetsListener((view, insets) -> {
            android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars());
            view.setPadding(padding + bars.left, padding + bars.top,
                    padding + bars.right, padding + bars.bottom);
            return insets;
        });
        TextView title = new TextView(this);
        title.setText("ZySU Visibility Probe");
        title.setTextSize(24);
        content.addView(title);
        TextView description = new TextView(this);
        description.setText("Capture what this ordinary app and its isolated process can read. "
                + "No root access is requested. A denied read is recorded as denied.");
        content.addView(description);
        scenario = new EditText(this);
        scenario.setHint("Scenario (for example: Manager closed, modules off)");
        scenario.setSingleLine(true);
        scenario.setFilters(new android.text.InputFilter[] {new android.text.InputFilter.LengthFilter(256)});
        content.addView(scenario);
        targets = new EditText(this);
        targets.setHint("Optional target PIDs, comma separated (up to 8)");
        targets.setSingleLine(true);
        targets.setFilters(new android.text.InputFilter[] {new android.text.InputFilter.LengthFilter(96)});
        content.addView(targets);
        capture = new Button(this);
        capture.setText("Capture");
        capture.setOnClickListener(view -> beginCapture());
        content.addView(capture);
        export = new Button(this);
        export.setText("Export JSON");
        export.setEnabled(false);
        export.setOnClickListener(view -> {
            Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("application/json");
            intent.putExtra(Intent.EXTRA_TITLE, "visibility-" + System.currentTimeMillis() + ".json");
            startActivityForResult(intent, EXPORT);
        });
        content.addView(export);
        status = new TextView(this);
        status.setText("Ready. Keep this probe unapproved in ZySU.");
        content.addView(status);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        setContentView(scroll);
        if (state != null) {
            scenario.setText(state.getString("scenario", ""));
            targets.setText(state.getString("targets", ""));
        }
        Object retained = getLastNonConfigurationInstance();
        if (retained instanceof String) {
            completedReport = (String) retained;
            export.setEnabled(true);
            status.setText("Capture ready to export.");
        }
    }

    private void beginCapture() {
        try {
            String text = targets.getText().toString().trim();
            String[] values = text.isEmpty() ? new String[0] : text.split(",", -1);
            if (values.length > 8) throw new IllegalArgumentException();
            requestedPids = new int[values.length];
            for (int i = 0; i < values.length; i++) {
                requestedPids[i] = Integer.parseInt(values[i].trim());
                if (requestedPids[i] <= 0) throw new IllegalArgumentException();
            }
        } catch (IllegalArgumentException error) {
            status.setText("Enter up to 8 positive PID numbers separated by commas.");
            return;
        }
        collecting = true;
        generation++;
        final int captureGeneration = generation;
        completedReport = null;
        capture.setEnabled(false);
        export.setEnabled(false);
        status.setText("Capturing ordinary process…");
        try {
            report = new JSONObject();
            report.put("schemaVersion", 1);
            report.put("capturedAtEpochMs", System.currentTimeMillis());
            String label = scenario.getText().toString();
            report.put("scenario", label.substring(0, Math.min(label.length(), 256)));
            report.put("packageName", getPackageName());
            android.content.pm.PackageInfo info = getPackageManager().getPackageInfo(
                    getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES);
            report.put("probeVersion", info.versionName);
            org.json.JSONArray signers = new org.json.JSONArray();
            if (info.signingInfo != null) {
                for (android.content.pm.Signature signer : info.signingInfo.getApkContentsSigners()) {
                    byte[] digest = java.security.MessageDigest.getInstance("SHA-256")
                            .digest(signer.toByteArray());
                    StringBuilder hex = new StringBuilder();
                    for (byte value : digest) hex.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
                    signers.put(hex.toString());
                }
            }
            report.put("signerSha256", signers);
            report.put("debuggable", (getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0);
            report.put("sdk", Build.VERSION.SDK_INT);
            report.put("androidRelease", Build.VERSION.RELEASE);
            report.put("securityPatch", Build.VERSION.SECURITY_PATCH);
            report.put("manufacturer", Build.MANUFACTURER);
            report.put("model", Build.MODEL);
            report.put("fingerprint", Build.FINGERPRINT);
            report.put("kernelRelease", android.system.Os.uname().release);
        } catch (Exception error) {
            collecting = false;
            capture.setEnabled(true);
            status.setText("Could not initialize report: " + error.getClass().getSimpleName());
            return;
        }
        worker.execute(() -> {
            JSONObject snapshot;
            try {
                snapshot = ProbeSnapshot.capture("ordinary", requestedPids);
            } catch (Exception error) {
                snapshot = new JSONObject();
                try {
                    snapshot.put("outcome", "capture_failed");
                    snapshot.put("errorClass", error.getClass().getSimpleName());
                } catch (Exception ignored) {}
            }
            JSONObject captured = snapshot;
            main.post(() -> {
                if (!collecting || isDestroyed() || captureGeneration != generation) return;
                try { report.put("ordinary", captured); } catch (Exception ignored) {}
                bindIsolated();
            });
        });
    }

    private void bindIsolated() {
        status.setText("Capturing isolated process…");
        connection = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder service) {
                if (!collecting) return;
                Message request = Message.obtain(null, IsolatedProbeService.CAPTURE);
                request.arg1 = generation;
                request.replyTo = replies;
                Bundle data = new Bundle();
                data.putIntArray("pids", requestedPids);
                request.setData(data);
                try {
                    new Messenger(service).send(request);
                } catch (Exception error) {
                    finishIsolated("{\"outcome\":\"binder_failed\"}");
                }
            }
            @Override public void onServiceDisconnected(ComponentName name) {
                if (collecting) finishIsolated("{\"outcome\":\"service_disconnected\"}");
            }
            @Override public void onNullBinding(ComponentName name) {
                if (collecting) finishIsolated("{\"outcome\":\"null_binding\"}");
            }
            @Override public void onBindingDied(ComponentName name) {
                if (collecting) finishIsolated("{\"outcome\":\"binding_died\"}");
            }
        };
        try {
            bound = bindService(new Intent(this, IsolatedProbeService.class), connection, BIND_AUTO_CREATE);
            if (!bound) {
                finishIsolated("{\"outcome\":\"bind_failed\"}");
                return;
            }
            main.postDelayed(timeout, 15000);
        } catch (Exception error) {
            finishIsolated("{\"outcome\":\"bind_failed\"}");
        }
    }

    private void finishIsolated(String json) {
        if (!collecting) return;
        collecting = false;
        releaseBinding();
        try {
            report.put("isolated", new JSONObject(json));
            report.put("snapshotsAreSequential", true);
            completedReport = report.toString(2);
            status.setText("Capture complete. Export JSON to compare both observers. "
                    + "Missing PIDs may have exited; denied reads do not prove hiding.");
            export.setEnabled(true);
        } catch (Exception error) {
            status.setText("Report failed: " + error.getClass().getSimpleName());
        }
        capture.setEnabled(true);
    }

    private void releaseBinding() {
        main.removeCallbacks(timeout);
        if (bound && connection != null) {
            unbindService(connection);
            bound = false;
        }
        connection = null;
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != EXPORT || result != RESULT_OK || data == null || data.getData() == null
                || completedReport == null) return;
        android.net.Uri uri = data.getData();
        String json = completedReport;
        worker.execute(() -> {
            String message;
            try (OutputStream output = getContentResolver().openOutputStream(uri, "wt")) {
                if (output == null) throw new java.io.IOException();
                output.write(json.getBytes(StandardCharsets.UTF_8));
                message = "JSON exported.";
            } catch (Exception error) {
                message = "Export failed: " + error.getClass().getSimpleName();
            }
            String text = message;
            main.post(() -> { if (!isDestroyed()) status.setText(text); });
        });
    }

    @Override protected void onDestroy() {
        collecting = false;
        releaseBinding();
        worker.shutdownNow();
        super.onDestroy();
    }

    @Override protected void onSaveInstanceState(Bundle state) {
        // Keep the large report out of the Activity saved-state Binder transaction.
        state.putString("scenario", scenario.getText().toString());
        state.putString("targets", targets.getText().toString());
        super.onSaveInstanceState(state);
    }

    @Override public Object onRetainNonConfigurationInstance() { return completedReport; }
}
