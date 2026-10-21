package com.zying.zysu.sumhobservertest;

import android.annotation.SuppressLint;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructStat;
import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** Pins every path component without following links; only the fixed verified map leaves this class. */
final class DelegatedMap {
    private DelegatedMap() {}
    @SuppressLint("SdCardPath") // Versioned request protocol fixes the synthetic path families; creation still uses Context.filesDir.
    static ParcelFileDescriptor open(RequestData request) throws Exception {
        String ownedTail;
        ParcelFileDescriptor directory;
        if (request.fixtureRoot.startsWith("/data/user/") || request.fixtureRoot.startsWith("/data/user_de/")) {
            int boundary = request.fixtureRoot.indexOf("/files/sumh-observer/") + "/files".length();
            // /data/user/0 is an Android-owned alias on some releases. The final files directory
            // and every app-owned component remain no-follow; only platform ancestors may resolve.
            directory = openFd(request.fixtureRoot.substring(0, boundary), true);
            ownedTail = request.fixtureRoot.substring(boundary + 1);
        } else { directory = openFd("/", true); ownedTail = request.fixtureRoot.substring(1); }
        try {
            for (String part : ownedTail.split("/")) {
                ParcelFileDescriptor next = openFd("/proc/self/fd/" + directory.getFd() + "/" + part, true);
                directory.close(); directory = next;
            }
            String anchor = "/proc/self/fd/" + directory.getFd() + "/";
            try (ParcelFileDescriptor marker = openFd(anchor + "fixture.txt", false)) {
                byte[] expected = ("sumh-observer-fixture-v1\n" + request.runId + "\n").getBytes(StandardCharsets.UTF_8);
                verify(marker, expected);
            }
            ParcelFileDescriptor map = openFd(anchor + "map.bin", false);
            try {
                byte[] expected = new byte[4096]; byte[] pattern = "0123456789abcdef".getBytes(StandardCharsets.US_ASCII);
                for (int i = 0; i < expected.length; i++) expected[i] = pattern[i % pattern.length];
                verify(map, expected);
                return map;
            } catch (Exception e) { map.close(); throw e; }
        } finally { directory.close(); }
    }
    private static ParcelFileDescriptor openFd(String path, boolean directory) throws Exception {
        // O_DIRECTORY is not exposed by Android's public OsConstants. A nonblocking no-follow
        // open followed by fstat before any traversal retains the directory check without magic flags.
        FileDescriptor fd = Os.open(path, OsConstants.O_RDONLY | OsConstants.O_CLOEXEC | OsConstants.O_NOFOLLOW | OsConstants.O_NONBLOCK, 0);
        try {
            if (directory && !OsConstants.S_ISDIR(Os.fstat(fd).st_mode)) throw new IOException("Synthetic anchor is not a directory");
            return ParcelFileDescriptor.dup(fd);
        } finally { Os.close(fd); }
    }
    private static void verify(ParcelFileDescriptor fd, byte[] expected) throws Exception {
        StructStat stat = Os.fstat(fd.getFileDescriptor());
        if (!OsConstants.S_ISREG(stat.st_mode) || stat.st_size != expected.length || stat.st_nlink != 1) throw new IOException("Unexpected synthetic fixture type, size or link count");
        byte[] actual = new byte[expected.length]; int offset = 0;
        while (offset < actual.length) { int n = Os.pread(fd.getFileDescriptor(), actual, offset, actual.length - offset, offset); if (n <= 0) throw new IOException("Incomplete synthetic fixture"); offset += n; }
        if (!Arrays.equals(actual, expected)) throw new IOException("Synthetic fixture content mismatch");
    }
}
