// gles_probe.cpp — HSR Android GLES probe (P1)
// 目标: 抓到每个 drawcall 的索引缓冲内容哈希 = 3Dmigoto/SRMI ini 里的 hash 键.
// 三条路径都堵: libGLESv2 导出函数直接 hook + eglGetProcAddress + dlsym.
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cerrno>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <thread>
#include <link.h>
#include <string>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <GLES3/gl3.h>

#include "dobby.h"
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

#ifndef GL_TEXTURE_WIDTH
#define GL_TEXTURE_WIDTH            0x1000
#define GL_TEXTURE_HEIGHT           0x1001
#define GL_TEXTURE_INTERNAL_FORMAT  0x1003
#endif

#define TAG "hsrprobe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

typedef void* (*PFNEGLGETPROCADDRESSPROC)(const char*);
typedef void* (*PFNDLSYM)(void*, const char*);

PFNEGLGETPROCADDRESSPROC o_eglGetProcAddress = nullptr;
PFNDLSYM                o_dlsym            = nullptr;

char g_dir[512] = {0};

struct TexOv { int w = 0, h = 0; std::vector<uint8_t> rgba; };
bool load_dds(const char* path, TexOv& out);

// ---------------------------------------------------------------- 状态

struct Attr { bool has=false; GLint size=0; GLenum type=0; GLsizei stride=0; GLuint buf=0; const void* ptr=nullptr; GLboolean norm=GL_FALSE; };
struct VaoState { GLuint elem = 0; Attr attr[16]; };
struct BufInfo {
    GLsizeiptr size = 0;
    uint32_t   h_ib = 0;   // 3Dmigoto IB 哈希 (crc32c + BUFFER_DESC bind=0x2)
    uint32_t   h_vb = 0;   // 3Dmigoto VB 哈希 (crc32c + BUFFER_DESC bind=0x1)
    GLenum     target = 0;
    std::vector<uint8_t> data;   // 索引缓冲副本, 用于按 draw 子段哈希
};

// 索引缓冲副本预算
size_t g_dataBytes = 0;
// 只缓存小缓冲: 角色网格一定在里面, 环境大缓冲直接跳过
const size_t kDataBudget  = 384u << 20;
const size_t kDataPerBuf  = 4u << 20;

std::unordered_map<GLuint, BufInfo>  g_bufs;
std::unordered_map<GLuint, VaoState> g_vaos;

GLuint g_curVao = 0, g_curArray = 0, g_curProg = 0;

std::atomic<uint64_t> c_egl{0}, c_dlsym{0}, c_draw{0}, c_bindBuf{0}, c_basev{0},
                      c_useProg{0}, c_attrib{0}, c_bufData{0}, c_vao{0}, c_genBuf{0}, c_skipped{0},
                      c_overridden{0}, c_subData{0};

uint64_t g_unique = 0;
std::unordered_set<uint64_t> g_seen;

FILE* g_fp = nullptr;
FILE* g_fpNames = nullptr;
bool  g_fileTried = false;

bool open_out() {
    if (g_fileTried) return g_fp != nullptr;
    g_fileTried = true;
    LOGE("out dir = '%s'", g_dir);
    if (!g_dir[0]) return false;
    std::string a = std::string(g_dir) + "/gles.tsv";
    g_fp = fopen(a.c_str(), "w");
    if (!g_fp) LOGE("fopen %s failed errno=%d", a.c_str(), errno);
    else { fprintf(g_fp, "# vao\telem_buf\tib_fnv1\tib_fnv1a\tidx_count\tidx_type\tmode\tprogram\tkind\tvtx_layout\n"); fflush(g_fp); }
    std::string b = std::string(g_dir) + "/eglnames.txt";
    g_fpNames = fopen(b.c_str(), "w");
    if (!g_fpNames) LOGE("fopen %s failed errno=%d", b.c_str(), errno);
    return g_fp != nullptr;
}

void log_name(const char* who, const char* name) {
    open_out();
    if (!g_fpNames || !name) return;
    fprintf(g_fpNames, "%s\t%s\n", who, name);
    fflush(g_fpNames);
}

// ---- CRC32C (Castagnoli), 3Dmigoto 用的就是它 ----
uint32_t g_crcTab[256];
bool     g_crcInit = false;

void crc_init() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? ((c >> 1) ^ 0x82F63B78u) : (c >> 1);
        g_crcTab[i] = c;
    }
    g_crcInit = true;
}

uint32_t crc32c(uint32_t crc, const uint8_t* p, size_t n) {
    if (!g_crcInit) crc_init();
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) crc = g_crcTab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// 3Dmigoto: hash = crc32c(crc32c(0, data, ByteWidth), &D3D11_BUFFER_DESC, 24)
uint32_t buffer_hash(const uint8_t* data, size_t size, uint32_t bind) {
    uint32_t base = crc32c(0, data, size);
    uint32_t desc[6] = {(uint32_t)size, 0u, bind, 0u, 0u, 0u};
    return crc32c(base, (const uint8_t*)desc, sizeof(desc));
}


GLuint cur_elem() {
    auto it = g_vaos.find(g_curVao);
    return it == g_vaos.end() ? 0 : it->second.elem;
}

// ---------------------------------------------------------------- 实现指针

decltype(&glBindBuffer)      o_glBindBuffer      = nullptr;
decltype(&glBindVertexArray) o_glBindVertexArray = nullptr;
decltype(&glBufferData)      o_glBufferData      = nullptr;
decltype(&glBufferSubData)   o_glBufferSubData   = nullptr;
decltype(&glGenBuffers)      o_glGenBuffers      = nullptr;
decltype(&glDeleteBuffers)   o_glDeleteBuffers   = nullptr;
decltype(&glUseProgram)      o_glUseProgram      = nullptr;
decltype(&glShaderSource)    o_glShaderSource    = nullptr;
decltype(&glGetUniformLocation) o_glGetUniformLocation = nullptr;
decltype(&glVertexAttribPointer) o_glVertexAttribPointer = nullptr;
decltype(&glDrawElements)    o_glDrawElements    = nullptr;
typedef void (*PFN_DRAWEBV)(GLenum, GLsizei, GLenum, const void*, GLint);
typedef void (*PFN_DRAWEBV_I)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint);
PFN_DRAWEBV   o_glDrawElementsBaseVertex = nullptr;
PFN_DRAWEBV_I o_glDrawElementsInstancedBaseVertex = nullptr;
decltype(&glDrawElementsInstanced) o_glDrawElementsInstanced = nullptr;
decltype(&glDrawRangeElements) o_glDrawRangeElements = nullptr;
decltype(&glMapBufferRange)  o_glMapBufferRange  = nullptr;
decltype(&glUnmapBuffer)     o_glUnmapBuffer     = nullptr;

void* gl_real(const char* name) {
    void* p = nullptr;
    if (o_eglGetProcAddress) p = o_eglGetProcAddress(name);
    if (!p && o_dlsym) p = o_dlsym(RTLD_DEFAULT, name);
    return p;
}

#define RESOLVE(slot, name)                                     \
    do {                                                        \
        if (!(slot)) (slot) = (decltype(slot))gl_real(name);    \
        if (!(slot)) { LOGE("no real %s", name); return; }      \
    } while (0)

void record_index(GLuint id, GLsizeiptr size, const void* data, GLenum target) {
    if (!id || size <= 0) return;
    BufInfo& b = g_bufs[id];
    b.size = size; b.target = target;   // 声明大小, 无论是否缓存数据
    if (!data) {
        // 先分配后填充: 留一块零缓冲等 glBufferSubData / glMapBufferRange 补
        g_dataBytes -= b.data.size();
        if ((size_t)size <= kDataPerBuf && g_dataBytes + (size_t)size <= kDataBudget)
            b.data.assign((size_t)size, 0);
        else
            b.data.clear();
        g_dataBytes += b.data.size();
        return;
    }
    const uint8_t* p = (const uint8_t*)data;
    b.h_ib = buffer_hash(p, (size_t)size, 2u);
    b.h_vb = buffer_hash(p, (size_t)size, 1u);

    // 索引缓冲用于哈希, 顶点缓冲用于导出; 超过 1MB 的一律不存
    if ((size_t)size > kDataPerBuf || g_dataBytes + (size_t)size > kDataBudget) {
        b.data.clear();
        return;
    }
    g_dataBytes -= b.data.size();
    b.data.assign(p, p + (size_t)size);
    g_dataBytes += b.data.size();
}

// ---------------------------------------------------------------- hooks

void glBindBuffer_hook(GLenum target, GLuint buffer) {
    RESOLVE(o_glBindBuffer, "glBindBuffer");
    c_bindBuf++;
    if (target == GL_ELEMENT_ARRAY_BUFFER) g_vaos[g_curVao].elem = buffer;
    else if (target == GL_ARRAY_BUFFER)    g_curArray = buffer;
    o_glBindBuffer(target, buffer);
}

void glBindVertexArray_hook(GLuint array) {
    RESOLVE(o_glBindVertexArray, "glBindVertexArray");
    c_vao++;
    g_curVao = array;
    o_glBindVertexArray(array);
}

void glGenBuffers_hook(GLsizei n, GLuint* buffers) {
    RESOLVE(o_glGenBuffers, "glGenBuffers");
    c_genBuf++;
    o_glGenBuffers(n, buffers);
}

void glDeleteBuffers_hook(GLsizei n, const GLuint* buffers) {
    RESOLVE(o_glDeleteBuffers, "glDeleteBuffers");
    for (GLsizei i = 0; i < n; i++) g_bufs.erase(buffers[i]);
    o_glDeleteBuffers(n, buffers);
}

void glBufferData_hook(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    RESOLVE(o_glBufferData, "glBufferData");
    c_bufData++;
    if (size > 0) {
        GLuint id = (target == GL_ELEMENT_ARRAY_BUFFER) ? cur_elem() : g_curArray;
        record_index(id, size, data, target);
    }
    o_glBufferData(target, size, data, usage);
}

void glBufferSubData_hook(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    RESOLVE(o_glBufferSubData, "glBufferSubData");
    c_subData++;
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        if (data && size > 0) record_index(cur_elem(), size, data, target);
    } else if (g_curArray && size > 0) {
        auto& b = g_bufs[g_curArray];
        if (b.size < offset + size) b.size = offset + size;
        if (data && !b.data.empty() && (size_t)(offset + size) <= b.data.size())
            memcpy(b.data.data() + offset, data, (size_t)size);
    }
    o_glBufferSubData(target, offset, size, data);
}

void glUseProgram_hook(GLuint program) {
    RESOLVE(o_glUseProgram, "glUseProgram");
    c_useProg++;
    g_curProg = program;
    o_glUseProgram(program);
}

// ---- shader 源码 dump: 用来定位景深/后处理的 program ----
std::unordered_set<GLuint> g_shaderDumped;
std::atomic<int> g_shaderCount{0};

void glShaderSource_hook(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths) {
    RESOLVE(o_glShaderSource, "glShaderSource");
    if (strings && count > 0 && g_shaderDumped.find(shader) == g_shaderDumped.end()) {
        std::string src;
        for (GLsizei i = 0; i < count; i++) {
            if (!strings[i]) continue;
            if (lengths && lengths[i] >= 0) src.append(strings[i], (size_t)lengths[i]);
            else src.append(strings[i]);
        }
        // 只收前 400 个, 单个不超过 256KB, 避免撑爆存储
        if (!src.empty() && src.size() <= (256u << 10) && g_shaderCount.load() < 400) {
            g_shaderDumped.insert(shader);
            int idx = g_shaderCount.fetch_add(1);
            std::string path = std::string(g_dir) + "/shaders.txt";
            FILE* f = fopen(path.c_str(), "a");
            if (f) {
                fprintf(f, "\n===== SHADER %u idx=%d len=%zu =====\n", shader, idx, src.size());
                fwrite(src.data(), 1, src.size(), f);
                fprintf(f, "\n===== END %u =====\n", shader);
                fclose(f);
                LOGI("shader dump %u idx=%d len=%zu", shader, idx, src.size());
            }
        }
    }
    o_glShaderSource(shader, count, strings, lengths);
}

// ---- uniform 名字 dump: 渲染 shader 是预编译二进制, 只能靠 uniform 名字定位渐隐参数 ----
std::unordered_set<std::string> g_uniDumped;

GLint glGetUniformLocation_hook(GLuint program, const GLchar* name) {
    // RESOLVE 里的 return; 在非 void 函数里不合法, 这里手写解析
    if (!o_glGetUniformLocation)
        o_glGetUniformLocation = (decltype(o_glGetUniformLocation))gl_real("glGetUniformLocation");
    if (!o_glGetUniformLocation) { LOGE("no real glGetUniformLocation"); return -1; }
    GLint loc = o_glGetUniformLocation(program, name);
    if (name && loc >= 0 && g_uniDumped.size() < 8000) {
        std::string n(name);
        if (g_uniDumped.insert(n).second) {
            std::string path = std::string(g_dir) + "/uniforms.txt";
            FILE* f = fopen(path.c_str(), "a");
            if (f) { fprintf(f, "%s\t%d\t%u\n", n.c_str(), loc, program); fclose(f); }
        }
    }
    return loc;
}

