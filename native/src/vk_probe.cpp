// vk_probe.cpp — HSR Android Vulkan probe (P0)
// 目标: 在游戏进程内 hook Vulkan, 输出 drawcall 表 + pipeline 顶点布局表
// 输出: /sdcard/Android/data/<pkg>/files/hsrprobe/probe.json
#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "dobby.h"

#define TAG "hsrprobe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

// ---------------------------------------------------------------- 数据结构

struct IbRange { VkDeviceSize off; VkDeviceSize size; uint64_t hash; };

struct BufferRec {
    VkDeviceSize   size      = 0;
    VkDeviceMemory mem       = VK_NULL_HANDLE;
    VkDeviceSize   memOffset = 0;
    uint8_t*       hostPtr   = nullptr;   // 若该 buffer 所在内存已 map, 这里可用
    std::vector<IbRange> ranges;          // 从 vkCmdUpdateBuffer / vkCmdCopyBuffer 抓到的内容
};

struct MapRec { uint8_t* ptr = nullptr; VkDeviceSize off = 0; VkDeviceSize size = 0; };

struct ShaderRec { uint64_t hash = 0; size_t size = 0; };

struct AttrRec { uint32_t loc, binding, offset; VkFormat fmt; };
struct BindRec { uint32_t binding, stride; VkVertexInputRate rate; };

struct PipeRec {
    uint64_t vs = 0, fs = 0;
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    std::vector<BindRec> binds;
    std::vector<AttrRec> attrs;
};

struct CmdState {
    VkPipeline    pipe   = VK_NULL_HANDLE;
    VkBuffer      ib     = VK_NULL_HANDLE;
    VkDeviceSize  ibOff  = 0;
    VkIndexType   ibType = VK_INDEX_TYPE_UINT32;
    uint32_t      dsCount = 0;
    uint32_t      vbCount = 0;
    VkBuffer      vb[16]  = {};
    VkDeviceSize  vbOff[16] = {};
};

struct DrawRec {
    uint64_t pipe = 0, ib = 0, ibHash = 0;
    uint32_t indexCount = 0, firstIndex = 0;
    int32_t  vertexOffset = 0;
    uint32_t ibBytes = 0;
    uint64_t hits = 1;
    uint32_t fp[8] = {};
};

// ---------------------------------------------------------------- 全局状态

std::mutex g_mu;

std::unordered_map<VkBuffer,       BufferRec> g_buffers;
std::unordered_map<VkDeviceMemory, MapRec>    g_maps;
std::unordered_map<VkShaderModule, ShaderRec> g_shaders;
std::unordered_map<VkPipeline,     PipeRec>   g_pipes;
std::unordered_map<VkCommandBuffer, CmdState> g_cmds;

typedef std::tuple<uint64_t, uint64_t, uint32_t, uint32_t, int32_t> DrawKey;
std::map<DrawKey, size_t> g_drawIndex;
std::vector<DrawRec>      g_draws;

std::atomic<uint64_t> g_frames{0};
uint64_t g_drawsTotal = 0, g_noIb = 0, g_unreadable = 0, g_noCmd = 0;

std::string g_pkg = "com.miHoYo.hkrpg";
std::string g_out;

constexpr size_t kMaxDrawRecs   = 4096;
constexpr uint64_t kFlushFrames = 300;

// ---------------------------------------------------------------- 小工具

uint64_t fnv1a(const void* data, size_t n) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

