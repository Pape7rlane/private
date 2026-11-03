// gles_probe.cpp — HSR Android GLES probe
// libunity.so 只 import egl* 符号, 所有 gl* 都经 eglGetProcAddress 拿.
// 因此 hook eglGetProcAddress 就能拦到全部 GL 调用.
// 目的: 抓 index buffer 内容哈希 + 索引数 = 3Dmigoto/SRMI ini 里的 hash 键.
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <GLES3/gl3.h>

#include "dobby.h"

#define TAG "hsrprobe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" const char* hsr_out_dir();

namespace {

typedef void* (*PFNEGLGETPROCADDRESSPROC)(const char*);

PFNEGLGETPROCADDRESSPROC o_eglGetProcAddress = nullptr;

// ---------------------------------------------------------------- 状态

struct Attr {
    bool    has    = false;
    GLint   size   = 0;
    GLenum  type   = 0;
    GLsizei stride = 0;
    GLuint  buf    = 0;
};

struct VaoState {
    GLuint elem = 0;
    Attr   attr[16];
};

struct BufInfo {
    GLsizeiptr size = 0;
    uint64_t   h1   = 0;   // fnv-1  32
    uint64_t   h1a  = 0;   // fnv-1a 32
    GLenum     target = 0;
};

std::unordered_map<GLuint, BufInfo>  g_bufs;
std::unordered_map<GLuint, VaoState> g_vaos;

GLuint g_curVao   = 0;
GLuint g_curArray = 0;
GLuint g_curProg  = 0;

uint64_t g_draws      = 0;
uint64_t g_unique     = 0;
std::unordered_set<uint64_t> g_seen;
FILE*    g_fp = nullptr;
bool     g_fileTried = false;

// ---------------------------------------------------------------- 工具

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

FILE* out() {
    if (g_fp || g_fileTried) return g_fp;
    g_fileTried = true;
    const char* d = hsr_out_dir();
    if (!d || !*d) return nullptr;
    std::string p = std::string(d) + "/gles.tsv";
    g_fp = fopen(p.c_str(), "w");
    if (!g_fp) { LOGE("cannot open %s", p.c_str()); return nullptr; }
    fprintf(g_fp,
            "# vao\telem_buf\tib_fnv1\tib_fnv1a\tidx_count\tidx_type\tmode\tprogram\tvtx_layout\n");
    fflush(g_fp);
    return g_fp;
}

void* gl_real(const char* name) {
    void* p = nullptr;
    if (o_eglGetProcAddress) p = o_eglGetProcAddress(name);
    if (!p) p = dlsym(RTLD_DEFAULT, name);
    return p;
}

GLuint cur_elem() {
    auto it = g_vaos.find(g_curVao);
    return it == g_vaos.end() ? 0 : it->second.elem;
}

// ---------------------------------------------------------------- gl* 实现指针

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

#define RESOLVE(slot, name)                                     \
    do {                                                        \
        if (!(slot)) (slot) = (decltype(slot))gl_real(name);    \
        if (!(slot)) { LOGE("no real %s", name); return; }      \
    } while (0)

// ---------------------------------------------------------------- buffer 记录

void record_index(GLuint id, GLsizeiptr size, const void* data, GLenum target) {
    BufInfo& b = g_bufs[id];
    b.size   = size;
    b.target = target;
    if (data && size > 0 && size <= (64 << 20)) {
        const uint8_t* p = (const uint8_t*)data;
        b.h1  = fnv1_32(p, (size_t)size);
        b.h1a = fnv1a_32(p, (size_t)size);
    }
}

// ---------------------------------------------------------------- hooks

void glBindBuffer_hook(GLenum target, GLuint buffer) {
    RESOLVE(o_glBindBuffer, "glBindBuffer");
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        g_vaos[g_curVao].elem = buffer;
    } else if (target == GL_ARRAY_BUFFER) {
        g_curArray = buffer;
    }
    o_glBindBuffer(target, buffer);
}

void glBindVertexArray_hook(GLuint array) {
    RESOLVE(o_glBindVertexArray, "glBindVertexArray");
    g_curVao = array;
    o_glBindVertexArray(array);
}

void glGenBuffers_hook(GLsizei n, GLuint* buffers) {
    RESOLVE(o_glGenBuffers, "glGenBuffers");
    o_glGenBuffers(n, buffers);
}

void glDeleteBuffers_hook(GLsizei n, const GLuint* buffers) {
    RESOLVE(o_glDeleteBuffers, "glDeleteBuffers");
    for (GLsizei i = 0; i < n; i++) g_bufs.erase(buffers[i]);
    o_glDeleteBuffers(n, buffers);
}

void glBufferData_hook(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    RESOLVE(o_glBufferData, "glBufferData");
    if (data && size > 0) {
        GLuint id = (target == GL_ELEMENT_ARRAY_BUFFER) ? cur_elem() : g_curArray;
        if (id) record_index(id, size, data, target);
    }
    o_glBufferData(target, size, data, usage);
}

void glBufferSubData_hook(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    RESOLVE(o_glBufferSubData, "glBufferSubData");
    if (data && size > 0 && target == GL_ELEMENT_ARRAY_BUFFER) {
        GLuint id = cur_elem();
        if (id) record_index(id, size, data, target);
    }
    o_glBufferSubData(target, offset, size, data);
}