void glVertexAttribPointer_hook(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                GLsizei stride, const void* pointer) {
    RESOLVE(o_glVertexAttribPointer, "glVertexAttribPointer");
    c_attrib++;
    if (index < 16) {
        Attr& a = g_vaos[g_curVao].attr[index];
        a.has = true; a.size = size; a.type = type; a.stride = stride; a.buf = g_curArray;
        a.ptr = pointer; a.norm = normalized;
    }
    o_glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

struct MapState { void* ptr = nullptr; GLuint buf = 0; GLintptr off = 0; GLsizeiptr len = 0; };
MapState g_mapArr, g_mapElem;

void* glMapBufferRange_hook(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    if (!o_glMapBufferRange) o_glMapBufferRange = (decltype(&glMapBufferRange))gl_real("glMapBufferRange");
    if (!o_glMapBufferRange) return nullptr;
    void* p = o_glMapBufferRange(target, offset, length, access);
    if (p) {
        MapState& m = (target == GL_ELEMENT_ARRAY_BUFFER) ? g_mapElem : g_mapArr;
        m.ptr = p;
        m.buf = (target == GL_ELEMENT_ARRAY_BUFFER) ? cur_elem() : g_curArray;
        m.off = offset;
        m.len = length;
    }
    return p;
}

GLboolean glUnmapBuffer_hook(GLenum target) {
    if (!o_glUnmapBuffer) o_glUnmapBuffer = (decltype(&glUnmapBuffer))gl_real("glUnmapBuffer");
    if (!o_glUnmapBuffer) return GL_TRUE;
    MapState& m = (target == GL_ELEMENT_ARRAY_BUFFER) ? g_mapElem : g_mapArr;
    if (m.ptr) {
        auto it = g_bufs.find(m.buf);
        if (it != g_bufs.end() && !it->second.data.empty() &&
            (size_t)(m.off + m.len) <= it->second.data.size())
            memcpy(it->second.data.data() + m.off, m.ptr, (size_t)m.len);
        m.ptr = nullptr;
    }
    return o_glUnmapBuffer(target);
}

std::string layout_of(GLuint vao) {
    std::string s; char buf[96];
    auto it = g_vaos.find(vao);
    if (it == g_vaos.end()) return s;
    for (int i = 0; i < 16; i++) {
        const Attr& a = it->second.attr[i];
        if (!a.has) continue;
        snprintf(buf, sizeof buf, "%d:%d/%x/s%d/b%u,", i, a.size, a.type, a.stride, a.buf);
        s += buf;
    }
    return s;
}

// ---------------------------------------------------------------- patch

struct Patch {
    uint64_t    hash  = 0;
    int         action = 0;   // 0=skip, 1=dump ib, 2=dump vb, 3=skipall, 4=skipcnt
    bool        hasFirst = false, hasCnt = false;
    uint32_t    first = 0, cnt = 0;
    std::string file;
    bool        done = false;
    uint32_t    hits = 0;
    uint32_t    attrIdx = 0;
    float       factor = 1.0f, fy = -1.0f, fz = -1.0f;
    // ib 替换
    std::vector<uint8_t> ib;
    GLuint      glBuf = 0;
    uint32_t    ibCount = 0;
    GLenum      ibType = GL_UNSIGNED_SHORT;
    uint32_t    drawCount = 0;   // 0 = 用原始 count
    // vbpos 替换
    uint32_t    srcStride = 0;
    uint32_t    srcOffset = 0;
    int flipV = 0;
    int mode = 1;
    float refCenter[3] = {0,0,0};
    std::vector<std::pair<int,GLuint>> uvBinds;
    std::vector<uint8_t>  dds;
    int ddsW = 0, ddsH = 0;
    std::vector<uint32_t> dynMap;
    std::vector<float>    dynBase;
    // vbmod: 全属性替换
    int         perm = 0;
    float       scale = 1.0f;
    std::vector<uint8_t> blend, tex;
    std::vector<std::pair<int,GLuint>> binds;
    // vbmoddyn: keep the replacement position in the game's current pose
    bool dynReady = false;
    int dynPosAttr = -1;
    GLuint dynSrcBuf = 0;
    GLsizei dynStride = 0;
    size_t dynOffset = 0;
    uint32_t dynCount = 0;
    std::vector<uint8_t> dynBuf;
    std::vector<float> dynRef;
};

std::vector<Patch> g_patches;
bool g_patchesLoaded = false;

time_t g_patchMtime = 0;

void load_patches() {
    if (!g_dir[0]) return;
    std::string p = std::string(g_dir) + "/patch.txt";
    struct stat st;
    time_t mt = (stat(p.c_str(), &st) == 0) ? st.st_mtime : 0;
    if (g_patchesLoaded && mt == g_patchMtime) return;
    g_patchMtime = mt;
    g_patchesLoaded = true;
    g_patches.clear();
    FILE* f = fopen(p.c_str(), "r");
    if (!f) { LOGI("no patch.txt"); return; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == 0) continue;
        char cmd[32] = {0}, a1[64] = {0}, a2[64] = {0}, a3[64] = {0}, a4[64] = {0}, a5[64] = {0}, a6[64] = {0}, a7[64] = {0};
        int n = sscanf(s, "%31s %63s %63s %63s %63s %63s %63s %63s", cmd, a1, a2, a3, a4, a5, a6, a7);
        if (n < 1) continue;
        Patch pt;
        if (!strcmp(cmd, "skipall")) {
            pt.action = 3;
        } else if (!strcmp(cmd, "skipcnt")) {
            if (n < 3) continue;
            pt.action = 4;
            pt.first = (uint32_t)strtoul(a1, nullptr, 0);
            pt.cnt   = (uint32_t)strtoul(a2, nullptr, 0);
        } else if (!strcmp(cmd, "skip")) {
            if (n < 2) continue;
            pt.action = 0;
            pt.hash = strtoull(a1, nullptr, 16);
            if (n >= 3) { pt.hasFirst = true; pt.first = (uint32_t)strtoul(a2, nullptr, 0); }
            if (n >= 4) { pt.hasCnt = true;   pt.cnt   = (uint32_t)strtoul(a3, nullptr, 0); }
        } else if (!strcmp(cmd, "ib")) {
            if (n < 3) continue;
            pt.action = 5;
            pt.hash = strtoull(a1, nullptr, 16);
            pt.file = a2;
            // ib <hash> <file> [first] [count] [u16|u32]
            if (n >= 4) { pt.hasFirst = true; pt.first = (uint32_t)strtoul(a3, nullptr, 0); }
            if (n >= 5) pt.drawCount = (uint32_t)strtoul(a4, nullptr, 0);
            if (n >= 6) pt.ibType = strstr(a5, "32") ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
        } else if (!strcmp(cmd, "vbscale")) {
            // vbscale <ibhash> <attr> <factor> [first]
            if (n < 4) continue;
            pt.action = 7;
            pt.hash = strtoull(a1, nullptr, 16);
            pt.attrIdx = (uint32_t)strtoul(a2, nullptr, 0);
            pt.factor = (float)atof(a3);
            if (n >= 5) pt.fy = (float)atof(a4);
            if (n >= 6) pt.fz = (float)atof(a5);
            LOGI("patch vbscale3 hash=%08llx attr=%u f=(%.2f %.2f %.2f)",
                 (unsigned long long)pt.hash, pt.attrIdx, pt.factor, pt.fy, pt.fz);
            LOGI("patch vbscale hash=%08llx attr=%u factor=%.2f", (unsigned long long)pt.hash, pt.attrIdx, pt.factor);
        } else if (!strcmp(cmd, "vbposp")) {
            // vbposp <ibhash> <attr> <file> <srcStride> <perm>
            if (n < 6) continue;
            pt.action = 8;
            pt.hash = strtoull(a1, nullptr, 16);
            pt.attrIdx = (uint32_t)strtoul(a2, nullptr, 0);
            pt.file = a3;
            pt.srcStride = (uint32_t)strtoul(a4, nullptr, 0);
            pt.perm = (int)strtoul(a5, nullptr, 0);
            LOGI("patch vbposp hash=%08llx attr=%u stride=%u perm=%d",
                 (unsigned long long)pt.hash, pt.attrIdx, pt.srcStride, pt.perm);
        } else if (!strcmp(cmd, "attrstat")) {
            if (n < 2) continue;
            pt.action = 11;
            pt.hash = strtoull(a1, nullptr, 16);
        } else if (!strcmp(cmd, "vbstatic")) {
            // vbstatic <ibhash> <posfile> <perm> [alignX]
            if (n < 4) continue;
            pt.action = 17;
            pt.hash = strtoull(a1, nullptr, 16);
            pt.file = a2;
            pt.perm = (int)strtoul(a3, nullptr, 0);
            if (n >= 7) pt.flipV = atoi(a6);
            if (n >= 8) pt.mode = atoi(a7);
            if (n >= 6 && !strcmp(a5, "red")) {
                pt.ddsW = 64; pt.ddsH = 64;
                pt.dds.assign(64 * 64 * 4, 0);
                for (size_t q = 0; q < pt.dds.size(); q += 4) {
                    pt.dds[q] = 255; pt.dds[q+1] = 0; pt.dds[q+2] = 0; pt.dds[q+3] = 255;
                }
                LOGI("patch vbstatic dds=RED 64x64");
            } else if (n >= 6) {
                std::string df = std::string(g_dir) + "/" + a5;
                TexOv t;
                if (load_dds(df.c_str(), t)) {
                    pt.ddsW = t.w; pt.ddsH = t.h;
                    pt.dds.assign(t.rgba.begin(), t.rgba.end());
                    LOGI("patch vbstatic dds=%s %dx%d", a5, t.w, t.h);
                } else {
                    LOGE("vbstatic dds load fail %s", df.c_str());
                }
            }
            if (n >= 5) {
                std::string uf = std::string(g_dir) + "/" + a4;
                FILE* ff = fopen(uf.c_str(), "rb");
                if (ff) {
                    fseek(ff, 0, SEEK_END); long usz = ftell(ff); fseek(ff, 0, SEEK_SET);
                    if (usz > 0 && usz <= (64 << 20)) {
                        pt.blend.resize((size_t)usz);
                        if (fread(pt.blend.data(), 1, (size_t)usz, ff) != (size_t)usz) pt.blend.clear();
                    }
                    fclose(ff);
                    LOGI("patch vbstatic uv=%s %ld bytes", a4, usz);
                }
            }
            std::string fp = std::string(g_dir) + "/" + pt.file;
            FILE* vf = fopen(fp.c_str(), "rb");
            if (!vf) { LOGE("vbstatic: cannot open %s", fp.c_str()); continue; }
            fseek(vf, 0, SEEK_END); long sz = ftell(vf); fseek(vf, 0, SEEK_SET);
            if (sz <= 0 || sz > (64 << 20)) { fclose(vf); continue; }
            pt.ib.resize((size_t)sz);
            if (fread(pt.ib.data(), 1, (size_t)sz, vf) != (size_t)sz) { fclose(vf); continue; }
            fclose(vf);
            LOGI("patch vbstatic hash=%08llx %s %ld bytes perm=%d",
                 (unsigned long long)pt.hash, pt.file.c_str(), sz, pt.perm);
        } else if (!strcmp(cmd, "iblist")) {
            pt.action = 16;
        } else if (!strcmp(cmd, "ibfind")) {
            // ibfind <file>  — 用文件前 64 字节在所有已缓存的 IB 里找匹配
            if (n < 2) continue;
            pt.action = 15;
            pt.file = a1;
            std::string fp = std::string(g_dir) + "/" + pt.file;
            FILE* vf = fopen(fp.c_str(), "rb");
            if (!vf) { LOGE("ibfind: cannot open %s", fp.c_str()); continue; }
            pt.ib.resize(64);
            size_t got = fread(pt.ib.data(), 1, 64, vf);
            fclose(vf);
            pt.ib.resize(got);
            LOGI("ibfind: loaded %zu bytes from %s", got, pt.file.c_str());
        } else if (!strcmp(cmd, "tex")) {
            if (n < 3) continue;
            pt.action = 13;
            pt.hash = strtoull(a1, nullptr, 16);
            pt.file = a2;
            LOGI("patch tex hash=%08llx file=%s", (unsigned long long)pt.hash, pt.file.c_str());
        } else if (!strcmp(cmd, "texstat")) {
            if (n < 2) continue;
            pt.action = 14;
            pt.hash = strtoull(a1, nullptr, 16);
        } else if (!strcmp(cmd, "vbmod") || !strcmp(cmd, "vbmoddyn")) {
            // vbmod <ibhash> <posfile> <blendfile> <texfile>
            if (n < 5) continue;
            pt.action = !strcmp(cmd, "vbmoddyn") ? 12 : 10;
            pt.hash = strtoull(a1, nullptr, 16);
            pt.file = a2;
            pt.blend = std::vector<uint8_t>();
            struct { std::string path; std::vector<uint8_t>* dst; } srcs[3] = {
                {a2, &pt.ib}, {a3, &pt.blend}, {a4, &pt.tex}};
            bool okall = true;
            for (int q = 0; q < 3; q++) {
                std::string fp = std::string(g_dir) + "/" + srcs[q].path;
                FILE* vf = fopen(fp.c_str(), "rb");
                if (!vf) { LOGE("cannot open %s", fp.c_str()); okall = false; break; }
                fseek(vf, 0, SEEK_END); long sz = ftell(vf); fseek(vf, 0, SEEK_SET);
                if (sz <= 0 || sz > (64 << 20)) { fclose(vf); okall = false; break; }
                srcs[q].dst->resize((size_t)sz);
                if (fread(srcs[q].dst->data(), 1, (size_t)sz, vf) != (size_t)sz) { fclose(vf); okall = false; break; }
                fclose(vf);
                LOGI("vbmod src %s %ld bytes", srcs[q].path.c_str(), sz);
            }
            if (!okall) continue;
            if (n >= 6) pt.perm = (int)strtoul(a5, nullptr, 0);
            LOGI("vbmod perm=%d", pt.perm);
        } else if (!strcmp(cmd, "vbbox")) {
            // vbbox <ibhash> [first]  — 打印该 draw 的 attr0 包围盒
            if (n < 2) continue;
            pt.action = 9;
            pt.hash = strtoull(a1, nullptr, 16);
            if (n >= 3) { pt.hasFirst = true; pt.first = (uint32_t)strtoul(a2, nullptr, 0); }
        } else if (!strcmp(cmd, "vbpos")) {
            // vbpos <ibhash> <attr> <file> <srcStride> [srcOffset] [first]
            if (n < 5) continue;
            pt.action = 8;
            pt.hash = strtoull(a1, nullptr, 16);
            pt.attrIdx = (uint32_t)strtoul(a2, nullptr, 0);
            pt.file = a3;
            pt.srcStride = (uint32_t)strtoul(a4, nullptr, 0);
            if (n >= 6) pt.srcOffset = (uint32_t)strtoul(a5, nullptr, 0);
            LOGI("patch vbpos hash=%08llx attr=%u src=%s stride=%u off=%u",
                 (unsigned long long)pt.hash, pt.attrIdx, pt.file.c_str(), pt.srcStride, pt.srcOffset);
        } else if (!strcmp(cmd, "dumpmesh")) {
            if (n < 3) continue;
            pt.action = 6;
            pt.hash = strtoull(a1, nullptr, 16);
            pt.file = a2;
            if (n >= 4) { pt.hasFirst = true; pt.first = (uint32_t)strtoul(a3, nullptr, 0); }
        } else if (!strcmp(cmd, "dump") || !strcmp(cmd, "dumpvb")) {
            if (n < 3) continue;
            pt.action = (cmd[4] == 'v') ? 2 : 1;
            pt.hash = strtoull(a1, nullptr, 16);
            pt.file = a2;
        } else { LOGW("unknown patch cmd %s", cmd); continue; }
        if (pt.action == 8) {
            std::string fp = std::string(g_dir) + "/" + pt.file;
            FILE* vf = fopen(fp.c_str(), "rb");
            if (!vf) { LOGE("cannot open vbpos %s", fp.c_str()); continue; }
            fseek(vf, 0, SEEK_END); long sz = ftell(vf); fseek(vf, 0, SEEK_SET);
            if (sz <= 0 || sz > (64 << 20)) { fclose(vf); LOGE("bad vbpos size %ld", sz); continue; }
            pt.ib.resize((size_t)sz);
            if (fread(pt.ib.data(), 1, (size_t)sz, vf) != (size_t)sz) { fclose(vf); continue; }
            fclose(vf);
            LOGI("vbpos loaded %s %ld bytes = %ld verts", pt.file.c_str(), sz, sz / (long)pt.srcStride);
        }
        if (pt.action == 5) {
            std::string fp = std::string(g_dir) + "/" + pt.file;
            FILE* ibf = fopen(fp.c_str(), "rb");
            if (!ibf) { LOGE("cannot open ib %s", fp.c_str()); continue; }
            fseek(ibf, 0, SEEK_END);
            long sz = ftell(ibf);
            fseek(ibf, 0, SEEK_SET);
            if (sz <= 0 || sz > (64 << 20)) { fclose(ibf); LOGE("bad ib size %ld", sz); continue; }
            pt.ib.resize((size_t)sz);
            if (fread(pt.ib.data(), 1, (size_t)sz, ibf) != (size_t)sz) { fclose(ibf); continue; }
            fclose(ibf);
            pt.ibCount = (uint32_t)(sz / (pt.ibType == GL_UNSIGNED_INT ? 4 : 2));
            if (!pt.drawCount) pt.drawCount = pt.ibCount;
            LOGI("patch ib: %s %zu bytes (%u idx, %s)", pt.file.c_str(), pt.ib.size(), pt.ibCount,
                 pt.ibType == GL_UNSIGNED_INT ? "u32" : "u16");
        }
        g_patches.push_back(std::move(pt));
        LOGI("patch: %s", cmd);
    }
    fclose(f);
    LOGI("patches loaded: %zu", g_patches.size());
}

// 一次 draw 的指纹: 整个 IB 的 hash + match_first_index + 索引数
struct Key {
    uint64_t wh1;
    uint32_t first;
    uint32_t cnt;
    bool operator<(const Key& o) const {
        if (wh1 != o.wh1) return wh1 < o.wh1;
        if (first != o.first) return first < o.first;
        return cnt < o.cnt;
    }
};

struct Rec {
    GLuint   vao = 0, eb = 0, prog = 0;
    GLenum   type = 0, mode = 0;
    uint64_t h1 = 0, h1a = 0, wh1a = 0;
    uint64_t hits = 0;
    bool     exact = false;
    std::string layout;
    char     kind[4] = {0};
};

std::map<Key, Rec> g_recs;

void dump_stats() {
    if (!open_out()) return;
    std::vector<const std::pair<const Key, Rec>*> v;
    v.reserve(g_recs.size());
    for (const auto& kv : g_recs) v.push_back(&kv);
    std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->second.hits > b->second.hits; });
    freopen((std::string(g_dir) + "/gles.tsv").c_str(), "w", g_fp);
    fprintf(g_fp, "# hits\tib_hash(crc32c)\tx\ty\tfirst_index\tidx_count\tidx_type\tmode\tprog\tvao\teb\tkind\tsub_exact\tvtx_layout\n");
    size_t n = 0;
    for (auto* p : v) {
        if (n++ >= 4000) break;
        const Rec& r = p->second;
        fprintf(g_fp, "%llu\t%08llx\t%08llx\t%08llx\t%u\t%u\t%x\t%x\t%u\t%u\t%u\t%s\t%d\t%s\n",
                (unsigned long long)r.hits, (unsigned long long)p->first.wh1,
                (unsigned long long)r.wh1a, (unsigned long long)r.wh1a,
                p->first.first, p->first.cnt, r.type, r.mode, r.prog, r.vao, r.eb,
                r.kind, r.exact ? 1 : 0, r.layout.c_str());
    }
    fflush(g_fp);
}

// 返回: 0=正常下发, 1=跳过, 2=用 g_overrideBuf 替换索引缓冲
GLuint g_overrideBuf = 0;
GLsizei g_overrideCount = 0;
GLenum g_overrideType = GL_UNSIGNED_SHORT;

// ---------------------------------------------------------------- 纹理替换
std::unordered_map<GLuint, TexOv> g_texOv;
std::unordered_set<GLuint> g_texDone;
GLuint g_tex2D = 0;
GLuint g_texUnitCur = 0;
GLuint g_texByUnit[8] = {0,0,0,0,0,0,0,0};

