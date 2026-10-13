#ifndef CORE_SHADER_RECOMPILER_SPECIALIZATIONCACHE_HPP
#define CORE_SHADER_RECOMPILER_SPECIALIZATIONCACHE_HPP

#include "BudgetedShaderCache.hpp"
#include "Optimization/DescriptorBindingBuilder.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ShaderRecompiler {

enum class SpecializationCachePart { Modules, Prepared, Registration, Plans };

inline std::size_t SpecializationCacheLimit(SpecializationCachePart part) {
    static const auto total = ParseShaderCacheMiB(std::getenv("APS5_SPECIALIZATION_CACHE_MIB"));
    switch (part) {
    case SpecializationCachePart::Modules: return total / 2u;
    case SpecializationCachePart::Prepared: return total / 4u;
    case SpecializationCachePart::Registration:
    case SpecializationCachePart::Plans: return total / 8u;
    }
    throw std::runtime_error("invalid specialization cache part");
}

inline std::size_t DescriptorPlanAllocationBytes(const DescriptorBindingPlan& plan) {
    auto bytes = AddCacheBytes(CacheVectorBytes(plan.bindings), CacheVectorBytes(plan.imageMetadata));
    bytes = AddCacheBytes(bytes, CacheVectorBytes(plan.specialization));
    bytes = AddCacheBytes(bytes, CacheVectorBytes(plan.runtimeState));
    for (const auto& binding : plan.bindings) {
        bytes = AddCacheBytes(bytes, CacheVectorBytes(binding.resources));
        bytes = AddCacheBytes(bytes, CacheVectorBytes(binding.identityImageSwizzle));
        bytes = AddCacheBytes(bytes, CacheVectorBytes(binding.samplerFilterElements));
        bytes = AddCacheBytes(bytes, CacheVectorBytes(binding.inactive));
        const auto& descriptor = binding.descriptor;
        bytes = AddCacheBytes(bytes, CacheVectorBytes(descriptor.guestDescriptor));
        bytes = AddCacheBytes(bytes, CacheVectorBytes(descriptor.imageSamplers));
        for (const auto* flags : {&descriptor.samplerDepthCompare, &descriptor.imageWritten, &descriptor.imageDepthCompare,
                &descriptor.imageAtomic, &descriptor.imageAtomic64, &descriptor.bufferAtomic, &descriptor.bufferWritten,
                &descriptor.samplerUnnormalized, &descriptor.samplerIntegerBorder, &descriptor.imageUnnormalized,
                &descriptor.imageUnnormalizedBaseLevel, &descriptor.bufferRead}) {
            bytes = AddCacheBytes(bytes, CacheVectorBytes(*flags));
        }
    }
    return bytes;
}

template<typename TModule>
std::size_t SpecializedModuleAllocationBytes(const std::shared_ptr<const TModule>& module) {
    if (!module) return 0u;
    auto bytes = AddCacheBytes(sizeof(TModule) + 4u * sizeof(void*), CacheVectorBytes(module->spirv.Words()));
    return AddCacheBytes(bytes, CacheVectorBytes(module->bindings));
}

template<typename TKey, typename TValue>
void ReportSpecializationCache(const BudgetedShaderCache<TKey, TValue>& cache, const char* name) {
    static const bool profile = [] { const auto* value = std::getenv("APS5_PROFILE_SHADER_CACHE"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    if (!profile) return;
    static std::atomic<std::int64_t> lastReport{0};
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = lastReport.load(std::memory_order_relaxed);
    if ((last != 0 && now - last < 10'000'000'000ll) || !lastReport.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
    const auto stats = cache.Stats();
    std::fprintf(stderr, "[shader-cache] %s bounded=%u budget_bytes=%zu retained_estimate_bytes=%zu entries=%zu pending=%zu hits=%llu misses=%llu evictions=%llu oversized=%llu\n",
        name, cache.Bounded() ? 1u : 0u, stats.budgetBytes, stats.retainedBytes, stats.entries, stats.pending,
        static_cast<unsigned long long>(stats.hits), static_cast<unsigned long long>(stats.misses),
        static_cast<unsigned long long>(stats.evictions), static_cast<unsigned long long>(stats.oversized));
}

}

#endif
