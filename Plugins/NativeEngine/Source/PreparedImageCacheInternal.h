#pragma once
#include <Babylon/Plugins/PreparedImageCache.h>
#include <atomic>
#include <array>
#include <memory>
#include <mutex>

namespace Babylon::Plugins::NativeEngine::Detail
{
    class PreparedImageCache final
    {
    public:
        using HashFunction = uint64_t (*)(const uint8_t*, size_t) noexcept;
        explicit PreparedImageCache(HashFunction hash = nullptr) noexcept;
        ~PreparedImageCache();
        bool Configure(uint64_t budgetBytes) noexcept;
        PreparedImageCacheStatistics Statistics() const noexcept;
        bimg::ImageContainer* Clone(const uint8_t*, size_t, const PreparedImageCacheContext&, bx::AllocatorI&) noexcept;
        bool Store(const uint8_t*, size_t, const PreparedImageCacheContext&, const bimg::ImageContainer&) noexcept;
        static constexpr size_t MaxEntries = 64;
    private:
        struct Entry;
        HashFunction m_hash;
        std::mutex m_mutex;
        std::array<std::unique_ptr<Entry>, MaxEntries> m_entries{};
        size_t m_size{};
        struct Counters
        {
            std::atomic<uint64_t> budgetBytes{}, residentBytes{}, entries{};
            std::atomic<uint64_t> lookups{}, hits{}, misses{}, stores{}, insertions{}, evictions{};
            std::atomic<uint64_t> oversize{}, bypasses{}, rejected{}, allocationFailures{}, keyComparisons{};
        } m_stats;
        static uint64_t Hash(const uint8_t*, size_t) noexcept;
        static bool ContextValid(const PreparedImageCacheContext&) noexcept;
        static bool ImageValid(const bimg::ImageContainer&) noexcept;
        static bool MetadataEqual(const bimg::ImageContainer&, const bimg::ImageContainer&) noexcept;
        bool Matches(const Entry&, uint64_t, const uint8_t*, size_t, const PreparedImageCacheContext&) noexcept;
        void Remove(size_t) noexcept;
        void Touch(size_t) noexcept;
    };
}
