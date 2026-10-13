#ifndef CORE_SHADER_RECOMPILER_BDARANGECACHE_HPP
#define CORE_SHADER_RECOMPILER_BDARANGECACHE_HPP

namespace ShaderRecompiler {

template<typename TBackend>
typename TBackend::Value CachedBdaUpperBound(TBackend& backend, typename TBackend::Value address,
        typename TBackend::Value count, typename TBackend::Value cached) {
    const auto valid = backend.And(backend.Nonzero(cached), backend.LessEqual32(cached, count));
    return backend.Choose32(valid, [&] {
        const auto begin = backend.LoadBegin(backend.Subtract32(cached, backend.U32(1u)));
        const auto belowNext = backend.ChooseBool(backend.Less32(cached, count), [&] {
            return backend.Less64(address, backend.LoadBegin(cached));
        }, [&] { return backend.Bool(true); });
        return backend.Select32(backend.And(backend.GreaterEqual64(address, begin), belowNext), cached, backend.U32(0u));
    }, [&] { return backend.U32(0u); });
}

}

#endif
