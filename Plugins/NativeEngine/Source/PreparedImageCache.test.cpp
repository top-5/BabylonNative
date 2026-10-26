// Pure CPU cache/ownership oracle. No renderer or JavaScript runtime is created.
#include "PreparedImageCacheInternal.h"
#include <bimg/bimg.h>
#include <bimg/encode.h>
#include <bx/allocator.h>
#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

namespace
{
    using namespace Babylon::Plugins::NativeEngine;
    using Babylon::Plugins::NativeEngine::Detail::PreparedImageCache;
    struct CountingAllocator final : bx::AllocatorI
    {
        bx::DefaultAllocator backend;
        std::atomic<int64_t> live{};
        std::atomic<bool> failNext{};
        void* realloc(void* ptr, size_t size, size_t align, const char* file, uint32_t line) override
        {
            if (size && failNext.exchange(false)) return nullptr;
            void* result = backend.realloc(ptr, size, align, file, line);
            if (!ptr && size && result) ++live;
            if (ptr && size == 0) --live;
            return result;
        }
    };
    struct ImageFree { void operator()(bimg::ImageContainer* p) const { if (p) bimg::imageFree(p); } };
    using Image = std::unique_ptr<bimg::ImageContainer, ImageFree>;
    std::atomic<int> checks{}, failures{};
    void Check(bool value, const char* label)
    { ++checks; if (!value) { ++failures; std::cerr << label << '\n'; } }
    uint64_t Collide(const uint8_t*, size_t) noexcept { return 7; }
    PreparedImageCacheContext Context()
    { return {4, true, true, true, false, {1, 0x10002, 0x20000}}; }
    bool Equal(const bimg::ImageContainer& a, const bimg::ImageContainer& b)
    {
        return a.m_format == b.m_format && a.m_orientation == b.m_orientation && a.m_parser == b.m_parser &&
            a.m_size == b.m_size && a.m_offset == b.m_offset && a.m_width == b.m_width && a.m_height == b.m_height &&
            a.m_depth == b.m_depth && a.m_numLayers == b.m_numLayers && a.m_numMips == b.m_numMips &&
            a.m_hasAlpha == b.m_hasAlpha && a.m_cubeMap == b.m_cubeMap && a.m_ktx == b.m_ktx &&
            a.m_ktx2 == b.m_ktx2 && a.m_pvr3 == b.m_pvr3 && a.m_srgb == b.m_srgb &&
            std::memcmp(a.m_data, b.m_data, a.m_size) == 0;
    }
    Image Reference(CountingAllocator& allocator, bimg::TextureFormat::Enum format, bool srgb)
    {
        Image base{bimg::imageAlloc(&allocator, format, 8, 8, 0, 1, false, false)};
        if (format == bimg::TextureFormat::RGBA8)
        {
            auto* values = static_cast<uint8_t*>(base->m_data);
            for (uint32_t i = 0; i < base->m_size; ++i) values[i] = static_cast<uint8_t>(i * 37 + i / 7);
        }
        else
        {
            auto* values = static_cast<float*>(base->m_data);
            for (uint32_t i = 0; i < base->m_size / sizeof(float); ++i) values[i] = float(i % 31) / 30.0f;
        }
        Image prepared{bimg::imageGenerateMips(&allocator, *base, srgb)};
        prepared->m_hasAlpha = true; prepared->m_orientation = bimg::Orientation::HFlip;
        prepared->m_parser = bimg::ImageParser::Png; prepared->m_srgb = srgb;
        return prepared;
    }
}

