package com.hsrprobe

import de.robv.android.xposed.IXposedHookLoadPackage
import de.robv.android.xposed.XC_MethodHook
import de.robv.android.xposed.XposedBridge
import de.robv.android.xposed.XposedHelpers
import de.robv.android.xposed.callbacks.XC_LoadPackage

class ProbeEntry : IXposedHookLoadPackage {

    override fun handleLoadPackage(lpparam: XC_LoadPackage.LoadPackageParam) {
        if (lpparam.packageName !in TARGETS) return
        XposedBridge.log("[hsrprobe] target: " + lpparam.packageName)

        try {
            XposedHelpers.findAndHookMethod(
                "android.app.Instrumentation",
                lpparam.classLoader,
                "callApplicationOnCreate",
                android.app.Application::class.java,
                object : XC_MethodHook() {
                    override fun afterHookedMethod(param: XC_MethodHook.MethodHookParam) {
                        try {
                            NativeLoader.load(lpparam)
                        } catch (t: Throwable) {
                            XposedBridge.log("[hsrprobe] native load failed")
                            XposedBridge.log(t)
                        }
                    }
                }
            )
        } catch (t: Throwable) {
            XposedBridge.log("[hsrprobe] hook failed")
            XposedBridge.log(t)
        }
    }

    companion object {
        private val TARGETS = setOf(
            "com.miHoYo.hkrpg",
            "com.miHoYo.hkrpg.bilibili",
            "com.HoYoverse.hkrpgoversea"
        )
    }
}
