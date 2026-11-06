package com.hsrprobe

import android.util.Log
import de.robv.android.xposed.XSharedPreferences
import de.robv.android.xposed.callbacks.XC_LoadPackage
import java.io.File
import java.util.zip.ZipFile

object NativeLoader {

    private const val TAG = "hsrprobe"
    private const val LIB = "libhsrprobe.so"
    // 模块自己的包名, 开关就存在它的 shared_prefs 里
    private const val MODULE_PKG = "com.hsrprobe"
    private const val PREF = "hsrprobe"
    private const val KEY_NOFADE = "nofade"

    @Volatile
    private var loaded = false

    external fun nativeDump(): String
    external fun nativeStatus(): String
    external fun nativeSetNoFade(on: Boolean)
    external fun nativeGetNoFade(): Boolean

    fun load(lpparam: XC_LoadPackage.LoadPackageParam, moduleApkPath: String?) {
        if (loaded) return
        synchronized(this) {
            if (loaded) return
            doLoad(lpparam, moduleApkPath)
            loaded = true
        }
    }

    private fun doLoad(lpparam: XC_LoadPackage.LoadPackageParam, moduleApkPath: String?) {
        val abi = if (android.os.Process.is64Bit()) "arm64-v8a" else "armeabi-v7a"
        val entryName = "lib/" + abi + "/" + LIB

        val apkPath = findModuleApk(lpparam, entryName, moduleApkPath)
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

        startNoFadeWatcher()
    }

    /**
     * 轮询模块 App 里的「角色全局去虚化」开关, 变化时推给探针.
     * 用 XSharedPreferences 读, LSPosed 会代读别的应用私有目录, 不受 SELinux 限制.
     */
    private fun startNoFadeWatcher() {
        Thread({
            var last: Boolean? = null
            while (true) {
                val on = try {
                    val p = XSharedPreferences(MODULE_PKG, PREF)
                    p.reload()
                    p.getBoolean(KEY_NOFADE, false)
                } catch (t: Throwable) {
                    // 读不到就维持上一次的值, 不影响探针
                    last ?: false
                }
                if (on != last) {
                    last = on
                    try {
                        nativeSetNoFade(on)
                        Log.i(TAG, "nofade=" + on)
                    } catch (t: Throwable) {
                        Log.e(TAG, "nativeSetNoFade failed", t)
                    }
                }
                try {
                    Thread.sleep(1000)
                } catch (e: InterruptedException) {
                    break
                }
            }
        }, "hsrprobe-nofade").apply { isDaemon = true }.start()
    }

    private fun findModuleApk(
        lpparam: XC_LoadPackage.LoadPackageParam,
        entryName: String,
        moduleApkPath: String?
    ): String? {
        val cands = LinkedHashSet<String>()

        // 0) initZygote 给的 modulePath, 正常情况下就是它
        if (!moduleApkPath.isNullOrEmpty()) cands.add(moduleApkPath)

        // 1) LSPosed 的 modulePath (可能被混淆, 一般拿不到)
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

        // 2) 自己的 codeSource
        try {
            val loc = NativeLoader::class.java.protectionDomain?.codeSource?.location
            if (loc != null && loc.protocol == "file") {
                val f = File(loc.toURI())
                if (f.isFile && f.name.endsWith(".apk")) cands.add(f.absolutePath)
            }
        } catch (_: Throwable) {
        }

        // 3) 类加载器 dexElements (Android 14+ 是 hidden API, 大概率被拦)
        cands.addAll(apkPathsFromClassLoader())

        // 4) /proc/self/maps: APK 被 mmap 过就一定在里面, 不依赖任何 hidden API
        cands.addAll(apkPathsFromMaps())

        Log.i(TAG, "apk candidates: " + cands.joinToString(" | "))

        for (p in cands) {
            if (hasEntry(p, entryName)) return p
        }
        return null
    }

    private fun apkPathsFromMaps(): List<String> {
        val out = ArrayList<String>()
        try {
            File("/proc/self/maps").forEachLine { line ->
                val idx = line.indexOf('/')
                if (idx >= 0) {
                    var p = line.substring(idx).trim()
                    if (p.endsWith(" (deleted)")) p = p.removeSuffix(" (deleted)")
                    if (p.endsWith(".apk") && !out.contains(p)) out.add(p)
                }
            }
        } catch (_: Throwable) {
        }
        return out
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