decltype(&glBindTexture)  o_glBindTexture  = nullptr;
decltype(&glActiveTexture) o_glActiveTexture = nullptr;
decltype(&glTexImage2D)   o_glTexImage2D   = nullptr;

void upload_tex(GLuint id) {
    if (!id || !o_glTexImage2D) return;
    auto it = g_texOv.find(id);
    if (it == g_texOv.end()) return;
    if (g_texDone.count(id)) return;
    o_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, it->second.w, it->second.h, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, it->second.rgba.data());
    g_texDone.insert(id);
    LOGI("tex upload id=%u %dx%d", id, it->second.w, it->second.h);
}

void glActiveTexture_hook(GLenum unit) {
    if (!o_glActiveTexture) o_glActiveTexture = (decltype(&glActiveTexture))gl_real("glActiveTexture");
    if (unit >= GL_TEXTURE0 && unit < GL_TEXTURE0 + 8) g_texUnitCur = unit - GL_TEXTURE0;
    if (o_glActiveTexture) o_glActiveTexture(unit);
}

void glBindTexture_hook(GLenum target, GLuint tex) {
    if (!o_glBindTexture) o_glBindTexture = (decltype(&glBindTexture))gl_real("glBindTexture");
    if (!o_glBindTexture) return;
    o_glBindTexture(target, tex);
    if (target == GL_TEXTURE_2D) {
        g_tex2D = tex;
        if (g_texUnitCur < 8) g_texByUnit[g_texUnitCur] = tex;
        upload_tex(tex);
    }
}

bool load_dds(const char* path, TexOv& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[148];
    if (fread(hdr, 1, 148, f) != 148) { fclose(f); return false; }
    if (memcmp(hdr, "DDS ", 4) != 0) { fclose(f); return false; }
    uint32_t hh = *(uint32_t*)(hdr + 12);
    uint32_t ww = *(uint32_t*)(hdr + 16);
    uint32_t dxgi = *(uint32_t*)(hdr + 128);
    if (!ww || !hh || ww > 4096 || hh > 4096) { fclose(f); return false; }
    size_t blocks = (size_t)(ww / 4) * (size_t)(hh / 4);
    size_t bsize = (dxgi == 77 || dxgi == 78) ? 16 : 16;
    std::vector<uint8_t> src(blocks * bsize);
    size_t got = fread(src.data(), 1, src.size(), f);
    fclose(f);
    if (got != src.size()) return false;
    out.w = (int)ww; out.h = (int)hh;
    out.rgba.assign((size_t)ww * hh * 4, 0);
    for (uint32_t by = 0; by < hh / 4; by++) {
        for (uint32_t bx = 0; bx < ww / 4; bx++) {
            uint8_t blk[64];
            const uint8_t* cp = src.data() + ((size_t)by * (ww / 4) + bx) * bsize;
            if (dxgi == 98 || dxgi == 99) bcdec_bc7(cp, blk, 16);
            else if (dxgi == 77 || dxgi == 78) bcdec_bc3(cp, blk, 16);
            else if (dxgi == 71 || dxgi == 72) bcdec_bc1(cp, blk, 16);
            else { return false; }
            for (int y = 0; y < 4; y++) {
                memcpy(out.rgba.data() + (((size_t)(by * 4 + y)) * ww + bx * 4) * 4, blk + y * 16, 16);
            }
        }
    }
    return true;
}

struct LiveAttr { bool enabled=false; GLint size=0; GLenum type=0; GLsizei stride=0;
                  GLuint buf=0; const void* ptr=nullptr; GLboolean norm=GL_FALSE; };

void capture_vao(LiveAttr out[16]) {
    typedef void (*PFNGETIV)(GLuint, GLenum, GLint*);
    typedef void (*PFNGETPTR)(GLuint, GLenum, void**);
    static PFNGETIV  pGetIv  = nullptr;
    static PFNGETPTR pGetPtr = nullptr;
    if (!pGetIv)  pGetIv  = (PFNGETIV)gl_real("glGetVertexAttribiv");
    if (!pGetPtr) pGetPtr = (PFNGETPTR)gl_real("glGetVertexAttribPointerv");
    for (int i = 0; i < 16; i++) out[i] = LiveAttr();
    if (!pGetIv || !pGetPtr) return;
    for (int i = 0; i < 16; i++) {
        GLint en = 0, sz = 0, ty = 0, st = 0, nm = 0, bd = 0;
        void* pt = nullptr;
        pGetIv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
        if (!en) continue;
        pGetIv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &sz);
        pGetIv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &ty);
        pGetIv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &st);
        pGetIv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &nm);
        pGetIv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &bd);
        pGetPtr((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pt);
        out[i].enabled = true; out[i].size = sz; out[i].type = (GLenum)ty;
        out[i].stride = st; out[i].norm = (GLboolean)nm;
        out[i].buf = (GLuint)bd; out[i].ptr = pt;
    }
}

// 顶点属性临时重绑（支持多个属性）
GLuint g_modTex = 0;
GLint  g_modTexPrev = 0;
int    g_modTexUnit = 1;
bool   g_modTexActive = false;
bool   g_modTexBound = false;

struct VbBind { int idx = -1; GLuint newBuf = 0; Attr saved; };
std::vector<VbBind> g_vbBinds;
GLuint g_vbSavedArray = 0;

void apply_vb() {
    if (g_modTex && g_modTexActive) {
        typedef void (*PFNACT)(GLenum);
        typedef void (*PFNBINDT)(GLenum, GLuint);
        typedef void (*PFNGETI)(GLenum, GLint*);
        static PFNACT pact = nullptr; static PFNBINDT pbind = nullptr; static PFNGETI pgeti = nullptr;
        if (!pact) pact = (PFNACT)gl_real("glActiveTexture");
        if (!pbind) pbind = (PFNBINDT)gl_real("glBindTexture");
        if (!pgeti) pgeti = (PFNGETI)gl_real("glGetIntegerv");
        if (pact && pbind) {
            pact(GL_TEXTURE0 + g_modTexUnit);
            if (pgeti) { g_modTexPrev = 0; pgeti(GL_TEXTURE_BINDING_2D, &g_modTexPrev); }
            pbind(GL_TEXTURE_2D, g_modTex);
            pact(GL_TEXTURE0);
            g_modTexBound = true;
        }
    }
    if (g_vbBinds.empty() || !o_glBindBuffer || !o_glVertexAttribPointer) return;
    LiveAttr la[16];
    capture_vao(la);
    g_vbSavedArray = g_curArray;
    for (auto& b : g_vbBinds) {
        if (b.idx < 0 || b.idx >= 16 || !la[b.idx].enabled) continue;
        b.saved.has = true;
        b.saved.size = la[b.idx].size;
        b.saved.type = la[b.idx].type;
        b.saved.stride = la[b.idx].stride;
        b.saved.buf = la[b.idx].buf;
        b.saved.ptr = la[b.idx].ptr;
        b.saved.norm = la[b.idx].norm;
        o_glBindBuffer(GL_ARRAY_BUFFER, b.newBuf);
        o_glVertexAttribPointer((GLuint)b.idx, b.saved.size, b.saved.type, b.saved.norm,
                                b.saved.stride, b.saved.ptr);
    }
    g_curArray = g_vbBinds.empty() ? g_vbSavedArray : g_vbBinds.back().newBuf;
}

void restore_vb() {
    if (g_modTexBound) {
        typedef void (*PFNACT)(GLenum);
        typedef void (*PFNBINDT)(GLenum, GLuint);
        static PFNACT pact = nullptr; static PFNBINDT pbind = nullptr;
        if (!pact) pact = (PFNACT)gl_real("glActiveTexture");
        if (!pbind) pbind = (PFNBINDT)gl_real("glBindTexture");
        if (pact && pbind) {
            pact(GL_TEXTURE0 + g_modTexUnit);
            pbind(GL_TEXTURE_2D, (GLuint)g_modTexPrev);
            pact(GL_TEXTURE0);
            g_modTexBound = false;
        }
    }
    if (g_vbBinds.empty()) return;
    if (o_glBindBuffer && o_glVertexAttribPointer) {
        for (auto& b : g_vbBinds) {
            if (b.idx < 0 || !b.saved.has) continue;
            o_glBindBuffer(GL_ARRAY_BUFFER, b.saved.buf);
            o_glVertexAttribPointer((GLuint)b.idx, b.saved.size, b.saved.type, b.saved.norm,
                                    b.saved.stride, b.saved.ptr);
        }
    }
    g_curArray = g_vbSavedArray;
    g_vbBinds.clear();
}

void permute_xyz(const float* s, int perm, float* d) {
    switch (perm) {
        case 1: d[0]=s[1]; d[1]=s[0]; d[2]=s[2]; break;
        case 2: d[0]=s[1]; d[1]=s[2]; d[2]=s[0]; break;
        case 3: d[0]=s[2]; d[1]=s[1]; d[2]=s[0]; break;
        case 4: d[0]=s[2]; d[1]=s[0]; d[2]=s[1]; break;
        case 5: d[0]=s[0]; d[1]=s[2]; d[2]=s[1]; break;
        default: d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; break;
    }
}


