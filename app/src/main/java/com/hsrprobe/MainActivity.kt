package com.hsrprobe

import android.app.Activity
import android.os.Bundle
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import java.io.File

class MainActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val prefs = getSharedPreferences(PREF, MODE_PRIVATE)

        val tv = TextView(this)
        val ver = try {
            packageManager.getPackageInfo(packageName, 0).versionName
        } catch (t: Throwable) {
            "?"
        }
        tv.text = INFO.replace("%VER%", ver ?: "?")
        tv.textSize = 13f
        tv.setPadding(56, 40, 56, 56)

        val sw = Switch(this).apply {
            text = "角色全局去虚化"
            textSize = 16f
            isChecked = prefs.getBoolean(KEY_NOFADE, false)
            setPadding(56, 60, 56, 8)
            setOnCheckedChangeListener { _, checked ->
                prefs.edit().putBoolean(KEY_NOFADE, checked).apply()
                // 游戏进程用 XSharedPreferences 读这个文件, 必须让别的应用可读
                makePrefsReadable()
            }
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(sw)
            addView(tv)
        }
        setContentView(ScrollView(this).also { it.addView(root) })

        // 每次进来都确保可读, 防止重装/清理后权限丢失
        makePrefsReadable()
    }

    /** 放开 shared_prefs 目录和 xml 的读权限, 游戏进程靠它读开关 */
    private fun makePrefsReadable() {
        try {
            val data = File(applicationInfo.dataDir)
            val dir = File(data, "shared_prefs")
            val f = File(dir, "$PREF.xml")
            data.setExecutable(true, false)
            dir.setExecutable(true, false)
            if (f.exists()) f.setReadable(true, false)
        } catch (_: Throwable) {
        }
    }

    companion object {
        private const val PREF = "hsrprobe"
        private const val KEY_NOFADE = "nofade"

        private val INFO = """
            HSRModProbe %VER%

            1. 在 LSPosed 里启用本模块, 作用域勾选崩坏:星穹铁道
            2. 强制停止游戏后重新启动
            3. 进游戏走到能看到角色的场景, 等 5 秒
            4. 取文件:
               /sdcard/Android/data/<包名>/files/hsrprobe/probe.json

            「角色全局去虚化」开关改完不用重启游戏,
            1 秒内生效; 关掉就恢复原来的靠近淡出。

            包名对照:
              com.miHoYo.hkrpg              国服
              com.miHoYo.hkrpg.bilibili     B服
              com.HoYoverse.hkrpgoversea    国际服

            日志: logcat -s hsrprobe
            停用: 在同目录建一个空文件 DISABLE 再重启游戏
        """.trimIndent()
    }
}
