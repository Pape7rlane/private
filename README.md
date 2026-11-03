# HSRModProbe — 崩坏:星穹铁道 安卓 Vulkan 探针 (P0)

一个 LSPosed 模块。注入游戏进程后 hook Vulkan，把**所有带索引的 drawcall** 和
**所有 graphics pipeline 的顶点输入布局** dump 成 JSON。

这是"移植 3Dmigoto / SRMI 到安卓"的第一步：不换任何东西，只观测。
观测结果决定后面整个项目怎么走（见文末"判定实验"）。

## 目录

~~~
app/                     LSPosed 模块 (Kotlin)
  src/main/assets/xposed_init    入口类
  src/main/java/com/hsrprobe/
    ProbeEntry.kt        注入点: Instrumentation.callApplicationOnCreate
    NativeLoader.kt      从模块 APK 里解出 .so, 加载
    MainActivity.kt      说明页
native/
  CMakeLists.txt
  src/vk_probe.cpp       全部 native 逻辑 (单文件, 约 680 行)
  third_party/Dobby/     已内置, 无需 submodule
  third_party/bcdec.h    已内置, P2 贴图转码用, 现在没接
.github/workflows/build.yml
~~~

## 构建

云端（推荐，本地没有 NDK）：

1. 推到 GitHub，Actions 自动跑
2. 取 artifacts 里的 HSRModProbe-apk

本地（需要 Android SDK + NDK 26.1 + CMake 3.22 + JDK 17）：

~~~
keytool -genkeypair -keystore app/ci.jks -alias ci -keyalg RSA \
  -keysize 2048 -validity 10000 -storepass android -keypass android -dname "CN=ci"
gradle assembleRelease
~~~

产物：app/build/outputs/apk/release/app-release.apk

## 使用

1. 安装 APK
2. LSPosed 里启用，作用域勾选星穹铁道
3. 强制停止游戏，重新启动
4. 进场景看到角色，等 5 秒
5. 取 /sdcard/Android/data/<包名>/files/hsrprobe/probe.json

包名：国服 com.miHoYo.hkrpg / B服 com.miHoYo.hkrpg.bilibili / 国际服 com.HoYoverse.hkrpgoversea

日志：logcat -s hsrprobe
紧急停用：同目录建空文件 DISABLE，重启游戏

## hook 了什么

| 函数 | 干什么 |
|---|---|
| vkCmdDrawIndexed | 记 drawcall（主表） |
| vkCmdBindPipeline | 记当前 pipeline，用于去重键 |
| vkCmdBindIndexBuffer | 记绑定 IB + offset + 索引类型 |
| vkCmdBindVertexBuffers | 记绑定 VB（P1 替换要用） |
| vkCmdBindDescriptorSets | 只记数量（P2 贴图要用） |
| vkCreateShaderModule | 算 SPIR-V 哈希 |
| vkCreateGraphicsPipelines | dump 顶点输入布局 + 关联 shader 哈希 |
| vkCreateBuffer / vkBindBufferMemory | 建 buffer→内存映射表 |
| vkMapMemory / vkUnmapMemory | 拿到 host 可读指针 |
| vkCmdUpdateBuffer / vkCmdCopyBuffer | 抓上传内容（device-local IB 只能这样读） |
| vkQueuePresentKHR | 帧计数 + 每 300 帧落盘 |
| vkGetDeviceProcAddr / vkGetInstanceProcAddr | 兜底：游戏若走 ICD 直连，返回我们的函数 |

## probe.json 怎么读

~~~json
{
  "meta": { "pkg": "...", "frames": 900, "draws_total": 812345, "unique": 1204,
            "no_ib": 0, "unreadable": 3, "no_cmd": 0 },
  "shaders":   [ { "h": "0f3a...", "size": 4096 } ],
  "pipelines": [ { "h": "7f00...", "vs": "...", "ps": "...", "topo": "tris",
                   "binds": [ { "b": 0, "stride": 40, "rate": 0 } ],
                   "attrs": [ { "loc": 0, "b": 0, "f": 106, "fn": "RGB32F", "off": 0 } ] } ],
  "draws":     [ { "p": "...", "ib": "...", "ibh": "...", "n": 4832, "first": 0,
                   "voff": 0, "bytes": 19328, "hits": 900,
                   "fp": [0,1,2,3,4,5,6,7] } ]
}
~~~

- **unreadable > 0**：有 drawcall 的 IB 内容读不到（既没 map 也没抓到上传）。
  这时候 ibh 是 0，但仍然能靠 n / first / hits 区分。
- **fp**：索引缓冲前 8 个索引值。**这个字段的意义是不依赖哈希算法**——
  拿来和 PC 端 dump 直接比对内容。
- **attrs/binds**：这就是你要的"真实顶点布局"。之前转换工具报的
  "重复顶点流族" 就是因为没有这个 ground truth，只能猜。

## 判定实验（P0 唯一目标）

在 PC 上用 SRMI 打开同一个角色、同一个动作，dump 一次；
拿安卓 probe.json 里那个角色的 draw 记录，比对：

- ibh 相同 → 3dmigoto 生态的 mod ini 可以直接复用，转换器只做格式转换
- ibh 不同但 fp / n 相同 → 索引数据一致，只是哈希算法不同（换 hash 函数即可）
- fp 和 n 都不同 → 模型数据在安卓包里被重新打包/加密了，
  转换器必须加"模型重导出"，工作量翻倍

## 已知限制

- 只支持 arm64（abiFilters 只放了 arm64-v8a）
- 每个 drawcall 都过一把全局 mutex，会掉帧（探针可以接受，P1 要换无锁）
- vkCmdDrawIndexedIndirect / multiDrawIndirect 没抓，如果 unreadable 很高再补
- 哈希用 FNV-1a 64，**不是** 3Dmigoto 的算法；要严格对齐得把
  hash_index_buffer 换成 3dmigoto 的实现（源码在 DirectX11/ 里）
- 只 dump，不改任何东西。P1 才是替换

## 下一步

P1：按 ibh 命中 → 在 vkCmdBindVertexBuffers 处把 VB 换成自定义 .buf。
  需要先在 probe.json 里找到目标 draw 的 pipeline，再看它 attrs 的
  loc/binding/offset，按这个布局重排 .buf。
