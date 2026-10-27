/* (C)2026 */
package viritualisres.phonevr;

import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.SwitchCompat;

/**
 * The settings screen shown from the start screen, before VR starts. Every change is saved at once
 * to the "settings" preferences and read by ALVRActivity when VR starts.
 */
public class SettingsActivity extends AppCompatActivity {

    // The same file ALVRActivity already uses for max_brightness.
    static final String PREFS = "settings";

    static final String KEY_ARCORE_ENABLED = "arcore_enabled";
    static final String KEY_FOV_SCALE = "fov_scale";
    static final String KEY_FLOOR_MODE = "floor_mode";

    static final boolean DEFAULT_ARCORE_ENABLED = true;
    static final float DEFAULT_FOV_SCALE = 0.8f;
    static final int DEFAULT_FLOOR_MODE = 2;

    // The slider covers 60% .. 100% in 1% steps (progress 0 .. 40).
    private static final int FOV_MIN_PERCENT = 60;
    private static final int FOV_MAX_PERCENT = 100;

    private SharedPreferences prefs;
    private TextView fovValue;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_settings);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);

        // ARCore on/off
        SwitchCompat arcoreSwitch = findViewById(R.id.settings_arcore_switch);
        arcoreSwitch.setChecked(prefs.getBoolean(KEY_ARCORE_ENABLED, DEFAULT_ARCORE_ENABLED));
        arcoreSwitch.setOnCheckedChangeListener(
                (button, isChecked) ->
                        prefs.edit().putBoolean(KEY_ARCORE_ENABLED, isChecked).apply());

        // Field of view slider
        fovValue = findViewById(R.id.settings_fov_value);
        SeekBar fovSeek = findViewById(R.id.settings_fov_seekbar);
        fovSeek.setMax(FOV_MAX_PERCENT - FOV_MIN_PERCENT);
        int percent = Math.round(prefs.getFloat(KEY_FOV_SCALE, DEFAULT_FOV_SCALE) * 100f);
        percent = Math.max(FOV_MIN_PERCENT, Math.min(FOV_MAX_PERCENT, percent));
        fovSeek.setProgress(percent - FOV_MIN_PERCENT);
        showFovPercent(percent);
        fovSeek.setOnSeekBarChangeListener(
                new SeekBar.OnSeekBarChangeListener() {
                    @Override
                    public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                        int p = FOV_MIN_PERCENT + progress;
                        showFovPercent(p);
                        if (fromUser) {
                            prefs.edit().putFloat(KEY_FOV_SCALE, p / 100f).apply();
                        }
                    }

                    @Override
                    public void onStartTrackingTouch(SeekBar bar) {}

                    @Override
                    public void onStopTrackingTouch(SeekBar bar) {}
                });

        // Floor (height) mode
        Spinner floorSpinner = findViewById(R.id.settings_floor_spinner);
        ArrayAdapter<CharSequence> adapter =
                ArrayAdapter.createFromResource(
                        this, R.array.floor_mode_choices, android.R.layout.simple_spinner_item);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        floorSpinner.setAdapter(adapter);
        int mode = prefs.getInt(KEY_FLOOR_MODE, DEFAULT_FLOOR_MODE);
        floorSpinner.setSelection(Math.max(0, Math.min(adapter.getCount() - 1, mode)));
        floorSpinner.setOnItemSelectedListener(
                new AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(
                            AdapterView<?> parent, View view, int position, long id) {
                        prefs.edit().putInt(KEY_FLOOR_MODE, position).apply();
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent) {}
                });
    }

    private void showFovPercent(int percent) {
        fovValue.setText(percent + "%");
    }
}
