package com.stt.mobile;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;

public class SttForegroundService extends Service {
    private static final String TAG = "STTService";
    private static final String ACTION_START = "com.stt.mobile.START";
    private static final String ACTION_STOP = "com.stt.mobile.STOP";
    private static final String EXTRA_MODEL_DIR = "modelDir";
    private static final String EXTRA_RUNTIME_DIR = "runtimeDir";
    private static final String CHANNEL_ID = "stt_foreground";
    private static final int NOTIFICATION_ID = 27000;

    private static volatile boolean running = false;
    private PowerManager.WakeLock wakeLock;

    public static void start(Context context, String modelDir, String runtimeDir) {
        Intent intent = new Intent(context, SttForegroundService.class);
        intent.setAction(ACTION_START);
        intent.putExtra(EXTRA_MODEL_DIR, modelDir);
        intent.putExtra(EXTRA_RUNTIME_DIR, runtimeDir);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(intent);
        } else {
            context.startService(intent);
        }
    }

    public static void stop(Context context) {
        Intent intent = new Intent(context, SttForegroundService.class);
        intent.setAction(ACTION_STOP);
        context.startService(intent);
    }

    public static boolean isRunning() {
        return running;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null ? intent.getAction() : ACTION_START;
        if (ACTION_STOP.equals(action)) {
            stopServer();
            stopSelf();
            return START_NOT_STICKY;
        }

        String modelDir = intent != null ? intent.getStringExtra(EXTRA_MODEL_DIR) : null;
        String runtimeDir = intent != null ? intent.getStringExtra(EXTRA_RUNTIME_DIR) : null;
        if (modelDir == null || modelDir.isEmpty()) {
            modelDir = resolveModelDir();
        }
        if (runtimeDir == null || runtimeDir.isEmpty()) {
            runtimeDir = prepareQnnRuntimeDir();
        }

        startInForeground();
        acquireWakeLock();
        if (!running) {
            MainActivity.nativeInit(modelDir, runtimeDir);
            MainActivity.nativeStart();
            running = true;
            Log.i(TAG, "Foreground STT server started");
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        stopServer();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void startInForeground() {
        Notification notification = buildNotification();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
    }

    private Notification buildNotification() {
        Intent launch = new Intent(this, MainActivity.class);
        launch.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        PendingIntent pendingLaunch = PendingIntent.getActivity(
                this,
                0,
                launch,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);
        return builder
                .setSmallIcon(android.R.drawable.ic_btn_speak_now)
                .setContentTitle("PocketVoice")
                .setContentText("语音服务正在运行")
                .setContentIntent(pendingLaunch)
                .setOngoing(true)
                .setShowWhen(false)
                .build();
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID,
                "PocketVoice",
                NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("PocketVoice 语音服务");
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) {
            manager.createNotificationChannel(channel);
        }
    }

    private void acquireWakeLock() {
        if (wakeLock != null && wakeLock.isHeld()) return;
        PowerManager powerManager = (PowerManager) getSystemService(Context.POWER_SERVICE);
        if (powerManager == null) return;
        wakeLock = powerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "PocketVoice:SttService");
        wakeLock.setReferenceCounted(false);
        wakeLock.acquire();
    }

    private void stopServer() {
        if (running) {
            MainActivity.nativeStop();
            running = false;
            Log.i(TAG, "Foreground STT server stopped");
        }
        if (wakeLock != null && wakeLock.isHeld()) {
            wakeLock.release();
        }
        wakeLock = null;
        stopForeground(true);
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
}
