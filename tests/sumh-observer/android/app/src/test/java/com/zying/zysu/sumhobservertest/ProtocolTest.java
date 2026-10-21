package com.zying.zysu.sumhobservertest;

import static org.junit.Assert.*;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import org.junit.Test;

public class ProtocolTest {
    static final String RUN = "0123456789abcdef0123456789abcdef";
    static final String ROOT = "/data/user/0/com.zying.zysu.sumhobservertest/files/sumh-observer/" + RUN;
    static String request() { return "{\"schema_version\":1,\"resource\":\"sumh_observer_request\",\"run_id\":\"" + RUN + "\",\"fixture_root\":\"" + ROOT + "\",\"context\":{\"boot_id\":null,\"config_revision\":null}}"; }
    static byte[] utf8(String s) { return s.getBytes(StandardCharsets.UTF_8); }
    private static void rejects(String text) { assertThrows(IllegalArgumentException.class, () -> RequestData.parse(utf8(text))); }
    @Test public void exactBytesArePreservedIncludingWhitespace() {
        byte[] bytes = utf8(" \n" + request() + "\t"); RequestData parsed = RequestData.parse(bytes);
        assertArrayEquals(bytes, parsed.bytes); assertNotEquals(RequestData.parse(utf8(request())).digest, parsed.digest);
    }
    @Test public void requestBoundariesAreByteLimits() {
        String exact = request() + " ".repeat(RequestData.MAX_REQUEST - request().length());
        assertEquals(RequestData.MAX_REQUEST, RequestData.parse(utf8(exact)).bytes.length); rejects(exact + " ");
    }
    @Test public void rejectsDuplicateKeysAtBothLevels() {
        rejects(request().replace("\"schema_version\":1", "\"schema_version\":1,\"schema_version\":1"));
        rejects(request().replace("\"boot_id\":null", "\"boot_id\":null,\"boot_id\":null"));
        rejects(request().replace("\"run_id\":", "\"r\\u0075n_id\":\"" + RUN + "\",\"run_id\":"));
    }
    @Test public void rejectsUnknownAndMissingFields() {
        rejects(request().replace("\"schema_version\":1,", ""));
        rejects(request().replace("\"schema_version\":1", "\"schema_version\":1,\"future\":0"));
    }
    @Test public void rejectsUnsafePathsAndMismatchedRun() {
        for (String path : new String[]{ROOT + "/", ROOT.replace("/files/", "/files/../files/"), ROOT.replace("/user/0/", "/user//"), "/sdcard/" + RUN, ROOT.substring(0, ROOT.length() - 1) + "0"}) rejects(request().replace(ROOT, path));
    }
    @Test public void acceptsBothExplicitSupportedRootFamilies() {
        assertEquals(RUN, RequestData.parse(utf8(request().replace("/data/user/0/", "/data/user_de/10/"))).runId);
        assertEquals(RUN, RequestData.parse(utf8(request().replace(ROOT, "/data/local/tmp/sumh-observer/" + RUN))).runId);
    }
    @Test public void userIdIsCanonicalAndBoundedToTenDigits() {
        rejects(request().replace("/user/0/", "/user/00/")); rejects(request().replace("/user/0/", "/user/12345678901/"));
        assertEquals(RUN, RequestData.parse(utf8(request().replace("/user/0/", "/user/1234567890/"))).runId);
    }
    @Test public void rejectsInvalidUtf8AndLoneSurrogates() {
        assertThrows(IllegalArgumentException.class, () -> RequestData.parse(new byte[]{(byte)0xc0, (byte)0xaf}));
        assertThrows(IllegalArgumentException.class, () -> StrictJson.parse(utf8("\"\\ud800\""), 100));
        assertThrows(IllegalArgumentException.class, () -> StrictJson.parse(utf8("\"\\udc00\""), 100));
        assertThrows(IllegalArgumentException.class, () -> StrictJson.parse(utf8("\"\\u００４１\""), 100));
        assertEquals("😀", StrictJson.parse(utf8("\"\\ud83d\\ude00\""), 100));
    }
    @Test public void rejectsMalformedNumbersAndTrailingData() {
        for (String number : new String[]{"NaN", "Infinity", "1e999", "01", "+1", "1.", "1e", "true", "\"1\"", "1.0"}) rejects(request().replace("\"schema_version\":1", "\"schema_version\":" + number));
        rejects(request() + "{}");
    }
    @Test public void contextRequiresBindingSyntax() {
        rejects(request().replace("\"boot_id\":null", "\"boot_id\":\"unmeasured\""));
        rejects(request().replace("\"config_revision\":null", "\"config_revision\":\"123\""));
        String valid = request().replace("\"boot_id\":null", "\"boot_id\":\"01234567-89ab-cdef-0123-456789abcdef\"").replace("\"config_revision\":null", "\"config_revision\":\"sha256-v1:" + "a".repeat(64) + "\"");
        assertEquals(RUN, RequestData.parse(utf8(valid)).runId);
    }
    @Test public void boundedStreamPreservesExactLimitRejectsOverflow() throws Exception {
        assertArrayEquals(new byte[32768], RequestData.readBounded(new ByteArrayInputStream(new byte[32768]), 32768));
        assertThrows(IOException.class, () -> RequestData.readBounded(new ByteArrayInputStream(new byte[32769]), 32768));
    }
    @Test public void nestedJsonAndNumberStringsRetainPrecision() {
        Map<String,Object> map = StrictJson.object(StrictJson.parse(utf8("{\"ino\":\"18446744073709551615\",\"n\":18446744073709551615}"), 200));
        assertEquals("18446744073709551615", map.get("ino"));
        assertEquals("18446744073709551615", ((StrictJson.NumberToken)map.get("n")).text);
    }
    static String sample(RequestData request, String role, int uid, int pid) {
        return "{\"schema_version\":1,\"resource\":\"sumh_observer_sample\",\"run_id\":\"" + request.runId + "\",\"request_sha256\":\"" + request.digest + "\",\"requested_role\":\"" + role + "\",\"identity\":{\"uid\":\"" + uid + "\",\"euid\":\"" + uid + "\",\"pid\":\"" + pid + "\"}}";
    }
    @Test public void rejectsRoleAndDigestMismatch() {
        RequestData r = RequestData.parse(utf8(request()));
        assertThrows(IllegalArgumentException.class, () -> ResultGuard.verify(utf8(sample(r, "isolated", 10001, 2)), r, "ordinary", 10001, 2));
        assertThrows(IllegalArgumentException.class, () -> ResultGuard.verify(utf8(sample(r, "ordinary", 10001, 2).replace(r.digest, "a".repeat(64))), r, "ordinary", 10001, 2));
    }
    @Test public void isolatedResponseMustHaveDistinctMeasuredIdentity() {
        RequestData r = RequestData.parse(utf8(request()));
        for (int[] ids : new int[][]{{10001,3},{99001,2},{0,3}}) assertThrows(IllegalArgumentException.class, () -> ResultGuard.verify(utf8(sample(r,"isolated",ids[0],ids[1])),r,"isolated",10001,2));
        ResultGuard.verify(utf8(sample(r,"isolated",99001,3)),r,"isolated",10001,2);
    }
    @Test public void ordinaryResponseMustComeFromCurrentProcess() {
        RequestData r = RequestData.parse(utf8(request()));
        ResultGuard.verify(utf8(sample(r,"ordinary",10001,2)),r,"ordinary",10001,2);
        assertThrows(IllegalArgumentException.class, () -> ResultGuard.verify(utf8(sample(r,"ordinary",10001,3)),r,"ordinary",10001,2));
    }
    @Test public void staleTerminalTokensCannotFinishNewOperation() {
        OperationGate gate = new OperationGate("session"); String first = gate.begin("digest");
        assertThrows(IllegalStateException.class, () -> gate.begin("digest")); assertTrue(gate.finish(first,"digest"));
        String second = gate.begin("digest"); assertFalse(gate.finish(first,"digest")); assertTrue(gate.busy());
        assertFalse(gate.finish(second,"different")); assertTrue(gate.finish(second,"digest")); assertFalse(gate.finish(second,"digest"));
    }
    @Test public void processSessionPreventsSerialReuse() {
        String old = new OperationGate("old-process").begin("same"); OperationGate restored = new OperationGate("new-process"); restored.begin("same"); assertFalse(restored.accept(old,"same"));
    }
}
