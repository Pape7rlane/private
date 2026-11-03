// gles_probe.cpp — HSR Android GLES probe (P1)
// 目标: 抓到每个 drawcall 的索引缓冲内容哈希 = 3Dmigoto/SRMI ini 里的 hash 键.
// 三条路径都堵: libGLESv2 导出函数直接 hook + eglGetProcAddress + dlsym.
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <string>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <GLES3/gl3.h>

#include "dobby.h"

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
        char cmd[32] = {0}, a1[64] = {0}, a2[64] = {0}, a3[64] = {0}, a4[64] = {0}, a5[64] = {0};
        int n = sscanf(s, "%31s %63s %63s %63s %63s %63s", cmd, a1, a2, a3, a4, a5);
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
struct VbBind { int idx = -1; GLuint newBuf = 0; Attr saved; };
std::vector<VbBind> g_vbBinds;
GLuint g_vbSavedArray = 0;

void apply_vb() {
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

int on_draw(const char* kind, GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basev = 0) {
    c_draw++;
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
            if (!pt.dynReady || pt.dynSrcBuf != a.buf || pt.dynStride != a.stride ||
                pt.dynOffset != off || pt.dynCount != vcount) {
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
    {"glBindVertexArray",       (void*)&glBindVertexArray_hook,       (void**)&o_glBindVertexArray},
    {"glGenBuffers",            (void*)&glGenBuffers_hook,            (void**)&o_glGenBuffers},
    {"glDeleteBuffers",         (void*)&glDeleteBuffers_hook,         (void**)&o_glDeleteBuffers},
    {"glBufferData",            (void*)&glBufferData_hook,            (void**)&o_glBufferData},
    {"glBufferSubData",         (void*)&glBufferSubData_hook,         (void**)&o_glBufferSubData},
    {"glUseProgram",            (void*)&glUseProgram_hook,            (void**)&o_glUseProgram},
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
}

