package com.stt.mobile;

import android.annotation.SuppressLint;
import android.Manifest;
import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Build;
import android.os.Handler;
import android.graphics.Color;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.view.ViewGroup;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

public class MainActivity extends Activity {
    private static final String TAG = "PocketVoice";
    private static final int PORT = 27000;
    private static final int MAX_LOG_ROWS = 200;
    private static final int APP_BACKGROUND = Color.rgb(18, 19, 19);
    private static final int REQUEST_POST_NOTIFICATIONS = 1001;

    private final Handler handler = new Handler();
    private final Object logLock = new Object();
    private final List<String> logs = new ArrayList<>();
    private boolean started = false;
    private WebView webView;
    private String modelDir = "";
    private String qnnRuntimeDir = "";
    private String lastError = "";
    private String lastLoggedStatus = "";
    private boolean recognitionCacheEnabled = false;
    private volatile boolean stopping = false;
    private static MainActivity activeInstance = null;

    static {
        try {
            System.loadLibrary("stt_native");
            Log.i(TAG, "Native lib loaded");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Native lib not found: " + e.getMessage());
        }
    }

    @Override
    @SuppressLint({"SetJavaScriptEnabled", "AddJavascriptInterface"})
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        activeInstance = this;

        configureSystemBars();
        modelDir = resolveModelDir();
        webView = new WebView(this);
        webView.setLayoutParams(new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        webView.setBackgroundColor(APP_BACKGROUND);
        webView.setOverScrollMode(View.OVER_SCROLL_NEVER);
        webView.setVerticalScrollBarEnabled(false);
        webView.setHorizontalScrollBarEnabled(false);
        WebSettings settings = webView.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(false);
        settings.setAllowFileAccess(true);
        settings.setAllowContentAccess(false);
        settings.setUseWideViewPort(false);
        settings.setLoadWithOverviewMode(false);
        settings.setSupportZoom(false);
        settings.setBuiltInZoomControls(false);
        settings.setDisplayZoomControls(false);
        webView.addJavascriptInterface(new SttBridge(), "STT");
        setContentView(webView);

        addLog("Model path: " + modelDir);
        addLog("Ready");

        webView.loadUrl("file:///android_asset/ui/index.html");
        requestNotificationPermissionIfNeeded();
        handler.postDelayed(statusChecker, 1000);
        maybeAutoStart(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        maybeAutoStart(intent);
    }

    private void maybeAutoStart(Intent intent) {
        if (intent != null && intent.getBooleanExtra("autoStart", false)) {
            handler.postDelayed(() -> startServer(), 500);
        }
    }

    private void requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return;
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) return;
        requestPermissions(new String[] { Manifest.permission.POST_NOTIFICATIONS }, REQUEST_POST_NOTIFICATIONS);
    }

    private void configureSystemBars() {
        Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        window.setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(APP_BACKGROUND));
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            window.setStatusBarContrastEnforced(false);
            window.setNavigationBarContrastEnforced(false);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            WindowManager.LayoutParams attrs = window.getAttributes();
            attrs.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            window.setAttributes(attrs);
        }
        window.setStatusBarColor(APP_BACKGROUND);
        window.setNavigationBarColor(APP_BACKGROUND);
        window.getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            configureSystemBars();
        }
    }

    private void addLog(String msg) {
        synchronized (logLock) {
            logs.add(msg);
            if (logs.size() > MAX_LOG_ROWS) {
                logs.remove(0);
            }
        }
        Log.i(TAG, msg);
    }

    private String resolveModelDir() {
        File root = new File(getApplicationContext().getExternalFilesDir(null), "models");
        File sensevoice = new File(root, "sensevoice");
        File zipformer = new File(root, "zipformer-ctc");
        File paraformer = new File(root, "paraformer");
        if ((new File(sensevoice, "model.bin").exists()
                || new File(sensevoice, "libmodel.so").exists())
                && new File(sensevoice, "tokens.txt").exists()) {
            return sensevoice.getAbsolutePath();
        }
        if (new File(zipformer, "model.int8.onnx").exists()
                && new File(zipformer, "bbpe.model").exists()
                && new File(zipformer, "tokens.txt").exists()) {
            return zipformer.getAbsolutePath();
        }
        return paraformer.getAbsolutePath();
    }

    private String prepareQnnRuntimeDir() {
        File runtimeDir = new File(getFilesDir(), "qnn-runtime");
        if (!runtimeDir.exists() && !runtimeDir.mkdirs()) {
            addLog("QNN runtime dir unavailable: " + runtimeDir.getAbsolutePath());
            return getApplicationInfo().nativeLibraryDir;
        }

        String[] libs = new String[] {
                "libQnnHtp.so",
                "libQnnHtpPrepare.so",
                "libQnnHtpNetRunExtensions.so",
                "libQnnHtpV68Stub.so",
                "libQnnHtpV68CalculatorStub.so",
                "libQnnHtpV68Skel.so",
                "libQnnHtpV69Stub.so",
                "libQnnHtpV69CalculatorStub.so",
                "libQnnHtpV69Skel.so",
                "libQnnHtpV73Stub.so",
                "libQnnHtpV73CalculatorStub.so",
                "libQnnHtpV73Skel.so",
                "libQnnHtpV75Stub.so",
                "libQnnHtpV75CalculatorStub.so",
                "libQnnHtpV75Skel.so",
                "libQnnHtpV79Stub.so",
                "libQnnHtpV79CalculatorStub.so",
                "libQnnHtpV79Skel.so",
                "libQnnHtpV81Stub.so",
                "libQnnHtpV81CalculatorStub.so",
                "libQnnHtpV81Skel.so",
                "libQnnSystem.so",
                "libmodel.so"
        };

        File nativeDir = new File(getApplicationInfo().nativeLibraryDir);
        for (String lib : libs) {
            File source = new File(nativeDir, lib);
            File target = new File(runtimeDir, lib);
            if (!source.exists()) continue;
            if (target.exists() && target.length() == source.length()) continue;
            try {
                copyFile(source, target);
            } catch (IOException e) {
                addLog("QNN runtime copy failed: " + lib);
                Log.e(TAG, "Failed to copy QNN runtime lib " + lib, e);
            }
        }
        return runtimeDir.getAbsolutePath();
    }

    private static void copyFile(File source, File target) throws IOException {
        byte[] buffer = new byte[1024 * 1024];
        try (FileInputStream in = new FileInputStream(source);
             FileOutputStream out = new FileOutputStream(target)) {
            int read;
            while ((read = in.read(buffer)) != -1) {
                out.write(buffer, 0, read);
            }
        }
        target.setReadable(true, true);
        target.setExecutable(true, true);
    }

    private String getStatusSafe() {
        if (stopping) {
            return "stopping";
        }
        if (started) {
            try {
                String status = nativeGetStatus();
                if (status.startsWith("error:")) {
                    lastError = status.substring("error:".length()).trim();
                }
                return status;
            } catch (RuntimeException e) {
                lastError = e.getMessage();
                return "error: " + e.getMessage();
            }
        }
        return "stopped";
    }

    private String snapshotJson() {
        String nativeStatus = getStatusSafe();
        String status = nativeStatus;
        if (nativeStatus.startsWith("error:")) {
            status = "error";
        }
        if (stopping) {
            status = "stopping";
        }

        JSONObject json = new JSONObject();
        try {
            JSONObject runtime = new JSONObject(nativeGetRuntimeSnapshot());
            json.put("status", status);
            json.put("backend", runtime.optString("backend",
                    modelDir.contains("sensevoice") ? "sensevoice_qnn" : "unknown"));
            json.put("cpuFallback", runtime.optBoolean("cpuFallback", false));
            json.put("modelDir", modelDir);
            json.put("port", PORT);
            json.put("lastText", runtime.optString("lastText", ""));
            json.put("lastAudioMs", runtime.optInt("lastAudioMs", 0));
            json.put("lastRecognizeMs", runtime.optInt("lastRecognizeMs", 0));
            json.put("lastUpdatedMs", runtime.optLong("lastUpdatedMs", 0));
            json.put("totalRequests", runtime.optInt("totalRequests", 0));
            json.put("cacheHits", runtime.optInt("cacheHits", 0));
            json.put("cacheEnabled", runtime.optBoolean("cacheEnabled", recognitionCacheEnabled));
            json.put("history", runtime.optJSONArray("history"));
            json.put("lastError", lastError);
            JSONArray logRows = new JSONArray();
            synchronized (logLock) {
                for (String row : logs) {
                    logRows.put(row);
                }
            }
            json.put("logs", logRows);
        } catch (JSONException e) {
            return "{\"status\":\"error\",\"lastError\":\"snapshot failed\"}";
        }
        return json.toString();
    }

    private final Runnable statusChecker = new Runnable() {
        @Override
        public void run() {
            if (started) {
                String status = nativeGetStatus();
                if (!status.equals(lastLoggedStatus)) {
                    addLog("Status: " + status);
                    lastLoggedStatus = status;
                }
                if (status.startsWith("error:") || status.equals("stopped")) {
                    started = SttForegroundService.isRunning();
                    if (status.startsWith("error:")) {
                        lastError = status.substring("error:".length()).trim();
                    }
                }
            }
            handler.postDelayed(this, 3000);
        }
    };

    @Override
    protected void onDestroy() {
        handler.removeCallbacks(statusChecker);
        if (webView != null) {
            webView.destroy();
        }
        if (activeInstance == this) activeInstance = null;
        super.onDestroy();
    }

    private void startServer() {
        modelDir = resolveModelDir();
        qnnRuntimeDir = prepareQnnRuntimeDir();
        addLog("Starting server on port 27000...");
        addLog("Model: " + modelDir);
        SttForegroundService.start(this, modelDir, qnnRuntimeDir);
        started = true;
        lastLoggedStatus = "";
        lastError = "";
    }

    private void stopServer() {
        if (stopping) return;
        stopping = true;
        addLog("Stopping server...");
        Thread stopThread = new Thread(() -> {
            SttForegroundService.stop(this);
            handler.post(() -> {
                started = false;
                stopping = false;
                addLog("Server stopped");
                addLog("Status: stopped");
                lastLoggedStatus = "stopped";
            });
        }, "stt-stop");
        stopThread.start();
    }

    public static class StartReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (activeInstance == null) {
                Log.w(TAG, "StartReceiver ignored: MainActivity is not active");
                return;
            }
            activeInstance.runOnUiThread(() -> activeInstance.startServer());
        }
    }

    public class SttBridge {
        @JavascriptInterface
        public void start() {
            runOnUiThread(() -> startServer());
        }

        @JavascriptInterface
        public void stop() {
            runOnUiThread(() -> stopServer());
        }

        @JavascriptInterface
        public String getSnapshot() {
            return snapshotJson();
        }

        @JavascriptInterface
        public void setCacheEnabled(boolean enabled) {
            recognitionCacheEnabled = enabled;
            nativeSetRecognitionCacheEnabled(enabled);
            addLog("Cache: " + (enabled ? "on" : "off"));
        }

        @JavascriptInterface
        public void clearLog() {
            synchronized (logLock) {
                logs.clear();
            }
            addLog("Log cleared");
        }
    }

    static native boolean nativeInit(String modelDir, String nativeLibraryDir);
    static native void nativeStart();
    static native void nativeStop();
    static native String nativeGetStatus();
    static native String nativeGetRuntimeSnapshot();
    static native void nativeSetRecognitionCacheEnabled(boolean enabled);
}
