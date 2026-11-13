package dev.zysu.visibilityprobe;

import android.os.Process;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.FileDescriptor;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.Set;

/** Observes only what this process can read through normal Android/Linux APIs. */
final class ProbeSnapshot {
    private static final int MAX_PIDS = 128;
    private static final int MAX_FILE_BYTES = 8192;
    private static final int MAX_SNAPSHOT_BYTES = 128 * 1024;
    private static final long SCAN_BUDGET_MS = 2000;
    private static final Set<String> STATUS_FIELDS = Set.of(
            "Name", "Tgid", "Pid", "PPid", "Uid", "Gid", "CapInh", "CapPrm",
            "CapEff", "CapBnd", "CapAmb", "NoNewPrivs", "Seccomp");

    private ProbeSnapshot() {}

    static JSONObject capture(String observer, int[] requestedPids) throws JSONException {
        long started = SystemClock.elapsedRealtime();
        JSONObject result = new JSONObject();
        result.put("observer", observer);
        result.put("startedElapsedRealtimeMs", started);
        result.put("pid", Process.myPid());
        result.put("tid", Process.myTid());
        result.put("uid", Os.getuid());
        result.put("euid", Os.geteuid());
        result.put("gid", Os.getgid());
        result.put("egid", Os.getegid());
        result.put("selfStatus", read("/proc/self/status", true));
        result.put("threadStatus", read("/proc/thread-self/status", true));
        result.put("threadSelinux", read("/proc/thread-self/attr/current", false));

        LinkedHashSet<Integer> pids = new LinkedHashSet<>();
        if (requestedPids != null) {
            for (int pid : requestedPids) {
                if (pid > 0 && pids.size() < 8) pids.add(pid);
            }
        }
        result.put("requestedPids", new JSONArray(new ArrayList<>(pids)));
        pids.add(Process.myPid());
        pids.add(1);
        JSONObject enumeration = new JSONObject();
        boolean truncated = false;
        int seen = 0;
        try (DirectoryStream<java.nio.file.Path> directory =
                     Files.newDirectoryStream(Paths.get("/proc"))) {
            for (java.nio.file.Path entry : directory) {
                if (SystemClock.elapsedRealtime() - started >= SCAN_BUDGET_MS) {
                    truncated = true;
                    break;
                }
                String name = entry.getFileName().toString();
                if (!name.matches("[0-9]{1,10}")) continue;
                long value = Long.parseLong(name);
                if (value <= 0 || value > Integer.MAX_VALUE) continue;
                seen++;
                if (pids.size() == MAX_PIDS && !pids.contains((int) value)) {
                    truncated = true;
                    break;
                }
                pids.add((int) value);
            }
            enumeration.put("outcome", "readable");
        } catch (Exception error) {
            enumeration = error(error);
        }
        enumeration.put("numericEntriesVisited", seen);
        enumeration.put("truncated", truncated);
        enumeration.put("pidLimit", MAX_PIDS);
        result.put("enumeration", enumeration);

        ArrayList<Integer> ordered = new ArrayList<>(pids);
        // Explicit targets remain first so the bounded sample always attempts them.
        int targetCount = requestedPids == null ? 0 : Math.min(requestedPids.length, 8);
        if (targetCount == 0) Collections.sort(ordered);
        JSONArray observations = new JSONArray();
        int bytes = result.toString().getBytes(StandardCharsets.UTF_8).length;
        boolean observationsTruncated = false;
        for (int pid : ordered) {
            if (SystemClock.elapsedRealtime() - started >= SCAN_BUDGET_MS) {
                observationsTruncated = true;
                break;
            }
            JSONObject process = new JSONObject();
            process.put("pid", pid);
            process.put("status", read("/proc/" + pid + "/status", true));
            process.put("selinux", read("/proc/" + pid + "/attr/current", false));
            int size = process.toString().getBytes(StandardCharsets.UTF_8).length;
            if (bytes + size + 1024 > MAX_SNAPSHOT_BYTES) {
                observationsTruncated = true;
                break;
            }
            observations.put(process);
            bytes += size;
        }
        result.put("processes", observations);
        result.put("observationsTruncated", observationsTruncated);
        result.put("fileByteLimit", MAX_FILE_BYTES);
        result.put("snapshotByteLimit", MAX_SNAPSHOT_BYTES);
        result.put("scanBudgetMs", SCAN_BUDGET_MS);
        result.put("durationMs", SystemClock.elapsedRealtime() - started);
        return result;
    }

    private static JSONObject read(String path, boolean status) throws JSONException {
        FileDescriptor descriptor = null;
        try {
            descriptor = Os.open(path, OsConstants.O_RDONLY | OsConstants.O_CLOEXEC
                    | OsConstants.O_NONBLOCK, 0);
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[1024];
            boolean truncated = false;
            for (;;) {
                int count = Os.read(descriptor, buffer, 0,
                        Math.min(buffer.length, MAX_FILE_BYTES + 1 - output.size()));
                if (count == 0) break;
                output.write(buffer, 0, count);
                if (output.size() > MAX_FILE_BYTES) {
                    truncated = true;
                    break;
                }
            }
            byte[] bytes = output.toByteArray();
            String text = new String(bytes, 0, Math.min(bytes.length, MAX_FILE_BYTES),
                    StandardCharsets.UTF_8).replace("\u0000", "").trim();
            JSONObject result = new JSONObject();
            result.put("outcome", "readable");
            result.put("truncated", truncated);
            if (status) {
                JSONObject fields = new JSONObject();
                for (String line : text.split("\n")) {
                    int colon = line.indexOf(':');
                    if (colon <= 0) continue;
                    String key = line.substring(0, colon);
                    if (STATUS_FIELDS.contains(key)) {
                        String value = line.substring(colon + 1).trim();
                        fields.put(key, value.substring(0, Math.min(value.length(), 512)));
                        if (value.length() > 512) result.put("truncated", true);
                    }
                }
                result.put("fields", fields);
            } else {
                result.put("value", text.substring(0, Math.min(text.length(), 512)));
                if (text.length() > 512) result.put("truncated", true);
            }
            return result;
        } catch (Exception failure) {
            return error(failure);
        } finally {
            if (descriptor != null) {
                try { Os.close(descriptor); } catch (ErrnoException ignored) {}
            }
        }
    }

    static JSONObject error(Throwable failure) throws JSONException {
        JSONObject result = new JSONObject();
        int errno = 0;
        for (Throwable cause = failure; cause != null; cause = cause.getCause()) {
            if (cause instanceof ErrnoException) {
                errno = ((ErrnoException) cause).errno;
                break;
            }
        }
        String outcome = "error";
        if (errno == OsConstants.EACCES || errno == OsConstants.EPERM
                || failure instanceof SecurityException
                || failure instanceof java.nio.file.AccessDeniedException) {
            outcome = "permission_denied";
        } else if (errno == OsConstants.ENOENT || errno == OsConstants.ESRCH
                || failure instanceof java.nio.file.NoSuchFileException) {
            outcome = "missing";
        }
        result.put("outcome", outcome);
        if (errno != 0) result.put("errno", errno);
        result.put("errorClass", failure.getClass().getSimpleName());
        return result;
    }
}