bool mkdirs(const std::string& p) {
    if (p.empty()) return false;
    std::string cur;
    for (char c : p) {
        cur.push_back(c);
        if (c == '/' && cur.size() > 1) mkdir(cur.c_str(), 0775);
    }
    mkdir(p.c_str(), 0775);
    struct stat st {};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void init_pkg() {
    FILE* f = fopen("/proc/self/cmdline", "rb");
    if (!f) return;
    char buf[256] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return;
    std::string s(buf, strnlen(buf, n));
    size_t c = s.find(':');
    if (c != std::string::npos) s = s.substr(0, c);
    if (!s.empty()) g_pkg = s;
}

std::string pick_out_dir() {
    const std::string cands[] = {
        "/sdcard/Android/data/" + g_pkg + "/files/hsrprobe",
        "/storage/emulated/0/Android/data/" + g_pkg + "/files/hsrprobe",
        "/data/data/" + g_pkg + "/files/hsrprobe",
        "/data/local/tmp/hsrprobe",
    };
    for (const auto& d : cands) {
        if (!mkdirs(d)) continue;
        if (access(d.c_str(), W_OK) == 0) return d;
    }
    return "/data/local/tmp/hsrprobe";
}

const char* fmt_name(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R32_SFLOAT:            return "R32F";
        case VK_FORMAT_R32G32_SFLOAT:         return "RG32F";
        case VK_FORMAT_R32G32B32_SFLOAT:      return "RGB32F";
        case VK_FORMAT_R32G32B32A32_SFLOAT:   return "RGBA32F";
        case VK_FORMAT_R16G16B16A16_SFLOAT:   return "RGBA16F";
        case VK_FORMAT_R16G16_SFLOAT:         return "RG16F";
        case VK_FORMAT_R8G8B8A8_UNORM:        return "RGBA8";
        case VK_FORMAT_R8G8B8A8_SNORM:        return "RGBA8S";
        case VK_FORMAT_R8G8B8A8_SRGB:         return "RGBA8_SRGB";
        case VK_FORMAT_R8G8B8_UNORM:          return "RGB8";
        case VK_FORMAT_R8G8_UNORM:            return "RG8";
        case VK_FORMAT_R8_UNORM:              return "R8";
        case VK_FORMAT_R16G16_UNORM:          return "RG16";
        case VK_FORMAT_R16G16B16A16_UNORM:    return "RGBA16";
        case VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16: return "RGB10A2";
        case VK_FORMAT_B8G8R8A8_UNORM:        return "BGRA8";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10";
        default:                              return "?";
    }
}

const char* topo_name(VkPrimitiveTopology t) {
    switch (t) {
        case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:     return "points";
        case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:      return "lines";
        case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:     return "linestrip";
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:  return "tris";
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return "tristrip";
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:   return "fan";
        case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:     return "patch";
        default:                                   return "?";
    }
}

// ---------------------------------------------------------------- 注册表操作 (调用方持锁)

uint8_t* host_ptr_locked(VkBuffer b, VkDeviceSize off, VkDeviceSize len) {
    auto it = g_buffers.find(b);
    if (it == g_buffers.end()) return nullptr;
    BufferRec& r = it->second;
    if (!r.hostPtr) return nullptr;
    if (off + len > r.size) return nullptr;
    return r.hostPtr + off;
}

bool range_hash_locked(VkBuffer b, VkDeviceSize off, VkDeviceSize len, uint64_t* out) {
    auto it = g_buffers.find(b);
    if (it == g_buffers.end()) return false;
    for (const IbRange& rg : it->second.ranges) {
        if (rg.off == off && rg.size == len) { *out = rg.hash; return true; }
    }
    return false;
}

void record_range_locked(VkBuffer b, VkDeviceSize off, VkDeviceSize len, uint64_t h) {
    if (!b || len == 0) return;
    auto& r = g_buffers[b];
    if (r.ranges.size() > 64) r.ranges.erase(r.ranges.begin());
    r.ranges.push_back(IbRange{off, len, h});
}

void rebind_host_ptrs_locked(VkDeviceMemory mem) {
    auto mit = g_maps.find(mem);
    if (mit == g_maps.end() || !mit->second.ptr) return;
    const MapRec& m = mit->second;
    for (auto& kv : g_buffers) {
        BufferRec& r = kv.second;
        if (r.mem != mem) continue;
        if (r.memOffset < m.off) { r.hostPtr = nullptr; continue; }
        VkDeviceSize delta = r.memOffset - m.off;
        if (delta + r.size > m.size) { r.hostPtr = nullptr; continue; }
        r.hostPtr = m.ptr + delta;
    }
}

