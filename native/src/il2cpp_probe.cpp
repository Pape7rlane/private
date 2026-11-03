// il2cpp_probe.cpp — 验证能否通过 il2cpp_get_api_table 拿到 il2cpp API
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define TAG "hsrprobe"
#define LI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

typedef void* (*GetApiTableFn)();
typedef void* (*Fn0)();
typedef void** (*FnAssemblies)(void*, size_t*);

char g_path[512];
uintptr_t g_bias = 0;

int find_lib(struct dl_phdr_info* info, size_t, void*) {
    if (info->dlpi_name && strstr(info->dlpi_name, "libil2cpp.so")) {
        strncpy(g_path, info->dlpi_name, sizeof(g_path) - 1);
        g_path[sizeof(g_path) - 1] = 0;
        g_bias = (uintptr_t)info->dlpi_addr;
        return 1;
    }
    return 0;
}

void* probe_thread(void*) {
    for (int i = 0; i < 120; i++) {
        g_path[0] = 0;
        g_bias = 0;
        dl_iterate_phdr(find_lib, nullptr);
        if (!g_path[0]) { sleep(1); continue; }

        LI("libil2cpp.so found: %s bias=0x%llx", g_path, (unsigned long long)g_bias);

        // dlopen 被 namespace 挡住, 直接用 bias + RVA (readelf 得到的 il2cpp_get_api_table)
        auto get_table = (GetApiTableFn)(g_bias + 0x6dcdc4cULL);
        LI("il2cpp_get_api_table @ %p  insn=0x%08x", (void*)get_table,
           *(uint32_t*)get_table);

        void** table = (void**)get_table();
        if (!table) { LE("api table null"); return nullptr; }
        LI("api table=%p", (void*)table);

        for (int k = 0; k < 8; k++) {
            LI("  entry[%d] rva=0x%llx", k,
               (unsigned long long)((uintptr_t)table[k] - g_bias));
        }

        // 索引来自 il2cpp-api-functions.h (Unity 2019.4 顺序)
        void* domain = ((Fn0)table[74])();
        LI("domain_get() -> %p  (rva 0x%llx)", domain,
           (unsigned long long)((uintptr_t)table[74] - g_bias));
        if (!domain || ((uintptr_t)domain & 7)) { LE("domain invalid, stop"); return nullptr; }

        size_t n = 0;
        void** asms = ((FnAssemblies)table[76])(domain, &n);
        LI("domain_get_assemblies() -> %p count=%zu", (void*)asms, n);
        if (!asms || n == 0 || n > 4096) { LE("assemblies invalid, stop"); return nullptr; }

        for (size_t k = 0; k < n && k < 5; k++) LI("  assembly[%zu] = %p", k, asms[k]);
        LI("IL2CPP API TABLE WORKS");
        return nullptr;
    }
    LE("libil2cpp.so never appeared");
    return nullptr;
}

}  // namespace

void il2cpp_probe_start() {
    pthread_t t;
    pthread_create(&t, nullptr, probe_thread, nullptr);
    pthread_detach(t);
}
