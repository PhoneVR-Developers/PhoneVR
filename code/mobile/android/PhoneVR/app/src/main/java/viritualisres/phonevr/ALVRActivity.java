/* (C)2023 */
package viritualisres.phonevr;

import android.Manifest;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.opengl.GLSurfaceView;
import android.os.BatteryManager;
import android.os.Build.VERSION;
import android.os.Build.VERSION_CODES;
import android.os.Bundle;
import android.provider.Settings;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.MenuInflater;
import android.view.MenuItem;
import android.view.View;
import android.view.WindowManager;
import android.widget.PopupMenu;
import android.widget.Toast;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import java.util.Objects;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class ALVRActivity extends AppCompatActivity
        implements PopupMenu.OnMenuItemClickListener, BatteryLevelListener {

    static {
        System.loadLibrary("native-lib-alvr");
    }

    private static final String TAG = ALVRActivity.class.getSimpleName() + "-Java";

    // Permission request codes
    private static final int PERMISSIONS_REQUEST_CODE = 2;

    private GLSurfaceView glView;

    private final BatteryMonitor bMonitor = new BatteryMonitor(this);

    public static class BatteryMonitor extends BroadcastReceiver {
        private final BatteryLevelListener listener;

        public BatteryMonitor(BatteryLevelListener listener) {
            this.listener = listener;
        }

        public void startMonitoring(Context context) {
            // Register BroadcastReceiver to monitor battery level changes
            IntentFilter filter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
            context.registerReceiver(this, filter);
        }

        public void stopMonitoring(Context context) {
            // Unregister the BroadcastReceiver
            context.unregisterReceiver(this);
        }

        @Override
        public void onReceive(Context context, Intent intent) {
            if (Objects.equals(intent.getAction(), Intent.ACTION_BATTERY_CHANGED)) {
                // Get the current battery level and scale
                int level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, 0);
                int scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 100);

                // Calculate the battery percentage as a float value between 0 and 1
                float batteryPercentage = (float) level / scale;

                // Get the charging state
                int chargingState = intent.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1);
                boolean isCharging =
                        chargingState == BatteryManager.BATTERY_PLUGGED_AC
                                || chargingState == BatteryManager.BATTERY_PLUGGED_USB
                                || chargingState == BatteryManager.BATTERY_PLUGGED_WIRELESS;

                // Notify the listener with the current battery percentage and charging state
                if (listener != null) {
                    listener.onBatteryLevelChanged(batteryPercentage, isCharging);
                }
            }
        }
    }

    @Override
    public void onCreate(Bundle savedInstance) {
        super.onCreate(savedInstance);
        Log.d(TAG, "onCreate ALVRActivity");

        DisplayMetrics displayMetrics = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getMetrics(displayMetrics);
        int width = displayMetrics.widthPixels;
        int height = displayMetrics.heightPixels;

        // get refresh rate of the device
        Display display = getWindowManager().getDefaultDisplay();
        float refreshRate = display.getRefreshRate();
        Log.i(TAG, "Refresh rate: " + refreshRate);

        // Options chosen on the Settings screen (InitActivity -> Settings).
        SharedPreferences appSettings = getSharedPreferences(SettingsActivity.PREFS, MODE_PRIVATE);
        boolean enableARCore =
                appSettings.getBoolean(
                        SettingsActivity.KEY_ARCORE_ENABLED,
                        SettingsActivity.DEFAULT_ARCORE_ENABLED);
        Log.i(TAG, "ARCore enabled in settings: " + enableARCore);

        initializeNative(width, height, refreshRate, enableARCore);
        setStreamFovScaleNative(
                appSettings.getFloat(
                        SettingsActivity.KEY_FOV_SCALE, SettingsActivity.DEFAULT_FOV_SCALE));
        setFloorModeNative(
                appSettings.getInt(
                        SettingsActivity.KEY_FLOOR_MODE, SettingsActivity.DEFAULT_FLOOR_MODE));

        setContentView(R.layout.activity_vr);
        glView = findViewById(R.id.surface_view);
        glView.setEGLContextClientVersion(3);
        Renderer renderer = new Renderer();
        glView.setRenderer(renderer);
        glView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);

        setImmersiveSticky();
        View decorView = getWindow().getDecorView();
        decorView.setOnSystemUiVisibilityChangeListener(
                (visibility) -> {
                    if ((visibility & View.SYSTEM_UI_FLAG_FULLSCREEN) == 0) {
                        setImmersiveSticky();
                    }
                });

        // Forces screen to max brightness.
        // get setting max_brightness boolean and set brightness to max if required
        SharedPreferences prefs = getSharedPreferences("settings", MODE_PRIVATE);
        if (prefs.getBoolean("max_brightness", true)) {
            WindowManager.LayoutParams layout = getWindow().getAttributes();
            layout.screenBrightness = 1.f;
            getWindow().setAttributes(layout);
        }

        // Prevents screen from dimming/locking.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    public void onBatteryLevelChanged(float batteryPercentage, boolean isPlugged) {
        sendBatteryLevel(batteryPercentage, isPlugged);
        Log.d(TAG, "Battery level changed: " + batteryPercentage + ", isPlugged in? :" + isPlugged);
    }

    @Override
    protected void onPause() {
        super.onPause();
        Log.d(TAG, "Pausing ALVR Activity");
        pauseNative();
        glView.onPause();
        bMonitor.stopMonitoring(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.d(TAG, "Resuming ALVR Activity");

        if (VERSION.SDK_INT < VERSION_CODES.Q && !isReadExternalStorageEnabled()) {
            final String[] permissions = new String[] {Manifest.permission.READ_EXTERNAL_STORAGE};
            ActivityCompat.requestPermissions(this, permissions, PERMISSIONS_REQUEST_CODE);

            return;
        }

        glView.onResume();
        resumeNative();
        bMonitor.startMonitoring(this);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.d(TAG, "Destroying ALVR Activity");
        destroyNative();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            setImmersiveSticky();
        }
    }

    private class Renderer implements GLSurfaceView.Renderer {
        @Override
        public void onSurfaceCreated(GL10 gl10, EGLConfig eglConfig) {
            surfaceCreatedNative();
        }

        @Override
        public void onSurfaceChanged(GL10 gl10, int width, int height) {
            setScreenResolutionNative(width, height);
        }

        @Override
        public void onDrawFrame(GL10 gl10) {
            renderNative();
        }
    }

    // Called from activity_vr.xml
    public void closeSample(View view) {
        Log.d(TAG, "Leaving VR sample");
        finish();
    }

    // Called from activity_vr.xml
    public void showSettings(View view) {
        PopupMenu popup = new PopupMenu(this, view);
        MenuInflater inflater = popup.getMenuInflater();
        inflater.inflate(R.menu.settings_menu, popup.getMenu());

        MenuItem toggleBrightness = popup.getMenu().findItem(R.id.max_brightness_toggle);
        // get setting max_brightness boolean from settings
        SharedPreferences prefs = getSharedPreferences("settings", MODE_PRIVATE);
        boolean isChecked = prefs.getBoolean("max_brightness", true);
        toggleBrightness.setChecked(isChecked);

        popup.setOnMenuItemClickListener(this);
        popup.show();
    }

    @Override
    public boolean onMenuItemClick(MenuItem item) {
        if (item.getItemId() == R.id.switch_viewer) {
            switchViewerNative();
            return true;
        } else if (item.getItemId() == R.id.switch_server) {
            // remove alvr_server prefs
            SharedPreferences.Editor editor = getSharedPreferences("prefs", MODE_PRIVATE).edit();
            editor.remove("alvr_server");
            editor.apply();

            // start InitActivity and clear back history
            Intent intent = new Intent(this, InitActivity.class);
            intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP);
            startActivity(intent);
        } else if (item.getItemId() == R.id.max_brightness_toggle) {
            // Save app setting boolean max_brightness == true
            item.setChecked(!item.isChecked());
            SharedPreferences.Editor editor = getSharedPreferences("settings", MODE_PRIVATE).edit();
            editor.putBoolean("max_brightness", item.isChecked());
            editor.apply();
            return true;
        }
        return false;
    }

    private boolean isReadExternalStorageEnabled() {
        return ActivityCompat.checkSelfPermission(this, Manifest.permission.READ_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
    }

    @Override
    public void onRequestPermissionsResult(
            int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (!isReadExternalStorageEnabled()) {
            Toast.makeText(this, R.string.read_storage_permission, Toast.LENGTH_LONG).show();
            if (!ActivityCompat.shouldShowRequestPermissionRationale(
                    this, Manifest.permission.READ_EXTERNAL_STORAGE)) {
                // Permission denied with checking "Do not ask again". Note that in Android R
                // "Do not ask again" is not available anymore.
                Intent intent = new Intent();
                intent.setAction(Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
                intent.setData(Uri.fromParts("package", getPackageName(), null));
                startActivity(intent);
            }
            finish();
        }
    }

    private void setImmersiveSticky() {
        getWindow()
                .getDecorView()
                .setSystemUiVisibility(
                        View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                                | View.SYSTEM_UI_FLAG_FULLSCREEN
                                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    private native void initializeNative(
            int screenWidth, int screenHeight, float screenRefreshRate, boolean enableARCore);

    private native void setStreamFovScaleNative(float scale);

    private native void setFloorModeNative(int mode);

    private native void destroyNative();

    private native void resumeNative();

    private native void pauseNative();

    private native void surfaceCreatedNative();

    private native void setScreenResolutionNative(int width, int height);

    private native void renderNative();

    private native void switchViewerNative();

    private native void sendBatteryLevel(float level, boolean plugged);
}
