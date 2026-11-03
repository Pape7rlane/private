# 崩铁安卓 mod — 可行性勘察结论

日期: 2026-11-03
设备: MT6895 / Mali-G610, Android 16, KernelSU, Zygisk 版 LSPosed
游戏: com.HoYoverse.hkrpgoverseacbtest

## 1. 已经打通的部分

LSPosed 注入 -> 模块加载 -> 解 so -> System.load -> Dobby hook 全部成功。

关键点（照抄 Yuanqi314/GIBox）:

- 模块 APK 路径只能从 IXposedHookZygoteInit.initZygote(StartupParam).modulePath 拿。
  api-82 里 handleLoadPackage 没有这个字段；LSPosed 自身类被 R8 混淆，反射也不行。
- Android 14+ 禁止 dlopen 应用可写目录里的 .so，解压后必须 setWritable(false)。
- 只在主进程跑: lpparam.processName == lpparam.packageName。

日志证据:

    I hsrprobe: target matched, moduleApk=/data/app/.../com.hsrprobe-.../base.apk
    I hsrprobe: hsrprobe loaded, pkg=com.HoYoverse.hkrpgoverseacbtest
    I hsrprobe: hooked vkCmdDrawIndexed ...
    I hsrprobe: hsrprobe ready

## 2. Vulkan 路线作废

游戏在这台机器上跑 GLES:

    D Unity : GL_EXT_debug_marker GL_ARM_mali_shader_binary GL_OES_...

17 个 Vulkan hook 安装成功，实际调用 0 次，probe.json 从未生成。
3Dmigoto 那套（drawcall + IB 哈希 + shader 替换）在 Android 上不成立。

## 3. 静态 Il2CppDumper 路线作废

    assets/bin/Data/Managed/Metadata/global-metadata.dat   105MB
    头 4 字节: 4d 48 59 00  ("MHY\0")
    标准 Unity metadata 魔数应为: AF 1B B1 FA

    startup-metadata.dat 同样是加密字节。

    libil2cpp.so 459MB, dynsym 只有 3 个 il2cpp_ 符号:
      il2cpp_get_api_table
      il2cpp_noelle_lock_world_mutex
      il2cpp_noelle_unlock_world_mutex

内存里也找不到解密后的 metadata:

    proc_scan 1521 SkinnedMeshRenderer       -> 命中, 但都在 libil2cpp.so 的 r--p (Unity 引擎自带字符串)
    proc_scan 1521 AvatarDataMonoBehaviour   -> 0 命中 (游戏自己的类名不是明文)
    proc_scan 1521 <AF 1B B1 FA>             -> 0 命中 (扫了 3.1GB)

结论: metadata 在磁盘和内存里都是加密的，类名/方法名拿不到。

## 4. 仍然可行的路线

libil2cpp.so 导出了 il2cpp_get_api_table。这是 Unity 2022+/6 的 API 表入口，
返回一个函数指针数组（约 1000+ 项）。有了它就能在运行时:

    il2cpp_domain_get
    il2cpp_domain_get_assemblies
    il2cpp_assembly_get_image
    il2cpp_image_get_class / il2cpp_image_get_class_count
    il2cpp_class_get_name / il2cpp_class_get_namespace
    il2cpp_class_get_methods -> il2cpp_method_get_name / il2cpp_method_get_pointer

等于在运行时重建出 dump.cs 的内容，完全绕开加密 metadata。

运行时内存里确实存在大规模函数指针表（ptrscan 扫到单段 41 万个连续指针，
指向 .text，即 il2cpp 的 method pointer table），说明运行时方法表是实体化的。

**卡点**: API 表的索引顺序由 Unity 的 il2cpp-api-functions.h 决定，版本相关。
需要确定崩铁用的 Unity 版本，拿到对应顺序，再逐个验证。

## 5. 参考实现

Yuanqi314/GIBox（原神安卓 mod，同技术栈）走的是另一条路:
按 libil2cpp.so 的 GNU Build ID + RVA 直接 hook Unity 引擎函数。

    buildId   = "2b4dd73155e845a2ef698ec500509baa4eb3bfa8"
    setMesh   = 0x8d9de7c
    lodBind   = 0x5798010
    meshBones = 0x56e4d80
    receive   = 0x88770e0   (游戏的异步模型交付)

它不需要认 drawcall、不碰 shader，在引擎绑定 mesh 前把指针换掉。
这条路对崩铁同样成立，但前提是能拿到方法 RVA —— 也就是回到第 4 节。

## 6. 工具

tools/proc_scan.c   扫描进程内存里的字节串
tools/memdump.c     读指定地址 / 列出进程的 maps 段
tools/ptrscan.c     在内存里找连续的指针数组

用法:

    clang -O2 -o proc_scan tools/proc_scan.c
    su -c "./proc_scan <pid> <needle> 1"
    su -c "./memdump -m <pid> libil2cpp"
    su -c "./memdump -d <pid> 0x<addr> 64"
    su -c "./ptrscan <pid> <lo_hex> <hi_hex> 100"

## 7. Unity 版本（补充）

    libunity.so   -> 2019.4.34f1
    libil2cpp.so  -> 2025.5.001 (米哈游自己的引擎版本串)

Unity 2019.4.34f1 对应的 metadata 版本是 24.1，Il2CppDumper 本身支持。
所以只要能把 105MB 的 MHY 容器解出来，静态路线立刻就活了。

注意 il2cpp 被米哈游改过（导出了 il2cpp_noelle_lock_world_mutex 这类符号，
"noelle" 是他们内部代号），所以 API 表的索引顺序不能直接套 Unity 官方枚举，
需要实测。

## 8. 下一步（按优先级）

1. 逆 global-metadata.dat 的解密。.rodata 里有明文的 "global-metadata" 字符串
   （运行时可搜到，见第 3 节），从它 xref 到解析函数，顺着找解密循环。
   解出来就是标准 v24.1 metadata，Il2CppDumper 直接出 dump.cs，后面全是体力活。
2. 或者走 API 表：dump 表内容，按函数体特征逐个确认索引，再枚举类/方法。
3. 拿到 RVA 之后照抄 GIBox 的 hook 结构（bindMesh / lodBind / meshBones / receive）。