// ---------------------------------------------------------------- 原始函数指针

PFN_vkCreateShaderModule        o_create_shader_module   = nullptr;
PFN_vkCreateGraphicsPipelines   o_create_graphics_pipelines = nullptr;
PFN_vkCmdBindPipeline           o_cmd_bind_pipeline      = nullptr;
PFN_vkCmdBindIndexBuffer        o_cmd_bind_index_buffer  = nullptr;
PFN_vkCmdBindVertexBuffers      o_cmd_bind_vertex_buffers = nullptr;
PFN_vkCmdBindDescriptorSets     o_cmd_bind_descriptor_sets = nullptr;
PFN_vkCmdDrawIndexed            o_cmd_draw_indexed       = nullptr;
PFN_vkCreateBuffer              o_create_buffer          = nullptr;
PFN_vkDestroyBuffer             o_destroy_buffer         = nullptr;
PFN_vkBindBufferMemory          o_bind_buffer_memory     = nullptr;
PFN_vkMapMemory                 o_map_memory             = nullptr;
PFN_vkUnmapMemory               o_unmap_memory           = nullptr;
PFN_vkCmdUpdateBuffer           o_cmd_update_buffer      = nullptr;
PFN_vkCmdCopyBuffer             o_cmd_copy_buffer        = nullptr;
PFN_vkQueuePresentKHR           o_queue_present          = nullptr;
PFN_vkGetDeviceProcAddr         o_get_device_proc_addr   = nullptr;
PFN_vkGetInstanceProcAddr       o_get_instance_proc_addr = nullptr;

// ---------------------------------------------------------------- 记录逻辑

void record_draw(VkCommandBuffer cb, uint32_t n, uint32_t first, int32_t voff) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_drawsTotal++;

    auto cit = g_cmds.find(cb);
    if (cit == g_cmds.end()) { g_noCmd++; return; }
    CmdState& st = cit->second;
    if (st.ib == VK_NULL_HANDLE) { g_noIb++; return; }

    const uint32_t isz = (st.ibType == VK_INDEX_TYPE_UINT16) ? 2u : 4u;
    const VkDeviceSize off = st.ibOff + static_cast<VkDeviceSize>(first) * isz;
    const VkDeviceSize len = static_cast<VkDeviceSize>(n) * isz;

    uint64_t h = 0;
    uint32_t fp[8] = {};
    const uint8_t* p = host_ptr_locked(st.ib, off, len);
    if (p) {
        h = fnv1a(p, static_cast<size_t>(len));
        uint32_t k = n < 8 ? n : 8;
        for (uint32_t i = 0; i < k; ++i) {
            fp[i] = (isz == 2) ? reinterpret_cast<const uint16_t*>(p)[i]
                               : reinterpret_cast<const uint32_t*>(p)[i];
        }
    } else if (!range_hash_locked(st.ib, off, len, &h)) {
        g_unreadable++;
    }

    DrawKey key{reinterpret_cast<uint64_t>(st.pipe), reinterpret_cast<uint64_t>(st.ib),
                n, first, voff};
    auto dit = g_drawIndex.find(key);
    if (dit != g_drawIndex.end()) {
        g_draws[dit->second].hits++;
        return;
    }
    if (g_draws.size() >= kMaxDrawRecs) return;

    DrawRec rec;
    rec.pipe         = reinterpret_cast<uint64_t>(st.pipe);
    rec.ib           = reinterpret_cast<uint64_t>(st.ib);
    rec.ibHash       = h;
    rec.indexCount   = n;
    rec.firstIndex   = first;
    rec.vertexOffset = voff;
    rec.ibBytes      = static_cast<uint32_t>(len);
    memcpy(rec.fp, fp, sizeof(fp));
    g_drawIndex[key] = g_draws.size();
    g_draws.push_back(rec);
}

