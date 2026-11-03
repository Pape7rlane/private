package com.hsrprobe

import android.util.Log
import de.robv.android.xposed.IXposedHookLoadPackage
import de.robv.android.xposed.IXposedHookZygoteInit
import de.robv.android.xposed.XposedBridge
import de.robv.android.xposed.callbacks.XC_LoadPackage

class ProbeEntry : IXposedHookLoadPackage, IXposedHookZygoteInit {

    override fun initZygote(startupParam: IXposedHookZygoteInit.StartupParam) {
        // api-82 的 modulePath 只在 initZygote 里给, 这是唯一稳定的来源
        moduleApk = startupParam.modulePath
    }

    override fun handleLoadPackage(lpparam: XC_LoadPackage.LoadPackageParam) {
        Log.i(TAG, "handleLoadPackage: " + lpparam.packageName + " / " + lpparam.processName)

        if (PREFIXES.none { lpparam.packageName.startsWith(it) }) return
        if (lpparam.processName != lpparam.packageName) {
            Log.i(TAG, "skip sub process: " + lpparam.processName)
            return
        }
        Log.i(TAG, "target matched, moduleApk=" + moduleApk)

        try {
            NativeLoader.load(lpparam, moduleApk)
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

        @Volatile
        var moduleApk: String? = null

        private val PREFIXES = listOf(
            "com.miHoYo.hkrpg",
            "com.HoYoverse.hkrpg"
        )
    }
}
