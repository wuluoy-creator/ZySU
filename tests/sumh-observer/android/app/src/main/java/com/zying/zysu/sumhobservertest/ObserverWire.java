package com.zying.zysu.sumhobservertest;

import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;

final class ObserverWire {
    static final String SERVICE = "com.zying.zysu.sumhobservertest.collect.v1";
    static final String CALLBACK = "com.zying.zysu.sumhobservertest.result.v1";
    static final int COLLECT = IBinder.FIRST_CALL_TRANSACTION;
    private ObserverWire() {}
    static byte[] readBytes(Parcel data, int max) {
        int count = data.readInt();
        if (count < 0 || count > max) throw new IllegalArgumentException("Binder payload exceeds limit");
        byte[] bytes = new byte[count]; data.readByteArray(bytes); return bytes;
    }
    static void writeBytes(Parcel data, byte[] bytes) { data.writeInt(bytes.length); data.writeByteArray(bytes); }
    static void end(Parcel data) { if (data.dataAvail() != 0) throw new IllegalArgumentException("Unexpected Binder fields"); }
    static void collect(IBinder service, String token, byte[] request, ParcelFileDescriptor fd, IBinder callback) throws RemoteException {
        Parcel data = Parcel.obtain();
        try {
            data.writeInterfaceToken(SERVICE); data.writeString(token); writeBytes(data, request);
            data.writeInt(fd == null ? 0 : 1); if (fd != null) fd.writeToParcel(data, 0); data.writeStrongBinder(callback);
            if (!service.transact(COLLECT, data, null, IBinder.FLAG_ONEWAY)) throw new RemoteException("Collector transaction rejected");
        } finally { data.recycle(); }
    }
    static void respond(IBinder callback, String token, byte[] result, String error) throws RemoteException {
        Parcel data = Parcel.obtain();
        try {
            data.writeInterfaceToken(CALLBACK); data.writeString(token); data.writeInt(error == null ? 0 : 1);
            if (error == null) writeBytes(data, result); else data.writeString(error.length() > 512 ? error.substring(0, 512) : error);
            if (!callback.transact(COLLECT, data, null, IBinder.FLAG_ONEWAY)) throw new RemoteException("Result transaction rejected");
        } finally { data.recycle(); }
    }
    abstract static class Callback extends Binder {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
            if (code != COLLECT) return false;
            try {
                data.enforceInterface(CALLBACK); String token = data.readString(); int kind = data.readInt();
                byte[] result = null; String error = null;
                if (kind == 0) result = readBytes(data, RequestData.MAX_RESULT);
                else if (kind == 1) { error = data.readString(); if (error == null || error.length() > 512) throw new IllegalArgumentException("Invalid transport error"); }
                else throw new IllegalArgumentException("Invalid response kind");
                end(data); delivered(token, result, error);
            } catch (RuntimeException e) { malformed(e); }
            return true;
        }
        abstract void delivered(String token, byte[] result, String error);
        abstract void malformed(RuntimeException error);
    }
}
