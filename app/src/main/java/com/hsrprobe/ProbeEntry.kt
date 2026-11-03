package com.hsrprobe

import android.util.Log
import de.robv.android.xposed.IXposedHookLoadPackage
import de.robv.android.xposed.XposedBridge
import de.robv.android.xposed.callbacks.XC_LoadPackage

class ProbeEntry : IXposedHookLoadPackage {

    override fun handleLoadPackage(lpparam: XC_LoadPackage.LoadPackageParam) {
        // 这行必须最先打, 用来判断 LSPosed 到底有没有把模块注入进来
        Log.i(TAG, "handleLoadPackage: " + lpparam.packageName + " / " + lpparam.processName)

        if (PREFIXES.none { lpparam.packageName.startsWith(it) }) return
        Log.i(TAG, "target matched: " + lpparam.packageName)

        try {
            NativeLoader.load(lpparam)
            Log.i(TAG, "native loaded")
        } catch (t: Throwable) {
            Log.e(TAG, "native load failed", t)
            try {
                XposedBridge.log(t)
            } catch (_: Throwable) {
            }
        }
    }

    companion object {
        const val TAG = "hsrprobe"
        private val PREFIXES = listOf(
            "com.miHoYo.hkrpg",
            "com.HoYoverse.hkrpg"
        )
    }
}
