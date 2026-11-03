// gles_probe.cpp — HSR Android GLES probe (P1)
// 目标: 抓到每个 drawcall 的索引缓冲内容哈希 = 3Dmigoto/SRMI ini 里的 hash 键.
// 三条路径都堵: libGLESv2 导出函数直接 hook + eglGetProcAddress + dlsym.
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cerrno>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

struct Attr { bool has=false; GLint size=0; GLenum type=0; GLsizei stride=0; GLuint buf=0; };
struct VaoState { GLuint elem = 0; Attr attr[16]; };
struct BufInfo {
    GLsizeiptr size = 0;
    uint64_t   h1   = 0;   // 整段 FNV-1
    uint64_t   h1a  = 0;   // 整段 FNV-1a
    GLenum     target = 0;
    std::vector<uint8_t> data;   // 索引缓冲副本, 用于按 draw 子段哈希
};

// 索引缓冲副本预算
size_t g_dataBytes = 0;
const size_t kDataBudget = 96u << 20;
const size_t kDataPerBuf = 4u << 20;

std::unordered_map<GLuint, BufInfo>  g_bufs;
std::unordered_map<GLuint, VaoState> g_vaos;

GLuint g_curVao = 0, g_curArray = 0, g_curProg = 0;

std::atomic<uint64_t> c_egl{0}, c_dlsym{0}, c_draw{0}, c_bindBuf{0},
                      c_useProg{0}, c_attrib{0}, c_bufData{0}, c_vao{0}, c_genBuf{0};

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

uint32_t fnv1_32(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h * 16777619u) ^ p[i];
    return h;
}
uint32_t fnv1a_32(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
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
decltype(&glDrawElementsInstanced) o_glDrawElementsInstanced = nullptr;
decltype(&glDrawRangeElements) o_glDrawRangeElements = nullptr;

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
    if (!id || !data || size <= 0) return;
    BufInfo& b = g_bufs[id];
    b.size = size; b.target = target;
    const uint8_t* p = (const uint8_t*)data;
    b.h1  = fnv1_32(p, (size_t)size);
    b.h1a = fnv1a_32(p, (size_t)size);

    // 只有索引缓冲需要保留副本 (draw 时按子段哈希)
    if (target != GL_ELEMENT_ARRAY_BUFFER) return;
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
    if (data && size > 0) {
        GLuint id = (target == GL_ELEMENT_ARRAY_BUFFER) ? cur_elem() : g_curArray;
        record_index(id, size, data, target);
    }
    o_glBufferData(target, size, data, usage);
}

void glBufferSubData_hook(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    RESOLVE(o_glBufferSubData, "glBufferSubData");
    if (data && size > 0 && target == GL_ELEMENT_ARRAY_BUFFER) record_index(cur_elem(), size, data, target);
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
    }
    o_glVertexAttribPointer(index, size, type, normalized, stride, pointer);
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
    fprintf(g_fp, "# hits\tib_hash(fnv1)\tib_fnv1a\twhole_fnv1a\tfirst_index\tidx_count\tidx_type\tmode\tprog\tvao\teb\tkind\tsub_exact\tvtx_layout\n");
    size_t n = 0;
    for (auto* p : v) {
        if (n++ >= 4000) break;
        const Rec& r = p->second;
        fprintf(g_fp, "%llu\t%08llx\t%08llx\t%08llx\t%u\t%u\t%x\t%x\t%u\t%u\t%u\t%s\t%d\t%s\n",
                (unsigned long long)r.hits, (unsigned long long)p->first.wh1,
                (unsigned long long)r.h1a, (unsigned long long)r.wh1a,
                p->first.first, p->first.cnt, r.type, r.mode, r.prog, r.vao, r.eb,
                r.kind, r.exact ? 1 : 0, r.layout.c_str());
    }
    fflush(g_fp);
}

void on_draw(const char* kind, GLenum mode, GLsizei count, GLenum type, const void* indices) {
    c_draw++;
    GLuint eb = cur_elem();
    const size_t esz = (type == GL_UNSIGNED_INT) ? 4 : (type == GL_UNSIGNED_BYTE ? 1 : 2);
    uint64_t h1 = 0, h1a = 0, wh1 = 0, wh1a = 0;
    uint32_t first = 0;
    bool exact = false;

    auto it = g_bufs.find(eb);
    if (eb && it != g_bufs.end()) {
        wh1 = it->second.h1; wh1a = it->second.h1a;
        size_t off = (size_t)(uintptr_t)indices;
        size_t bytes = (size_t)count * esz;
        first = (uint32_t)(off / esz);
        if (!it->second.data.empty() && off + bytes <= it->second.data.size()) {
            h1  = fnv1_32(it->second.data.data() + off, bytes);
            h1a = fnv1a_32(it->second.data.data() + off, bytes);
            exact = true;
        } else {
            h1 = wh1; h1a = wh1a;
        }
    } else if (!eb && indices && count > 0) {
        size_t bytes = (size_t)count * esz;
        if (bytes <= (16 << 20)) {
            h1  = fnv1_32((const uint8_t*)indices, bytes);
            h1a = fnv1a_32((const uint8_t*)indices, bytes);
            wh1 = h1; wh1a = h1a;
            exact = true;
        }
    }

    Key k{wh1, first, (uint32_t)count};
    auto it2 = g_recs.find(k);
    if (it2 == g_recs.end()) {
        if (g_recs.size() >= 60000) return;
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
    }
}

void glDrawElements_hook(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    RESOLVE(o_glDrawElements, "glDrawElements");
    on_draw("", mode, count, type, indices);
    o_glDrawElements(mode, count, type, indices);
}
void glDrawElementsInstanced_hook(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei n) {
    RESOLVE(o_glDrawElementsInstanced, "glDrawElementsInstanced");
    on_draw("I", mode, count, type, indices);
    o_glDrawElementsInstanced(mode, count, type, indices, n);
}
void glDrawRangeElements_hook(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
    RESOLVE(o_glDrawRangeElements, "glDrawRangeElements");
    on_draw("R", mode, count, type, indices);
    o_glDrawRangeElements(mode, start, end, count, type, indices);
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

