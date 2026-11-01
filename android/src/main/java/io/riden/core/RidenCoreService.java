package io.riden.core;
import android.app.*;
import android.content.*;
import android.hardware.usb.*;
import android.os.*;
import java.io.IOException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Foreground service is Android's resident-device host; it is not a Unix daemon.
 * Start after USB permission, while the host app can legally start a foreground service. */
public final class RidenCoreService extends Service {
    public static final String EXTRA_DEVICE = "device";
    private final ExecutorService lifecycle = Executors.newSingleThreadExecutor();
    private volatile RidenCore core;
    private volatile boolean destroyed;
    private final IRidenCore.Stub binder = new IRidenCore.Stub() {
        private RidenCore require() { RidenCore c = core; if (c == null) throw new IllegalStateException("Core not ready"); return c; }
        public long[] snapshot() { return require().snapshot(); }
        public int[] read(int start, int count, int age) { return require().read(start, count, age); }
        public int write(int start, int[] values) { return require().write(start, values); }
    };
    @Override public void onCreate() {
        super.onCreate();
        NotificationManager manager = getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel("riden_core", "RIDEN device connection", NotificationManager.IMPORTANCE_LOW));
        Notification notification = new Notification.Builder(this, "riden_core")
            .setSmallIcon(R.drawable.ic_riden_core).setContentTitle("RIDEN-Core")
            .setContentText("Power supply connection is active").setOngoing(true).build();
        startForeground(6006, notification);
    }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) { stopSelf(startId); return START_NOT_STICKY; }
        @SuppressWarnings("deprecation") UsbDevice device = intent.getParcelableExtra(EXTRA_DEVICE);
        int mode = intent.getIntExtra("mode", RidenCore.HOT);
        boolean writes = intent.getBooleanExtra("allowWrites", false);
        boolean multi = intent.getBooleanExtra("allowMultiwrite", false);
        lifecycle.execute(() -> {
            if (destroyed || core != null) return;
            UsbTransport transport = null;
            try {
                if (device == null) throw new IOException("Missing USB device");
                transport = new UsbTransport((UsbManager)getSystemService(USB_SERVICE), device);
                RidenCore created = new RidenCore(transport, mode, writes, multi);
                if (destroyed) created.close(); else core = created;
            } catch (IOException | RuntimeException e) {
                if (transport != null) try { transport.close(); } catch (IOException ignored) { }
                android.util.Log.e("RIDEN-Core", "Device connection failed", e); stopSelf(startId);
            }
        });
        return START_NOT_STICKY;
    }
    @Override public IBinder onBind(Intent intent) { return binder; }
    @Override public void onDestroy() {
        destroyed = true;
        lifecycle.execute(() -> { RidenCore c = core; core = null; if (c != null) try { c.close(); } catch (IOException ignored) { } });
        lifecycle.shutdown(); super.onDestroy();
    }
}