// ---- 局部刚体拟合 (Horn 四元数法) ----
// 给定 cnt 组对应点 A[i]->B[i] 与权重 W[i], 求最优旋转 R 和质心 ca/cb。
static void fit_rigid(const float* A, const float* B, const float* W, int cnt,
                      float R[9], float ca[3], float cb[3]) {
    float sw = 0.0f;
    for (int i = 0; i < cnt; i++) sw += W[i];
    ca[0]=ca[1]=ca[2]=0.0f; cb[0]=cb[1]=cb[2]=0.0f;
    if (sw <= 1e-6f) { for (int i=0;i<9;i++) R[i] = (i%4==0)?1.0f:0.0f; return; }
    for (int i = 0; i < cnt; i++)
        for (int c = 0; c < 3; c++) { ca[c] += W[i]*A[i*3+c]; cb[c] += W[i]*B[i*3+c]; }
    for (int c = 0; c < 3; c++) { ca[c]/=sw; cb[c]/=sw; }
    float S[9] = {0,0,0,0,0,0,0,0,0};
    for (int i = 0; i < cnt; i++) {
        float a0=A[i*3+0]-ca[0], a1=A[i*3+1]-ca[1], a2=A[i*3+2]-ca[2];
        float b0=B[i*3+0]-cb[0], b1=B[i*3+1]-cb[1], b2=B[i*3+2]-cb[2];
        float w=W[i];
        S[0]+=w*a0*b0; S[1]+=w*a0*b1; S[2]+=w*a0*b2;
        S[3]+=w*a1*b0; S[4]+=w*a1*b1; S[5]+=w*a1*b2;
        S[6]+=w*a2*b0; S[7]+=w*a2*b1; S[8]+=w*a2*b2;
    }
    float Sxx=S[0],Sxy=S[1],Sxz=S[2],Syx=S[3],Syy=S[4],Syz=S[5],Szx=S[6],Szy=S[7],Szz=S[8];
    float N[4][4] = {
        {Sxx+Syy+Szz, Syz-Szy,     Szx-Sxz,     Sxy-Syx},
        {Syz-Szy,     Sxx-Syy-Szz, Sxy+Syx,     Szx+Sxz},
        {Szx-Sxz,     Sxy+Syx,    -Sxx+Syy-Szz, Syz+Szy},
        {Sxy-Syx,     Szx+Sxz,     Syz+Szy,    -Sxx-Syy+Szz}
    };
    float shift = 0.0f;
    for (int r = 0; r < 4; r++) {
        float s = 0.0f; for (int c = 0; c < 4; c++) s += fabsf(N[r][c]);
        if (s > shift) shift = s;
    }
    for (int r = 0; r < 4; r++) N[r][r] += shift;
    float q[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    for (int it = 0; it < 40; it++) {
        float nq[4];
        for (int r = 0; r < 4; r++) {
            float s = 0.0f; for (int c = 0; c < 4; c++) s += N[r][c]*q[c];
            nq[r] = s;
        }
        float nrm = sqrtf(nq[0]*nq[0]+nq[1]*nq[1]+nq[2]*nq[2]+nq[3]*nq[3]);
        if (nrm < 1e-12f) break;
        for (int r = 0; r < 4; r++) q[r] = nq[r]/nrm;
    }
    float q0=q[0], qx=q[1], qy=q[2], qz=q[3];
    R[0]=q0*q0+qx*qx-qy*qy-qz*qz; R[1]=2*(qx*qy-q0*qz);         R[2]=2*(qx*qz+q0*qy);
    R[3]=2*(qx*qy+q0*qz);         R[4]=q0*q0-qx*qx+qy*qy-qz*qz; R[5]=2*(qy*qz-q0*qx);
    R[6]=2*(qx*qz-q0*qy);         R[7]=2*(qy*qz+q0*qx);         R[8]=q0*q0-qx*qx-qy*qy+qz*qz;
}
int on_draw(const char* kind, GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basev = 0) {
    c_draw++;
    g_modTexActive = false;
    GLuint eb = cur_elem();
    const size_t esz = (type == GL_UNSIGNED_INT) ? 4 : (type == GL_UNSIGNED_BYTE ? 1 : 2);
    uint64_t h1 = 0, h1a = 0, wh1 = 0, wh1a = 0;
    uint32_t first = 0;
    bool exact = false;

    auto it = g_bufs.find(eb);
    if (eb && it != g_bufs.end()) {
        wh1 = it->second.h_ib; wh1a = 0;
        size_t off = (size_t)(uintptr_t)indices;
        size_t bytes = (size_t)count * esz;
        first = (uint32_t)(off / esz);
        if (basev) { c_basev++; if (c_basev <= 20) LOGI("BASEV %08x first=%u cnt=%u bv=%d", (unsigned)wh1, first, (unsigned)count, basev); }
        h1 = wh1; h1a = 0;
        (void)off; (void)bytes;
        exact = true;
    } else if (!eb && indices && count > 0) {
        size_t bytes = (size_t)count * esz;
        if (bytes <= (16 << 20)) {
            h1  = buffer_hash((const uint8_t*)indices, bytes, 2u);
            h1a = 0;
            wh1 = h1; wh1a = h1a;
            exact = true;
        }
    }

    bool skip = false, over = false;
    load_patches();
    for (auto& pt : g_patches) {
        if (pt.action == 3) { c_skipped++; skip = true; continue; }
        if (pt.action == 4) {
            if (count >= (GLsizei)pt.first && count <= (GLsizei)pt.cnt) { c_skipped++; skip = true; }
            continue;
        }
        if (pt.action == 16) {
            if (pt.done) continue;
            if (pt.hits++ < 3000) continue;
            pt.done = true;
            LOGI("IBLIST %zu cached", g_bufs.size());
            for (auto& kv : g_bufs) {
                const auto& bd = kv.second.data;
                if (bd.size() < 16) continue;
                char hx[40];
                snprintf(hx, sizeof hx, "%02x%02x%02x%02x%02x%02x%02x%02x",
                         bd[0], bd[1], bd[2], bd[3], bd[4], bd[5], bd[6], bd[7]);
                LOGI("  ib id=%u size=%zu hash=%08x tgt=%x pre=%s",
                     kv.first, bd.size(), kv.second.h_ib, kv.second.target, hx);
            }
            continue;
        }
        if (pt.action == 15) {
            if (pt.done) continue;
            if (pt.hits++ < 3000) continue;
            if (pt.ib.size() < 16) { pt.done = true; continue; }
            size_t npre = pt.ib.size();
            int found = 0;
            for (auto& kv : g_bufs) {
                const auto& bd = kv.second.data;
                if (bd.size() < npre) continue;
                // 子串搜索: 索引池里可能只取一段
                for (size_t off = 0; off + npre <= bd.size(); off += 2) {
                    if (bd[off] != pt.ib[0]) continue;
                    if (memcmp(bd.data() + off, pt.ib.data(), npre) != 0) continue;
                    uint32_t hh = buffer_hash(bd.data(), bd.size(), 0x2);
                    LOGI("IBFIND %s -> buf=%u size=%zu off=%zu hash=%08x tgt=%x",
                         pt.file.c_str(), kv.first, bd.size(), off, hh, kv.second.target);
                    found++;
                    break;
                }
            }
            LOGI("IBFIND %s done, %d match(es), cached bufs=%zu", pt.file.c_str(), found, g_bufs.size());
            pt.done = true;
            continue;
        }
        if (!wh1 || pt.hash != wh1) continue;
        if (pt.hasFirst && first != pt.first) continue;
        if (pt.hasCnt && (uint32_t)count != pt.cnt) continue;
        if (pt.action == 0) { c_skipped++; skip = true; continue; }
        if (pt.action == 7) {
            if (pt.attrIdx >= 16) continue;
            LiveAttr la[16];
            capture_vao(la);
            if (!la[pt.attrIdx].enabled) continue;
            Attr a;
            a.has = true; a.size = la[pt.attrIdx].size; a.type = la[pt.attrIdx].type;
            a.stride = la[pt.attrIdx].stride; a.buf = la[pt.attrIdx].buf;
            a.ptr = la[pt.attrIdx].ptr; a.norm = la[pt.attrIdx].norm;
            if (!a.buf || a.stride <= 0) continue;
            if (!pt.glBuf) {
                uint32_t vcount = 0;
                if (it != g_bufs.end() && !it->second.data.empty()) {
                    const uint16_t* ip = (const uint16_t*)it->second.data.data();
                    size_t in = it->second.data.size() / 2;
                    for (size_t k = 0; k < in; k++) if (ip[k] + 1u > vcount) vcount = ip[k] + 1u;
                }
                size_t want = (size_t)vcount * (size_t)a.stride;
                if (!vcount || want > (8u << 20)) continue;
                GLuint savedArr = g_curArray;
                o_glBindBuffer(GL_ARRAY_BUFFER, a.buf);
                void* mp = o_glMapBufferRange
                               ? o_glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)want, GL_MAP_READ_BIT)
                               : nullptr;
                if (!mp) { o_glBindBuffer(GL_ARRAY_BUFFER, savedArr); continue; }
                std::vector<uint8_t> buf((const uint8_t*)mp, (const uint8_t*)mp + want);
                if (o_glUnmapBuffer) o_glUnmapBuffer(GL_ARRAY_BUFFER);
                o_glBindBuffer(GL_ARRAY_BUFFER, savedArr);

                size_t off = (size_t)(uintptr_t)a.ptr, vs = (size_t)a.stride;
                float fa[4] = {pt.factor, pt.fy, pt.fz, pt.factor};
                if (pt.fy < 0) { fa[1] = pt.factor; fa[2] = pt.factor; }
                for (size_t k = 0; k + off + 4 * (size_t)a.size <= buf.size(); k += vs) {
                    float* f = (float*)(buf.data() + k + off);
                    for (int c = 0; c < a.size; c++) f[c] *= fa[c];
                }
                GLuint nb = 0;
                o_glGenBuffers(1, &nb);
                o_glBindBuffer(GL_ARRAY_BUFFER, nb);
                o_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)buf.size(), buf.data(), GL_STATIC_DRAW);
                o_glBindBuffer(GL_ARRAY_BUFFER, savedArr);
                pt.glBuf = nb;
                LOGI("vbscale: attr%u verts=%u stride=%d -> buf %u", pt.attrIdx, vcount, a.stride, nb);
            }
            g_vbBinds.clear();
            { VbBind vb; vb.idx = (int)pt.attrIdx; vb.newBuf = pt.glBuf; g_vbBinds.push_back(vb); }
            continue;
        }
        if (pt.action == 11) {
            LiveAttr la[16];
            capture_vao(la);
            std::string sig;
            char sb[64];
            for (int i = 0; i < 16; i++) {
                if (!la[i].enabled) continue;
                snprintf(sb, sizeof sb, "%d:%d/%x/%d/%d,", i, la[i].size, la[i].type, la[i].stride, (int)(uintptr_t)la[i].ptr);
                sig += sb;
            }
            if (sig.empty() || sig == pt.file) continue;
            if (pt.binds.size() >= 10) continue;
            pt.file = sig;
            pt.binds.push_back({(int)pt.binds.size(), 0});
            uint32_t vcount = 0;
            if (it != g_bufs.end() && !it->second.data.empty()) {
                const uint16_t* ip = (const uint16_t*)it->second.data.data();
                size_t in = it->second.data.size() / 2;
                for (size_t k = 0; k < in; k++) if (ip[k] + 1u > vcount) vcount = ip[k] + 1u;
            }
            LOGI("ATTRSTAT hash=%08llx vao=%u verts=%u", (unsigned long long)wh1, g_curVao, vcount);
            for (int i = 0; i < 16; i++) {
                if (!la[i].enabled) continue;
                GLuint savedArr = g_curArray;
                o_glBindBuffer(GL_ARRAY_BUFFER, la[i].buf);
                size_t want = (size_t)std::min<uint32_t>(vcount, 64) * (size_t)la[i].stride;
                if (!want || want > (1u << 20)) { o_glBindBuffer(GL_ARRAY_BUFFER, savedArr); continue; }
                void* mp = o_glMapBufferRange
                               ? o_glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)(uintptr_t)la[i].ptr,
                                                    (GLsizeiptr)want, GL_MAP_READ_BIT)
                               : nullptr;
                if (!mp) { o_glBindBuffer(GL_ARRAY_BUFFER, savedArr); continue; }
                const uint8_t* b = (const uint8_t*)mp;
                std::string line;
                char hb[8];
                for (int v = 0; v < 4; v++) {
                    snprintf(hb, sizeof hb, " [%d]", v);
                    line += hb;
                    for (GLsizei q = 0; q < la[i].stride && q < 24; q++) {
                        snprintf(hb, sizeof hb, "%02x", b[v * (size_t)la[i].stride + (size_t)q]);
                        line += hb;
                    }
                }
                if (o_glUnmapBuffer) o_glUnmapBuffer(GL_ARRAY_BUFFER);
                o_glBindBuffer(GL_ARRAY_BUFFER, savedArr);
                LOGI("  a%d sz=%d ty=%x st=%d ptr=%p buf=%u |%s",
                     i, la[i].size, la[i].type, la[i].stride, la[i].ptr, la[i].buf, line.c_str());
            }
            continue;
        }
        if (pt.action == 13) {
            if (!pt.done) {
                pt.done = true;
                std::string fp = std::string(g_dir) + "/" + pt.file;
                TexOv t;
                if (load_dds(fp.c_str(), t)) {
                    g_texOv[g_tex2D] = std::move(t);
                    g_texDone.erase(g_tex2D);
                    upload_tex(g_tex2D);
                    LOGI("tex bind id=%u from %s", g_tex2D, pt.file.c_str());
                } else {
                    LOGE("tex load fail %s (id=%u)", fp.c_str(), g_tex2D);
                }
            }
            continue;
        }
        if (pt.action == 14) {
            if (!pt.done) {
                // 只在有 UV 的主渲染 pass 上报告
                LiveAttr lc[16];
                capture_vao(lc);
                int uvN = 0;
                for (int i = 0; i < 16; i++)
                    if (lc[i].enabled && lc[i].size == 2 && lc[i].type == GL_FLOAT) uvN++;
                if (!uvN) continue;
                pt.done = true;
                typedef void (*PFNGETTEX)(GLenum, GLint, GLenum, GLint*);
                static PFNGETTEX pg = nullptr;
                if (!pg) pg = (PFNGETTEX)gl_real("glGetTexLevelParameteriv");
                if (pg) {
                    for (int u = 0; u < 6; u++) {
                        GLuint id = g_texByUnit[u];
                        if (!id) continue;
                        if (!o_glBindTexture) break;
                        o_glBindTexture(GL_TEXTURE_2D, id);
                        GLint w = 0, h = 0, fmt = 0;
                        pg(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
                        pg(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
                        pg(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
                        LOGI("TEXSTAT %08llx unit=%d id=%u %dx%d fmt=0x%x",
                             (unsigned long long)wh1, u, id, w, h, fmt);
                    }
                    o_glBindTexture(GL_TEXTURE_2D, g_tex2D);
                } else {
                    LOGI("TEXSTAT %08llx no query fn", (unsigned long long)wh1);
                }
            }
            continue;
        }
        if (pt.action == 17) {
            g_modTexActive = true;
            if (pt.done) {
                if (!pt.glBuf) continue;
                // 每帧: 读当前安卓顶点, 把位移传给 mod 顶点
                LiveAttr lb[16];
                capture_vao(lb);
                const int pp = pt.dynPosAttr;
                if (pp < 0 || pp >= 16 || !lb[pp].enabled || !o_glMapBufferRange) {
                    g_vbBinds.clear();
                    for (auto& uv : pt.uvBinds) { VbBind vb; vb.idx = uv.first; vb.newBuf = uv.second; g_vbBinds.push_back(vb); }
                    { VbBind vb; vb.idx = pp; vb.newBuf = pt.glBuf; g_vbBinds.push_back(vb); }
                    continue;
                }
                const LiveAttr& aa = lb[pp];
                const size_t oo = (size_t)(uintptr_t)aa.ptr;
                const size_t need = oo + (size_t)pt.dynRef.size() / 3 * (size_t)aa.stride;
                if (need <= (16u << 20) && aa.buf) {
                    GLuint sv = g_curArray;
                    o_glBindBuffer(GL_ARRAY_BUFFER, aa.buf);
                    void* mp2 = o_glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)need, GL_MAP_READ_BIT);
                    if (mp2) {
                        const uint8_t* nw = (const uint8_t*)mp2;
                        size_t cnt = pt.dynMap.size();
                        if (pt.mode == 3) {
                            for (size_t v = 0; v < cnt; v++) {
                                uint32_t ri = pt.dynMap[v];
                                const float* src = (const float*)(nw + oo + (size_t)ri * (size_t)aa.stride);
                                float* dst = (float*)(pt.dynBuf.data() + oo + v * (size_t)aa.stride);
                                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                            }
                        } else if (pt.mode == 4) {
                            for (size_t v = 0; v < cnt; v++) {
                                uint32_t ri = pt.dynMap[v];
                                const float* src = (const float*)(nw + oo + (size_t)ri * (size_t)aa.stride);
                                float* dst = (float*)(pt.dynBuf.data() + oo + v * (size_t)aa.stride);
                                dst[0] = 0.65f * src[0] + 0.35f * pt.dynBase[v*3+0];
                                dst[1] = 0.65f * src[1] + 0.35f * pt.dynBase[v*3+1];
                                dst[2] = 0.65f * src[2] + 0.35f * pt.dynBase[v*3+2];
                            }
                        } else if (pt.mode == 2) {
                            size_t nvc = pt.dynRef.size() / 3;
                            float cx = 0, cy = 0, cz = 0;
                            for (size_t v = 0; v < nvc; v++) {
                                const float* p = (const float*)(nw + oo + v * (size_t)aa.stride);
                                cx += p[0]; cy += p[1]; cz += p[2];
                            }
                            if (nvc) { cx /= nvc; cy /= nvc; cz /= nvc; }
                            float dx = cx - pt.refCenter[0], dy = cy - pt.refCenter[1], dz = cz - pt.refCenter[2];
                            for (size_t v = 0; v < cnt; v++) {
                                float* dst = (float*)(pt.dynBuf.data() + oo + v * (size_t)aa.stride);
                                dst[0] = pt.dynBase[v*3+0] + dx;
                                dst[1] = pt.dynBase[v*3+1] + dy;
                                dst[2] = pt.dynBase[v*3+2] + dz;
                            }
                        } else
                        for (size_t v = 0; v < cnt; v++) {
                            uint32_t ri = pt.dynMap[v];
                            const float* cur3 = (const float*)(nw + oo + (size_t)ri * (size_t)aa.stride);
                            float* dst = (float*)(pt.dynBuf.data() + oo + v * (size_t)aa.stride);
                            dst[0] = pt.dynBase[v*3+0] + (cur3[0] - pt.dynRef[ri*3+0]);
                            dst[1] = pt.dynBase[v*3+1] + (cur3[1] - pt.dynRef[ri*3+1]);
                            dst[2] = pt.dynBase[v*3+2] + (cur3[2] - pt.dynRef[ri*3+2]);
                        }
                        if (o_glUnmapBuffer) o_glUnmapBuffer(GL_ARRAY_BUFFER);
                        if (o_glBufferSubData) {
                            o_glBindBuffer(GL_ARRAY_BUFFER, pt.glBuf);
                            o_glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)pt.dynBuf.size(), pt.dynBuf.data());
                        }
                    }
                    o_glBindBuffer(GL_ARRAY_BUFFER, sv);
                }
                g_vbBinds.clear();
                for (auto& uv : pt.uvBinds) { VbBind vb; vb.idx = uv.first; vb.newBuf = uv.second; g_vbBinds.push_back(vb); }
                // 和位置共用 buffer 的属性一起换, 否则位置和骨骼权重会分家
                for (int i = 0; i < 16; i++)
                    if (lb[i].enabled && lb[i].buf == aa.buf)
                        { VbBind vb; vb.idx = i; vb.newBuf = pt.glBuf; g_vbBinds.push_back(vb); }
                continue;
            }
            LiveAttr la[16];
            capture_vao(la);
            int pos = -1;
            for (int i = 0; i < 16; i++) {
                if (la[i].enabled && la[i].size >= 3 && la[i].type == GL_FLOAT) { pos = i; break; }
            }
            uint32_t vcount = 0;
            if (it != g_bufs.end() && !it->second.data.empty()) {
                const uint16_t* ip = (const uint16_t*)it->second.data.data();
                size_t in = it->second.data.size() / 2;
                for (size_t k = 0; k < in; k++) if ((uint32_t)ip[k] + 1u > vcount) vcount = (uint32_t)ip[k] + 1u;
            }
            // mod 是高模, 顶点数可能多于安卓原版; 新 buffer 取两者较大值
            const uint32_t modVerts  = (uint32_t)(pt.ib.size() / 40);
            const uint32_t newVcount = modVerts > vcount ? modVerts : vcount;
            if (pos < 0 || !vcount || pt.ib.size() < 40 || !o_glMapBufferRange || !o_glBufferData) continue;
            // 只在主渲染 pass 上初始化: 必须有 UV 属性
            // 安卓的 UV 可能是 float / half float / 量化 u16
            if (!pt.blend.empty()) {
                int uvCnt = 0;
                for (int i = 0; i < 16; i++)
                    if (la[i].enabled && la[i].size == 2 &&
                        (la[i].type == GL_FLOAT || la[i].type == GL_HALF_FLOAT ||
                         la[i].type == GL_UNSIGNED_SHORT)) uvCnt++;
                if (!uvCnt) continue;
            }
            pt.done = true;
            pt.dynPosAttr = pos;
            const LiveAttr& a = la[pos];
            if (!a.buf || a.stride <= 0) continue;
            const size_t off = (size_t)(uintptr_t)a.ptr;
            const size_t want = off + (size_t)vcount * (size_t)a.stride;
            if (want > (16u << 20)) continue;
            GLuint savedArr = g_curArray;
            o_glBindBuffer(GL_ARRAY_BUFFER, a.buf);
            void* mp = o_glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)want, GL_MAP_READ_BIT);
            if (!mp) { o_glBindBuffer(GL_ARRAY_BUFFER, savedArr); continue; }
            std::vector<uint8_t> cur((const uint8_t*)mp, (const uint8_t*)mp + want);
            if (o_glUnmapBuffer) o_glUnmapBuffer(GL_ARRAY_BUFFER);
            o_glBindBuffer(GL_ARRAY_BUFFER, savedArr);

            // 参考包围盒（安卓）
            float rmin[3] = {1e30f,1e30f,1e30f}, rmax[3] = {-1e30f,-1e30f,-1e30f};
            for (uint32_t v = 0; v < vcount; v++) {
                const float* p = (const float*)(cur.data() + off + (size_t)v * (size_t)a.stride);
                for (int c = 0; c < 3; c++) {
                    if (p[c] != p[c]) continue;
                    if (p[c] < rmin[c]) rmin[c] = p[c];
                    if (p[c] > rmax[c]) rmax[c] = p[c];
                }
            }
            // mod 包围盒（置换后）
            const size_t n = std::min((size_t)newVcount, (size_t)modVerts);
            float mmin[3] = {1e30f,1e30f,1e30f}, mmax[3] = {-1e30f,-1e30f,-1e30f};
            std::vector<float> mv(n * 3);
            for (size_t v = 0; v < n; v++) {
                const float* src = (const float*)(pt.ib.data() + v * 40);
                float out[3];
                permute_xyz(src, pt.perm, out);
                mv[v*3+0] = out[0]; mv[v*3+1] = out[1]; mv[v*3+2] = out[2];
                for (int c = 0; c < 3; c++) {
                    if (out[c] < mmin[c]) mmin[c] = out[c];
                    if (out[c] > mmax[c]) mmax[c] = out[c];
                }
            }
            float rspan = rmax[0] - rmin[0], mspan = mmax[0] - mmin[0];
            float sc = (mspan > 1e-6f && rspan > 1e-6f) ? (rspan / mspan) : 1.0f;
            float offY = 0.5f * (rmin[1] + rmax[1]) - 0.5f * (mmin[1] + mmax[1]) * sc;
            float offZ = 0.5f * (rmin[2] + rmax[2]) - 0.5f * (mmin[2] + mmax[2]) * sc;
            LOGI("vbstatic bbox ref X %.3f..%.3f Y %.3f..%.3f Z %.3f..%.3f", rmin[0],rmax[0],rmin[1],rmax[1],rmin[2],rmax[2]);
            LOGI("vbstatic bbox mod X %.3f..%.3f Y %.3f..%.3f Z %.3f..%.3f scale=%.4f", mmin[0],mmax[0],mmin[1],mmax[1],mmin[2],mmax[2], sc);

            // 新顶点数可能大于原版: 其余属性按模循环铺满, 再覆盖 mod 的位置
            std::vector<uint8_t> outb;
            if (newVcount > vcount) {
                outb.assign(off + (size_t)newVcount * (size_t)a.stride, 0);
                for (uint32_t v = 0; v < newVcount; v++)
                    memcpy(outb.data() + off + (size_t)v * (size_t)a.stride,
                           cur.data() + off + (size_t)(v % vcount) * (size_t)a.stride,
                           (size_t)a.stride);
            } else {
                outb = cur;
            }
            for (size_t v = 0; v < n; v++) {
                float* dst = (float*)(outb.data() + off + v * (size_t)a.stride);
                dst[0] = (mv[v*3+0] - mmin[0]) * sc + rmin[0];
                dst[1] = mv[v*3+1] * sc + offY;
                dst[2] = mv[v*3+2] * sc + offZ;
            }
            // 最近邻映射: mod 顶点 -> 安卓参考顶点
            const float CELL = 0.06f;
            std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
            auto cellkey = [&](float x, float y, float z) -> uint64_t {
                int ix = (int)floorf((x - rmin[0]) / CELL);
                int iy = (int)floorf((y - rmin[1]) / CELL);
                int iz = (int)floorf((z - rmin[2]) / CELL);
                if (ix < 0) ix = 0; if (iy < 0) iy = 0; if (iz < 0) iz = 0;
                if (ix > 1023) ix = 1023; if (iy > 1023) iy = 1023; if (iz > 1023) iz = 1023;
                return ((uint64_t)ix << 20) | ((uint64_t)iy << 10) | (uint64_t)iz;
            };
            pt.dynRef.assign((size_t)vcount * 3, 0.0f);
            for (uint32_t v = 0; v < vcount; v++) {
                const float* p = (const float*)(cur.data() + off + (size_t)v * (size_t)a.stride);
                pt.dynRef[v*3+0] = p[0]; pt.dynRef[v*3+1] = p[1]; pt.dynRef[v*3+2] = p[2];
                grid[cellkey(p[0], p[1], p[2])].push_back(v);
            }
            pt.dynMap.assign(n, 0);
            size_t miss = 0;
            for (size_t v = 0; v < n; v++) {
                const float* q = &mv[v*3];
                float qx = (q[0] - mmin[0]) * sc + rmin[0];
                float qy = q[1] * sc + offY;
                float qz = q[2] * sc + offZ;
                int cx = (int)floorf((qx - rmin[0]) / CELL);
                int cy = (int)floorf((qy - rmin[1]) / CELL);
                int cz = (int)floorf((qz - rmin[2]) / CELL);
                float best = 1e30f; uint32_t bi = 0; bool ok = false;
                for (int dx = -2; dx <= 2; dx++) for (int dy = -2; dy <= 2; dy++) for (int dz = -2; dz <= 2; dz++) {
                    int ix = cx+dx, iy = cy+dy, iz = cz+dz;
                    if (ix < 0 || iy < 0 || iz < 0 || ix > 1023 || iy > 1023 || iz > 1023) continue;
                    uint64_t k = ((uint64_t)ix << 20) | ((uint64_t)iy << 10) | (uint64_t)iz;
                    auto git = grid.find(k);
                    if (git == grid.end()) continue;
                    for (uint32_t ri : git->second) {
                        float ddx = pt.dynRef[ri*3+0]-qx, ddy = pt.dynRef[ri*3+1]-qy, ddz = pt.dynRef[ri*3+2]-qz;
                        float d2 = ddx*ddx + ddy*ddy + ddz*ddz;
                        if (d2 < best) { best = d2; bi = ri; ok = true; }
                    }
                }
                if (!ok) miss++;
                pt.dynMap[v] = bi;
            }
            LOGI("vbstatic nn map done: %zu verts, %zu unmatched", n, miss);
            // mod 顶点比原版多时, 多出来的顶点没有对应权重:
            // 用最近邻映射把原版同一 buffer 里的非位置属性(骨骼索引/权重、法线等)搬过来,
            // 否则后半段顶点会按循环复制的权重乱飞.
            if (newVcount > vcount) {
                for (int ai = 0; ai < 16; ai++) {
                    if (!la[ai].enabled || ai == pos) continue;
                    if (la[ai].buf != a.buf) continue;
                    const size_t ap = (size_t)(uintptr_t)la[ai].ptr;
                    size_t bytes = 0;
                    switch (la[ai].type) {
                        case GL_BYTE: case GL_UNSIGNED_BYTE:
                            bytes = (size_t)la[ai].size; break;
                        case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT:
                            bytes = (size_t)la[ai].size * 2; break;
                        case GL_FLOAT:
                            bytes = (size_t)la[ai].size * 4; break;
                        default: continue;
                    }
                    if (!bytes || ap + bytes > (size_t)a.stride) continue;
                    for (size_t v = 0; v < n; v++) {
                        uint32_t ri = pt.dynMap[v];
                        if (ri >= vcount) ri = 0;
                        memcpy(outb.data() + ap + v * (size_t)a.stride,
                               cur.data() + ap + (size_t)ri * (size_t)a.stride, bytes);
                    }
                    LOGI("vbstatic carry attr%d (%zu bytes) via nn map", ai, bytes);
                }
            }
            pt.refCenter[0] = pt.refCenter[1] = pt.refCenter[2] = 0.0f;
            {
                size_t cntr = 0;
                for (uint32_t v = 0; v < vcount; v++) {
                    const float* p = (const float*)(cur.data() + off + (size_t)v * (size_t)a.stride);
                    pt.refCenter[0] += p[0]; pt.refCenter[1] += p[1]; pt.refCenter[2] += p[2];
                    cntr++;
                }
                if (cntr) { pt.refCenter[0] /= cntr; pt.refCenter[1] /= cntr; pt.refCenter[2] /= cntr; }
                LOGI("vbstatic ref center %.3f %.3f %.3f", pt.refCenter[0], pt.refCenter[1], pt.refCenter[2]);
            }

            // mode=5: 用 mod 空间 KNN + 安卓最近邻做局部刚体拟合,
            // 把 A-pose 一次性掰成安卓的参考姿势 (shape matching)。
            if (pt.mode == 5) {
                const float MC = 0.05f;
                std::unordered_map<uint64_t, std::vector<uint32_t>> mgrid;
                auto mkey = [MC](float x, float y, float z) -> uint64_t {
                    int ix=(int)floorf(x/MC), iy=(int)floorf(y/MC), iz=(int)floorf(z/MC);
                    if (ix<0)ix=0; if(iy<0)iy=0; if(iz<0)iz=0;
                    if (ix>1023)ix=1023; if(iy>1023)iy=1023; if(iz>1023)iz=1023;
                    return ((uint64_t)ix<<20)|((uint64_t)iy<<10)|(uint64_t)iz;
                };
                for (size_t v = 0; v < n; v++)
                    mgrid[mkey(mv[v*3],mv[v*3+1],mv[v*3+2])].push_back((uint32_t)v);
                const int K = 8;
                std::vector<float> fA((size_t)K*3), fB((size_t)K*3), fW((size_t)K);
                std::vector<std::pair<float,uint32_t>> cand;
                size_t okCnt = 0;
                for (size_t v = 0; v < n; v++) {
                    float px=mv[v*3], py=mv[v*3+1], pz=mv[v*3+2];
                    int cx=(int)floorf(px/MC), cy=(int)floorf(py/MC), cz=(int)floorf(pz/MC);
                    cand.clear();
                    for (int dx=-2;dx<=2;dx++) for (int dy=-2;dy<=2;dy++) for (int dz=-2;dz<=2;dz++) {
                        int ix=cx+dx, iy=cy+dy, iz=cz+dz;
                        if (ix<0||iy<0||iz<0||ix>1023||iy>1023||iz>1023) continue;
                        auto g2 = mgrid.find(((uint64_t)ix<<20)|((uint64_t)iy<<10)|(uint64_t)iz);
                        if (g2 == mgrid.end()) continue;
                        for (uint32_t j : g2->second) {
                            float ddx=mv[j*3]-px, ddy=mv[j*3+1]-py, ddz=mv[j*3+2]-pz;
                            cand.push_back(std::make_pair(ddx*ddx+ddy*ddy+ddz*ddz, j));
                        }
                    }
                    if (cand.size() < 4) continue;
                    std::sort(cand.begin(), cand.end());
                    int m = 0;
                    for (size_t i = 0; i < cand.size() && m < K; i++) {
                        uint32_t j = cand[i].second;
                        uint32_t ri = pt.dynMap[j];
                        fA[m*3+0]=mv[j*3]; fA[m*3+1]=mv[j*3+1]; fA[m*3+2]=mv[j*3+2];
                        fB[m*3+0]=pt.dynRef[ri*3+0]; fB[m*3+1]=pt.dynRef[ri*3+1]; fB[m*3+2]=pt.dynRef[ri*3+2];
                        float dd = sqrtf(cand[i].first);
                        fW[m] = 1.0f/(1.0f + dd*dd*400.0f);
                        m++;
                    }
                    if (m < 4) continue;
                    float R9[9], ca[3], cb[3];
                    fit_rigid(fA.data(), fB.data(), fW.data(), m, R9, ca, cb);
                    float d0=mv[v*3]-ca[0], d1=mv[v*3+1]-ca[1], d2=mv[v*3+2]-ca[2];
                    float nx=R9[0]*d0+R9[1]*d1+R9[2]*d2+cb[0];
                    float ny=R9[3]*d0+R9[4]*d1+R9[5]*d2+cb[1];
                    float nz=R9[6]*d0+R9[7]*d1+R9[8]*d2+cb[2];
                    if (!(fabsf(nx)<10.0f && fabsf(ny)<10.0f && fabsf(nz)<10.0f)) continue;
                    float* dst = (float*)(outb.data() + off + v*(size_t)a.stride);
                    dst[0]=nx; dst[1]=ny; dst[2]=nz;
                    okCnt++;
                }
                LOGI("vbstatic pose fit: %zu/%zu verts posed", okCnt, n);
            }
            o_glGenBuffers(1, &pt.glBuf);
            o_glBindBuffer(GL_ARRAY_BUFFER, pt.glBuf);
            o_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)outb.size(), outb.data(), GL_DYNAMIC_DRAW);
            o_glBindBuffer(GL_ARRAY_BUFFER, savedArr);
            pt.dynBuf = outb;
            pt.dynBase.assign(n * 3, 0.0f);
            for (size_t v = 0; v < n; v++) {
                const float* d = (const float*)(pt.dynBuf.data() + off + v * (size_t)a.stride);
                pt.dynBase[v*3+0] = d[0]; pt.dynBase[v*3+1] = d[1]; pt.dynBase[v*3+2] = d[2];
            }
            LOGI("vbstatic built buf=%u verts=%u/%u orig=%u stride=%d", pt.glBuf, (unsigned)n, newVcount, vcount, a.stride);
            if (!pt.dds.empty()) {
                typedef void (*PFNGENTEX)(GLsizei, GLuint*);
                typedef void (*PFNTEXPARAM)(GLenum, GLenum, GLint);
                static PFNGENTEX pgen = nullptr; static PFNTEXPARAM pparam = nullptr;
                if (!pgen) pgen = (PFNGENTEX)gl_real("glGenTextures");
                if (!pparam) pparam = (PFNTEXPARAM)gl_real("glTexParameteri");
                if (!o_glTexImage2D) o_glTexImage2D = (decltype(&glTexImage2D))gl_real("glTexImage2D");
                if (!o_glBindTexture) o_glBindTexture = (decltype(&glBindTexture))gl_real("glBindTexture");
                if (pgen && o_glBindTexture && o_glTexImage2D) {
                    GLuint nt = 0;
                    pgen(1, &nt);
                    GLint prev = 0;
                    o_glBindTexture(GL_TEXTURE_2D, nt);
                    o_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pt.ddsW, pt.ddsH, 0,
                                   GL_RGBA, GL_UNSIGNED_BYTE, pt.dds.data());
                    typedef void (*PFNGENMIP)(GLenum);
                    static PFNGENMIP pmip = nullptr;
                    if (!pmip) pmip = (PFNGENMIP)gl_real("glGenerateMipmap");
                    if (pmip) pmip(GL_TEXTURE_2D);
                    if (pparam) {
                        pparam(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                        pparam(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        pparam(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                        pparam(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                    }
                    (void)prev;
                    g_modTex = nt;
                    LOGI("vbstatic mod texture id=%u %dx%d", nt, pt.ddsW, pt.ddsH);
                }
            }
            g_vbBinds.clear();
            // UV 替换: mod 的 Texcoord stride 因角色而异 (身体 20, 头发 8)
            if (!pt.blend.empty()) {
                // 用顶点数反推源 stride, 整除才可信, 否则回退 20
                size_t uvSrcStride = 20;
                if (modVerts && (pt.blend.size() % modVerts) == 0) {
                    size_t s = pt.blend.size() / modVerts;
                    if (s >= 8) uvSrcStride = s;
                }
                const size_t nuv = pt.blend.size() / uvSrcStride;
                LOGI("vbstatic uv src stride=%zu verts=%zu", uvSrcStride, nuv);
                for (int i = 0; i < 16; i++) {
                    if (!la[i].enabled || la[i].size != 2) continue;
                    const bool u16  = (la[i].type == GL_UNSIGNED_SHORT);
                    const bool half = (la[i].type == GL_HALF_FLOAT);
                    if (!u16 && !half && la[i].type != GL_FLOAT) continue;
                    if (!la[i].buf || la[i].stride <= 0) continue;
                    const size_t uoff = (size_t)(uintptr_t)la[i].ptr;
                    // 单套 UV (源 stride 8) 从 0 开始; 双套 (源 stride 20) 按属性偏移选
                    const size_t srcOff = (uvSrcStride >= 16) ? ((uoff >= 12) ? 12 : 4) : 0;
                    const size_t wbytes = (u16 || half) ? 4 : 8;
                    if (uoff + wbytes > (size_t)la[i].stride) continue;
                    // 注意: uoff 是 stride 内部偏移, 不能加到总长度上,
                    // 否则 map 长度超过 buffer 实际大小, glMapBufferRange 直接返回 NULL
                    const size_t unw = (size_t)vcount * (size_t)la[i].stride;
                    if (unw > (16u << 20)) continue;
                    GLuint sv = g_curArray;
                    o_glBindBuffer(GL_ARRAY_BUFFER, la[i].buf);
                    void* up = o_glMapBufferRange ? o_glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)unw, GL_MAP_READ_BIT) : nullptr;
                    if (!up) { LOGW("vbstatic uv map fail attr=%d len=%zu", i, unw); o_glBindBuffer(GL_ARRAY_BUFFER, sv); continue; }
                    std::vector<uint8_t> ub((const uint8_t*)up, (const uint8_t*)up + unw);
                    if (o_glUnmapBuffer) o_glUnmapBuffer(GL_ARRAY_BUFFER);
                    o_glBindBuffer(GL_ARRAY_BUFFER, sv);
                    // 顶点数扩展: 原 UV 按模循环铺满新长度
                    if (newVcount > vcount) {
                        std::vector<uint8_t> ub2((size_t)newVcount * (size_t)la[i].stride, 0);
                        for (uint32_t v = 0; v < newVcount; v++)
                            memcpy(ub2.data() + (size_t)v * (size_t)la[i].stride,
                                   ub.data() + (size_t)(v % vcount) * (size_t)la[i].stride,
                                   (size_t)la[i].stride);
                        ub.swap(ub2);
                    }
                    const size_t cnt = std::min((size_t)newVcount, nuv);
                    for (size_t v = 0; v < cnt; v++) {
                        const float* sp = (const float*)(pt.blend.data() + v * uvSrcStride + srcOff);
                        uint8_t* dp = ub.data() + uoff + v * (size_t)la[i].stride;
                        float uv[2];
                        if (pt.flipV == 2) { uv[0] = 0.5f; uv[1] = 0.5f; }
                        else if (pt.flipV == 3) { uv[0] = 0.02f; uv[1] = 0.02f; }
                        else { uv[0] = sp[0]; uv[1] = pt.flipV ? (1.0f - sp[1]) : sp[1]; }
                        if (half) {
                            // float -> half, UV 限定在 0..1 不会溢出
                            uint16_t* d16 = (uint16_t*)dp;
                            for (int k = 0; k < 2; k++) {
                                float t = uv[k];
                                if (!(t > 0.0f)) t = 0.0f;
                                if (t > 1.0f) t = 1.0f;
                                uint32_t x; memcpy(&x, &t, 4);
                                uint32_t sg = (x >> 16) & 0x8000u;
                                int32_t e = (int32_t)((x >> 23) & 0xffu) - 112;
                                uint32_t m = x & 0x7fffffu;
                                if (e <= 0) d16[k] = (uint16_t)sg;
                                else if (e >= 31) d16[k] = (uint16_t)(sg | 0x7c00u);
                                else d16[k] = (uint16_t)(sg | ((uint32_t)e << 10) | (m >> 13));
                            }
                        } else if (u16) {
                            // 量化 UV: 0..1 映射到 0..65535
                            uint16_t* d16 = (uint16_t*)dp;
                            for (int k = 0; k < 2; k++) {
                                float t = uv[k];
                                if (!(t > 0.0f)) t = 0.0f;
                                if (t > 1.0f) t = 1.0f;
                                d16[k] = (uint16_t)(t * 65535.0f + 0.5f);
                            }
                        } else {
                            ((float*)dp)[0] = uv[0];
                            ((float*)dp)[1] = uv[1];
                        }
                    }
                    // 同一 buffer 里通常还塞着法线/切线(安卓把它们和 UV 交错在一起).
                    // 只换 UV 会让法线停在原版顶点数上, 索引越界后光照全乱.
                    // mod 的 Position.buf 布局: 位置(12) + 法线(12) + 切线(12) + 4
                    for (int j = 0; j < 16; j++) {
                        if (!la[j].enabled || j == i) continue;
                        if (la[j].buf != la[i].buf || la[j].size < 3) continue;
                        if (la[j].type != GL_UNSIGNED_BYTE && la[j].type != GL_BYTE) continue;
                        const size_t jp = (size_t)(uintptr_t)la[j].ptr;
                        if (jp + 3 > (size_t)la[j].stride) continue;
                        // 法线排在 UV 前面, 切线排在后面
                        const size_t nOff = (jp < uoff) ? 12 : 24;
                        const bool sgn = (la[j].type == GL_BYTE);
                        for (size_t v = 0; v < n; v++) {
                            const float* nv = (const float*)(pt.ib.data() + v * 40 + nOff);
                            uint8_t* dp = ub.data() + jp + v * (size_t)la[j].stride;
                            for (int k = 0; k < 3; k++) {
                                float t = nv[k];
                                if (!(t > -1.0f)) t = -1.0f;
                                if (t > 1.0f) t = 1.0f;
                                dp[k] = sgn ? (uint8_t)(int8_t)(t * 127.0f)
                                            : (uint8_t)((t * 0.5f + 0.5f) * 255.0f + 0.5f);
                            }
                        }
                        LOGI("vbstatic normal attr%d off=%zu src=mod+%zu", j, jp, nOff);
                    }
                    GLuint nub = 0;
                    o_glGenBuffers(1, &nub);
                    o_glBindBuffer(GL_ARRAY_BUFFER, nub);
                    o_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)ub.size(), ub.data(), GL_STATIC_DRAW);
                    o_glBindBuffer(GL_ARRAY_BUFFER, sv);
                    pt.uvBinds.push_back({i, nub});
                    { VbBind vb; vb.idx = i; vb.newBuf = nub; g_vbBinds.push_back(vb); }
                    LOGI("vbstatic uv attr=%d off=%zu stride=%d verts=%zu buf=%u flipV=%d", i, uoff, la[i].stride, cnt, nub, pt.flipV);
                }
            }
            // 和位置共用同一个 buffer 的属性(骨骼索引/权重、法线)一起换到新 buffer,
            // 否则位置和权重会分家, 蒙皮结果完全错乱
            for (int i = 0; i < 16; i++) {
                if (!la[i].enabled || la[i].buf != a.buf) continue;
                VbBind vb; vb.idx = i; vb.newBuf = pt.glBuf; g_vbBinds.push_back(vb);
            }
            continue;
        }
        if (pt.action == 12) {
            LiveAttr la[16];
            capture_vao(la);
            int pos = -1;
            for (int i = 0; i < 16; i++) {
                if (la[i].enabled && la[i].size >= 3 && la[i].type == GL_FLOAT) { pos = i; break; }
            }
            uint32_t vcount = 0;
            if (it != g_bufs.end() && !it->second.data.empty()) {
                const uint16_t* ip = (const uint16_t*)it->second.data.data();
                size_t in = it->second.data.size() / 2;
                for (size_t k = 0; k < in; k++) if ((uint32_t)ip[k] + 1u > vcount) vcount = (uint32_t)ip[k] + 1u;
            }
            if (pos < 0 || !vcount || pt.ib.size() < 40 || !o_glMapBufferRange || !o_glBufferData) continue;
            const LiveAttr& a = la[pos];
            if (!a.buf || a.stride <= 0) continue;
            const size_t off = (size_t)(uintptr_t)a.ptr;
            const size_t want = off + (size_t)vcount * (size_t)a.stride;
            if (want > (16u << 20)) continue;
            GLuint savedArr = g_curArray;
            o_glBindBuffer(GL_ARRAY_BUFFER, a.buf);
            void* mp = o_glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)want, GL_MAP_READ_BIT);
            if (!mp) { o_glBindBuffer(GL_ARRAY_BUFFER, savedArr); continue; }
            std::vector<uint8_t> cur((const uint8_t*)mp, (const uint8_t*)mp + want);
            if (o_glUnmapBuffer) o_glUnmapBuffer(GL_ARRAY_BUFFER);
            o_glBindBuffer(GL_ARRAY_BUFFER, savedArr);

            const size_t srcN = pt.ib.size() / 40;
            const size_t n = std::min((size_t)vcount, srcN);
            if (pt.perm < 0) {
                static std::chrono::steady_clock::time_point dlast;
                auto dnow = std::chrono::steady_clock::now();
                if (dlast.time_since_epoch().count() == 0) dlast = dnow;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(dnow - dlast).count() >= 2500) {
                    dlast = dnow;
                    pt.perm = (pt.perm == -1) ? 0 : ((pt.perm + 1) % 6);
                    pt.dynReady = false;
                    LOGI("vbmoddyn CYCLE perm=%d", pt.perm);
                }
            }
            // Unity may orphan/reallocate the source VBO every frame.  A new
            // source buffer id is still the same skinned stream, so keep the
            // reference pose across buffer ids and only reset on layout/count changes.
            if (!pt.dynReady || pt.dynStride != a.stride || pt.dynOffset != off ||
                pt.dynCount != vcount) {
                pt.dynReady = true;
                pt.dynPosAttr = pos;
                pt.dynSrcBuf = a.buf;
                pt.dynStride = a.stride;
                pt.dynOffset = off;
                pt.dynCount = vcount;
                pt.dynRef.assign((size_t)vcount * 3, 0.0f);
                pt.dynBuf = cur;
                for (size_t v = 0; v < n; v++) {
                    const float* now = (const float*)(cur.data() + off + v * (size_t)a.stride);
                    float* ref = pt.dynRef.data() + v * 3;
                    ref[0] = now[0]; ref[1] = now[1]; ref[2] = now[2];
                    const float* src = (const float*)(pt.ib.data() + v * 40);
                    float mod[3]; permute_xyz(src, pt.perm, mod);
                    float* dst = (float*)(pt.dynBuf.data() + off + v * (size_t)a.stride);
                    dst[0] = mod[0]; dst[1] = mod[1]; dst[2] = mod[2];
                }
                o_glGenBuffers(1, &pt.glBuf);
                o_glBindBuffer(GL_ARRAY_BUFFER, pt.glBuf);
                o_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)pt.dynBuf.size(), pt.dynBuf.data(), GL_DYNAMIC_DRAW);
                o_glBindBuffer(GL_ARRAY_BUFFER, savedArr);
                LOGI("vbmoddyn init attr=%d verts=%u stride=%d perm=%d buf=%u", pos, vcount, a.stride, pt.perm, pt.glBuf);
            } else {
                for (size_t v = 0; v < n; v++) {
                    const float* now = (const float*)(cur.data() + off + v * (size_t)a.stride);
                    const float* ref = pt.dynRef.data() + v * 3;
                    const float* src = (const float*)(pt.ib.data() + v * 40);
                    float mod[3]; permute_xyz(src, pt.perm, mod);
                    float* dst = (float*)(pt.dynBuf.data() + off + v * (size_t)a.stride);
                    dst[0] = mod[0] + (now[0] - ref[0]);
                    dst[1] = mod[1] + (now[1] - ref[1]);
                    dst[2] = mod[2] + (now[2] - ref[2]);
                }
                if (o_glBufferSubData) {
                    o_glBindBuffer(GL_ARRAY_BUFFER, pt.glBuf);
                    o_glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)pt.dynBuf.size(), pt.dynBuf.data());
                    o_glBindBuffer(GL_ARRAY_BUFFER, savedArr);
                }
            }
            g_vbBinds.clear();
            { VbBind vb; vb.idx = pt.dynPosAttr; vb.newBuf = pt.glBuf; g_vbBinds.push_back(vb); }
            continue;
        }
        if (pt.action == 10) {
            if (pt.done) {
                if (pt.perm < 0) {
                    // 轮换模式: 每 2 秒换一种轴置换（按真实时间，不受帧率影响）
                    static std::chrono::steady_clock::time_point last;
                    auto now = std::chrono::steady_clock::now();
                    if (last.time_since_epoch().count() == 0) last = now;
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() >= 2000) {
                        last = now;
                        pt.perm = (pt.perm + 1) % 6;
                        pt.done = false;
                        LOGI("vbmod CYCLE perm=%d", pt.perm);
                    } else {
                        g_vbBinds.clear();
                        for (auto& pr : pt.binds) { VbBind vb; vb.idx = pr.first; vb.newBuf = pr.second; g_vbBinds.push_back(vb); }
                        continue;
                    }
                } else {
                    g_vbBinds.clear();
                    for (auto& pr : pt.binds) { VbBind vb; vb.idx = pr.first; vb.newBuf = pr.second; g_vbBinds.push_back(vb); }
                    continue;
                }
            }
            pt.done = true;
            pt.binds.clear();
            if (pt.ib.empty() || pt.blend.empty()) continue;
            LiveAttr la[16];
            capture_vao(la);
            uint32_t vcount = 0;
            if (it != g_bufs.end() && !it->second.data.empty()) {
                const uint16_t* ip = (const uint16_t*)it->second.data.data();
                size_t in = it->second.data.size() / 2;
                for (size_t k = 0; k < in; k++) if (ip[k] + 1u > vcount) vcount = ip[k] + 1u;
            }
            if (!vcount) continue;

            // 分类属性
            int iPos = -1, iNrm = -1, iTan = -1, iBld = -1, iUv0 = -1, iUv1 = -1;
            for (int i = 0; i < 16; i++) {
                if (!la[i].enabled) continue;
                if (la[i].size == 3 && la[i].type == GL_FLOAT && iPos < 0) iPos = i;
                else if (la[i].size == 4 && (la[i].type == GL_BYTE || la[i].type == GL_UNSIGNED_BYTE)) {
                    if (iNrm < 0) iNrm = i; else if (iTan < 0) iTan = i;
                } else if (la[i].size == 2 && la[i].type == GL_FLOAT) {
                    if (iUv0 < 0) iUv0 = i; else if (iUv1 < 0) iUv1 = i;
                }
            }
            LOGI("vbmod attrs pos=%d nrm=%d tan=%d uv0=%d uv1=%d vcount=%u", iPos, iNrm, iTan, iUv0, iUv1, vcount);
            // 只处理主渲染 pass（有 UV 的），阴影/深度 pass 保持原样
            if (iPos < 0 || iUv0 < 0) { pt.done = false; continue; }

            // 按缓冲分组
            std::map<GLuint, std::vector<int>> groups;
            for (int i = 0; i < 16; i++) if (la[i].enabled && la[i].buf) groups[la[i].buf].push_back(i);

            const size_t nPos = pt.ib.size() / 40;
            const size_t nBld = pt.blend.size() / 32;
            const size_t nTex = pt.tex.empty() ? 0 : pt.tex.size() / 20;
            const size_t nUse = std::min((size_t)vcount, nPos);
            pt.binds.clear();

            for (auto& g : groups) {
                GLsizei maxStride = 0;
                for (int i : g.second) maxStride = std::max(maxStride, la[i].stride);
                if (maxStride <= 0) continue;
                size_t want = (size_t)vcount * (size_t)maxStride;
                if (want > (16u << 20)) continue;
                GLuint savedArr = g_curArray;
                o_glBindBuffer(GL_ARRAY_BUFFER, g.first);
                void* mp = o_glMapBufferRange
                               ? o_glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)want, GL_MAP_READ_BIT)
                               : nullptr;
                if (!mp) { o_glBindBuffer(GL_ARRAY_BUFFER, savedArr); LOGW("vbmod map fail buf %u", g.first); continue; }
                std::vector<uint8_t> buf((const uint8_t*)mp, (const uint8_t*)mp + want);
                if (o_glUnmapBuffer) o_glUnmapBuffer(GL_ARRAY_BUFFER);
                o_glBindBuffer(GL_ARRAY_BUFFER, savedArr);

                for (int i : g.second) {
                    const GLsizei st = la[i].stride;
                    const size_t off = (size_t)(uintptr_t)la[i].ptr;
                    if (i == iPos) {
                        for (size_t v = 0; v < nUse; v++) {
                            if ((size_t)st < off + 12) break;
                            const float* src = (const float*)(pt.ib.data() + v * 40);
                            float* dst = (float*)(buf.data() + v * (size_t)st + off);
                            switch (pt.perm) {
                                case 1: dst[0]=src[1]; dst[1]=src[0]; dst[2]=src[2]; break;
                                case 2: dst[0]=src[1]; dst[1]=src[2]; dst[2]=src[0]; break;
                                case 3: dst[0]=src[2]; dst[1]=src[1]; dst[2]=src[0]; break;
                                case 4: dst[0]=src[2]; dst[1]=src[0]; dst[2]=src[1]; break;
                                case 5: dst[0]=src[0]; dst[1]=src[2]; dst[2]=src[1]; break;
                                default: dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; break;
                            }
                        }
                        LOGI("vbmod pos: %zu verts perm=%d", nUse, pt.perm);
                    } else if (i == iNrm) {
                        for (size_t v = 0; v < nUse; v++) {
                            if ((size_t)st < off + 4) break;
                            const float* f0 = (const float*)(pt.ib.data() + v * 40 + 12);
                            float f[3] = {f0[0], f0[1], f0[2]};
                            if (pt.perm == 1 || pt.perm == 2) { f[0]=f0[1]; f[1]=f0[0]; f[2]=f0[2]; }
                            else if (pt.perm == 3) { f[0]=f0[2]; f[1]=f0[1]; f[2]=f0[0]; }
                            else if (pt.perm == 4) { f[0]=f0[2]; f[1]=f0[0]; f[2]=f0[1]; }
                            else if (pt.perm == 5) { f[0]=f0[0]; f[1]=f0[2]; f[2]=f0[1]; }
                            int8_t* d = (int8_t*)(buf.data() + v * (size_t)st + off);
                            for (int c = 0; c < 3; c++) {
                                float t = f[c] * 127.0f;
                                d[c] = (int8_t)(t > 127.f ? 127 : (t < -127.f ? -127 : (int)lrintf(t)));
                            }
                            d[3] = 0;
                        }
                        LOGI("vbmod nrm: %zu verts", nUse);
                    } else if (i == iTan) {
                        for (size_t v = 0; v < nUse; v++) {
                            if ((size_t)st < off + 4) break;
                            const float* f0 = (const float*)(pt.ib.data() + v * 40 + 24);
                            float f[4] = {f0[0], f0[1], f0[2], f0[3]};
                            if (pt.perm == 1 || pt.perm == 2) { f[0]=f0[1]; f[1]=f0[0]; f[2]=f0[2]; }
                            else if (pt.perm == 3) { f[0]=f0[2]; f[1]=f0[1]; f[2]=f0[0]; }
                            else if (pt.perm == 4) { f[0]=f0[2]; f[1]=f0[0]; f[2]=f0[1]; }
                            else if (pt.perm == 5) { f[0]=f0[0]; f[1]=f0[2]; f[2]=f0[1]; }
                            int8_t* d = (int8_t*)(buf.data() + v * (size_t)st + off);
                            for (int c = 0; c < 3; c++) {
                                float t = f[c] * 127.0f;
                                d[c] = (int8_t)(t > 127.f ? 127 : (t < -127.f ? -127 : (int)lrintf(t)));
                            }
                            d[3] = (int8_t)(f[3] < 0.f ? -127 : 127);
                        }
                        LOGI("vbmod tan: %zu verts", nUse);
                    } else if (i == iUv0 || i == iUv1) {
                        if (!nTex) continue;
                        size_t base = (i == iUv0) ? 0 : 8;
                        size_t nt = std::min(nUse, nTex);
                        for (size_t v = 0; v < nt; v++) {
                            if ((size_t)st < off + 8) break;
                            memcpy(buf.data() + v * (size_t)st + off, pt.tex.data() + v * 20 + base, 8);
                        }
                        LOGI("vbmod uv%d: %zu verts", i == iUv0 ? 0 : 1, nt);
                    }
                }

                GLuint nb = 0;
                o_glGenBuffers(1, &nb);
                o_glBindBuffer(GL_ARRAY_BUFFER, nb);
                o_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)buf.size(), buf.data(), GL_STATIC_DRAW);
                o_glBindBuffer(GL_ARRAY_BUFFER, savedArr);
                for (int i : g.second) pt.binds.push_back({i, nb});
                LOGI("vbmod buf %u -> %u (%zu bytes, %zu attrs)", g.first, nb, buf.size(), g.second.size());
            }

            g_vbBinds.clear();
            for (auto& pr : pt.binds) { VbBind vb; vb.idx = pr.first; vb.newBuf = pr.second; g_vbBinds.push_back(vb); }
            continue;
        }
        if (pt.action == 9) {
            if (pt.done) continue;
            pt.done = true;
            auto vit = g_vaos.find(g_curVao);
            if (vit == g_vaos.end()) { LOGI("vbbox: no vao"); continue; }
            const Attr& a = vit->second.attr[0];
            if (!a.has || !a.buf || a.stride <= 0) { LOGI("vbbox: attr0 missing"); continue; }
            uint32_t vcount = 0;
            if (it != g_bufs.end() && !it->second.data.empty()) {
                const uint16_t* ip = (const uint16_t*)it->second.data.data();
                size_t in = it->second.data.size() / 2;
                for (size_t k = 0; k < in; k++) if (ip[k] + 1u > vcount) vcount = ip[k] + 1u;
            }
            if (!vcount) { LOGI("vbbox: no ib data"); continue; }
            size_t want = (size_t)vcount * (size_t)a.stride;
            if (want > (16u << 20)) { LOGI("vbbox: too big %zu", want); continue; }
            GLuint savedArr = g_curArray;
            o_glBindBuffer(GL_ARRAY_BUFFER, a.buf);
            void* mp = o_glMapBufferRange
                           ? o_glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)want, GL_MAP_READ_BIT)
                           : nullptr;
            if (!mp) { o_glBindBuffer(GL_ARRAY_BUFFER, savedArr); LOGI("vbbox: map failed"); continue; }
            std::vector<uint8_t> buf((const uint8_t*)mp, (const uint8_t*)mp + want);
            if (o_glUnmapBuffer) o_glUnmapBuffer(GL_ARRAY_BUFFER);
            o_glBindBuffer(GL_ARRAY_BUFFER, savedArr);
            float mn[4] = {1e30f,1e30f,1e30f,1e30f}, mx[4] = {-1e30f,-1e30f,-1e30f,-1e30f};
            size_t off = (size_t)(uintptr_t)a.ptr, vs = (size_t)a.stride;
            int nc = a.size > 4 ? 4 : (int)a.size;
            for (size_t k = 0; k + off + 4 * (size_t)nc <= buf.size(); k += vs) {
                const float* f = (const float*)(buf.data() + k + off);
                for (int c = 0; c < nc; c++) {
                    if (!(f[c] == f[c])) continue;
                    if (f[c] < mn[c]) mn[c] = f[c];
                    if (f[c] > mx[c]) mx[c] = f[c];
                }
            }
            LOGI("BBOX %08llx first=%u cnt=%u vao=%u buf=%u n=%u stride=%d ptr=%p :: (%.3f %.3f %.3f %.3f) -> (%.3f %.3f %.3f %.3f)",
                 (unsigned long long)pt.hash, first, count, g_curVao, a.buf, vcount, a.stride, a.ptr,
                 mn[0], mn[1], mn[2], mn[3], mx[0], mx[1], mx[2], mx[3]);
            continue;
        }
        if (pt.action == 8) {
            if (pt.attrIdx >= 16) continue;
            LiveAttr la[16];
            capture_vao(la);
            if (!la[pt.attrIdx].enabled) continue;
            Attr a;
            a.has = true; a.size = la[pt.attrIdx].size; a.type = la[pt.attrIdx].type;
            a.stride = la[pt.attrIdx].stride; a.buf = la[pt.attrIdx].buf;
            a.ptr = la[pt.attrIdx].ptr; a.norm = la[pt.attrIdx].norm;
            if (!a.buf || a.stride <= 0) continue;
            if (!pt.glBuf) {
                uint32_t vcount = 0;
                if (it != g_bufs.end() && !it->second.data.empty()) {
                    const uint16_t* ip = (const uint16_t*)it->second.data.data();
                    size_t in = it->second.data.size() / 2;
                    for (size_t k = 0; k < in; k++) if (ip[k] + 1u > vcount) vcount = ip[k] + 1u;
                }
                if (!vcount) continue;
                size_t want = (size_t)vcount * (size_t)a.stride;
                if (want > (16u << 20)) continue;
                GLuint savedArr = g_curArray;
                o_glBindBuffer(GL_ARRAY_BUFFER, a.buf);
                void* mp = o_glMapBufferRange
                               ? o_glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)want, GL_MAP_READ_BIT)
                               : nullptr;
                if (!mp) { o_glBindBuffer(GL_ARRAY_BUFFER, savedArr); continue; }
                std::vector<uint8_t> buf((const uint8_t*)mp, (const uint8_t*)mp + want);
                if (o_glUnmapBuffer) o_glUnmapBuffer(GL_ARRAY_BUFFER);
                o_glBindBuffer(GL_ARRAY_BUFFER, savedArr);

                size_t dstOff = (size_t)(uintptr_t)a.ptr;
                size_t copy = (size_t)a.size * 4;   // 假设 float
                if (copy > 12) copy = 12;
                size_t srcN = pt.ib.size() / pt.srcStride;
                size_t n = std::min((size_t)vcount, srcN);
                for (size_t i = 0; i < n; i++) {
                    const float* sp = (const float*)(pt.ib.data() + i * pt.srcStride + pt.srcOffset);
                    float* dp = (float*)(buf.data() + i * (size_t)a.stride + dstOff);
                    if ((size_t)a.stride < dstOff + copy) continue;
                    switch (pt.perm) {
                        case 1: dp[0]=sp[1]; dp[1]=sp[0]; dp[2]=sp[2]; break;
                        case 2: dp[0]=sp[1]; dp[1]=sp[2]; dp[2]=sp[0]; break;
                        case 3: dp[0]=sp[2]; dp[1]=sp[1]; dp[2]=sp[0]; break;
                        case 4: dp[0]=sp[2]; dp[1]=sp[0]; dp[2]=sp[1]; break;
                        case 5: dp[0]=sp[0]; dp[1]=sp[2]; dp[2]=sp[1]; break;
                        default: dp[0]=sp[0]; dp[1]=sp[1]; dp[2]=sp[2]; break;
                    }
                }
                GLuint nb = 0;
                o_glGenBuffers(1, &nb);
                o_glBindBuffer(GL_ARRAY_BUFFER, nb);
                o_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)buf.size(), buf.data(), GL_STATIC_DRAW);
                o_glBindBuffer(GL_ARRAY_BUFFER, savedArr);
                pt.glBuf = nb;
                LOGI("vbpos: attr%u verts=%u/%zu stride=%d copy=%zu -> buf %u",
                     pt.attrIdx, vcount, n, a.stride, copy, nb);
            }
            g_vbBinds.clear();
            { VbBind vb; vb.idx = (int)pt.attrIdx; vb.newBuf = pt.glBuf; g_vbBinds.push_back(vb); }
            continue;
        }
        if (pt.action == 5) {
            if (!pt.glBuf && !pt.ib.empty() && o_glGenBuffers && o_glBindBuffer && o_glBufferData) {
                GLuint nb = 0;
                o_glGenBuffers(1, &nb);
                GLuint saved = cur_elem();
                o_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, nb);
                o_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)pt.ib.size(), pt.ib.data(),
                               GL_STATIC_DRAW);
                o_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, saved);
                pt.glBuf = nb;
                LOGI("ib buffer %u created for %08llx (%u idx)", nb,
                     (unsigned long long)pt.hash, pt.ibCount);
            }
            if (pt.glBuf) {
                g_overrideBuf = pt.glBuf;
                g_overrideCount = (GLsizei)(pt.drawCount ? pt.drawCount : pt.ibCount);
                g_overrideType = pt.ibType;
                over = true;
                c_overridden++;
            }
            continue;
        }
        if (pt.done) continue;

        std::string path = std::string(g_dir) + "/" + pt.file;
        if (pt.action == 1) {
            if (it == g_bufs.end() || it->second.data.empty()) continue;
            FILE* f = fopen(path.c_str(), "wb");
            if (f) {
                fwrite(it->second.data.data(), 1, it->second.data.size(), f);
                fclose(f);
                LOGI("dumped ib %s (%zu bytes)", path.c_str(), it->second.data.size());
                pt.done = true;
            }
        } else if (pt.action == 6) {
            // 只抓一次: 打印完整布局 + 每个属性流原始字节
            if (pt.done) continue;
            if (pt.hits++ < 3000) continue;
            pt.done = true;
            auto vit = g_vaos.find(g_curVao);
            if (vit == g_vaos.end()) continue;
            uint32_t vcount = 0;
            if (it != g_bufs.end() && !it->second.data.empty()) {
                const uint16_t* ip = (const uint16_t*)it->second.data.data();
                size_t in = it->second.data.size() / 2;
                for (size_t k = 0; k < in; k++) if (ip[k] + 1u > vcount) vcount = ip[k] + 1u;
            }
            std::string base = std::string(g_dir) + "/" + pt.file;
            std::string meta = base + ".txt";
            FILE* mf = fopen(meta.c_str(), "w");
            if (!mf) continue;
            fprintf(mf, "ib_hash=%08llx first=%u count=%u type=%x vao=%u verts=%u prog=%u\n",
                    (unsigned long long)wh1, first, (unsigned)count, type, g_curVao, vcount, g_curProg);
            LOGI("DUMP %08llx first=%u cnt=%u verts=%u vao=%u prog=%u",
                 (unsigned long long)wh1, first, (unsigned)count, vcount, g_curVao, g_curProg);
            LiveAttr la[16];
            capture_vao(la);
            for (int i = 0; i < 16; i++) {
                if (!la[i].enabled) continue;
                struct { GLint size; GLenum type; GLsizei stride; GLuint buf; const void* ptr; } a;
                a.size = la[i].size; a.type = la[i].type; a.stride = la[i].stride;
                a.buf = la[i].buf; a.ptr = la[i].ptr;
                GLint bufsz = 0;
                {
                    typedef void (*PFNGETBUF)(GLenum, GLenum, GLint*);
                    static PFNGETBUF pget = nullptr;
                    if (!pget) pget = (PFNGETBUF)gl_real("glGetBufferParameteriv");
                    if (pget) {
                        GLuint savedB = g_curArray;
                        o_glBindBuffer(GL_ARRAY_BUFFER, a.buf);
                        pget(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &bufsz);
                        o_glBindBuffer(GL_ARRAY_BUFFER, savedB);
                    }
                }
                fprintf(mf, "attr%d size=%d type=%x stride=%d buf=%u ptr=%p bufsize=%d\n",
                        i, a.size, a.type, a.stride, a.buf, a.ptr, bufsz);
                LOGI("  a%d size=%d type=%x stride=%d buf=%u ptr=%p bufsize=%d",
                     i, a.size, a.type, a.stride, a.buf, a.ptr, bufsz);
                if (!a.buf || a.stride <= 0 || !vcount) continue;
                size_t want = (size_t)vcount * (size_t)a.stride;
                if (want > (8u << 20)) { fprintf(mf, "  skip big\n"); continue; }
                GLuint savedArr = g_curArray;
                o_glBindBuffer(GL_ARRAY_BUFFER, a.buf);
                void* mp = o_glMapBufferRange
                               ? o_glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)(uintptr_t)a.ptr,
                                                    (GLsizeiptr)want, GL_MAP_READ_BIT)
                               : nullptr;
                if (!mp) { o_glBindBuffer(GL_ARRAY_BUFFER, savedArr); fprintf(mf, "  mapped=0\n"); continue; }
                char fn[256];
                snprintf(fn, sizeof fn, "%s.a%d.bin", base.c_str(), i);
                FILE* bf = fopen(fn, "wb");
                if (bf) { fwrite(mp, 1, want, bf); fclose(bf); }
                std::vector<uint8_t> copy((const uint8_t*)mp, (const uint8_t*)mp + want);
                if (o_glUnmapBuffer) o_glUnmapBuffer(GL_ARRAY_BUFFER);
                o_glBindBuffer(GL_ARRAY_BUFFER, savedArr);
                fprintf(mf, "  mapped=1 wrote=%zu\n", want);
                for (int v = 0; v < 2 && (size_t)(v + 1) * (size_t)a.stride <= copy.size(); v++) {
                    std::string hex;
                    char hb[4];
                    size_t lim = (size_t)a.stride < 32 ? (size_t)a.stride : 32;
                    for (size_t q = 0; q < lim; q++) {
                        snprintf(hb, sizeof hb, "%02x", copy[v * (size_t)a.stride + q]);
                        hex += hb;
                    }
                    fprintf(mf, "  v%d %s\n", v, hex.c_str());
                }
                if (i == 0 && a.size >= 3 && a.type == GL_FLOAT) {
                    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                    for (uint32_t k = 0; k < vcount; k++) {
                        const float* f = (const float*)(copy.data() + (size_t)k * (size_t)a.stride);
                        for (int c = 0; c < 3; c++) {
                            float v = f[c];
                            if (v != v || v > 1e6f || v < -1e6f) continue;
                            if (v < lo[c]) lo[c] = v;
                            if (v > hi[c]) hi[c] = v;
                        }
                    }
                    LOGI("  bbox %.3f %.3f %.3f -> %.3f %.3f %.3f", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
                }
            }
            fclose(mf);
            LOGI("dumped mesh %s verts=%u", base.c_str(), vcount);
        } else if (pt.action == 2) {
            auto vit = g_vaos.find(g_curVao);
            if (vit == g_vaos.end() || !vit->second.attr[0].has) continue;
            GLuint vb = vit->second.attr[0].buf;
            GLsizei stride = vit->second.attr[0].stride;
            auto bit = g_bufs.find(vb);
            if (bit == g_bufs.end() || bit->second.data.empty() || stride <= 0) continue;
            const uint8_t* base = bit->second.data.data();
            size_t nv = bit->second.data.size() / (size_t)stride;
            std::vector<float> o3;
            o3.reserve(nv * 3);
            for (size_t i = 0; i < nv; i++) {
                const float* f3 = (const float*)(base + i * (size_t)stride);
                o3.push_back(f3[0]); o3.push_back(f3[1]); o3.push_back(f3[2]);
            }
            FILE* f = fopen(path.c_str(), "wb");
            if (f) {
                fwrite(o3.data(), 4, o3.size(), f);
                fclose(f);
                LOGI("dumped vb %s nv=%zu stride=%d", path.c_str(), nv, stride);
                pt.done = true;
            }
        }
    }

    Key k{wh1, first, (uint32_t)count};
    auto it2 = g_recs.find(k);
    if (it2 == g_recs.end()) {
        if (g_recs.size() >= 60000) { if (skip) return 1; return over ? 2 : 0; }
        Rec r;
        r.vao = g_curVao; r.eb = eb; r.prog = g_curProg;
        r.type = type; r.mode = mode;
        r.h1 = h1; r.h1a = h1a; r.wh1a = wh1a;
        r.exact = exact;
        r.layout = layout_of(g_curVao);
        strncpy(r.kind, kind, sizeof(r.kind) - 1);
        it2 = g_recs.emplace(k, std::move(r)).first;
        g_unique++;
        if (g_unique <= 200) {
            LOGI("NEW%s ib=%08llx first=%u cnt=%u type=%x prog=%u %s",
                 kind, (unsigned long long)wh1, first, (unsigned)count, type, g_curProg,
                 it2->second.layout.c_str());
        }
    }
    it2->second.hits++;

    if (c_draw % 3000 == 0) {
        dump_stats();
        LOGI("STAT keys=%llu draws=%llu egl=%llu dlsym=%llu bindBuf=%llu vao=%llu genBuf=%llu bufData=%llu useProg=%llu attrib=%llu",
             (unsigned long long)g_recs.size(), (unsigned long long)c_draw.load(),
             (unsigned long long)c_egl.load(), (unsigned long long)c_dlsym.load(),
             (unsigned long long)c_bindBuf.load(), (unsigned long long)c_vao.load(),
             (unsigned long long)c_genBuf.load(), (unsigned long long)c_bufData.load(),
             (unsigned long long)c_useProg.load(), (unsigned long long)c_attrib.load());
        LOGI("subData=%llu", (unsigned long long)c_subData.load());
        if (c_skipped || c_overridden)
            LOGI("skipped=%llu overridden=%llu", (unsigned long long)c_skipped.load(),
                 (unsigned long long)c_overridden.load());
    }
    if (skip) return 1;
    return over ? 2 : 0;
}

