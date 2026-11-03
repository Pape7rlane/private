package com.hsrprobe

import de.robv.android.xposed.XposedBridge
import de.robv.android.xposed.callbacks.XC_LoadPackage
import java.io.File
import java.util.zip.ZipFile

object NativeLoader {

    private const val LIB = "libhsrprobe.so"

    @Volatile
    private var loaded = false

    external fun nativeDump(): String
    external fun nativeStatus(): String

    fun load(lpparam: XC_LoadPackage.LoadPackageParam) {
        if (loaded) return
        synchronized(this) {
            if (loaded) return
            doLoad(lpparam)
            loaded = true
        }
    }

    private fun doLoad(lpparam: XC_LoadPackage.LoadPackageParam) {
        val abi = if (android.os.Process.is64Bit()) "arm64-v8a" else "armeabi-v7a"
        val apk = File(lpparam.modulePath)
        if (!apk.exists()) throw IllegalStateException("module apk not found: " + apk)

        // 我们的代码就运行在游戏进程里, uid 就是游戏 uid, 可以直接写它的私有目录
        val dir = File("/data/data/" + lpparam.packageName + "/files/probe")
        if (!dir.exists() && !dir.mkdirs()) {
            throw IllegalStateException("cannot create " + dir)
        }

        val out = File(dir, LIB)
        if (out.exists()) out.delete()
        ZipFile(apk).use { zip ->
            val entry = zip.getEntry("lib/" + abi + "/" + LIB)
                ?: throw IllegalStateException("no " + abi + " " + LIB + " in module apk")
            zip.getInputStream(entry).use { input ->
                out.outputStream().use { output -> input.copyTo(output) }
            }
        }
        out.setReadable(true, false)
        out.setExecutable(true, false)
        // Android 14+ 禁止 dlopen 应用可写目录里的文件, 必须置只读
        out.setWritable(false, false)

        XposedBridge.log("[hsrprobe] loading " + out.absolutePath)
        System.load(out.absolutePath)
    }
}
