# 崩铁安卓mod — 在安卓上跑 PC 版 3Dmigoto / SRMI 模型 mod

让 PC 端的**崩坏:星穹铁道**模型 mod（`.ini` + `.ib` + `.buf` + `.dds`）在**安卓版**上生效。

一个 LSPosed 模块 + GLES 层 hook。不需要重打包游戏，不需要改系统分区。

> 当前状态（2026-11-04）：**底层四项替换全部在实机生效**（索引 / 顶点 / UV / 贴图），角色能跟随游戏动作。
> 卡在**几何配准**：mod 是 A-pose、安卓是 CPU 蒙皮的待机姿势；轴向映射之前判反了，导致衣服呈锥形、贴图呈碎片状。
> 详见第 3.5 节「当前卡点与进度」。

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

安卓的模型空间是**横躺**的。实测（三月七 body，15185 顶点）：

| | mod（PC / 3Dmigoto） | 安卓（GLES） |
|---|---|---|
| 身高轴 | **Y**，0.000 – 1.456（跨度 1.455） | **X**，-0.497 – 0.908（跨度 1.405） |
| 左右轴 | **X**，-0.481 – 0.481（跨度 0.962，A-pose 手臂张开） | **Y**，-0.198 – 0.446（跨度 0.643） |
| 前后轴 | **Z**，-0.309 – 0.192（跨度 0.501） | **Z**，-0.226 – 0.193（跨度 0.418） |

所以正确置换是 **`perm=1`**：mod 的 `Y→X`、`X→Y`、`Z→Z`。

> 早期版本写的是 `perm=2`（把左右和前后对调），这是错的 —— 它把 mod 张开的 0.962 塞进只有 0.418 的前后轴，网格在前后方向炸开，正面看就是「锥形长裙」。

另外注意两边**最低点定义不同**：mod 的最低点是脚（层 0 横截面 0.23），安卓的最低点是裙摆（层 0 横截面 0.585）。包围盒对齐时不能简单拿 `rmin` 对 `mmin`。

### 3.5 当前卡点与进度（2026-11-06）

#### 已确认的实机事实

| 项 | 结论 |
|---|---|
| mod 顶点数 | 14754（`Position.buf` 40 字节/顶点 = POSITION + NORMAL + TANGENT） |
| 安卓顶点数 | 15185（stride 20 = 3×float 位置 + 4×s8 法线 + 4×s8 切线） |
| mod 索引 | 92694 个 u16，全局最大索引 14753（用满全部顶点） |
| 安卓 4 段 draw | first=0/30828/41466/53751，count=30828/10638/12285/1674，共用同一 IB |
| IB 替换 | 整体覆盖正确，4 段索引在 mod IB 里都落在合法范围 |
| UV 替换 | 生效（UV 全填 0.5 时衣服变纯黑，说明写入与采样都通） |
| 贴图替换 | 生效（换纯红贴图衣服变红） |

#### 三个卡点

1. **轴向映射判反**（最大 bug）
   已改成 `perm=1`，见上一节。这一条就能解释「锥形长裙」。

2. **A-pose → 待机姿势**
   mod 的 `Position.buf` 是 A-pose（手臂向下斜张约 45°），PC 端靠 GPU 蒙皮实时驱动；安卓是 CPU 蒙皮，骨骼矩阵不外露，没法复用 mod 的 blend 权重。
   现在的「位移传递」只能平移，处理不了手臂下垂这种大角度旋转。
   已实现的方案：**`mode=5` 逐顶点局部刚体拟合**（shape matching）——
   对每个 mod 顶点取 K=8 个 mod 空间最近邻，用它们各自的安卓最近邻构造对应点对，做 Horn 四元数拟合，把 A-pose 一次性掰成参考姿势，之后仍然走位移传递。
   实测日志：`vbstatic pose fit: N/14754 verts posed`。
   要点：权重按 mod 空间距离衰减（`1/(1+400d²)`），并做了 ±10 的限幅，避免个别离群顶点被拉飞。

3. **贴图「纸扎碎片」**
   红蓝条纹同时出现，说明**只有一部分材质层被换掉了**。身体这个 IB 有 4 段 draw，目前只对 `5212ce68` 整体生效，其余材质层仍采样原图集。
   下一步要把 4 段分别绑到 mod 的对应贴图，或者确认 mod 的 diffuse 是否覆盖全部段。

#### 头部与头发