void glDrawElements_hook(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    RESOLVE(o_glDrawElements, "glDrawElements");
    int act = on_draw("", mode, count, type, indices);
    if (act == 1) { g_vbBinds.clear(); return; }
    apply_vb();
    if (act == 2 && o_glBindBuffer) {
        GLuint saved = cur_elem();
        o_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_overrideBuf);
        o_glDrawElements(mode, g_overrideCount, g_overrideType, 0);
        o_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, saved);
    } else {
        o_glDrawElements(mode, count, type, indices);
    }
    restore_vb();
}
void glDrawElementsInstanced_hook(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei n) {
    RESOLVE(o_glDrawElementsInstanced, "glDrawElementsInstanced");
    int act = on_draw("I", mode, count, type, indices);
    if (act == 1) { g_vbBinds.clear(); return; }
    apply_vb();
    if (act == 2 && o_glBindBuffer) {
        GLuint saved = cur_elem();
        o_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_overrideBuf);
        o_glDrawElementsInstanced(mode, g_overrideCount, g_overrideType, 0, n);
        o_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, saved);
    } else {
        o_glDrawElementsInstanced(mode, count, type, indices, n);
    }
    restore_vb();
}
void glDrawRangeElements_hook(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
    RESOLVE(o_glDrawRangeElements, "glDrawRangeElements");
    int act = on_draw("R", mode, count, type, indices);
    if (act == 1) { g_vbBinds.clear(); return; }
    apply_vb();
    if (act == 2 && o_glBindBuffer) {
        GLuint saved = cur_elem();
        o_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_overrideBuf);
        o_glDrawRangeElements(mode, 0, g_overrideCount, g_overrideCount, g_overrideType, 0);
        o_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, saved);
        restore_vb();
        return;
    }
    o_glDrawRangeElements(mode, start, end, count, type, indices);
    restore_vb();
}

