# 崩铁安卓mod — 在安卓上跑 PC 版 3Dmigoto / SRMI 模型 mod

让 PC 端的**崩坏:星穹铁道**模型 mod（`.ini` + `.ib` + `.buf` + `.dds`）在**安卓版**上生效。

一个 LSPosed 模块 + GLES 层 hook。不需要重打包游戏，不需要改系统分区。

> 当前状态：**模型替换已跑通**。索引缓冲、顶点位置、UV、贴图都能替换，角色能跟随游戏动作。仍在打磨贴图与多部件匹配。

---

## 一、为什么不能直接搬 3Dmigoto

3Dmigoto 是 DX11 专属的 `d3d11.dll` 代理。安卓上没有 DX，游戏跑的是 **GLES**（不是 Vulkan）。

米哈游还把 il2cpp 这条路堵死了：`il2cpp_get_api_table` 是 4 字节跳转进混淆派发器，调用即 SIGSEGV；4.2GB 内存里扫不到 metadata 明文。

所以整个项目走**图形层**：hook `libGLESv2.so`，在 drawcall 前后做手脚。

## 二、已验证跑通的链路

| 能力 | 状态 | 说明 |
|---|---|---|
| LSPosed 注入 | ✅ | 从模块 APK 解出 `.so`，绕过 Android 14+ 可写目录 dlopen 限制 |
| GLES 三路 hook | ✅ | 直接 DobbyHook 导出函数 + `eglGetProcAddress` + `dlsym` |
| IB 哈希 | ✅ | 与 PC 端 3Dmigoto 算法完全一致 |
| 索引缓冲替换 | ✅ | 换拓扑 / 换衣服形状 |
| 顶点位置替换 | ✅ | 含坐标系对齐与缩放 |
| 顶点跟随动作 | ✅ | 最近邻位移传递，解决 CPU 蒙皮问题 |
| UV 替换 | ✅ | 修正贴图采样 |
| 贴图替换 | ✅ | BC7 DDS 解码 → RGBA8，draw 时顶替图集 |
| 配置热重载 | ✅ | 改 `patch.txt` 不用重启游戏 |

实测：三月七的 mod 在安卓上能换成目标模型，走动、攻击、放技能时身体跟随。

## 三、几个关键发现

### 1. 哈希算法（能不能复用 PC mod 的分水岭）

3Dmigoto 的 buffer hash：

~~~c
hash = crc32c(0, data, ByteWidth);
hash = crc32c(hash, &D3D11_BUFFER_DESC, 24);
~~~

- 算法：**CRC32C（Castagnoli）**，多项式 `0x82F63B78`
- IB 用 `BindFlags = 0x2`，VB 用 `0x1`，`Usage = 0`，其余全 0
- 源码位置：`bo3b/3Dmigoto` 的 `DirectX11/HackerDevice.cpp:2144`

**验证结果**：三月七身体 IB 算出 `5212ce68`，与 mod 的 `March7thBodyIB` 完全一致；四个 `match_first_index`（0 / 30828 / 41466 / 53751）全部对上。说明**安卓和 PC 的索引缓冲逐字节相同**，PC mod 的 IB 可以直接用。

### 2. 崩铁安卓是 CPU 蒙皮

安卓这边**没有骨骼索引/权重顶点属性**。主渲染 pass 只有：

| 属性 | 格式 | 偏移 |
|---|---|---|
| 位置 | 3 × float | 0 |
| 法线 | 4 × s8 | 12 |
| 切线 | 4 × s8 | 16 |

另一个 buffer 放 `法线 4×u8 + UV0 + UV1`。

游戏每帧在 CPU 上算好蒙皮后的顶点位置，整块上传。所以：

- **静态替换顶点会"头身分离"**：身体停在快照那一帧，头继续动
- 正确做法是**位移传递**：记录首帧参考姿势，每帧算 `delta = 当前 - 参考`，把最近邻顶点的 delta 加到 mod 顶点上

### 3. 坐标系不一样

安卓的模型空间是**横躺**的：X 轴是身高方向（跨度 1.43），Z 轴是左右（0.61），Y 轴是前后（0.33）。

PC 的 mod 是 Y-up。所以需要轴置换 + 按身高缩放 + 包围盒对齐。对三月七身体，正确置换是 `perm=2`（mod 的 Y→X、Z→Y、X→Z）。

### 4. 属性状态必须运行时查询

自己缓存的 VAO 属性会串味（不同 pass 混在一起）。必须用 `glGetVertexAttribiv` / `glGetVertexAttribPointerv` 在 draw 时实时读。

## 四、patch.txt 命令

配置文件放在 `/sdcard/Android/data/<包名>/files/hsrprobe/patch.txt`，**按 mtime 热重载**。

~~~text
# 替换索引缓冲
ib <ib_hash> <file.ib> [first] [count] [u16|u32]

# 完整替换: 位置 + UV + 贴图，并跟随动作
vbstatic <ib_hash> <Position.buf> <perm> [Texcoord.buf] [Diffuse.dds]

# 只缩放顶点（调试）
vbscale <ib_hash> <attr> <fx> [fy] [fz]

# 只替换位置
vbpos   <ib_hash> <attr> <file> <srcStride> [srcOffset] [first]
vbposp  <ib_hash> <attr> <file> <srcStride> <perm>