// ---------------------------------------------------------------- JSON 输出

std::string build_json() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::string s;
    s.reserve(256 * 1024);
    char b[512];

    snprintf(b, sizeof(b),
             "{\n\"meta\":{\"pkg\":\"%s\",\"frames\":%llu,\"draws_total\":%llu,"
             "\"unique\":%zu,\"no_ib\":%llu,\"unreadable\":%llu,\"no_cmd\":%llu},\n",
             g_pkg.c_str(), (unsigned long long)g_frames.load(),
             (unsigned long long)g_drawsTotal, g_draws.size(),
             (unsigned long long)g_noIb, (unsigned long long)g_unreadable,
             (unsigned long long)g_noCmd);
    s += b;

    s += "\"shaders\":[";
    bool firstItem = true;
    for (auto& kv : g_shaders) {
        snprintf(b, sizeof(b), "%s{\"h\":\"%016llx\",\"size\":%zu}",
                 firstItem ? "" : ",", (unsigned long long)kv.second.hash, kv.second.size);
        s += b;
        firstItem = false;
    }
    s += "],\n";

    s += "\"pipelines\":[";
    firstItem = true;
    for (auto& kv : g_pipes) {
        const PipeRec& p = kv.second;
        snprintf(b, sizeof(b),
                 "%s{\"h\":\"%llx\",\"vs\":\"%016llx\",\"ps\":\"%016llx\",\"topo\":\"%s\",\"binds\":[",
                 firstItem ? "" : ",", (unsigned long long)reinterpret_cast<uint64_t>(kv.first),
                 (unsigned long long)p.vs, (unsigned long long)p.fs, topo_name(p.topo));
        s += b;
        for (size_t i = 0; i < p.binds.size(); ++i) {
            snprintf(b, sizeof(b), "%s{\"b\":%u,\"stride\":%u,\"rate\":%d}",
                     i ? "," : "", p.binds[i].binding, p.binds[i].stride,
                     (int)p.binds[i].rate);
            s += b;
        }
        s += "],\"attrs\":[";
        for (size_t i = 0; i < p.attrs.size(); ++i) {
            snprintf(b, sizeof(b), "%s{\"loc\":%u,\"b\":%u,\"f\":%d,\"fn\":\"%s\",\"off\":%u}",
                     i ? "," : "", p.attrs[i].loc, p.attrs[i].binding,
                     (int)p.attrs[i].fmt, fmt_name(p.attrs[i].fmt), p.attrs[i].offset);
            s += b;
        }
        s += "]}";
        firstItem = false;
    }
    s += "],\n";

    s += "\"draws\":[";
    firstItem = true;
    for (const DrawRec& d : g_draws) {
        snprintf(b, sizeof(b),
                 "%s{\"p\":\"%llx\",\"ib\":\"%llx\",\"ibh\":\"%016llx\",\"n\":%u,\"first\":%u,"
                 "\"voff\":%d,\"bytes\":%u,\"hits\":%llu,\"fp\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
                 firstItem ? "" : ",", (unsigned long long)d.pipe, (unsigned long long)d.ib,
                 (unsigned long long)d.ibHash, d.indexCount, d.firstIndex, d.vertexOffset,
                 d.ibBytes, (unsigned long long)d.hits,
                 d.fp[0], d.fp[1], d.fp[2], d.fp[3], d.fp[4], d.fp[5], d.fp[6], d.fp[7]);
        s += b;
        firstItem = false;
    }
    s += "]\n}\n";
    return s;
}

