package com.zying.zysu.sumhobservertest;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Map;
import java.util.Set;

final class RequestData {
    static final int MAX_REQUEST = 32768, MAX_RESULT = 262144;
    final byte[] bytes;
    final String text, runId, fixtureRoot, digest;
    private RequestData(byte[] bytes, String runId, String fixtureRoot) {
        this.bytes = bytes.clone(); this.text = StrictJson.utf8(bytes, MAX_REQUEST);
        this.runId = runId; this.fixtureRoot = fixtureRoot; digest = sha256(bytes);
    }
    static RequestData parse(byte[] bytes) {
        Map<String, Object> map = StrictJson.object(StrictJson.parse(bytes, MAX_REQUEST));
        if (!map.keySet().equals(Set.of("schema_version", "resource", "run_id", "fixture_root", "context"))) throw new IllegalArgumentException("Unknown or missing request key");
        if (!(map.get("schema_version") instanceof StrictJson.NumberToken n) || !n.text.equals("1")) throw new IllegalArgumentException("Unsupported schema");
        if (!"sumh_observer_request".equals(map.get("resource"))) throw new IllegalArgumentException("Wrong resource");
        String runId = StrictJson.string(map.get("run_id")), root = StrictJson.string(map.get("fixture_root"));
        if (!runId.matches("[0-9a-f]{32}")) throw new IllegalArgumentException("Invalid run ID");
        if (root.getBytes(StandardCharsets.UTF_8).length > 1024 || !root.matches("(?:/data/user(?:_de)?/(?:0|[1-9][0-9]{0,9})/com\\.zying\\.zysu\\.sumhobservertest/files|/data/local/tmp)/sumh-observer/" + runId)) throw new IllegalArgumentException("Fixture path is outside synthetic scope");
        Map<String, Object> context = StrictJson.object(map.get("context"));
        if (!context.keySet().equals(Set.of("boot_id", "config_revision"))) throw new IllegalArgumentException("Invalid context keys");
        Object boot = context.get("boot_id"), revision = context.get("config_revision");
        if (boot != null && (!(boot instanceof String s) || !s.matches("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"))) throw new IllegalArgumentException("Invalid context boot ID");
        if (revision != null && (!(revision instanceof String s) || !s.matches("sha256-v1:[0-9a-f]{64}"))) throw new IllegalArgumentException("Invalid context revision");
        return new RequestData(bytes, runId, root);
    }
    static byte[] readBounded(InputStream in, int limit) throws IOException {
        if (in == null) throw new IOException("No document stream");
        ByteArrayOutputStream out = new ByteArrayOutputStream(); byte[] buffer = new byte[4096];
        while (true) { int n = in.read(buffer, 0, Math.min(buffer.length, limit + 1 - out.size())); if (n < 0) break; if (n == 0) throw new IOException("No progress while reading document"); out.write(buffer, 0, n); if (out.size() > limit) throw new IOException("Document exceeds byte limit"); }
        return out.toByteArray();
    }
    static String sha256(byte[] bytes) {
        try { StringBuilder b = new StringBuilder(); for (byte v : MessageDigest.getInstance("SHA-256").digest(bytes)) b.append(String.format(java.util.Locale.ROOT, "%02x", v & 255)); return b.toString(); }
        catch (NoSuchAlgorithmException e) { throw new AssertionError(e); }
    }
}
