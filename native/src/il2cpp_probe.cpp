// il2cpp_probe.cpp — 验证能否通过 il2cpp_get_api_table 拿到 il2cpp API
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>

#define TAG "hsrprobe"
#define LI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

typedef void* (*GetApiTableFn)();
typedef void* (*Fn0)();
typedef void** (*FnAssemblies)(void*, size_t*);

void* g_base = nullptr;

void* probe_thread(void*) {
    for (int i = 0; i < 90; i++) {
        void* h = dlopen("libil2cpp.so", RTLD_NOW);
        if (!h) { sleep(1); continue; }

        auto get_table = (GetApiTableFn)dlsym(h, "il2cpp_get_api_table");
        LI("il2cpp handle=%p il2cpp_get_api_table=%p", h, (void*)get_table);
        if (!get_table) { LE("no il2cpp_get_api_table"); return nullptr; }

        void** table = (void**)get_table();
        if (!table) { LE("api table null"); return nullptr; }

        Dl_info di {};
        if (dladdr((void*)table, &di)) g_base = di.dli_fbase;
        LI("api table=%p base=%p", (void*)table, g_base);
        if (!g_base) { LE("no base"); return nullptr; }

        for (int k = 0; k < 8; k++) {
            LI("  entry[%d] rva=0x%llx", k,
               (unsigned long long)((uintptr_t)table[k] - (uintptr_t)g_base));
        }

        // 索引来自 il2cpp-api-functions.h (Unity 2019.4 顺序)
        //   74 = il2cpp_domain_get
        //   76 = il2cpp_domain_get_assemblies
        void* domain = ((Fn0)table[74])();
        LI("domain_get() -> %p  (rva 0x%llx)", domain,
           (unsigned long long)((uintptr_t)table[74] - (uintptr_t)g_base));
        if (!domain || ((uintptr_t)domain & 7)) { LE("domain looks invalid, stop"); return nullptr; }

        size_t n = 0;
        void** asms = ((FnAssemblies)table[76])(domain, &n);
        LI("domain_get_assemblies() -> %p count=%zu", (void*)asms, n);
        if (!asms || n == 0 || n > 4096) { LE("assemblies look invalid, stop"); return nullptr; }

        for (size_t k = 0; k < n && k < 5; k++) {
            LI("  assembly[%zu] = %p", k, asms[k]);
        }
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
