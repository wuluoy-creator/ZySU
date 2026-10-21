package com.zying.zysu.sumhobservertest;

/** Coordinator calls this on the main thread. A terminal token can never be accepted again. */
final class OperationGate {
    private long serial;
    private String active;
    private String requestDigest;
    private final String session;
    OperationGate(String session) { this.session = session; }
    String begin(String digest) {
        if (active != null) throw new IllegalStateException("An operation is already in progress");
        requestDigest = digest;
        return active = session + ":" + (++serial);
    }
    boolean accept(String token, String digest) { return active != null && active.equals(token) && requestDigest.equals(digest); }
    boolean finish(String token, String digest) { if (!accept(token, digest)) return false; active = null; requestDigest = null; return true; }
    boolean busy() { return active != null; }
}