void glDrawElementsBaseVertex_hook(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
    RESOLVE(o_glDrawElementsBaseVertex, "glDrawElementsBaseVertex");
    int act = on_draw("B", mode, count, type, indices, basevertex);
    if (act == 1) { g_vbBinds.clear(); return; }
    apply_vb();
    if (act == 2 && o_glBindBuffer) {
        GLuint saved = cur_elem();
        o_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_overrideBuf);
        o_glDrawElementsBaseVertex(mode, g_overrideCount, g_overrideType, 0, basevertex);
        o_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, saved);
        restore_vb();
        return;
    }
    o_glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
    restore_vb();
}

void glDrawElementsInstancedBaseVertex_hook(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei n, GLint basevertex) {
    RESOLVE(o_glDrawElementsInstancedBaseVertex, "glDrawElementsInstancedBaseVertex");
    int act = on_draw("IB", mode, count, type, indices, basevertex);
    if (act == 1) { g_vbBinds.clear(); return; }
    apply_vb();
    if (act == 2 && o_glBindBuffer) {
        GLuint saved = cur_elem();
        o_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_overrideBuf);
        o_glDrawElementsInstancedBaseVertex(mode, g_overrideCount, g_overrideType, 0, n, basevertex);
        o_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, saved);
        restore_vb();
        return;
    }
    o_glDrawElementsInstancedBaseVertex(mode, count, type, indices, n, basevertex);
    restore_vb();
}

