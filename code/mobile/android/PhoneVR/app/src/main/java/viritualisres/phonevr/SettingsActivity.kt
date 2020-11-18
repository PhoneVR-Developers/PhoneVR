package viritualisres.phonevr


import android.content.Context
import android.os.Bundle
import android.os.Environment.getExternalStorageDirectory
import android.util.Log
import android.widget.EditText
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import kotlinx.android.synthetic.main.activity_settings.*
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.io.RandomAccessFile
import java.lang.Exception
import java.lang.RuntimeException
import java.util.*

class SettingsActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        try {
            throw RuntimeException("Meowwwww Meow Motherfuker!!!!")

            setContentView(R.layout.activity_settings)
            loadPrefs()

            setTextToLogger(findViewById<EditText>(R.id.etLogView))
        }
        catch (e : Exception)
        {
            Log.d("PVR-JAVA", "Settings::onCreate() Thrw Exception with Message : " + e.message + "\n\n And with StackTrance :" + e.printStackTrace());
        }
    }

    /*2020-11-15 00:57:48.506 8620-8620/viritualisres.phonevr W/System.err: java.lang.RuntimeException: Meowwwww Meow Motherfuker!!!!
2020-11-15 00:57:48.506 8620-8620/viritualisres.phonevr W/System.err:     at viritualisres.phonevr.SettingsActivity.onCreate(SettingsActivity.kt:25)
2020-11-15 00:57:48.506 8620-8620/viritualisres.phonevr W/System.err:     at android.app.Activity.performCreate(Activity.java:7820)
2020-11-15 00:57:48.506 8620-8620/viritualisres.phonevr W/System.err:     at android.app.Activity.performCreate(Activity.java:7809)
2020-11-15 00:57:48.506 8620-8620/viritualisres.phonevr W/System.err:     at android.app.Instrumentation.callActivityOnCreate(Instrumentation.java:1318)
2020-11-15 00:57:48.506 8620-8620/viritualisres.phonevr W/System.err:     at android.app.ActivityThread.performLaunchActivity(ActivityThread.java:3363)
2020-11-15 00:57:48.506 8620-8620/viritualisres.phonevr W/System.err:     at android.app.ActivityThread.handleLaunchActivity(ActivityThread.java:3527)
2020-11-15 00:57:48.506 8620-8620/viritualisres.phonevr W/System.err:     at android.app.servertransaction.LaunchActivityItem.execute(LaunchActivityItem.java:83)
2020-11-15 00:57:48.506 8620-8620/viritualisres.phonevr W/System.err:     at android.app.servertransaction.TransactionExecutor.executeCallbacks(TransactionExecutor.java:135)
2020-11-15 00:57:48.506 8620-8620/viritualisres.phonevr W/System.err:     at android.app.servertransaction.TransactionExecutor.execute(TransactionExecutor.java:95)
2020-11-15 00:57:48.507 8620-8620/viritualisres.phonevr W/System.err:     at android.app.ActivityThread$H.handleMessage(ActivityThread.java:2123)
2020-11-15 00:57:48.507 8620-8620/viritualisres.phonevr W/System.err:     at android.os.Handler.dispatchMessage(Handler.java:107)
2020-11-15 00:57:48.507 8620-8620/viritualisres.phonevr W/System.err:     at android.os.Looper.loop(Looper.java:214)
2020-11-15 00:57:48.507 8620-8620/viritualisres.phonevr W/System.err:     at android.app.ActivityThread.main(ActivityThread.java:7710)
2020-11-15 00:57:48.507 8620-8620/viritualisres.phonevr W/System.err:     at java.lang.reflect.Method.invoke(Native Method)
2020-11-15 00:57:48.507 8620-8620/viritualisres.phonevr W/System.err:     at com.android.internal.os.RuntimeInit$MethodAndArgsCaller.run(RuntimeInit.java:516)
2020-11-15 00:57:48.507 8620-8620/viritualisres.phonevr W/System.err:     at com.android.internal.os.ZygoteInit.main(ZygoteInit.java:950)
This is called STACKTRACE, it will show where exactly tthe error Originated from and why !


 W/System.err: java.lang.RuntimeException: Meowwwww Meow Motherfuker!!!!
  This is called e.ErrorMEssage() !!


*/

    private fun setTextToLogger(et: EditText){
        val file: File = File(getExternalFilesDir(null).toString() + "/PVR/pvrlog.txt")
        et.setText(tail2(file, 100))
    }

    fun tail2(file: File, lines: Int): String {
        var fileHandler: RandomAccessFile? = null
        return try {
            fileHandler = RandomAccessFile(file, "r")
            val fileLength = fileHandler.length() - 1
            val sb = java.lang.StringBuilder()
            var line = 0
            for (filePointer in fileLength downTo -1 + 1) {
                fileHandler.seek(filePointer)
                val readByte = fileHandler.readByte().toInt()
                if (readByte == 0xA) {
                    if (filePointer < fileLength) {
                        line += 1
                    }
                } else if (readByte == 0xD) {
                    if (filePointer < fileLength - 1) {
                        line += 1
                    }
                }
                if (line >= lines) {
                    break
                }
                sb.append(readByte.toChar())
            }
            val toString = sb.reverse().toString()
            toString
        } catch (e: FileNotFoundException) {
            e.printStackTrace()
            ""
        } catch (e: IOException) {
            e.printStackTrace()
            ""
        } finally {
            if (fileHandler != null) try {
                fileHandler.close()
            } catch (e: IOException) {
            }
        }
    }

    override fun onPause() {
        super.onPause()
        savePrefs()
    }

    private fun savePrefs() {
        try {
            val prefs = getSharedPreferences(pvrPrefsKey, Context.MODE_PRIVATE)
            val edit = prefs.edit()
            with(edit) {
                putString(pcIpKey, pcIp.text.toString())
                if(Util_IsVaildPort(connPort.text.toString().toInt())) putInt(connPortKey, connPort.text.toString().toInt())
                if(Util_IsVaildPort(videoPort.text.toString().toInt())) putInt(videoPortKey, videoPort.text.toString().toInt())
                if(Util_IsVaildPort(posePort.text.toString().toInt())) putInt(posePortKey, posePort.text.toString().toInt())
                putInt(resMulKey, resMul.text.toString().toInt())
                putFloat(mt2phKey, mt2ph.text.toString().toFloat())
                putFloat(offFovKey, offFov.text.toString().toFloat())
                putBoolean(warpKey, warp.isChecked)
                putBoolean(debugKey, debug.isChecked)
                apply()
            }
        }
        catch(e : Exception)
        {
            Log.d("PVR-Java", "Exception caught in savePrefs.SettingsActivity : " + e.message);
            e.printStackTrace();
        }
    }

    private fun loadPrefs() {
        val prefs = getSharedPreferences(pvrPrefsKey, Context.MODE_PRIVATE)

        val l = Locale.getDefault()
        val fmt = "%1\$d"
        val fmt2 = "%1$.1f"
        val fmt3 = "%1$.3f"
        pcIp.setText(prefs.getString(pcIpKey, pcIpDef))
        connPort.setText(String.format(l, fmt, prefs.getInt(connPortKey, connPortDef)))
        videoPort.setText(String.format(l, fmt, prefs.getInt(videoPortKey, videoPortDef)))
        posePort.setText(String.format(l, fmt, prefs.getInt(posePortKey, posePortDef)))
        resMul.setText(String.format(l, fmt, prefs.getInt(resMulKey, resMulDef)))
        mt2ph.setText(String.format(l, fmt3, prefs.getFloat(mt2phKey, mt2phDef)))
        offFov.setText(String.format(l, fmt2, prefs.getFloat(offFovKey, offFovDef)))
        warp.isChecked = prefs.getBoolean(warpKey, warpDef)
        debug.isChecked = prefs.getBoolean(debugKey, debugDef)
    }

    private fun Util_IsVaildPort(port : Int) : Boolean
    {
        return ((port > 0)  && (port <= 65536));
    }
}