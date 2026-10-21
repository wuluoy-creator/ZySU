package com.zying.zysu.sumhobservertest;

import static org.junit.Assert.*;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 35, application = android.app.Application.class)
public class WireTest {
    @Test public void requestLengthRejectedBeforePayloadAllocation() {
        Parcel parcel=Parcel.obtain(); try { parcel.writeInt(32769); parcel.setDataPosition(0); assertThrows(IllegalArgumentException.class,()->ObserverWire.readBytes(parcel,32768)); } finally { parcel.recycle(); }
    }
    @Test public void exactResultBoundaryRoundTrips() {
        Parcel parcel=Parcel.obtain(); try { byte[] bytes=new byte[262144]; ObserverWire.writeBytes(parcel,bytes); parcel.setDataPosition(0); assertArrayEquals(bytes,ObserverWire.readBytes(parcel,262144)); ObserverWire.end(parcel); } finally { parcel.recycle(); }
    }
    @Test public void trailingBinderFieldsAreRejected() {
        Parcel parcel=Parcel.obtain(); try { ObserverWire.writeBytes(parcel,new byte[1]); parcel.writeInt(9); parcel.setDataPosition(0); ObserverWire.readBytes(parcel,32768); assertThrows(IllegalArgumentException.class,()->ObserverWire.end(parcel)); } finally { parcel.recycle(); }
    }
    @Test public void malformedResponseProducesOnlyTransportFailure() {
        int[] received={0},errors={0};
        ObserverWire.Callback callback=new ObserverWire.Callback() {
            @Override void delivered(String token,byte[] result,String error) { received[0]++; }
            @Override void malformed(RuntimeException error) { errors[0]++; }
        };
        Parcel parcel=Parcel.obtain(); try {
            parcel.writeInterfaceToken(ObserverWire.CALLBACK); parcel.writeString("token"); parcel.writeInt(0); parcel.writeInt(262145); parcel.setDataPosition(0);
            assertTrue(callback.onTransact(ObserverWire.COLLECT,parcel,null,0)); assertEquals(0,received[0]); assertEquals(1,errors[0]);
        } finally { parcel.recycle(); }
    }
    @Test public void canceledQueuedTaskClosesDescriptorAndNeverRuns() throws Exception {
        ParcelFileDescriptor[] pipe=ParcelFileDescriptor.createPipe(); int[] runs={0},finished={0};
        try {
            IsolatedObserverService.OwnedCollection task=new IsolatedObserverService.OwnedCollection(pipe[0],()->runs[0]++,()->finished[0]++);
            task.cancel(); task.cancel(); task.run(); assertEquals(0,runs[0]); assertEquals(1,finished[0]);
            assertThrows(IllegalStateException.class,pipe[0]::getFd);
        } finally { pipe[0].close(); pipe[1].close(); }
    }
    @Test public void cancellationDoesNotCloseDescriptorBorrowedByRunningCollector() throws Exception {
        ParcelFileDescriptor[] pipe=ParcelFileDescriptor.createPipe(); int[] finished={0};
        IsolatedObserverService.OwnedCollection[] task=new IsolatedObserverService.OwnedCollection[1];
        try {
            int originalFd=pipe[0].getFd(); // Robolectric on Windows may use a negative synthetic handle.
            task[0]=new IsolatedObserverService.OwnedCollection(pipe[0],()->{ task[0].cancel(); assertEquals(originalFd,pipe[0].getFd()); },()->finished[0]++);
            task[0].run(); task[0].cancel(); assertEquals(1,finished[0]); assertThrows(IllegalStateException.class,pipe[0]::getFd);
        } finally { pipe[0].close(); pipe[1].close(); }
    }
}
