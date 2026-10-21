package com.zying.zysu.sumhobservertest;

import android.app.Application;

public final class ObserverApplication extends Application {
    private ObserverCoordinator coordinator;
    synchronized ObserverCoordinator coordinator() {
        if (coordinator == null) coordinator = new ObserverCoordinator(this);
        return coordinator;
    }
    @Override public void onTerminate() { if (coordinator != null) coordinator.close(); super.onTerminate(); }
}