- mod 里**只有 `March7thHeadADiffuse.dds` 贴图，没有 Head 的几何**，所以头部只能换贴图，不能换形状。
- **2026-11-06 更正**：头发 IB hash `84ba7549` 在安卓侧实测命中，和 PC mod ini 里的值**完全一致**，之前「运行时重建、hash 每次不同」的结论是错的。
  离线用探针同款 CRC32C + BUFFER_DESC 复算 dump 出的 `ib_hair.bin`（27408 字节）得 `84ba7549`，身体 `bb.bin`（110850 字节）得 `5212ce68`，两端对得上。
  之前算出 `3cee2afc` / `e2f289a1` 是离线脚本的 CRC 实现和探针不一致导致的，数据本身没问题。

| | 安卓 | mod |
|---|---|---|
| 头发 IB | 13704 索引（27408 字节） | 58206 索引（116412 字节） |
| 头发顶点 | ≤ 4139 | 9550（stride 40） |
| 身体 IB | 55425 索引（110850 字节） | 92694 索引 |
| 身体顶点 | 15185（stride 20） | 14754（stride 40） |

安卓头发是低模，mod 是高模，顶点数差 2.3 倍。`vbstatic` 原来只填 `min(原顶点, mod顶点)`，直接套会有一半索引指向垃圾数据，模型炸开。
现在改成：新顶点 buffer 按两者较大值开，多余部分按模循环铺满原属性，再覆盖成 mod 的位置；UV 流同样处理。

实机 `attrstat 84ba7549` 拿到的头发顶点布局（stride 20）：

| attr | size | type | stride | ptr | 含义 |
|---|---|---|---|---|---|
| a0 | 3 | `0x1406` GL_FLOAT | 20 | 0 | 位置 |
| a1 | 4 | `0x1400` GL_BYTE | 20 | 12 | 骨骼索引/权重（与位置交错在同一 buffer） |
| a2 | 4 | `0x1401` GL_UNSIGNED_BYTE | 8 | 0 | 法线 + 切线 |
| a3 | 2 | **`0x140b` GL_HALF_FLOAT** | 8 | 4 | **UV，半精度浮点** |

三个坑：
1. 安卓的 UV 是 **half float**（`0x140b`），不是 float 也不是 u16（`0x1403`）。原来只认 `GL_FLOAT`，UV 整段被跳过 → 位置换了但贴图采样用原 UV，头发变成红白杂乱色块。
   注意别把 `0x140b` 看成 `GL_UNSIGNED_SHORT`，两者差 8。
2. mod 的 `Texcoord.buf` stride **因角色而异**：身体 20、头发 8（`76400 / 9550 = 8`）。原来写死 20，头发解析全错。
   现在按 `blend.size() / modVerts` 反推，整除且 ≥8 才采用，否则回退 20。
3. **位置和骨骼权重交错在同一个 buffer 里**（stride 20：前 12 字节位置、后 8 字节权重）。
   只替换位置流会让两个属性指向不同 buffer，权重和位置分家。现在同一个 buffer 上的所有属性一起换。
   又因为 mod 有 9550 顶点、安卓只有 4139，多出来的顶点没有权重，按最近邻映射从原版顶点搬过来。
4. **法线和 UV 也交错在同一个 buffer 里**（stride 8：偏移 0 是法线 u8×4，偏移 4 是 UV half×2）。
   只换 UV 会让法线停在 4139 个顶点上，索引越界后光照全乱，头发发白、发薄。
   现在从 mod 的 `Position.buf` 生成法线（布局是 `位置(12) + 法线(12) + 切线(12) + 4`，法线在 offset 12）。
5. **mod 的 IB 是 32 位索引**（`DXGI_FORMAT_R32_UINT`）。`March7thHairA.ib` 116412 字节 = 29103 个 u32，
   不是 58206 个 u16。按 u16 解析等于把每个索引拆成高低字节，三角形全乱连，模型炸成放射状尖刺。
   patch.txt 要写 `ib 84ba7549 March7thHairA.ib 0 29103 u32`。

### 3.6 角色全局去虚化（2026-11-06）

相机贴近角色 / 视角往下压时，角色会 dither 渐隐成空气。目标是做成可开关的全局功能。

#### 定位过程