int main()
{
    CountingAllocator allocator;
    const auto context = Context(); const std::vector<uint8_t> encoded{1, 3, 5, 7, 9, 11, 13, 15};
    {
        auto reference = Reference(allocator, bimg::TextureFormat::RGBA8, true);
        PreparedImageCache cache;
        Check(!cache.Statistics().enabled && cache.Statistics().entries == 0, "default cache is disabled");
        const auto allocations = allocator.live.load();
        Check(!cache.Store(encoded.data(), encoded.size(), context, *reference), "disabled store bypasses");
        Check(cache.Clone(encoded.data(), encoded.size(), context, allocator) == nullptr && allocator.live == allocations, "disabled lookup allocates nothing");
        Check(cache.Configure(1 << 20), "bounded budget configures");
        Check(!cache.Configure(PreparedImageCacheMaxBudgetBytes + 1) && cache.Statistics().budgetBytes == 1 << 20, "oversized configuration preserves old budget");
        Check(cache.Clone(encoded.data(), encoded.size(), context, allocator) == nullptr, "first lookup is a miss");
        Check(cache.Store(encoded.data(), encoded.size(), context, *reference), "prepared mip chain stores");
        Image first{cache.Clone(encoded.data(), encoded.size(), context, allocator)};
        Image second{cache.Clone(encoded.data(), encoded.size(), context, allocator)};
        Check(first && second && Equal(*first, *reference) && Equal(*second, *reference), "all mip bytes and metadata match actual bimg oracle");
        Check(first->m_allocator == &allocator && first->m_data != reference->m_data && first->m_data != second->m_data, "clone ownership and storage are independent");
        static_cast<uint8_t*>(first->m_data)[0] ^= 0xff;
        Check(Equal(*second, *reference), "mutating one upload clone cannot mutate cache or another clone");
        Check(cache.Store(encoded.data(), encoded.size(), context, *reference) && cache.Statistics().insertions == 1, "identical store is deduplicated");
        Check(!cache.Store(encoded.data(), std::numeric_limits<size_t>::max(), context, *reference), "size overflow refuses before hashing or buffer reads");
        Check(cache.Statistics().residentBytes >= encoded.size() + reference->m_size && cache.Statistics().residentBytes <= cache.Statistics().budgetBytes, "stored accounting includes encoded and prepared bytes");
        Check(cache.Configure(0) && cache.Statistics().residentBytes == 0, "disable releases cached storage");
        reference.reset();
        Check(second && second->m_numMips == 4, "returned upload clone survives source and cache destruction");
    }
    Check(allocator.live == 0, "default/clone/source allocations all freed through real allocator");
    for (const auto format : {bimg::TextureFormat::RGBA8, bimg::TextureFormat::RGBA32F}) for (const bool srgb : {false, true})
    {
        auto reference = Reference(allocator, format, srgb);
        PreparedImageCache cache; cache.Configure(1 << 20);
        auto c = context; c.srgb = srgb;
        Check(cache.Store(encoded.data(), encoded.size(), c, *reference), "reference format stores");
        Image clone{cache.Clone(encoded.data(), encoded.size(), c, allocator)};
        Check(clone && Equal(*clone, *reference), "RGBA8/float linear and srgb prepared bytes are exact");
        for (uint8_t mip = 0; mip < reference->m_numMips; ++mip)
        {
            bimg::ImageMip a{}, b{};
            const bool valid = bimg::imageGetRawData(*reference, 0, mip, reference->m_data, reference->m_size, a) &&
                bimg::imageGetRawData(*clone, 0, mip, clone->m_data, clone->m_size, b);
            Check(valid && a.m_size == b.m_size && a.m_width == b.m_width && a.m_height == b.m_height &&
                std::memcmp(a.m_data, b.m_data, a.m_size) == 0, "each generated mip matches bimg including smallest level");
        }
    }
    {
        auto reference = Reference(allocator, bimg::TextureFormat::RGBA8, true);
        PreparedImageCache cache{Collide}; cache.Configure(1 << 20);
        Check(cache.Store(encoded.data(), encoded.size(), context, *reference), "forced-collision entry stores");
        auto other = encoded; other[0] ^= 1;
        Check(cache.Clone(other.data(), other.size(), context, allocator) == nullptr, "hash collision cannot substitute encoded bytes");
        std::vector<PreparedImageCacheContext> variants(7, context);
        variants[0].rendererType++; variants[1].invertY = !context.invertY; variants[2].srgb = !context.srgb;
        variants[3].generateMips = !context.generateMips; variants[4].originBottomLeft = !context.originBottomLeft;
        variants[5].formatCapabilities[0] ^= 0x10000; variants[6].formatCapabilities.push_back(3);
        for (const auto& variant : variants)
            Check(cache.Clone(encoded.data(), encoded.size(), variant, allocator) == nullptr, "every context field participates in exact identity");
        const auto before = cache.Statistics();
        allocator.failNext = true;
        Check(cache.Clone(encoded.data(), encoded.size(), context, allocator) == nullptr, "allocator null is refused without bx null dereference");
        Check(cache.Statistics().allocationFailures == before.allocationFailures + 1, "allocation failure is observable");
        Image after{cache.Clone(encoded.data(), encoded.size(), context, allocator)};
        Check(after && Equal(*after, *reference), "allocation failure does not poison valid entry");
        auto bad = context; bad.formatCapabilities.resize(513);
        Check(!cache.Store(encoded.data(), encoded.size(), bad, *reference), "unbounded capabilities refuse");
        auto metadata = *reference; metadata.m_offset = 1;
        Check(!cache.Store(encoded.data(), encoded.size(), context, metadata), "offset prepared layout refuses");
        metadata = *reference; metadata.m_size--;
        Check(!cache.Store(encoded.data(), encoded.size(), context, metadata), "truncated mip layout refuses");
        static_cast<uint8_t*>(reference->m_data)[3] ^= 1;
        Check(!cache.Store(encoded.data(), encoded.size(), context, *reference), "conflicting same-key output refuses");
        Check(cache.Clone(encoded.data(), encoded.size(), context, allocator) == nullptr, "conflicting entry is evicted rather than reused");
    }
    {
        auto reference = Reference(allocator, bimg::TextureFormat::RGBA8, true);
        PreparedImageCache cache; cache.Configure(1 << 20);
        cache.Store(encoded.data(), encoded.size(), context, *reference);
        const auto charge = cache.Statistics().residentBytes;
        cache.Configure(charge * 2);
        auto a = encoded, b = encoded, c = encoded; b[0]++; c[0] += 2;
        cache.Store(a.data(), a.size(), context, *reference); cache.Store(b.data(), b.size(), context, *reference);
        Image touched{cache.Clone(a.data(), a.size(), context, allocator)};
        cache.Store(c.data(), c.size(), context, *reference);
        Check(cache.Clone(b.data(), b.size(), context, allocator) == nullptr, "least recently used entry is evicted");
        Image kept{cache.Clone(a.data(), a.size(), context, allocator)};
        Check(kept && cache.Statistics().entries == 2 && cache.Statistics().residentBytes <= charge * 2, "LRU preserves touched entry and hard stored-byte bound");
        cache.Configure(1 << 20);
        for (size_t i = 0; i < 65; ++i) { a[0] = static_cast<uint8_t>(i); cache.Store(a.data(), a.size(), context, *reference); }
        Check(cache.Statistics().entries == 64 && cache.Statistics().evictions >= 2, "entry-count bound applies even with spare byte budget");
    }
    {
        auto reference = Reference(allocator, bimg::TextureFormat::RGBA8, true);
        PreparedImageCache cache{Collide}; cache.Configure(1 << 20);
        std::atomic<bool> done{};
        std::thread reader{[&] { while (!done.load()) { auto s = cache.Statistics(); if (s.entries > 64 || s.residentBytes > s.budgetBytes) ++failures; } }};
        std::vector<std::thread> threads;
        for (uint8_t worker = 0; worker < 6; ++worker) threads.emplace_back([&, worker] {
            auto key = encoded; key[0] = worker % 2;
            for (int i = 0; i < 250; ++i)
            {
                Check(cache.Store(key.data(), key.size(), context, *reference), "concurrent immutable store succeeds");
                Image clone{cache.Clone(key.data(), key.size(), context, allocator)};
                Check(clone && Equal(*clone, *reference), "concurrent clone remains exact");
            }
        });
        for (auto& thread : threads) thread.join(); done = true; reader.join();
        Check(cache.Statistics().insertions == 2 && cache.Statistics().entries == 2 && cache.Statistics().hits == 1500,
            "concurrent duplicate insertion and lookup accounting exact after quiescence");
    }
    Check(allocator.live == 0, "all concurrent clones/source images freed without dangling cache allocator");
    Check(ConfigurePreparedImageCache(1 << 20), "public cache configures");
    { auto reference = Reference(allocator, bimg::TextureFormat::RGBA8, true);
      Check(StorePreparedImage(encoded.data(), encoded.size(), context, *reference), "public store routes exact image");
      Image image{ClonePreparedImage(encoded.data(), encoded.size(), context, allocator)};
      Check(image && Equal(*image, *reference) && GetPreparedImageCacheStatistics().hits == 1, "public snapshot/clone route to same cache"); }
    Check(ConfigurePreparedImageCache(0), "public cache returns to default disabled state");
    Check(allocator.live == 0, "public clone allocator pointer outlives proxy and is released exactly");
    std::cout << "PreparedImageCache: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