void dump_json() {
    if (g_out.empty()) return;
    std::string js = build_json();
    std::string path = g_out + "/probe.json";
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { LOGE("open %s failed", path.c_str()); return; }
    fwrite(js.data(), 1, js.size(), f);
    fclose(f);
    LOGI("dump %s  (%zu bytes)", path.c_str(), js.size());
}

// ---------------------------------------------------------------- Hook 实现

VKAPI_ATTR VkResult VKAPI_CALL hk_create_shader_module(
        VkDevice dev, const VkShaderModuleCreateInfo* ci,
        const VkAllocationCallbacks* ac, VkShaderModule* out) {
    VkResult r = o_create_shader_module(dev, ci, ac, out);
    if (r == VK_SUCCESS && out && *out && ci && ci->pCode && ci->codeSize) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_shaders[*out] = ShaderRec{fnv1a(ci->pCode, ci->codeSize), ci->codeSize};
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hk_create_graphics_pipelines(
        VkDevice dev, VkPipelineCache cache, uint32_t count,
        const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks* ac,
        VkPipeline* out) {
    VkResult r = o_create_graphics_pipelines(dev, cache, count, ci, ac, out);
    if (r != VK_SUCCESS) return r;
    std::lock_guard<std::mutex> lk(g_mu);
    for (uint32_t i = 0; i < count; ++i) {
        PipeRec rec;
        for (uint32_t s = 0; s < ci[i].stageCount; ++s) {
            const VkPipelineShaderStageCreateInfo& st = ci[i].pStages[s];
            auto sit = g_shaders.find(st.module);
            uint64_t h = (sit != g_shaders.end()) ? sit->second.hash : 0;
            if (st.stage == VK_SHADER_STAGE_VERTEX_BIT)   rec.vs = h;
            if (st.stage == VK_SHADER_STAGE_FRAGMENT_BIT) rec.fs = h;
        }
        if (ci[i].pInputAssemblyState) rec.topo = ci[i].pInputAssemblyState->topology;
        const VkPipelineVertexInputStateCreateInfo* vi = ci[i].pVertexInputState;
        if (vi) {
            for (uint32_t k = 0; k < vi->vertexBindingDescriptionCount; ++k) {
                const auto& bd = vi->pVertexBindingDescriptions[k];
                rec.binds.push_back(BindRec{bd.binding, bd.stride, bd.inputRate});
            }
            for (uint32_t k = 0; k < vi->vertexAttributeDescriptionCount; ++k) {
                const auto& ad = vi->pVertexAttributeDescriptions[k];
                rec.attrs.push_back(AttrRec{ad.location, ad.binding, ad.offset, ad.format});
            }
        }
        g_pipes[out[i]] = std::move(rec);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL hk_cmd_bind_pipeline(VkCommandBuffer cb,
                                               VkPipelineBindPoint bp, VkPipeline p) {
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_cmds[cb].pipe = p;
    }
    o_cmd_bind_pipeline(cb, bp, p);
}

VKAPI_ATTR void VKAPI_CALL hk_cmd_bind_index_buffer(VkCommandBuffer cb, VkBuffer buf,
                                                    VkDeviceSize off, VkIndexType t) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        CmdState& st = g_cmds[cb];
        st.ib = buf; st.ibOff = off; st.ibType = t;
    }
    o_cmd_bind_index_buffer(cb, buf, off, t);
}

VKAPI_ATTR void VKAPI_CALL hk_cmd_bind_vertex_buffers(VkCommandBuffer cb, uint32_t first,
                                                      uint32_t count, const VkBuffer* bufs,
                                                      const VkDeviceSize* offs) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        CmdState& st = g_cmds[cb];
        for (uint32_t i = 0; i < count && (first + i) < 16; ++i) {
            st.vb[first + i]    = bufs ? bufs[i] : VK_NULL_HANDLE;
            st.vbOff[first + i] = offs ? offs[i] : 0;
        }
        st.vbCount = (first + count) < 16 ? (first + count) : 16;
    }
    o_cmd_bind_vertex_buffers(cb, first, count, bufs, offs);
}