1. 先怀疑景深 → 游戏 prefs 里没有 DOF 开关，排除。
2. dump 全部 `glShaderSource` → 242 段全是 **compute shader**（138 个），渲染用的 vertex/fragment 是预编译二进制，走不到这个入口，改不了源码。
3. 改成 dump **uniform 名字**：hook `glGetUniformLocation` 建 `(program,location) → name` 表，再在 `glUniform*` 里反查。
4. 三轮试探：
   - 第一轮 `_OneMinusOpacityDitherScale` → 命中，但值**恒为 0.0000**，不是变量。
   - 第二轮 `_AlphaOutside` → 命中，值**恒为 1.0000**，也不是变量。
   - 第三轮加 `glUniform1fv` / `glUniform4fv`（数组版），同时记录每次设置的值 → **命中**。

#### 结论

| uniform | program / loc | 值 |
|---|---|---|
| `_OneMinusOpacityDitherScale` | 824/5、835/5、953/4 | 恒 0.0000 |
| `_AlphaOutside` | 1016/13 | 恒 1.0000 |
| `_AlphaIntensity` | 1278/48 | 恒 2.0000 |
| `_AlphaRange` | 1278/49 | 恒 1.0000 |
| **`_AlphaInside`** | **1019/14** | **1.0000 → 0.1180（拉近时）** |

真正随相机距离变化的是 **`_AlphaInside`**，而且它走的是 `glUniform1fv`（count=1 的数组版），
不是 `glUniform1f` —— 只 hook 标量版会完全看不到。

#### 实现

- `patch.txt` 里加 `nofade on|off`（热重载，改完 1 秒内生效）。
- 模块 App 里加「角色全局去虚化」开关，存 `shared_prefs/hsrprobe.xml` 的 `nofade`。
  游戏进程用 `XSharedPreferences` 轮询（LSPosed 代读别的应用私有目录，不受 SELinux 限制），
  变化时经 JNI `nativeSetNoFade` 推给探针。
- 两个来源取或：`g_noFadeApp || g_noFadePatch`。
- 生效时把 `_AlphaInside` 钉在 `1.0`（`glUniform1f` / `4f` / `1fv` / `4fv` 四个入口都拦）。
  因为是逐帧逐 draw 拦，**传送地图、切场景也一样生效**，不需要重启游戏。

#### 2026-11-04 傍晚：找到骨骼矩阵的入口

`libunity.so` 没有被完全 strip（431 个导出函数），里面有 Unity 的 CPU 蒙皮实现：

~~~
_s_SkinVertices_NEON
_s_SkinVertices2Bones_NEON
_s_SkinVertices4Bones_NEON
_s_SkinVertices4Bones_Tangents_NEON  ...
~~~

反汇编 `_s_SkinVertices4Bones_NEON` 得到的签名：

| 寄存器 | 含义 |
|---|---|
| x0 | 骨骼矩阵数组，每个 64 字节（4x4） |
| x1 | 输入顶点，24 字节/顶点 |
| x2 | 输入结束指针 |
| x3 | 骨骼索引 + 权重，24 字节/顶点 |
| x4 | 输出顶点 |

**hook 它就能直接拿到骨骼矩阵**，不需要 il2cpp、不需要 dump metadata、不需要破 MHY 加密。
有了骨骼矩阵 + mod 自带的 `March7thBodyBlend.buf`，就能做真正的 GPU 蒙皮，A-pose 问题从根上解决。

探针会挂上全部 8 个带骨骼的变体（2Bones / 4Bones × 普通 / NoNormals / Tangents / Tangents1），
第一次被调用时 dump `skin<N>_bones.bin` / `skin<N>_in.bin` / `skin<N>_bd.bin` / `skin<N>_out.bin`，
用来离线确认骨骼数量、索引顺序和真实步长。

> 踩坑记录：先只挂了 `4Bones_NEON` 和 `2Bones_NEON`，实机跑了 130 万次 draw 都没触发 —— 崩铁用的是别的变体，必须全挂。
> 另外写文件不能用 `fopen`：蒙皮函数在 Unity 渲染线程里调用，`fopen` 会触发 fdsan 的 fd 所有权检查直接 SIGABRT，得用 `open/write/close`。

#### metadata 的实际情况

运行时内存映射显示 metadata 是**明文映射在 sdcard 上**的：

~~~
7550400000-75568ae000 r--s  .../files/il2cpp/Metadata/global-metadata.dat
~~~

105 MB，文件头是 `4d 48 59 00` = `MHY\0`，不是标准的 `AF1BB1FA` —— 米哈游自定义的加密格式。
`libil2cpp.so` 动态符号表里只有 3 个 `il2cpp_*`，全是 4 字节跳转桩，GIBox 依赖的 `il2cpp_domain_get` 等一个都没有。
内存里能搜到明文的 `SkinnedMeshRenderer` / `UnityEngine` 字符串，说明字符串表没加密，但结构体仍是加密的。

