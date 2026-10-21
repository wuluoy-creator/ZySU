package com.zying.zysu.sumhobservertest;

final class NativeBridge {
    static { System.loadLibrary("sumh_observer_jni"); }
    private NativeBridge() {}
    static native String nCollect(String request, String role, int delegatedFd);
    static native String nCreateFixture(String filesDir, String runId);
    static native void nValidateRequest(String request);
}