# 诊断
dump     <ib_hash> <out> [first]       # dump 索引缓冲
dumpmesh <ib_hash> <out> [first]       # dump 顶点布局 + 原始字节
vbbox    <ib_hash> [first]             # 打印顶点包围盒
attrstat <ib_hash>                     # 打印每个属性的原始字节
iblist                                 # 列出所有缓存的索引缓冲
ibfind   <file.ib>                     # 在缓存里找这个 mod 的 IB
texstat  <ib_hash>                     # 打印各纹理单元的绑定与尺寸

# 跳过
skip / skipall / skipcnt <min> <max>
~~~

## 五、构建与使用

### 构建

推到 GitHub，Actions 自动跑（约 1.5 分钟），取 artifacts 里的 `probe-apk`。

本地构建需要 Android SDK + NDK 26.1 + CMake 3.22 + JDK 17：

~~~bash
keytool -genkeypair -keystore app/ci.jks -alias ci -keyalg RSA \
  -keysize 2048 -validity 10000 -storepass android -keypass android -dname "CN=ci"
gradle assembleRelease
~~~

### 使用

1. 安装 APK
2. LSPosed 里启用模块，作用域勾选星穹铁道
3. **强制停止游戏**再启动（模块 APK 更新后必须重启目标进程）
4. 日志：`logcat -s hsrprobe`
5. 紧急停用：在同目录建空文件 `DISABLE`

包名：国服 `com.miHoYo.hkrpg` / B服 `com.miHoYo.hkrpg.bilibili` / 国际服 `com.HoYoverse.hkrpgoversea`

## 六、目录结构

~~~
app/                          LSPosed 模块 (Kotlin)
  src/main/java/com/hsrprobe/
    ProbeEntry.kt             注入点
    NativeLoader.kt           解 so + System.load
native/
  src/gles_probe.cpp          核心: GLES hook + 替换逻辑
  src/vk_probe.cpp            入口 + Vulkan hook (备用)
  src/il2cpp_probe.cpp        il2cpp 探针 (已禁用, 会崩)
  third_party/Dobby/          inline hook
  third_party/bcdec.h         BC1/3/7 解码
tools/                        辅助脚本
docs/re-findings.md           逆向勘察记录
.github/workflows/build.yml   CI
~~~

## 七、参考项目

这个项目的每一个关键决策都能追溯到某个开源实现。

### 核心参考

| 项目 | 参考了什么 |
|---|---|
| [bo3b/3Dmigoto](https://github.com/bo3b/3Dmigoto) | **最重要的一个**。buffer hash 算法（`HackerDevice.cpp:2144`）、`match_first_index` 语义、ini 的 `TextureOverride` / `Resource` 结构、`draw` 命令。整个 mod 生态的规范就是它定的 |
| [SpectrumQT/XXMI-Launcher](https://github.com/SpectrumQT/XXMI-Launcher) | 米哈游系游戏 mod 加载器的组织方式：mod 目录结构、多游戏适配层、ini 分发策略 |
| [LSPosed/LSPosed](https://github.com/LSPosed/LSPosed) | 注入框架。`IXposedHookLoadPackage` / `IXposedHookZygoteInit`，`StartupParam.modulePath` 拿模块 APK 路径 |
| [Yuanqi314/GIBox](https://github.com/Yuanqi314/GIBox) | **原神安卓 mod 的完整实现**，同引擎家族。参考了整体思路、`model-format.md` 里的安卓顶点布局分析，以及"安卓侧没有骨骼属性"这个结论 |

### 基础设施

| 项目 | 用途 |
|---|---|
| [jmpews/Dobby](https://github.com/jmpews/Dobby) | inline hook 框架。`DobbyHook` 挂 `libGLESv2.so` 的导出函数，这是三路 hook 里最可靠的一路 |
| [iOrange/bcdec](https://github.com/iOrange/bcdec) | BC1/BC2/BC3/BC4/BC5/BC6H/BC7 解码。Mali GPU 不支持 BCn，mod 的 DDS 必须解码成 RGBA8 再上传 |
| [alorma/Compose-Settings](https://github.com/alorma/Compose-Settings) | 转换器 GUI 的参考（尚未接入），用来做 mod 管理界面 |

### 逆向得出的事实（非项目依赖）

- 崩铁安卓使用 **Unity 2019.4.34f1 / IL2CPP / metadata v24.1**
- 图形后端：GLES（`libGLES_meow.so` + `mt6895/libGLES_mali.so`）
- `libunity.so` 只 import `egl*`，所有 `gl*` 靠运行时解析 —— 所以只 hook `eglGetProcAddress` 不够，Unity 走 `dlsym`

## 八、路线图

- [x] LSPosed 注入 + GLES hook
- [x] 破解 IB 哈希，与 PC 对齐
- [x] 索引缓冲替换
- [x] 顶点位置替换（含坐标系对齐）
- [x] 位移传递，跟随动作
- [x] UV 替换
- [x] 贴图替换（BC7 → RGBA8）
- [ ] 多部件匹配（头发 / 头部 / 配饰的独立 IB hash）
- [ ] ini 解析器，直接吃 PC mod 目录
- [ ] 转换器 GUI（Compose-Settings）
- [ ] 打包成普通用户可安装的 APK

## 九、已知限制

- 只支持 arm64
- 每次替换都要 `glMapBufferRange` 读回顶点，有性能开销（当前只在命中的 draw 上做）
- 图集贴图（2112×2112）里的 diffuse 是多角色共享的，直接替换会影响同图集的其他 draw（目前靠 draw 前后绑/解绑规避）
- 多部件 mod（头发、头饰）还没自动匹配，需要手工指定 hash
- 头身比例在 A-pose mod 上会有偏差（mod 静止姿势与游戏待机姿势不同）
