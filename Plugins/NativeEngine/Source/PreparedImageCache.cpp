#include "PreparedImageCacheInternal.h"
#include <bimg/bimg.h>
#include <bx/allocator.h>
#include <bx/hash.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <new>

namespace Babylon::Plugins::NativeEngine::Detail
{
    struct PreparedImageCache::Entry
    {
        uint64_t hash{}, charge{};
        size_t encodedSize{};
        uint32_t rendererType{};
        bool invertY{}, srgb{}, generateMips{}, originBottomLeft{};
        size_t capabilityCount{};
        std::array<uint32_t, 512> capabilities{};
        bimg::ImageContainer metadata{};
        std::unique_ptr<uint8_t[]> encoded, prepared;
    };

    PreparedImageCache::PreparedImageCache(HashFunction hash) noexcept : m_hash{hash ? hash : Hash} {}
    PreparedImageCache::~PreparedImageCache() = default;
    uint64_t PreparedImageCache::Hash(const uint8_t* data, size_t size) noexcept
    {
        bx::HashMurmur2A hash;
        hash.begin(); hash.add(data, static_cast<int32_t>(size)); return hash.end();
    }
    bool PreparedImageCache::ContextValid(const PreparedImageCacheContext& c) noexcept
    {
        return !c.formatCapabilities.empty() && c.formatCapabilities.size() <= 512;
    }
    bool PreparedImageCache::MetadataEqual(const bimg::ImageContainer& a, const bimg::ImageContainer& b) noexcept
    {
        return a.m_format == b.m_format && a.m_orientation == b.m_orientation && a.m_parser == b.m_parser &&
            a.m_size == b.m_size && a.m_offset == b.m_offset && a.m_width == b.m_width && a.m_height == b.m_height &&
            a.m_depth == b.m_depth && a.m_numLayers == b.m_numLayers && a.m_numMips == b.m_numMips &&
            a.m_hasAlpha == b.m_hasAlpha && a.m_cubeMap == b.m_cubeMap && a.m_ktx == b.m_ktx &&
            a.m_ktx2 == b.m_ktx2 && a.m_pvr3 == b.m_pvr3 && a.m_srgb == b.m_srgb;
    }
    bool PreparedImageCache::ImageValid(const bimg::ImageContainer& image) noexcept
    {
        if (!image.m_data || image.m_offset != 0 || image.m_width == 0 || image.m_height == 0 || image.m_depth > 1 ||
            image.m_width > 65535 || image.m_height > 65535 || image.m_numLayers != 1 || image.m_cubeMap ||
            static_cast<uint32_t>(image.m_format) >= bimg::TextureFormat::Count || image.m_numMips == 0 || image.m_size == 0) return false;
        // Existing bimg leaves one-dimensional tails unwritten. Cache only
        // deterministic full square power-of-two chains, or single-level images.
        if (image.m_numMips > 1 && (image.m_width != image.m_height || (image.m_width & (image.m_width - 1)) != 0 ||
            (image.m_format != bimg::TextureFormat::RGBA8 && image.m_format != bimg::TextureFormat::RGBA32F))) return false;
        const uint32_t depth = std::max<uint32_t>(1, image.m_depth);
        const uint8_t mips = image.m_numMips > 1 ? bimg::imageGetNumMips(image.m_format, image.m_width, image.m_height, depth) : 1;
        return mips == image.m_numMips && bimg::imageGetSize(nullptr, image.m_width, image.m_height, depth, false,
            image.m_numMips > 1, 1, image.m_format) == image.m_size;
    }
    bool PreparedImageCache::Configure(uint64_t budget) noexcept
    {
        if (budget > PreparedImageCacheMaxBudgetBytes) { ++m_stats.rejected; return false; }
        try
        {
            const std::lock_guard<std::mutex> lock{m_mutex};
            for (auto& entry : m_entries) entry.reset();
            m_size = 0; m_stats.residentBytes.store(0); m_stats.entries.store(0); m_stats.budgetBytes.store(budget);
            return true;
        }
        catch (...) { ++m_stats.rejected; return false; }
    }
    PreparedImageCacheStatistics PreparedImageCache::Statistics() const noexcept
    {
        PreparedImageCacheStatistics s{};
#define READ(name) s.name = m_stats.name.load(std::memory_order_relaxed)
        READ(budgetBytes); READ(residentBytes); READ(entries); READ(lookups); READ(hits); READ(misses);
        READ(stores); READ(insertions); READ(evictions); READ(oversize); READ(bypasses); READ(rejected);
        READ(allocationFailures); READ(keyComparisons);
#undef READ
        s.enabled = s.budgetBytes != 0; return s;
    }
    bool PreparedImageCache::Matches(const Entry& e, uint64_t hash, const uint8_t* encoded, size_t size,
        const PreparedImageCacheContext& c) noexcept
    {
        if (hash != e.hash) return false;
        ++m_stats.keyComparisons;
        return e.encodedSize == size && e.rendererType == c.rendererType && e.invertY == c.invertY && e.srgb == c.srgb &&
            e.generateMips == c.generateMips && e.originBottomLeft == c.originBottomLeft && e.capabilityCount == c.formatCapabilities.size() &&
            std::equal(c.formatCapabilities.begin(), c.formatCapabilities.end(), e.capabilities.begin()) &&
            std::memcmp(e.encoded.get(), encoded, size) == 0;
    }
    void PreparedImageCache::Remove(size_t index) noexcept
    {
        m_stats.residentBytes.fetch_sub(m_entries[index]->charge); m_stats.entries.fetch_sub(1);
        m_entries[index].reset();
        for (size_t i = index; i + 1 < m_size; ++i) m_entries[i] = std::move(m_entries[i + 1]);
        --m_size;
    }
    void PreparedImageCache::Touch(size_t index) noexcept
    {
        auto entry = std::move(m_entries[index]);
        for (size_t i = index; i > 0; --i) m_entries[i] = std::move(m_entries[i - 1]);
        m_entries[0] = std::move(entry);
    }
    namespace
    {
        // Use the real bimg/bx allocation path and alignment header. Throw before
        // bx dereferences a null allocation, then restore the real owner pointer.
        struct FailFastAllocator final : bx::AllocatorI
        {
            explicit FailFastAllocator(bx::AllocatorI& a) : real{a} {}
            bx::AllocatorI& real;
            void* realloc(void* ptr, size_t size, size_t align, const char* file, uint32_t line) override
            {
                void* result = real.realloc(ptr, size, align, file, line);
                if (size && !result) throw std::bad_alloc{};
                return result;
            }
        };
    }
    bimg::ImageContainer* PreparedImageCache::Clone(const uint8_t* encoded, size_t size,
        const PreparedImageCacheContext& c, bx::AllocatorI& allocator) noexcept
    {
        const uint64_t budget = m_stats.budgetBytes.load();
        if (!budget) { ++m_stats.bypasses; return nullptr; }
        if (!encoded || size == 0 || !ContextValid(c)) { ++m_stats.rejected; return nullptr; }
        if (size > budget) { ++m_stats.oversize; return nullptr; }
        const auto hash = m_hash(encoded, size);
        try
        {
            const std::lock_guard<std::mutex> lock{m_mutex};
            if (!m_stats.budgetBytes.load()) { ++m_stats.bypasses; return nullptr; }
            ++m_stats.lookups;
            for (size_t index = 0; index < m_size; ++index)
            {
                const Entry& e = *m_entries[index];
                if (!Matches(e, hash, encoded, size, c)) continue;
                const auto& meta = e.metadata;
                FailFastAllocator proxy{allocator};
                auto* image = bimg::imageAlloc(&proxy, meta.m_format, meta.m_width, meta.m_height, meta.m_depth, 1, false, meta.m_numMips > 1);
                if (!image) { ++m_stats.allocationFailures; return nullptr; }
                image->m_allocator = &allocator;
                if (image->m_size != meta.m_size || image->m_numMips != meta.m_numMips)
                { bimg::imageFree(image); ++m_stats.rejected; return nullptr; }
                void* data = image->m_data;
                *image = meta; image->m_allocator = &allocator; image->m_data = data;
                std::memcpy(data, e.prepared.get(), meta.m_size);
                Touch(index); ++m_stats.hits;
                return image;
            }
            ++m_stats.misses; return nullptr;
        }
        catch (...) { ++m_stats.allocationFailures; return nullptr; }
    }
    bool PreparedImageCache::Store(const uint8_t* encoded, size_t size, const PreparedImageCacheContext& c,
        const bimg::ImageContainer& image) noexcept
    {
        const uint64_t budget = m_stats.budgetBytes.load();
        if (!budget) { ++m_stats.bypasses; return false; }
        if (!encoded || size == 0 || !ContextValid(c) || !ImageValid(image)) { ++m_stats.rejected; return false; }
        if (size > budget) { ++m_stats.oversize; return false; }
        const uint64_t overhead = sizeof(Entry);
        const uint64_t charge = overhead + uint64_t(size) + image.m_size;
        if (charge > budget) { ++m_stats.oversize; return false; }
        const auto hash = m_hash(encoded, size);
        try
        {
            const std::lock_guard<std::mutex> lock{m_mutex};
            const auto nowBudget = m_stats.budgetBytes.load();
            if (!nowBudget) { ++m_stats.bypasses; return false; }
            if (charge > nowBudget) { ++m_stats.oversize; return false; }
            ++m_stats.stores;
            for (size_t index = 0; index < m_size; ++index)
            {
                const auto& existing = *m_entries[index];
                if (!Matches(existing, hash, encoded, size, c)) continue;
                if (!MetadataEqual(existing.metadata, image) || std::memcmp(existing.prepared.get(), image.m_data, image.m_size) != 0)
                {
                    Remove(index); ++m_stats.evictions; ++m_stats.rejected; return false;
                }
                Touch(index); return true;
            }
            while (m_size && (m_size >= MaxEntries || m_stats.residentBytes.load() > nowBudget - charge))
            { Remove(m_size - 1); ++m_stats.evictions; }
            auto entry = std::make_unique<Entry>();
            entry->encoded = std::make_unique<uint8_t[]>(size);
            entry->prepared = std::make_unique<uint8_t[]>(image.m_size);
            entry->hash = hash; entry->charge = charge; entry->encodedSize = size; entry->rendererType = c.rendererType;
            entry->invertY = c.invertY; entry->srgb = c.srgb; entry->generateMips = c.generateMips; entry->originBottomLeft = c.originBottomLeft;
            entry->capabilityCount = c.formatCapabilities.size();
            std::copy(c.formatCapabilities.begin(), c.formatCapabilities.end(), entry->capabilities.begin());
            entry->metadata = image; entry->metadata.m_allocator = nullptr; entry->metadata.m_data = nullptr;
            std::memcpy(entry->encoded.get(), encoded, size); std::memcpy(entry->prepared.get(), image.m_data, image.m_size);
            for (size_t i = m_size; i > 0; --i) m_entries[i] = std::move(m_entries[i - 1]);
            m_entries[0] = std::move(entry); ++m_size;
            m_stats.residentBytes.fetch_add(charge); m_stats.entries.fetch_add(1); ++m_stats.insertions;
            return true;
        }
        catch (...) { ++m_stats.allocationFailures; return false; }
    }
}

namespace Babylon::Plugins::NativeEngine
{
    namespace { Detail::PreparedImageCache& Cache() { static Detail::PreparedImageCache cache; return cache; } }
    bool ConfigurePreparedImageCache(uint64_t budget) noexcept { return Cache().Configure(budget); }
    PreparedImageCacheStatistics GetPreparedImageCacheStatistics() noexcept { return Cache().Statistics(); }
    bimg::ImageContainer* ClonePreparedImage(const uint8_t* data, size_t size, const PreparedImageCacheContext& context, bx::AllocatorI& allocator) noexcept
    { return Cache().Clone(data, size, context, allocator); }
    bool StorePreparedImage(const uint8_t* data, size_t size, const PreparedImageCacheContext& context, const bimg::ImageContainer& image) noexcept
    { return Cache().Store(data, size, context, image); }
}
