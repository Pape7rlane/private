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
        val apkPath = moduleApkPath(lpparam)
            ?: throw IllegalStateException("cannot locate module apk")
        XposedBridge.log("[hsrprobe] module apk: " + apkPath)

        // 代码运行在游戏进程里, uid 就是游戏 uid, 可以直接写它的私有目录
        val dir = File("/data/data/" + lpparam.packageName + "/files/probe")
        if (!dir.exists() && !dir.mkdirs()) {
            throw IllegalStateException("cannot create " + dir)
        }

        val out = File(dir, LIB)
        if (out.exists()) out.delete()

        ZipFile(apkPath).use { zip ->
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

    // api-82 里没有 modulePath, 但运行时 (LSPosed/LSPatch) 的对象上有, 用反射拿
    private fun moduleApkPath(lpparam: XC_LoadPackage.LoadPackageParam): String? {
        try {
            val f = lpparam.javaClass.getDeclaredField("modulePath")
            f.isAccessible = true
            val v = f.get(lpparam) as? String
            if (!v.isNullOrEmpty()) return v
        } catch (_: Throwable) {
        }
        try {
            val m = lpparam.javaClass.getMethod("getModulePath")
            val v = m.invoke(lpparam) as? String
            if (!v.isNullOrEmpty()) return v
        } catch (_: Throwable) {
        }
        try {
            val loc = NativeLoader::class.java.protectionDomain?.codeSource?.location
            if (loc != null && loc.protocol == "file") {
                val f = File(loc.toURI())
                if (f.isFile && f.name.endsWith(".apk")) return f.absolutePath
            }
        } catch (_: Throwable) {
        }
        return null
    }
}
