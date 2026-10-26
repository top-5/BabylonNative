#pragma once

#include <Babylon/Plugins/ShaderCacheInternal.h>
#include <Babylon/Plugins/ShaderCache.h>

#include <fstream>
#include <memory>
#include <string_view>
#include <map>
#include <atomic>
#include <mutex>

#include "xxhash.h"

namespace Babylon::Plugins::ShaderCache
{
    class ShaderCacheImpl final
    {
    public:
        ShaderCacheImpl() = default;

        uint32_t Save(std::ostream& stream);
        uint32_t Load(std::istream& stream);

        void Clear();
        Statistics GetStatistics() const;

        std::shared_ptr<Graphics::BgfxShaderInfo> AddShader(std::string_view vertexSource, std::string_view fragmentSource, Graphics::BgfxShaderInfo shaderInfo, const std::map<std::string, uint32_t>& instancedAttributes);
        std::shared_ptr<Graphics::BgfxShaderInfo> GetShader(std::string_view vertexSource, std::string_view fragmentSource, const std::map<std::string, uint32_t>& instancedAttributes);

        std::shared_ptr<Graphics::BgfxShaderInfo> AddTransformFeedbackShader(std::string_view vertexSource, const std::vector<std::string>& varyings, Graphics::BgfxShaderInfo shaderInfo);
        std::shared_ptr<Graphics::BgfxShaderInfo> GetTransformFeedbackShader(std::string_view vertexSource, const std::vector<std::string>& varyings);

        static inline std::unique_ptr<ShaderCacheImpl> Instance;

    private:
        using ShaderHash = std::pair<uint64_t, uint64_t>;
        using ShaderMap = std::map<ShaderHash, std::shared_ptr<Graphics::BgfxShaderInfo>>;

        ShaderHash Hash(std::string_view vertexSource, std::string_view fragmentSource, const std::map<std::string, uint32_t>& instancedAttributes);
        ShaderHash Hash(std::string_view vertexSource, const std::vector<std::string>& varyings);

        static void SaveEntries(std::ostream& stream, const ShaderMap& entries);
        static uint32_t LoadEntries(std::istream& stream, ShaderMap& entries);

        ShaderMap m_cache;
        ShaderMap m_computeCache;
        // Compiler work is dispatched to workers; cache maps and persistence must not race those lookups.
        std::mutex m_mutex;
        struct Counters
        {
            std::atomic<uint64_t> programHits{}, programMisses{}, programInsertions{}, programEntries{};
            std::atomic<uint64_t> transformFeedbackHits{}, transformFeedbackMisses{}, transformFeedbackInsertions{}, transformFeedbackEntries{};
            std::atomic<uint64_t> loadCalls{}, loadFailures{}, loadedEntries{};
            std::atomic<uint64_t> saveCalls{}, saveFailures{}, savedEntries{};
            std::atomic<uint64_t> clearCalls{};
        } m_counters;
        void UpdateEntryCounts(); // caller holds m_mutex
    };
}