// ---------------------------------------------------------------- 表

struct Entry { const char* name; void* fn; void** tramp; };

const Entry kEntries[] = {
    {"glBindBuffer",            (void*)&glBindBuffer_hook,            (void**)&o_glBindBuffer},
    {"glBindTexture",           (void*)&glBindTexture_hook,           (void**)&o_glBindTexture},
    {"glActiveTexture",         (void*)&glActiveTexture_hook,         (void**)&o_glActiveTexture},
    {"glBindVertexArray",       (void*)&glBindVertexArray_hook,       (void**)&o_glBindVertexArray},
    {"glGenBuffers",            (void*)&glGenBuffers_hook,            (void**)&o_glGenBuffers},
    {"glDeleteBuffers",         (void*)&glDeleteBuffers_hook,         (void**)&o_glDeleteBuffers},
    {"glBufferData",            (void*)&glBufferData_hook,            (void**)&o_glBufferData},
    {"glBufferSubData",         (void*)&glBufferSubData_hook,         (void**)&o_glBufferSubData},
    {"glUseProgram",            (void*)&glUseProgram_hook,            (void**)&o_glUseProgram},
    {"glShaderSource",          (void*)&glShaderSource_hook,          (void**)&o_glShaderSource},
    {"glGetUniformLocation",    (void*)&glGetUniformLocation_hook,    (void**)&o_glGetUniformLocation},
    {"glVertexAttribPointer",   (void*)&glVertexAttribPointer_hook,   (void**)&o_glVertexAttribPointer},
    {"glDrawElements",          (void*)&glDrawElements_hook,          (void**)&o_glDrawElements},
    {"glDrawElementsInstanced", (void*)&glDrawElementsInstanced_hook, (void**)&o_glDrawElementsInstanced},
    {"glDrawRangeElements",     (void*)&glDrawRangeElements_hook,     (void**)&o_glDrawRangeElements},
    {"glDrawElementsBaseVertex",          (void*)&glDrawElementsBaseVertex_hook,          (void**)&o_glDrawElementsBaseVertex},
    {"glDrawElementsInstancedBaseVertex", (void*)&glDrawElementsInstancedBaseVertex_hook, (void**)&o_glDrawElementsInstancedBaseVertex},
    {"glMapBufferRange",        (void*)&glMapBufferRange_hook,        (void**)&o_glMapBufferRange},
    {"glUnmapBuffer",           (void*)&glUnmapBuffer_hook,           (void**)&o_glUnmapBuffer},
};

