package com.zying.zysu.sumhobservertest;

import java.util.Map;

final class ResultGuard {
    private ResultGuard() {}
    static void verify(byte[] bytes, RequestData request, String role, int ownUid, int ownPid) {
        Map<String, Object> result = StrictJson.object(StrictJson.parse(bytes, RequestData.MAX_RESULT));
        if (!(result.get("schema_version") instanceof StrictJson.NumberToken n) || !n.text.equals("1") ||
            !"sumh_observer_sample".equals(result.get("resource")) || !role.equals(result.get("requested_role")) ||
            !request.runId.equals(result.get("run_id")) || !request.digest.equals(result.get("request_sha256")))
            throw new IllegalArgumentException("Response does not match this request and actor");
        Map<String, Object> identity = StrictJson.object(result.get("identity"));
        long uid = decimal(identity.get("uid")), euid = decimal(identity.get("euid")), pid = decimal(identity.get("pid"));
        if (role.equals("ordinary") && (uid != ownUid || euid != ownUid || pid != ownPid)) throw new IllegalArgumentException("Ordinary process identity mismatch");
        if (role.equals("isolated") && (uid == ownUid || euid == ownUid || uid == 0 || euid == 0 || pid == ownPid || pid == 0)) throw new IllegalArgumentException("Service did not report a distinct isolated identity");
    }
    private static long decimal(Object value) {
        String s = StrictJson.string(value); if (!s.matches("0|[1-9][0-9]{0,18}")) throw new IllegalArgumentException("Invalid identity value");
        return Long.parseLong(s);
    }
}