void glUseProgram_hook(GLuint program) {
    RESOLVE(o_glUseProgram, "glUseProgram");
    g_curProg = program;
    o_glUseProgram(program);
}

void glVertexAttribPointer_hook(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                GLsizei stride, const void* pointer) {
    RESOLVE(o_glVertexAttribPointer, "glVertexAttribPointer");
    if (index < 16) {
        Attr& a = g_vaos[g_curVao].attr[index];
        a.has = true; a.size = size; a.type = type; a.stride = stride; a.buf = g_curArray;
    }
    o_glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

std::string layout_of(GLuint vao) {
    std::string s;
    auto it = g_vaos.find(vao);
    if (it == g_vaos.end()) return s;
    char buf[96];
    for (int i = 0; i < 16; i++) {
        const Attr& a = it->second.attr[i];
        if (!a.has) continue;
        snprintf(buf, sizeof buf, "%d:%d/%x/s%d/b%u,", i, a.size, a.type, a.stride, a.buf);
        s += buf;
    }
    return s;
}

void on_draw(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    g_draws++;
    GLuint eb = cur_elem();
    uint64_t h1 = 0, h1a = 0;
    GLsizeiptr bsz = 0;
    auto it = g_bufs.find(eb);
    if (it != g_bufs.end()) { h1 = it->second.h1; h1a = it->second.h1a; bsz = it->second.size; }

    uint64_t key = (h1 << 20) ^ ((uint64_t)count << 4) ^ (uint64_t)type ^ ((uint64_t)mode << 40);
    bool fresh = g_seen.insert(key).second;
    if (!fresh || g_unique >= 5000) return;
    g_unique++;

    std::string lay = layout_of(g_curVao);
    if (FILE* f = out()) {
        fprintf(f, "%u\t%u\t%08llx\t%08llx\t%d\t%x\t%x\t%u\t%s\n",
                g_curVao, eb, (unsigned long long)h1, (unsigned long long)h1a,
                (int)count, type, mode, g_curProg, lay.c_str());
        fflush(f);
    }
    if (g_unique <= 200) {
        LOGI("DRAW vao=%u eb=%u sz=%lld h1=%08llx h1a=%08llx cnt=%d type=%x mode=%x prog=%u %s",
             g_curVao, eb, (long long)bsz, (unsigned long long)h1, (unsigned long long)h1a,
             (int)count, type, mode, g_curProg, lay.c_str());
    }
}

void glDrawElements_hook(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    RESOLVE(o_glDrawElements, "glDrawElements");
    on_draw(mode, count, type, indices);
    o_glDrawElements(mode, count, type, indices);
}

void glDrawElementsInstanced_hook(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                  GLsizei n) {
    RESOLVE(o_glDrawElementsInstanced, "glDrawElementsInstanced");
    on_draw(mode, count, type, indices);
    o_glDrawElementsInstanced(mode, count, type, indices, n);
}

void glDrawRangeElements_hook(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                              const void* indices) {
    RESOLVE(o_glDrawRangeElements, "glDrawRangeElements");
    on_draw(mode, count, type, indices);
    o_glDrawRangeElements(mode, start, end, count, type, indices);
}

// ---------------------------------------------------------------- eglGetProcAddress

struct Entry { const char* name; void* fn; };

const Entry kEntries[] = {
    {"glBindBuffer",              (void*)&glBindBuffer_hook},
    {"glBindVertexArray",         (void*)&glBindVertexArray_hook},
    {"glGenBuffers",              (void*)&glGenBuffers_hook},
    {"glDeleteBuffers",           (void*)&glDeleteBuffers_hook},
    {"glBufferData",              (void*)&glBufferData_hook},
    {"glBufferSubData",           (void*)&glBufferSubData_hook},
    {"glUseProgram",              (void*)&glUseProgram_hook},
    {"glVertexAttribPointer",     (void*)&glVertexAttribPointer_hook},
    {"glDrawElements",            (void*)&glDrawElements_hook},
    {"glDrawElementsInstanced",   (void*)&glDrawElementsInstanced_hook},
    {"glDrawRangeElements",       (void*)&glDrawRangeElements_hook},
    // GLES2 扩展名, 老路径可能用这些
    {"glBindVertexArrayOES",      (void*)&glBindVertexArray_hook},
    {"glDrawElementsOES",         (void*)&glDrawElements_hook},
};

void* eglGetProcAddress_hook(const char* name) {
    if (name) {
        for (const auto& e : kEntries) {
            if (strcmp(e.name, name) == 0) return e.fn;
        }
    }
    return o_eglGetProcAddress ? o_eglGetProcAddress(name) : nullptr;
}

}  // namespace

void gles_probe_start() {
    void* h = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) { LOGE("dlopen libEGL.so failed: %s", dlerror()); return; }
    void* p = dlsym(h, "eglGetProcAddress");
    if (!p) { LOGE("no eglGetProcAddress"); return; }

    int rc = DobbyHook(p, (void*)&eglGetProcAddress_hook, (void**)&o_eglGetProcAddress);
    if (rc != 0 || !o_eglGetProcAddress) {
        LOGE("DobbyHook eglGetProcAddress failed rc=%d", rc);
        return;
    }
    LOGI("hooked eglGetProcAddress @ %p (orig %p)", p, (void*)o_eglGetProcAddress);
}
