/* (C)2023 */
package viritualisres.phonevr

import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.text.method.LinkMovementMethod
import android.text.util.Linkify
import android.util.Log
import android.view.View
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SwitchCompat
import androidx.core.text.HtmlCompat

class InitActivity : AppCompatActivity() {

    private val noGvrFlavour = BuildConfig.FLAVOR.equals("noGvr")

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // switch the layout based on flavour
        if (noGvrFlavour) {
            setContentView(R.layout.activity_init_nogvr)
        } else {
            setContentView(R.layout.activity_init)
        }

        val tvVersion: TextView = findViewById<TextView>(R.id.head)
        tvVersion.text = "PhoneVR v" + BuildConfig.VERSION_NAME

        val result =
            HtmlCompat.fromHtml(
                "<a href=\"https://github.com/PhoneVR-Developers/PhoneVR#readme\">Readme.md</a>",
                HtmlCompat.FROM_HTML_MODE_LEGACY)
        val tvBody = findViewById<TextView>(R.id.body)
        Log.d(
            "PVR-JAVA",
            "res: '${ resources.getString(R.string.phonevr_support_common) }', html: '${result.toString()}'")

        tvBody.text = String.format(resources.getString(R.string.phonevr_support_common), result)
        tvBody.movementMethod = LinkMovementMethod.getInstance()
        Linkify.addLinks(tvBody, Linkify.WEB_URLS)

        val prefs = getSharedPreferences("prefs", MODE_PRIVATE)
        val swVRemember = findViewById<SwitchCompat>(R.id.remeber_server)
        swVRemember.isChecked = prefs.getBoolean("remember", false)

        // check if alvr_server setting exists == first run
        // if user has chosen to show server selection menu from respective main activities,
        // ALVRActivity or MainActivity
        // Not kept in Resume() to prevent looping when coming back from AlvrActivity, or others
        if (prefs.contains("alvr_server") && prefs.getBoolean("remember", false)) {
            val intentClass =
                if (!prefs.getBoolean("alvr_server", false) && !noGvrFlavour)
                    MainActivity::class.java
                else ALVRActivity::class.java

            startActivity(Intent(this, intentClass))
            if (intentClass == ALVRActivity::class.java) finish()
        }

        swVRemember.setOnCheckedChangeListener { _, isChecked ->
            prefs.edit().putBoolean("remember", isChecked).apply()
            if (!isChecked) {
                prefs.edit().remove("alvr_server").apply()
            }
        }
    }

    fun btOnClickSettings(view: View) {
        startActivity(Intent(this, SettingsActivity::class.java))
    }

    fun btOnClickALVRStreamer(view: View) {
        val prefs = getSharedPreferences("prefs", MODE_PRIVATE)
        // Save choice after first open. alvr_server as boolean
        if (prefs.getBoolean("remember", false))
            prefs.edit().putBoolean("alvr_server", true).apply()

        val intent = Intent(this, ALVRActivity::class.java)
        startActivity(intent)
        finish()
    }

    fun btOnClickPVRStreamer(view: View) {
        // Check if SYSTEM ABI supports other than x86_64 ABI, since libGVR has no support for
        // x86_64
        if (Build.SUPPORTED_ABIS.contains("x86") ||
            Build.SUPPORTED_ABIS.contains("arm64-v8a") ||
            Build.SUPPORTED_ABIS.contains("armeabi-v7a")) {
            val prefs = getSharedPreferences("prefs", MODE_PRIVATE)

            if (prefs.getBoolean("remember", false))
                prefs.edit().putBoolean("alvr_server", false).apply()

            val intent = Intent(this, MainActivity::class.java)
            startActivity(intent)
        } else {
            Toast.makeText(
                    this,
                    "Your device ONLY supports x86_64 ABI, which is not supported by GoogleVR",
                    Toast.LENGTH_LONG)
                .show()
        }
    }
}