void* lookup_entry(const char* name) {
    for (const auto& e : kEntries)
        if (strcmp(e.name, name) == 0) return *e.tramp ? e.fn : nullptr;
    return nullptr;
}

void* eglGetProcAddress_hook(const char* name) {
    c_egl++;
    if (name && c_egl <= 120) log_name("egl", name);
    void* f = lookup_entry(name);
    if (f) return f;
    return o_eglGetProcAddress ? o_eglGetProcAddress(name) : nullptr;
}

void* dlsym_hook(void* handle, const char* name) {
    c_dlsym++;
    if (name && c_dlsym <= 120) log_name("dlsym", name);
    void* f = lookup_entry(name);
    if (f) return f;
    return o_dlsym ? o_dlsym(handle, name) : nullptr;
}



// ---- ELF 符号查找: 不走 dlopen, 直接遍历已加载的 so 手动解析 .dynsym ----
// 游戏的 libunity.so 在独立 linker namespace 里, dlopen 名字找不到。
static uint32_t gnu_hash_of(const char* s) {
    uint32_t h = 5381;
    for (const uint8_t* p = (const uint8_t*)s; *p; p++) h = (h * 33u) + *p;
    return h;
}

static void* elf_sym(void* base, const char* name) {
    ElfW(Ehdr)* eh = (ElfW(Ehdr)*)base;
    if (!eh || memcmp(eh->e_ident, ELFMAG, 4) != 0) return nullptr;
    ElfW(Phdr)* ph = (ElfW(Phdr)*)((char*)base + eh->e_phoff);
    ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) { dyn = (ElfW(Dyn)*)((char*)base + ph[i].p_vaddr); break; }
    if (!dyn) return nullptr;
    const char* strtab = nullptr;
    ElfW(Sym)* symtab = nullptr;
    const uint32_t* ghash = nullptr;
    const uint32_t* shash = nullptr;
    for (ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_STRTAB) strtab = (const char*)((char*)base + d->d_un.d_ptr);
        else if (d->d_tag == DT_SYMTAB) symtab = (ElfW(Sym)*)((char*)base + d->d_un.d_ptr);
        else if (d->d_tag == DT_GNU_HASH) ghash = (const uint32_t*)((char*)base + d->d_un.d_ptr);
        else if (d->d_tag == DT_HASH) shash = (const uint32_t*)((char*)base + d->d_un.d_ptr);
    }
    if (!strtab || !symtab) return nullptr;
    if (ghash) {
        uint32_t nbuckets = ghash[0], symoff = ghash[1], bloom = ghash[2];
        const uint32_t* buckets = &ghash[4 + bloom * 2];
        const uint32_t* chain = buckets + nbuckets;
        uint32_t h = gnu_hash_of(name);
        uint32_t b = buckets[h % nbuckets];
        if (b < symoff) return nullptr;
        for (;;) {
            uint32_t ch = chain[b - symoff];
            if ((ch | 1u) == (h | 1u) && !strcmp(strtab + symtab[b].st_name, name))
                return (void*)((char*)base + symtab[b].st_value);
            if (ch & 1u) break;
            b++;
        }
        return nullptr;
    }
    if (shash) {
        uint32_t nchain = shash[1];
        for (uint32_t i = 1; i < nchain; i++)
            if (!strcmp(strtab + symtab[i].st_name, name))
                return (void*)((char*)base + symtab[i].st_value);
    }
    return nullptr;
}

static void* g_unityBase = nullptr;
static int find_unity_cb(struct dl_phdr_info* info, size_t, void*) {
    if (info->dlpi_name && strstr(info->dlpi_name, "libunity.so")) {
        g_unityBase = (void*)info->dlpi_addr;
        return 1;
    }
    return 0;
}
// ---- Unity CPU 蒙皮 hook: 抓骨骼矩阵 ----
// libunity.so 导出 _s_SkinVertices4Bones_NEON, 签名由反汇编得到:
//   x0 = 骨骼矩阵数组 (每个 64 字节 4x4), x1 = 输入顶点 (24 字节/顶点),
//   x2 = 输入结束指针, x3 = 骨骼索引+权重 (24 字节/顶点), x4 = 输出顶点
typedef void (*SkinFn)(const float* bones, const float* in, const float* end,
                       const uint8_t* boneData, float* out);

// libunity.so 里的蒙皮变体, 按骨骼数和是否带切线/法线分。崩铁走哪个未知, 全挂。
static const char* kSkinSyms[] = {
    "_s_SkinVertices2Bones_NEON",
    "_s_SkinVertices2Bones_NoNormals_NEON",
    "_s_SkinVertices2Bones_Tangents_NEON",
    "_s_SkinVertices2Bones_Tangents1_NEON",
    "_s_SkinVertices4Bones_NEON",
    "_s_SkinVertices4Bones_NoNormals_NEON",
    "_s_SkinVertices4Bones_Tangents_NEON",
    "_s_SkinVertices4Bones_Tangents1_NEON",
};
static const int kSkinCount = 8;
static SkinFn g_skinOrig[8] = {nullptr};
static std::atomic<int> g_skinHits[8];

// 用 POSIX open/write, 不用 stdio: 蒙皮函数在 Unity 渲染线程里调用,
// fopen/freopen 会触发 fdsan 的 fd 所有权检查直接 SIGABRT。
static void dump_bytes(const char* name, const void* p, size_t n) {
    if (!g_dir[0] || !p || !n) return;
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { LOGE("dump open fail %s errno=%d", path, errno); return; }
    const uint8_t* q = (const uint8_t*)p;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, q, left);
        if (w <= 0) break;
        q += w; left -= (size_t)w;
    }
    close(fd);
    LOGI("dump %s %zu bytes", name, n);
}

template <int I>
static void skin_thunk(const float* bones, const float* in, const float* end,
                       const uint8_t* bd, float* out) {
    if (g_skinHits[I].fetch_add(1) == 0) {
        size_t vn = (size_t)((const uint8_t*)end - (const uint8_t*)in) / 24;
        if (vn > 60000) vn = 60000;
        LOGI("skin hit [%d] %s verts=%zu", I, kSkinSyms[I], vn);
        char nm[64];
        snprintf(nm, sizeof(nm), "skin%d_bones.bin", I);
        dump_bytes(nm, bones, 64 * 256);
        snprintf(nm, sizeof(nm), "skin%d_in.bin", I);
        dump_bytes(nm, in, vn * 24);
        snprintf(nm, sizeof(nm), "skin%d_bd.bin", I);
        dump_bytes(nm, bd, vn * 24);
        if (g_skinOrig[I]) g_skinOrig[I](bones, in, end, bd, out);
        snprintf(nm, sizeof(nm), "skin%d_out.bin", I);
        dump_bytes(nm, out, vn * 12);
        return;
    }
    if (g_skinOrig[I]) g_skinOrig[I](bones, in, end, bd, out);
}

static void install_skin(int i, void* addr) {
    switch (i) {
        case 0: DobbyHook(addr, (void*)&skin_thunk<0>, (void**)&g_skinOrig[0]); break;
        case 1: DobbyHook(addr, (void*)&skin_thunk<1>, (void**)&g_skinOrig[1]); break;
        case 2: DobbyHook(addr, (void*)&skin_thunk<2>, (void**)&g_skinOrig[2]); break;
        case 3: DobbyHook(addr, (void*)&skin_thunk<3>, (void**)&g_skinOrig[3]); break;
        case 4: DobbyHook(addr, (void*)&skin_thunk<4>, (void**)&g_skinOrig[4]); break;
        case 5: DobbyHook(addr, (void*)&skin_thunk<5>, (void**)&g_skinOrig[5]); break;
        case 6: DobbyHook(addr, (void*)&skin_thunk<6>, (void**)&g_skinOrig[6]); break;
        case 7: DobbyHook(addr, (void*)&skin_thunk<7>, (void**)&g_skinOrig[7]); break;
        default: break;
    }
}
}  // namespace

void gles_probe_start(const char* out_dir) {
    if (out_dir) {
        strncpy(g_dir, out_dir, sizeof(g_dir) - 1);
        g_dir[sizeof(g_dir) - 1] = 0;
    }
    LOGI("gles_probe_start dir=%s", g_dir);

    // 1) 直接 hook libGLESv2 的导出函数 (dlsym 路径 / 直接链接路径都覆盖)
    void* gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    if (gles) {
        int ok = 0;
        for (const auto& e : kEntries) {
            void* t = dlsym(gles, e.name);
            if (!t) { LOGW("no export %s", e.name); continue; }
            void* o = nullptr;
            int rc = DobbyHook(t, e.fn, &o);
            if (rc == 0 && o) { *e.tramp = o; ok++; LOGI("hook %-24s %p", e.name, t); }
            else LOGE("DobbyHook %s failed rc=%d", e.name, rc);
        }
        LOGI("libGLESv2 exports hooked: %d", ok);
    } else {
        LOGE("dlopen libGLESv2.so failed: %s", dlerror());
    }

    // 2) eglGetProcAddress
    void* h = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    if (h) {
        void* p = dlsym(h, "eglGetProcAddress");
        if (p) {
            int rc = DobbyHook(p, (void*)&eglGetProcAddress_hook, (void**)&o_eglGetProcAddress);
            if (rc == 0 && o_eglGetProcAddress) LOGI("hooked eglGetProcAddress @ %p", p);
            else LOGE("DobbyHook eglGetProcAddress failed rc=%d", rc);
        }
    }

    // 3) dlsym (放最后, 前面已经用完真 dlsym)
    void* dl = dlopen("libdl.so", RTLD_NOW | RTLD_LOCAL);
    if (dl) {
        void* p = dlsym(dl, "dlsym");
        if (p) {
            int rc = DobbyHook(p, (void*)&dlsym_hook, (void**)&o_dlsym);
            if (rc == 0 && o_dlsym) LOGI("hooked dlsym @ %p", p);
            else { LOGE("DobbyHook dlsym failed rc=%d", rc); o_dlsym = nullptr; }
        }
    }

    // 4) Unity CPU 蒙皮: 抓骨骼矩阵
    // libunity.so 是游戏自己 dlopen 的, 注入时还没加载, 起线程等它出现。
    std::thread([] {
        for (int i = 0; i < 1800; i++) {
            g_unityBase = nullptr;
            dl_iterate_phdr(find_unity_cb, nullptr);
            if (g_unityBase) {
                LOGI("libunity base=%p", g_unityBase);
                int n = 0;
                for (int k = 0; k < kSkinCount; k++) {
                    void* t = elf_sym(g_unityBase, kSkinSyms[k]);
                    if (!t) { LOGW("sym %s missing", kSkinSyms[k]); continue; }
                    install_skin(k, t);
                    if (g_skinOrig[k]) { n++; LOGI("hook %s @ %p", kSkinSyms[k], t); }
                }
                LOGI("skin hooks installed: %d/%d", n, kSkinCount);
                break;
            }
            usleep(100000);
        }
    }).detach();
}

