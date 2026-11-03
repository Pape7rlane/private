package com.hsrprobe

import android.app.Activity
import android.os.Bundle
import android.widget.ScrollView
import android.widget.TextView

class MainActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val tv = TextView(this)
        tv.text = INFO
        tv.textSize = 13f
        tv.setPadding(56, 120, 56, 56)
        setContentView(ScrollView(this).also { it.addView(tv) })
    }

    companion object {
        private val INFO = """
            HSRModProbe 0.1.0  (P0 probe)

            1. 在 LSPosed 里启用本模块, 作用域勾选崩坏:星穹铁道
            2. 强制停止游戏后重新启动
            3. 进游戏走到能看到角色的场景, 等 5 秒
            4. 取文件:
               /sdcard/Android/data/<包名>/files/hsrprobe/probe.json

            包名对照:
              com.miHoYo.hkrpg              国服
              com.miHoYo.hkrpg.bilibili     B服
              com.HoYoverse.hkrpgoversea    国际服

            日志: logcat -s hsrprobe
            停用: 在同目录建一个空文件 DISABLE 再重启游戏
        """.trimIndent()
    }
}
