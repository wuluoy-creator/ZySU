package com.zying.zysu.sumhobservertest;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.ServiceConnection;
import android.net.Uri;
import android.os.Looper;
import android.os.Process;
import java.io.File;
import java.nio.file.Files;
import java.time.Duration;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Robolectric;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.LooperMode;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 35, application = ObserverApplication.class)
@LooperMode(LooperMode.Mode.PAUSED)
public class CoordinatorTest {
    private TestContext context;
    private ObserverCoordinator coordinator;
    private final AtomicInteger calls = new AtomicInteger();
    private CountDownLatch entered, release;
    private boolean block;
    private String fixtureRequest=ProtocolTest.request();
    private final ObserverCoordinator.NativeCalls nativeCalls = new ObserverCoordinator.NativeCalls() {
        @Override public String create(String filesDir, String runId) { return fixtureRequest; }
        @Override public void validate(String request) { RequestData.parse(ProtocolTest.utf8(request)); }
        @Override public String collect(String request, String role, int fd) {
            calls.incrementAndGet();
            if (block) { entered.countDown(); try { release.await(3, TimeUnit.SECONDS); } catch (InterruptedException e) { Thread.currentThread().interrupt(); } }
            return ProtocolTest.sample(RequestData.parse(ProtocolTest.utf8(request)), role, Process.myUid(), Process.myPid());
        }
    };
    @Before public void setup() throws Exception {
        context = new TestContext(Files.createTempDirectory("sumh-observer-coordinator").toFile());
        coordinator = new ObserverCoordinator(context, nativeCalls);
        coordinator.createFixture(); drain(); assertNotNull(coordinator.request);
    }
    @After public void finish() { if (release != null) release.countDown(); coordinator.close(); }
    private void drain() throws Exception {
        for (int i = 0; i < 100 && coordinator.busy(); i++) { Thread.sleep(10); shadowOf(Looper.getMainLooper()).idle(); }
        shadowOf(Looper.getMainLooper()).idle(); assertFalse("Operation did not finish",coordinator.busy());
    }
    @Test public void detachReattachDoesNotResubmitAndResultSurvives() throws Exception {
        AtomicInteger first = new AtomicInteger(), second = new AtomicInteger();
        ObserverCoordinator.Listener old = first::incrementAndGet; coordinator.attach(old);
        coordinator.collectOrdinary(); coordinator.detach(old); int detachedCount = first.get();
        coordinator.attach(second::incrementAndGet); drain();
        assertEquals(detachedCount, first.get()); assertTrue(second.get() > 0); assertEquals(1, calls.get()); assertNotNull(coordinator.ordinary);
    }
    @Test public void actualActivityRecreationKeepsApplicationOperationAndDraft() throws Exception {
        org.robolectric.util.ReflectionHelpers.setField(RuntimeEnvironment.getApplication(), "coordinator", coordinator);
        var activity = Robolectric.buildActivity(MainActivity.class).setup();
        block = true; entered = new CountDownLatch(1); release = new CountDownLatch(1);
        coordinator.collectOrdinary(); assertTrue(entered.await(1,TimeUnit.SECONDS));
        activity.recreate(); assertEquals(1,calls.get()); assertTrue(coordinator.busy());
        release.countDown(); drain(); assertNotNull(coordinator.ordinary); assertEquals(ProtocolTest.RUN,coordinator.request.runId);
        activity.pause().stop().destroy();
    }
    @Test public void timeoutKeepsUnknownAndDiscardsLateCompletionWithoutRetry() throws Exception {
        block = true; entered = new CountDownLatch(1); release = new CountDownLatch(1);
        coordinator.collectOrdinary(); assertTrue(entered.await(1,TimeUnit.SECONDS));
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(11));
        assertEquals(R.string.status_unknown, coordinator.status); assertTrue(coordinator.busy()); assertNull(coordinator.ordinary);
        assertThrows(IllegalStateException.class, () -> coordinator.collectOrdinary());
        release.countDown(); drain(); assertEquals(R.string.status_unknown,coordinator.status); assertNull(coordinator.ordinary); assertEquals(1,calls.get());
    }
    @Test public void timeoutAnnotationSurvivesAnotherActionAndProcessRecreation() throws Exception {
        coordinator.collectOrdinary(); drain(); byte[] previous=coordinator.ordinary.clone();
        block=true; entered=new CountDownLatch(1); release=new CountDownLatch(1);
        coordinator.collectOrdinary(); assertTrue(entered.await(1,TimeUnit.SECONDS)); shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(11));
        assertFalse(coordinator.ordinaryError.isEmpty()); release.countDown(); drain(); assertArrayEquals(previous,coordinator.ordinary);
        coordinator.createFixture(); drain(); assertFalse(coordinator.ordinaryError.isEmpty());
        coordinator.close(); coordinator=new ObserverCoordinator(context,nativeCalls);
        assertFalse(coordinator.ordinaryError.isEmpty()); assertArrayEquals(previous,coordinator.ordinary); assertEquals(2,calls.get());
    }
    @Test public void processInterruptedActorIsAssociatedWithRetainedSample() throws Exception {
        coordinator.collectOrdinary(); drain();
        File journal=new File(context.getFilesDir(),"observer-state/pending");
        Files.writeString(journal.toPath(),"previous:1\n"+coordinator.request.digest+"\nordinary");
        coordinator.close(); coordinator=new ObserverCoordinator(context,nativeCalls);
        assertEquals(R.string.status_interrupted,coordinator.status); assertNotNull(coordinator.ordinary); assertFalse(coordinator.ordinaryError.isEmpty());
    }
    @Test public void corruptNewRequestEvidenceCannotExportOldRequestSample() throws Exception {
        coordinator.collectOrdinary(); drain(); assertNotNull(coordinator.ordinary);
        fixtureRequest=" "+ProtocolTest.request(); RequestData next=RequestData.parse(ProtocolTest.utf8(fixtureRequest));
        File directory=new File(context.getFilesDir(),"observer-state/"+next.digest); assertTrue(directory.mkdirs());
        Files.writeString(new File(directory,"ordinary.json").toPath(),"corrupt"); coordinator.createFixture(); drain();
        assertEquals(next.digest,coordinator.request.digest); assertNull(coordinator.ordinary);
        assertThrows(IllegalStateException.class,()->coordinator.prepareExport("ordinary"));
    }
    @Test public void serviceDeathIsTerminalAndDoesNotStartNativeOrdinaryCollector() {
        coordinator.collectIsolated(); assertNotNull(context.connection);
        context.connection.onServiceDisconnected(new android.content.ComponentName(context, IsolatedObserverService.class));
        assertFalse(coordinator.busy()); assertNull(coordinator.isolated); assertEquals(0,calls.get());
        assertEquals(R.string.status_error,coordinator.status); assertEquals(1,context.unbinds);
    }
    @Test public void restoredEvidenceKeepsBytesAndDoesNotCollect() throws Exception {
        coordinator.collectOrdinary(); drain(); byte[] saved = coordinator.ordinary;
        coordinator.close(); coordinator = new ObserverCoordinator(context,nativeCalls);
        assertArrayEquals(saved,coordinator.ordinary); assertEquals(1,calls.get()); assertEquals(R.string.status_restored,coordinator.status);
    }
    @Test public void interruptedJournalDoesNotReplayAfterProcessRecreation() throws Exception {
        File journal = new File(context.getFilesDir(),"observer-state/pending"); Files.writeString(journal.toPath(),"previous-process:1");
        coordinator.close(); coordinator = new ObserverCoordinator(context,nativeCalls);
        assertEquals(R.string.status_interrupted,coordinator.status); assertEquals(0,calls.get()); assertFalse(coordinator.busy());
    }
    @Test public void expiredExportDoesNotWriteAndPreservesCurrentRequest() {
        coordinator.prepareExport("request");
        coordinator.request = RequestData.parse(ProtocolTest.utf8(" " + ProtocolTest.request()));
        coordinator.export(Uri.parse("content://invalid-provider/should-not-open"));
        assertEquals(R.string.export_expired,coordinator.status); assertFalse(coordinator.busy());
    }
    @Test public void exportAfterProcessDeathRequiresNewSelection() {
        coordinator.prepareExport("request"); coordinator.close(); coordinator = new ObserverCoordinator(context,nativeCalls);
        coordinator.export(Uri.parse("content://invalid-provider/should-not-open")); assertEquals(R.string.export_expired,coordinator.status);
    }
    private static final class TestContext extends ContextWrapper {
        final File files; ServiceConnection connection; int unbinds;
        TestContext(File files) { super(RuntimeEnvironment.getApplication()); this.files = files; }
        @Override public File getFilesDir() { return files; }
        @Override public boolean bindService(Intent intent,ServiceConnection connection,int flags) { this.connection = connection; return true; }
        @Override public void unbindService(ServiceConnection connection) { assertSame(this.connection,connection); unbinds++; }
    }
}
