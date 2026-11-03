package com.hsrprobe

import de.robv.android.xposed.IXposedHookLoadPackage
import de.robv.android.xposed.XC_MethodHook
import de.robv.android.xposed.XposedBridge
import de.robv.android.xposed.XposedHelpers
import de.robv.android.xposed.callbacks.XC_LoadPackage

class ProbeEntry : IXposedHookLoadPackage {

    override fun handleLoadPackage(lpparam: XC_LoadPackage.LoadPackageParam) {
        if (PREFIXES.none { lpparam.packageName.startsWith(it) }) return
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
        // 前缀匹配, 正式服 / B服 / 国际服 / CBT 测试服 全都覆盖
        private val PREFIXES = listOf(
            "com.miHoYo.hkrpg",
            "com.HoYoverse.hkrpg"
        )
    }
}
