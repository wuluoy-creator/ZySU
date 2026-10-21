package com.zying.zysu.sumhobservertest;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.view.View;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;

public final class MainActivity extends Activity implements ObserverCoordinator.Listener {
    private static final int IMPORT_REQUEST = 10, EXPORT_DOCUMENT = 11;
    private ObserverCoordinator coordinator;
    private TextView status, request, ordinary, isolated;
    private final List<Button> controls = new ArrayList<>();
    private Button exportRequest, collectOrdinary, collectIsolated, exportOrdinary, exportIsolated;
    @Override public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        coordinator = ((ObserverApplication) getApplication()).coordinator();
        ScrollView scroll = new ScrollView(this); scroll.setFillViewport(true);
        LinearLayout body = new LinearLayout(this); body.setOrientation(LinearLayout.VERTICAL); body.setPadding(dp(20), dp(20), dp(20), dp(24));
        scroll.addView(body); setContentView(scroll);
        scroll.setOnApplyWindowInsetsListener((view, insets) -> {
            var safe = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            view.setPadding(safe.left, safe.top, safe.right, safe.bottom); return insets;
        });
        text(body, R.string.app_name, 26); text(body, R.string.introduction, 16);
        status = text(body, R.string.status_ready, 16); status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        text(body, R.string.request_heading, 20); request = text(body, R.string.request_none, 15); request.setTextIsSelectable(true);
        button(body, R.string.create, () -> coordinator.createFixture());
        button(body, R.string.import_request, () -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("application/json").addCategory(Intent.CATEGORY_OPENABLE);
            startActivityForResult(intent, IMPORT_REQUEST);
        });
        exportRequest = button(body, R.string.export_request, () -> chooseExport("request"));
        text(body, R.string.ordinary_heading, 20); ordinary = text(body, R.string.sample_none, 15);
        collectOrdinary = button(body, R.string.collect_ordinary, () -> coordinator.collectOrdinary());
        exportOrdinary = button(body, R.string.export_ordinary, () -> chooseExport("ordinary"));
        text(body, R.string.isolated_heading, 20); isolated = text(body, R.string.sample_none, 15);
        collectIsolated = button(body, R.string.collect_isolated, () -> coordinator.collectIsolated());
        exportIsolated = button(body, R.string.export_isolated, () -> chooseExport("isolated"));
        text(body, R.string.scope_note, 15); text(body, R.string.cleanup_note, 15);
    }
    @Override protected void onStart() { super.onStart(); coordinator.attach(this); }
    @Override protected void onStop() { coordinator.detach(this); super.onStop(); }
    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private TextView text(LinearLayout body, int resource, int sp) {
        TextView view = new TextView(this); view.setText(resource); view.setTextSize(sp); view.setPadding(0, dp(10), 0, dp(8));
        body.addView(view, new LinearLayout.LayoutParams(-1, -2)); return view;
    }
    private Button button(LinearLayout body, int label, Runnable action) {
        Button button = new Button(this); button.setText(label); button.setAllCaps(false); button.setMinHeight(dp(48));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2); params.bottomMargin = dp(8); body.addView(button, params); controls.add(button);
        button.setOnClickListener(view -> { try { action.run(); } catch (Exception | LinkageError e) { coordinator.report(R.string.status_error, e.getClass().getSimpleName() + ": " + String.valueOf(e.getMessage())); } });
        return button;
    }
    private void chooseExport(String role) {
        ObserverCoordinator.ExportSnapshot snapshot = coordinator.prepareExport(role);
        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("application/json").addCategory(Intent.CATEGORY_OPENABLE).putExtra(Intent.EXTRA_TITLE, snapshot.filename);
        startActivityForResult(intent, EXPORT_DOCUMENT);
    }
    @Override public void changed() {
        boolean available = !coordinator.busy(); for (Button button : controls) button.setEnabled(available);
        boolean hasRequest = coordinator.request != null;
        exportRequest.setEnabled(available && hasRequest); collectOrdinary.setEnabled(available && hasRequest); collectIsolated.setEnabled(available && hasRequest);
        exportOrdinary.setEnabled(available && coordinator.ordinary != null); exportIsolated.setEnabled(available && coordinator.isolated != null);
        String message = getString(coordinator.status);
        if (!coordinator.detail.isEmpty()) message += "\n" + coordinator.detail;
        if (coordinator.busy() && coordinator.status == R.string.status_unknown) message += "\n" + getString(R.string.native_still_running);
        status.setText(message);
        request.setText(hasRequest ? getString(R.string.request_details, coordinator.request.runId, coordinator.request.fixtureRoot, coordinator.request.digest) : getString(R.string.request_none));
        ordinary.setText(sampleText(coordinator.ordinary, coordinator.ordinaryError)); isolated.setText(sampleText(coordinator.isolated, coordinator.isolatedError));
    }
    private String sampleText(byte[] bytes, String error) {
        String message = bytes == null ? getString(R.string.sample_none) : getString(R.string.sample_saved, bytes.length);
        return error.isEmpty() ? message : message + "\n" + getString(R.string.latest_attempt, error);
    }
    @Override protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) { if (code == EXPORT_DOCUMENT) coordinator.pendingExport = null; return; }
        try {
            if (code == IMPORT_REQUEST) coordinator.importRequest(data.getData());
            else if (code == EXPORT_DOCUMENT) coordinator.export(data.getData());
        } catch (Exception e) { coordinator.report(R.string.status_error, e.getClass().getSimpleName() + ": " + String.valueOf(e.getMessage())); }
    }
}