VKAPI_ATTR void VKAPI_CALL hk_cmd_bind_descriptor_sets(
        VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout,
        uint32_t first, uint32_t count, const VkDescriptorSet* sets,
        uint32_t dynCount, const uint32_t* dynOffs) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_cmds[cb].dsCount = first + count;
    }
    o_cmd_bind_descriptor_sets(cb, bp, layout, first, count, sets, dynCount, dynOffs);
}

VKAPI_ATTR void VKAPI_CALL hk_cmd_draw_indexed(VkCommandBuffer cb, uint32_t n,
                                               uint32_t inst, uint32_t first,
                                               int32_t voff, uint32_t firstInst) {
    record_draw(cb, n, first, voff);
    o_cmd_draw_indexed(cb, n, inst, first, voff, firstInst);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_create_buffer(VkDevice dev, const VkBufferCreateInfo* ci,
                                                const VkAllocationCallbacks* ac, VkBuffer* out) {
    VkResult r = o_create_buffer(dev, ci, ac, out);
    if (r == VK_SUCCESS && out && *out) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_buffers[*out].size = ci ? ci->size : 0;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL hk_destroy_buffer(VkDevice dev, VkBuffer b,
                                             const VkAllocationCallbacks* ac) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_buffers.erase(b);
    }
    o_destroy_buffer(dev, b, ac);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_bind_buffer_memory(VkDevice dev, VkBuffer b,
                                                     VkDeviceMemory mem, VkDeviceSize off) {
    VkResult r = o_bind_buffer_memory(dev, b, mem, off);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lk(g_mu);
        BufferRec& rec = g_buffers[b];
        rec.mem = mem; rec.memOffset = off;
        rebind_host_ptrs_locked(mem);
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hk_map_memory(VkDevice dev, VkDeviceMemory mem,
                                             VkDeviceSize off, VkDeviceSize size,
                                             VkMemoryMapFlags flags, void** ppData) {
    VkResult r = o_map_memory(dev, mem, off, size, flags, ppData);
    if (r == VK_SUCCESS && ppData && *ppData) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_maps[mem] = MapRec{static_cast<uint8_t*>(*ppData), off, size};
        rebind_host_ptrs_locked(mem);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL hk_unmap_memory(VkDevice dev, VkDeviceMemory mem) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_maps.erase(mem);
        for (auto& kv : g_buffers) {
            if (kv.second.mem == mem) kv.second.hostPtr = nullptr;
        }
    }
    o_unmap_memory(dev, mem);
}

VKAPI_ATTR void VKAPI_CALL hk_cmd_update_buffer(VkCommandBuffer cb, VkBuffer dst,
                                                VkDeviceSize off, VkDeviceSize size,
                                                const void* data) {
    if (data && size) {
        uint64_t h = fnv1a(data, static_cast<size_t>(size));
        std::lock_guard<std::mutex> lk(g_mu);
        record_range_locked(dst, off, size, h);
    }
    o_cmd_update_buffer(cb, dst, off, size, data);
}

VKAPI_ATTR void VKAPI_CALL hk_cmd_copy_buffer(VkCommandBuffer cb, VkBuffer src,
                                              VkBuffer dst, uint32_t count,
                                              const VkBufferCopy* regions) {
    if (regions) {
        std::lock_guard<std::mutex> lk(g_mu);
        for (uint32_t i = 0; i < count; ++i) {
            const VkBufferCopy& rg = regions[i];
            uint8_t* p = host_ptr_locked(src, rg.srcOffset, rg.size);
            if (!p) continue;
            record_range_locked(dst, rg.dstOffset, rg.size, fnv1a(p, static_cast<size_t>(rg.size)));
        }
    }
    o_cmd_copy_buffer(cb, src, dst, count, regions);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_queue_present(VkQueue q, const VkPresentInfoKHR* pi) {
    uint64_t f = ++g_frames;
    if (f == 1 || (f % kFlushFrames) == 0) dump_json();
    return o_queue_present(q, pi);
}

// ---------------------------------------------------------------- proc-addr 兜底

PFN_vkVoidFunction fake_for(const char* n) {
    if (!n) return nullptr;
    if (!strcmp(n, "vkCmdDrawIndexed")      && o_cmd_draw_indexed)
        return reinterpret_cast<PFN_vkVoidFunction>(hk_cmd_draw_indexed);
    if (!strcmp(n, "vkCmdBindIndexBuffer")  && o_cmd_bind_index_buffer)
        return reinterpret_cast<PFN_vkVoidFunction>(hk_cmd_bind_index_buffer);
    if (!strcmp(n, "vkCmdBindVertexBuffers") && o_cmd_bind_vertex_buffers)
        return reinterpret_cast<PFN_vkVoidFunction>(hk_cmd_bind_vertex_buffers);
    if (!strcmp(n, "vkCmdBindPipeline")     && o_cmd_bind_pipeline)
        return reinterpret_cast<PFN_vkVoidFunction>(hk_cmd_bind_pipeline);
    if (!strcmp(n, "vkCmdBindDescriptorSets") && o_cmd_bind_descriptor_sets)
        return reinterpret_cast<PFN_vkVoidFunction>(hk_cmd_bind_descriptor_sets);
    if (!strcmp(n, "vkCreateGraphicsPipelines") && o_create_graphics_pipelines)
        return reinterpret_cast<PFN_vkVoidFunction>(hk_create_graphics_pipelines);
    if (!strcmp(n, "vkCreateShaderModule")  && o_create_shader_module)
        return reinterpret_cast<PFN_vkVoidFunction>(hk_create_shader_module);
    if (!strcmp(n, "vkCreateBuffer")        && o_create_buffer)
        return reinterpret_cast<PFN_vkVoidFunction>(hk_create_buffer);
    if (!strcmp(n, "vkBindBufferMemory")    && o_bind_buffer_memory)
        return reinterpret_cast<PFN_vkVoidFunction>(hk_bind_buffer_memory);
    if (!strcmp(n, "vkMapMemory")           && o_map_memory)
        return reinterpret_cast<PFN_vkVoidFunction>(hk_map_memory);
    if (!strcmp(n, "vkUnmapMemory")         && o_unmap_memory)
        return reinterpret_cast<PFN_vkVoidFunction>(hk_unmap_memory);
    if (!strcmp(n, "vkCmdUpdateBuffer")     && o_cmd_update_buffer)
        return reinterpret_cast<PFN_vkVoidFunction>(hk_cmd_update_buffer);
    if (!strcmp(n, "vkCmdCopyBuffer")       && o_cmd_copy_buffer)
        return reinterpret_cast<PFN_vkVoidFunction>(hk_cmd_copy_buffer);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_get_device_proc_addr(VkDevice d, const char* name) {
    PFN_vkVoidFunction f = o_get_device_proc_addr(d, name);
    PFN_vkVoidFunction fake = fake_for(name);
    return fake ? fake : f;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_get_instance_proc_addr(VkInstance i, const char* name) {
    PFN_vkVoidFunction f = o_get_instance_proc_addr(i, name);
    PFN_vkVoidFunction fake = fake_for(name);
    return fake ? fake : f;
}

// ---------------------------------------------------------------- 安装

void* g_vk = nullptr;

void* vk_sym(const char* name) {
    if (!g_vk) {
        g_vk = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!g_vk) g_vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    }
    return g_vk ? dlsym(g_vk, name) : nullptr;
}

template <typename T>
bool hook_sym(const char* name, T fake, T* orig) {
    void* target = vk_sym(name);
    if (!target) { LOGW("missing symbol %s", name); return false; }
    void* o = nullptr;
    int rc = DobbyHook(target, reinterpret_cast<void*>(fake), &o);
    if (rc != 0 || !o) { LOGE("DobbyHook %s failed rc=%d", name, rc); return false; }
    *orig = reinterpret_cast<T>(o);
    LOGI("hooked %-26s %p (orig %p)", name, target, o);
    return true;
}

void install_hooks() {
    hook_sym("vkCreateShaderModule",        hk_create_shader_module,       &o_create_shader_module);
    hook_sym("vkCreateGraphicsPipelines",   hk_create_graphics_pipelines,  &o_create_graphics_pipelines);
    hook_sym("vkCmdBindPipeline",           hk_cmd_bind_pipeline,          &o_cmd_bind_pipeline);
    hook_sym("vkCmdBindIndexBuffer",        hk_cmd_bind_index_buffer,      &o_cmd_bind_index_buffer);
    hook_sym("vkCmdBindVertexBuffers",      hk_cmd_bind_vertex_buffers,    &o_cmd_bind_vertex_buffers);
    hook_sym("vkCmdBindDescriptorSets",     hk_cmd_bind_descriptor_sets,   &o_cmd_bind_descriptor_sets);
    hook_sym("vkCmdDrawIndexed",            hk_cmd_draw_indexed,           &o_cmd_draw_indexed);
    hook_sym("vkCreateBuffer",              hk_create_buffer,              &o_create_buffer);
    hook_sym("vkDestroyBuffer",             hk_destroy_buffer,             &o_destroy_buffer);
    hook_sym("vkBindBufferMemory",          hk_bind_buffer_memory,         &o_bind_buffer_memory);
    hook_sym("vkMapMemory",                 hk_map_memory,                 &o_map_memory);
    hook_sym("vkUnmapMemory",               hk_unmap_memory,               &o_unmap_memory);
    hook_sym("vkCmdUpdateBuffer",           hk_cmd_update_buffer,          &o_cmd_update_buffer);
    hook_sym("vkCmdCopyBuffer",             hk_cmd_copy_buffer,            &o_cmd_copy_buffer);
    hook_sym("vkQueuePresentKHR",           hk_queue_present,              &o_queue_present);
    hook_sym("vkGetDeviceProcAddr",         hk_get_device_proc_addr,       &o_get_device_proc_addr);
    hook_sym("vkGetInstanceProcAddr",       hk_get_instance_proc_addr,     &o_get_instance_proc_addr);
}

}  // namespace

// ---------------------------------------------------------------- 入口

void gles_probe_start(const char* out_dir);
void il2cpp_probe_start();

__attribute__((constructor)) static void probe_on_load() {
    init_pkg();
    g_out = pick_out_dir();
    LOGI("hsrprobe loaded, pkg=%s out=%s", g_pkg.c_str(), g_out.c_str());

    std::string flag = g_out + "/DISABLE";
    if (access(flag.c_str(), F_OK) == 0) {
        LOGW("found %s, hooks skipped", flag.c_str());
        return;
    }
    install_hooks();
    gles_probe_start(g_out.c_str());
    il2cpp_probe_start();
    LOGI("hsrprobe ready");
}

extern "C" __attribute__((visibility("default")))
jstring Java_com_hsrprobe_NativeLoader_nativeDump(JNIEnv* env, jclass) {
    dump_json();
    return env->NewStringUTF(g_out.c_str());
}

extern "C" __attribute__((visibility("default")))
jstring Java_com_hsrprobe_NativeLoader_nativeStatus(JNIEnv* env, jclass) {
    char b[512];
    snprintf(b, sizeof(b), "pkg=%s\nout=%s\nframes=%llu\ndraws=%llu\nunique=%zu",
             g_pkg.c_str(), g_out.c_str(), (unsigned long long)g_frames.load(),
             (unsigned long long)g_drawsTotal, g_draws.size());
    return env->NewStringUTF(b);
}