#### 走不通的路（记录一下，避免重复踩）

- **il2cpp dump**：`libil2cpp.so` 实测 **459 MB**（正常 il2cpp 50–100 MB），重度加固。`il2cpp_get_api_table` 是 4 字节跳进混淆派发器，调用即 SIGSEGV；4.2 GB 内存里扫不到 `AF1BB1FA` metadata 明文。要拆得先定位壳的解密 stub，成本远高于图形层。
- **GIBox 路线**：原神没加固，所以 GIBox 能直接吃 il2cpp；崩铁不行。

---

### 4. 属性状态必须运行时查询

自己缓存的 VAO 属性会串味（不同 pass 混在一起）。必须用 `glGetVertexAttribiv` / `glGetVertexAttribPointerv` 在 draw 时实时读。

## 四、patch.txt 命令

配置文件放在 `/sdcard/Android/data/<包名>/files/hsrprobe/patch.txt`，**按 mtime 热重载**。

~~~text
# 替换索引缓冲
ib <ib_hash> <file.ib> [first] [count] [u16|u32]

# 完整替换: 位置 + UV + 贴图，并跟随动作
vbstatic <ib_hash> <Position.buf> <perm> [Texcoord.buf] [Diffuse.dds|red] [flipV] [mode]
#   perm: 1 = Y->X,X->Y,Z->Z   2 = Y->X,Z->Y,X->Z (三月七身体用 2)
#   mode: 1 = 位移传递(默认)  2 = 只跟随整体平移  3 = 完全吸附到安卓
#         4 = 65% 吸附 + 35% 原始形状   5 = 局部刚体姿势匹配 (把 A-pose 掰成待机姿势)

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

# 全局去虚化（角色贴近相机不再淡出）
nofade on|off
~~~

## 五、构建与使用

### 构建

推到 GitHub，Actions 自动跑（约 1.5 分钟）。

**产物是一个裸 `.apk` 文件，不是压缩包。** GitHub 的 `upload-artifact` 无论传什么都会自动打 zip，所以 CI 改成把 APK 直接推到 `dist` 分支：

~~~
https://github.com/Pape7rlane/private/raw/dist/HSRModProbe-probe<构建号>.apk
~~~

每次构建覆盖 `dist` 分支，只保留最新一个（文件名带构建号，方便对版本）。Actions 运行页面的 Summary 里也会打印这个直链。

**版本号和构建号对齐**：CI 把 `github.run_number` 通过 `-PbuildNo` 传给 gradle，
`versionCode = buildNo`、`versionName = "0.1.$buildNo"`。所以 `HSRModProbe-probe113.apk`
装上去就是 `0.1.113`，模块界面顶部显示的就是这个值。

命令行拉取：

~~~bash
gh api repos/Pape7rlane/private/contents/HSRModProbe-probe<构建号>.apk?ref=dist \
  -H 'Accept: application/vnd.github.raw' > HSRModProbe.apk
~~~

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
- [x] 顶点云离线分析（Python 点云渲染 + 横截面曲线，用来定轴向）
- [ ] **修正轴向映射 perm=1**（当前最大 bug）
- [x] A-pose → 参考姿势的局部刚体拟合（`mode=5`，Horn 四元数 + KNN，待实机验证）
- [ ] 4 段 draw 的贴图分层绑定（消除红蓝条纹）
- [x] 多部件匹配（头发 `84ba7549` 已定位，两端 hash 一致）
- [x] 高模换低模的顶点数扩展（`vbstatic` 支持 newVcount > 原顶点）
- [ ] 只替换头发 + 脸的实机验证
- [ ] ini 解析器，直接吃 PC mod 目录
- [ ] 转换器 GUI（Compose-Settings）
- [ ] 打包成普通用户可安装的 APK

## 九、已知限制

- 只支持 arm64
- 每次替换都要 `glMapBufferRange` 读回顶点，有性能开销（当前只在命中的 draw 上做）
- 图集贴图（2112×2112）里的 diffuse 是多角色共享的，直接替换会影响同图集的其他 draw（目前靠 draw 前后绑/解绑规避）
- 多部件 mod（头发、头饰）还没自动匹配，需要手工指定 hash
- 头身比例在 A-pose mod 上会有偏差（mod 静止姿势与游戏待机姿势不同），这是当前最影响观感的问题
- 轴向映射、缩放基准目前依赖手工指定，没有自动配准
