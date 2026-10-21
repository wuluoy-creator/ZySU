package com.zying.zysu.sumhobservertest;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.net.Uri;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.SecureRandom;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Application lifetime owner. Activity recreation only detaches listeners; it never submits a new run. */
final class ObserverCoordinator {
    interface Listener { void changed(); }
    interface NativeCalls {
        String collect(String request, String role, int fd);
        String create(String filesDir, String runId);
        void validate(String request);
    }
    private final NativeCalls nativeCalls;
    private final Context context;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final OperationGate gate = new OperationGate(randomId());
    private final File stateDir;
    private Listener listener;
    private boolean workerOccupied;
    private String activeToken, activeDigest;
    private Runnable timeout;
    private ServiceConnection connection;
    private boolean bound;
    private IBinder remote;
    private IBinder.DeathRecipient death;
    RequestData request;
    byte[] ordinary, isolated;
    String ordinaryError = "", isolatedError = "", detail = "";
    int status = R.string.status_ready;
    ExportSnapshot pendingExport;

    ObserverCoordinator(Context context) {
        this(context, new NativeCalls() {
            public String collect(String request, String role, int fd) { return NativeBridge.nCollect(request, role, fd); }
            public String create(String filesDir, String runId) { return NativeBridge.nCreateFixture(filesDir, runId); }
            public void validate(String request) { NativeBridge.nValidateRequest(request); }
        });
    }
    ObserverCoordinator(Context context, NativeCalls calls) {
        this.context = context; nativeCalls = calls; stateDir = new File(context.getFilesDir(), "observer-state");
        restore();
    }
    void attach(Listener listener) { this.listener = listener; listener.changed(); }
    void detach(Listener listener) { if (this.listener == listener) this.listener = null; }
    boolean busy() { return gate.busy() || workerOccupied; }
    private void changed() { if (listener != null) listener.changed(); }
    static String randomId() { byte[] b = new byte[16]; new SecureRandom().nextBytes(b); StringBuilder result = new StringBuilder(); for (byte v : b) result.append(String.format(java.util.Locale.ROOT, "%02x", v & 255)); return result.toString(); }
    private void restore() {
        try {
            File pointer = new File(stateDir, "current");
            if (!pointer.exists()) { if (new File(stateDir, "pending").exists()) status = R.string.status_interrupted; return; }
            String digest = new String(read(pointer, 64), StandardCharsets.US_ASCII);
            if (!digest.matches("[0-9a-f]{64}")) throw new IllegalArgumentException("Invalid saved request pointer");
            RequestData saved = RequestData.parse(read(new File(new File(stateDir, digest), "request.json"), RequestData.MAX_REQUEST));
            if (!saved.digest.equals(digest)) throw new IllegalArgumentException("Saved request digest mismatch");
            request = saved;
            ordinary = restoreSample("ordinary"); isolated = restoreSample("isolated");
            ordinaryError = restoreError("ordinary"); isolatedError = restoreError("isolated");
            File pending = new File(stateDir, "pending");
            if (pending.exists()) {
                status = R.string.status_interrupted;
                String[] fields = StrictJson.utf8(read(pending, 4096),4096).split("\n", -1);
                if (fields.length == 3 && fields[1].equals(request.digest)) recordActorError(fields[2],context.getString(R.string.status_interrupted));
            }
            else status = R.string.status_restored;
        } catch (Exception e) { request = null; ordinary = null; isolated = null; status = R.string.status_error; detail = message(e); }
    }
    private byte[] restoreSample(String role) throws Exception {
        File f = new File(runDir(), role + ".json"); if (!f.exists()) return null;
        byte[] bytes = read(f, RequestData.MAX_RESULT);
        // Restored samples retain their original process identity; they are never relabeled as this process.
        var map = StrictJson.object(StrictJson.parse(bytes, RequestData.MAX_RESULT));
        if (!request.digest.equals(map.get("request_sha256")) || !request.runId.equals(map.get("run_id")) || !role.equals(map.get("requested_role"))) throw new IllegalArgumentException("Saved sample does not match request");
        return bytes;
    }
    private String restoreError(String role) throws Exception {
        File f = new File(runDir(), role + ".error"); return f.exists() ? StrictJson.utf8(read(f, 4096), 4096) : "";
    }
    private File runDir() { return new File(stateDir, request.digest); }
    private static byte[] read(File file, int limit) throws Exception { try (InputStream in = new FileInputStream(file)) { return RequestData.readBounded(in, limit); } }
    private static void write(File file, byte[] bytes) throws Exception {
        File parent = file.getParentFile(); if (parent == null || (!parent.isDirectory() && !parent.mkdirs())) throw new IllegalStateException("Cannot create private evidence directory");
        // AtomicFile.finishWrite only logs rename failure. The public NIO move reports it,
        // so a failed evidence replacement cannot be acknowledged as a saved sample.
        File staged = new File(parent, file.getName() + ".new");
        try {
            try (FileOutputStream out = new FileOutputStream(staged)) { out.write(bytes); out.getFD().sync(); }
            Files.move(staged.toPath(),file.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        } catch (Exception e) { if (staged.exists() && !staged.delete()) e.addSuppressed(new java.io.IOException("Cannot remove staged evidence file")); throw e; }
    }
    private void persistRequest(RequestData data) throws Exception {
        write(new File(new File(stateDir, data.digest), "request.json"), data.bytes);
        write(new File(stateDir, "current"), data.digest.getBytes(StandardCharsets.US_ASCII));
    }
    private String begin(String digest) { return begin(digest, ""); }
    private String begin(String digest, String actor) {
        if (busy()) throw new IllegalStateException("A previous operation is still running");
        String token = gate.begin(digest); activeToken = token; activeDigest = digest;
        status = R.string.status_working; detail = "";
        try { write(new File(stateDir, "pending"), (token + "\n" + digest + "\n" + actor).getBytes(StandardCharsets.UTF_8)); }
        catch (Exception e) { gate.finish(token, digest); throw new IllegalStateException("Cannot journal operation", e); }
        timeout = () -> {
            if (gate.finish(token, digest)) {
                closeService(); status = R.string.status_unknown; detail = "";
                if (request != null && request.digest.equals(digest)) {
                    try { recordActorError(actor,context.getString(R.string.status_unknown)); }
                    catch (Exception e) { detail = message(e); }
                }
                // Keep pending on disk: process recreation must not reinterpret an unknown operation as success.
                changed();
            }
        };
        main.postDelayed(timeout, 10000); changed(); return token;
    }
    private boolean finish(String token, String digest, int nextStatus, String message) {
        if (!gate.finish(token, digest)) return false;
        if (timeout != null) main.removeCallbacks(timeout);
        closeService(); status = nextStatus; detail = message;
        File pending = new File(stateDir, "pending");
        if (pending.exists() && !pending.delete()) { status = R.string.status_error; detail = "Cannot remove private operation journal"; }
        return true;
    }
    private void recordActorError(String actor, String error) throws Exception {
        if (!actor.equals("ordinary") && !actor.equals("isolated")) return;
        if (actor.equals("ordinary")) ordinaryError = error; else isolatedError = error;
        write(new File(runDir(), actor + ".error"),error.getBytes(StandardCharsets.UTF_8));
    }
    private void work(Runnable task) {
        workerOccupied = true;
        worker.execute(() -> { try { task.run(); } finally { main.post(() -> { workerOccupied = false; changed(); }); } });
    }
    void createFixture() {
        String token = begin("create");
        work(() -> {
            try {
                RequestData created = RequestData.parse(nativeCalls.create(context.getFilesDir().getAbsolutePath(), randomId()).getBytes(StandardCharsets.UTF_8));
                main.post(() -> installRequest(token, "create", created));
            } catch (Exception | LinkageError e) { fail(token, "create", e); }
        });
    }
    void importRequest(Uri uri) {
        String token = begin("import");
        work(() -> {
            try (InputStream in = context.getContentResolver().openInputStream(uri)) {
                byte[] bytes = RequestData.readBounded(in, RequestData.MAX_REQUEST);
                nativeCalls.validate(StrictJson.utf8(bytes, RequestData.MAX_REQUEST));
                RequestData imported = RequestData.parse(bytes);
                main.post(() -> installRequest(token, "import", imported));
            } catch (Exception | LinkageError e) { fail(token, "import", e); }
        });
    }
    private void installRequest(String token, String digest, RequestData data) {
        if (!gate.accept(token, digest)) return;
        try {
            persistRequest(data); request = data;
            ordinary = null; isolated = null; ordinaryError = ""; isolatedError = "";
            ordinary = restoreSample("ordinary"); isolated = restoreSample("isolated"); ordinaryError = restoreError("ordinary"); isolatedError = restoreError("isolated");
            finish(token, digest, R.string.status_request, "");
        } catch (Exception e) { finish(token, digest, R.string.status_error, message(e)); }
        changed();
    }
    void collectOrdinary() {
        RequestData selected = requireRequest(); String token = begin(selected.digest, "ordinary");
        work(() -> {
            try {
                byte[] bytes = nativeCalls.collect(selected.text, "ordinary", -1).getBytes(StandardCharsets.UTF_8);
                main.post(() -> receive(token, selected, "ordinary", bytes, null));
            } catch (Exception | LinkageError e) { main.post(() -> receive(token, selected, "ordinary", null, message(e))); }
        });
    }
    void collectIsolated() {
        RequestData selected = requireRequest(); String token = begin(selected.digest, "isolated");
        connection = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder service) {
                if (!gate.accept(token, selected.digest)) return;
                remote = service;
                death = () -> main.post(() -> receive(token, selected, "isolated", null, context.getString(R.string.service_died)));
                try { service.linkToDeath(death, 0); }
                catch (Exception e) { receive(token, selected, "isolated", null, message(e)); return; }
                work(() -> {
                    ParcelFileDescriptor map = null;
                    String fdFailure = "";
                    try { map = DelegatedMap.open(selected); }
                    catch (Exception e) { fdFailure = "Delegated FD unavailable: " + message(e); }
                    final String fdNote = fdFailure;
                    try {
                        ObserverWire.collect(service, token, selected.bytes, map, new ObserverWire.Callback() {
                            @Override void delivered(String responseToken, byte[] result, String error) {
                                if (!token.equals(responseToken)) return;
                                main.post(() -> receive(token, selected, "isolated", result, error, fdNote));
                            }
                            @Override void malformed(RuntimeException error) { main.post(() -> receive(token, selected, "isolated", null, message(error))); }
                        });
                    } catch (Exception e) { main.post(() -> receive(token, selected, "isolated", null, message(e))); }
                    finally { if (map != null) try { map.close(); } catch (Exception ignored) {} }
                });
            }
            @Override public void onServiceDisconnected(ComponentName name) { receive(token, selected, "isolated", null, context.getString(R.string.service_died)); }
            @Override public void onNullBinding(ComponentName name) { receive(token, selected, "isolated", null, context.getString(R.string.service_rejected)); }
            @Override public void onBindingDied(ComponentName name) { receive(token, selected, "isolated", null, context.getString(R.string.service_died)); }
        };
        try { bound = context.bindService(new Intent(context, IsolatedObserverService.class), connection, Context.BIND_AUTO_CREATE); if (!bound) receive(token, selected, "isolated", null, context.getString(R.string.service_rejected)); }
        catch (Exception e) { receive(token, selected, "isolated", null, message(e)); }
    }
    private void receive(String token, RequestData selected, String role, byte[] result, String error) { receive(token, selected, role, result, error, ""); }
    private void receive(String token, RequestData selected, String role, byte[] result, String error, String note) {
        if (!gate.accept(token, selected.digest) || request == null || !request.digest.equals(selected.digest)) return;
        try {
            if (error == null) {
                ResultGuard.verify(result, selected, role, Process.myUid(), Process.myPid());
                write(new File(runDir(), role + ".json"), result);
                write(new File(runDir(), role + ".error"), note.getBytes(StandardCharsets.UTF_8));
                if (role.equals("ordinary")) { ordinary = result; ordinaryError = note; } else { isolated = result; isolatedError = note; }
                finish(token, selected.digest, R.string.status_collected, note);
            } else {
                recordActorError(role,error);
                finish(token, selected.digest, R.string.status_error, error);
            }
        } catch (Exception e) { finish(token, selected.digest, R.string.status_error, message(e)); }
        changed();
    }
    private void fail(String token, String digest, Throwable e) { main.post(() -> { if (finish(token, digest, R.string.status_error, message(e))) changed(); }); }
    private void closeService() {
        if (remote != null && death != null) try { remote.unlinkToDeath(death, 0); } catch (RuntimeException ignored) {}
        remote = null; death = null;
        if (connection != null && bound) { try { context.unbindService(connection); } catch (IllegalArgumentException ignored) {} }
        bound = false; connection = null;
    }
    private RequestData requireRequest() { if (request == null) throw new IllegalStateException("Create or import a request first"); return request; }
    ExportSnapshot prepareExport(String role) {
        RequestData selected = requireRequest();
        byte[] bytes = role.equals("request") ? selected.bytes : role.equals("ordinary") ? ordinary : isolated;
        if (bytes == null) throw new IllegalStateException("No collected sample for this observer");
        return pendingExport = new ExportSnapshot(selected.digest, bytes, role + "-" + selected.runId + ".json");
    }
    void export(Uri uri) {
        ExportSnapshot snapshot = pendingExport; pendingExport = null;
        if (snapshot == null || request == null || !snapshot.digest.equals(request.digest)) { report(R.string.export_expired, ""); return; }
        String token = begin(request.digest); String digest = request.digest;
        work(() -> {
            try (OutputStream out = context.getContentResolver().openOutputStream(uri, "wt")) {
                if (out == null) throw new IllegalStateException("No output stream"); out.write(snapshot.bytes); out.flush();
                main.post(() -> { if (finish(token, digest, R.string.status_exported, "")) changed(); });
            } catch (Exception e) { fail(token, digest, e); }
        });
    }
    void report(int message, String detail) { this.status = message; this.detail = detail; changed(); }
    void close() {
        if (activeToken != null) gate.finish(activeToken, activeDigest);
        if (timeout != null) main.removeCallbacks(timeout);
        closeService(); listener = null; worker.shutdownNow();
    }
    private static String message(Throwable e) { String text = e.getClass().getSimpleName() + ": " + String.valueOf(e.getMessage()); return text.length() > 1024 ? text.substring(0, 1024) : text; }
    static final class ExportSnapshot {
        final String digest, filename; final byte[] bytes;
        ExportSnapshot(String digest, byte[] bytes, String filename) { this.digest = digest; this.bytes = bytes.clone(); this.filename = filename; }
    }
}
