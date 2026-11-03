package com.hsrprobe

import android.util.Log
import de.robv.android.xposed.callbacks.XC_LoadPackage
import java.io.File
import java.util.zip.ZipFile

object NativeLoader {

    private const val TAG = "hsrprobe"
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
        val entryName = "lib/" + abi + "/" + LIB

        val apkPath = findModuleApk(lpparam, entryName)
            ?: throw IllegalStateException("cannot locate module apk")
        Log.i(TAG, "module apk: " + apkPath)

        val dir = File("/data/data/" + lpparam.packageName + "/files/probe")
        if (!dir.exists() && !dir.mkdirs()) {
            throw IllegalStateException("cannot create " + dir)
        }

        val out = File(dir, LIB)
        if (out.exists()) out.delete()

        ZipFile(apkPath).use { zip ->
            val entry = zip.getEntry(entryName)
                ?: throw IllegalStateException("no " + entryName + " in " + apkPath)
            zip.getInputStream(entry).use { input ->
                out.outputStream().use { output -> input.copyTo(output) }
            }
        }

        out.setReadable(true, false)
        out.setExecutable(true, false)
        // Android 14+ 禁止 dlopen 应用可写目录里的文件, 必须置只读
        out.setWritable(false, false)

        Log.i(TAG, "loading " + out.absolutePath)
        System.load(out.absolutePath)
    }

    // LSPosed 自己的类被 R8 混淆了, modulePath 字段名不存在, 只能从
    // 类加载器的 dexElements 里翻, 再用 zip 里有没有我们的 so 来确认是哪个 APK
    private fun findModuleApk(lpparam: XC_LoadPackage.LoadPackageParam, entryName: String): String? {
        val cands = LinkedHashSet<String>()

        for (name in arrayOf("modulePath", "getModulePath")) {
            try {
                val c = lpparam.javaClass
                val v = if (name.startsWith("get")) {
                    c.getMethod(name).invoke(lpparam)
                } else {
                    val f = c.getDeclaredField(name)
                    f.isAccessible = true
                    f.get(lpparam)
                }
                if (v is String && v.isNotEmpty()) cands.add(v)
            } catch (_: Throwable) {
            }
        }

        try {
            val loc = NativeLoader::class.java.protectionDomain?.codeSource?.location
            if (loc != null && loc.protocol == "file") {
                val f = File(loc.toURI())
                if (f.isFile && f.name.endsWith(".apk")) cands.add(f.absolutePath)
            }
        } catch (_: Throwable) {
        }

        cands.addAll(apkPathsFromClassLoader())

        Log.i(TAG, "apk candidates: " + cands.joinToString(" | "))

        for (p in cands) {
            if (hasEntry(p, entryName)) return p
        }
        return null
    }

    private fun apkPathsFromClassLoader(): List<String> {
        val out = ArrayList<String>()
        try {
            val cl = NativeLoader::class.java.classLoader ?: return out
            val base = Class.forName("dalvik.system.BaseDexClassLoader")
            val plf = base.getDeclaredField("pathList")
            plf.isAccessible = true
            val pathList = plf.get(cl) ?: return out
            val ef = pathList.javaClass.getDeclaredField("dexElements")
            ef.isAccessible = true
            val elements = ef.get(pathList) as? Array<*> ?: return out
            for (e in elements) {
                if (e == null) continue
                for (field in arrayOf("path", "file")) {
                    try {
                        val f = e.javaClass.getDeclaredField(field)
                        f.isAccessible = true
                        val v = f.get(e)
                        val p = when (v) {
                            is File -> v.absolutePath
                            is String -> v
                            else -> null
                        }
                        if (p != null && p.endsWith(".apk")) {
                            out.add(p)
                            break
                        }
                    } catch (_: Throwable) {
                    }
                }
            }
        } catch (_: Throwable) {
        }
        return out
    }

    private fun hasEntry(apk: String, entry: String): Boolean {
        return try {
            ZipFile(apk).use { it.getEntry(entry) != null }
        } catch (t: Throwable) {
            false
        }
    }
}
