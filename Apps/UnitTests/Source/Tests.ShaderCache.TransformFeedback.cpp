#include <gtest/gtest.h>

#include <Babylon/Plugins/ShaderCache.h>
#include <Babylon/Plugins/ShaderCacheInternal.h>

#include <gsl/util>

#include <sstream>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

TEST(ShaderCache, TransformFeedbackEntriesRoundTripSeparatelyFromGraphics)
{
    Babylon::Plugins::ShaderCache::Enable();
    Babylon::Plugins::ShaderCache::Clear();
    const auto disableCache = gsl::finally([] {
        Babylon::Plugins::ShaderCache::Clear();
        Babylon::Plugins::ShaderCache::Disable();
    });

    const std::string vertexSource = "void main() { gl_Position = vec4(0.0); }";
    const std::string fragmentSource = "void main() { gl_FragColor = vec4(1.0); }";
    const std::string feedbackSource = "#version 300 es\nout vec4 outPosition;\nvoid main() { outPosition = vec4(1.0); }\n";
    const std::vector<std::string> varyings{"outPosition"};

    Babylon::Graphics::BgfxShaderInfo graphics{};
    graphics.VertexBytes = {1, 2, 3};
    graphics.FragmentBytes = {4, 5};
    graphics.VertexAttributeLocations = {{"position", 0}};
    graphics.UniformStages = {{"diffuseSampler", uint8_t{2}}};

    Babylon::Graphics::BgfxShaderInfo compute{};
    compute.ComputeBytes = {9, 8, 7, 6};
    compute.UniformStages = {{"depthInput", uint8_t{5}}};
    compute.UniformNames = {{"_12_textureSize", "textureSize"}};

    Babylon::Plugins::ShaderCache::AddShader(vertexSource, fragmentSource, graphics);
    Babylon::Plugins::ShaderCache::AddTransformFeedbackShader(feedbackSource, varyings, compute);

    // Transform feedback and graphics entries use separate key spaces, and varyings are part of the key.
    EXPECT_EQ(Babylon::Plugins::ShaderCache::GetTransformFeedbackShader(vertexSource, varyings), nullptr);
    EXPECT_EQ(Babylon::Plugins::ShaderCache::GetTransformFeedbackShader(feedbackSource, {"other"}), nullptr);
    EXPECT_EQ(Babylon::Plugins::ShaderCache::GetShader(feedbackSource, ""), nullptr);

    std::stringstream stream{std::ios::in | std::ios::out | std::ios::binary};
    ASSERT_EQ(Babylon::Plugins::ShaderCache::Save(stream), 2u);
    Babylon::Plugins::ShaderCache::Clear();
    EXPECT_EQ(Babylon::Plugins::ShaderCache::GetTransformFeedbackShader(feedbackSource, varyings), nullptr);
    ASSERT_EQ(Babylon::Plugins::ShaderCache::Load(stream), 2u);

    std::string crlfFeedbackSource;
    for (const char ch : feedbackSource)
    {
        if (ch == '\n')
        {
            crlfFeedbackSource.push_back('\r');
        }
        crlfFeedbackSource.push_back(ch);
    }
    const auto loadedCompute = Babylon::Plugins::ShaderCache::GetTransformFeedbackShader(crlfFeedbackSource, varyings);
    ASSERT_NE(loadedCompute, nullptr);
    EXPECT_EQ(loadedCompute->ComputeBytes, compute.ComputeBytes);
    EXPECT_TRUE(loadedCompute->VertexBytes.empty());
    EXPECT_TRUE(loadedCompute->FragmentBytes.empty());
    EXPECT_EQ(loadedCompute->UniformStages, compute.UniformStages);
    EXPECT_EQ(loadedCompute->UniformNames, compute.UniformNames);

    const auto loadedGraphics = Babylon::Plugins::ShaderCache::GetShader(vertexSource, fragmentSource);
    ASSERT_NE(loadedGraphics, nullptr);
    EXPECT_EQ(loadedGraphics->VertexBytes, graphics.VertexBytes);
    EXPECT_EQ(loadedGraphics->FragmentBytes, graphics.FragmentBytes);
    EXPECT_TRUE(loadedGraphics->ComputeBytes.empty());
    EXPECT_EQ(loadedGraphics->VertexAttributeLocations, graphics.VertexAttributeLocations);
    EXPECT_EQ(loadedGraphics->UniformStages, graphics.UniformStages);

    std::stringstream resaved{std::ios::in | std::ios::out | std::ios::binary};
    EXPECT_EQ(Babylon::Plugins::ShaderCache::Save(resaved), 2u);
    EXPECT_EQ(resaved.str(), stream.str());
}

TEST(ShaderCache, StatisticsCountLookupsInsertionsAndPersistenceWithinEnabledLifetime)
{
    using namespace Babylon::Plugins::ShaderCache;
    Disable();
    EXPECT_FALSE(GetStatistics().enabled);
    EXPECT_EQ(GetStatistics().programHits, 0u);
    Enable();
    const auto disableCache = gsl::finally([] { Disable(); });
    EXPECT_TRUE(GetStatistics().enabled);
    EXPECT_EQ(GetShader("v", "f"), nullptr);
    Babylon::Graphics::BgfxShaderInfo graphics{};
    graphics.VertexBytes = {1, 2}; graphics.FragmentBytes = {3};
    const auto first = AddShader("v", "f", graphics);
    EXPECT_EQ(AddShader("v", "f", graphics), first);
    EXPECT_EQ(GetShader("v", "f"), first);
    EXPECT_EQ(GetShader("v", "f", {{"instance", 0}}), nullptr);
    EXPECT_NE(AddShader("v", "f", graphics, {{"instance", 0}}), first);
    EXPECT_NE(GetShader("v", "f", {{"instance", 0}}), nullptr);
    EXPECT_EQ(GetTransformFeedbackShader("v", {"outA"}), nullptr);
    Babylon::Graphics::BgfxShaderInfo feedback{}; feedback.ComputeBytes = {4, 5};
    const auto compute = AddTransformFeedbackShader("v", {"outA"}, feedback);
    EXPECT_EQ(AddTransformFeedbackShader("v", {"outA"}, feedback), compute);
    EXPECT_EQ(GetTransformFeedbackShader("v", {"outA"}), compute);
    EXPECT_EQ(GetTransformFeedbackShader("v", {"outB"}), nullptr);
    auto stats = GetStatistics();
    EXPECT_EQ(stats.programHits, 2u); EXPECT_EQ(stats.programMisses, 2u);
    EXPECT_EQ(stats.programInsertions, 2u); EXPECT_EQ(stats.programEntries, 2u);
    EXPECT_EQ(stats.transformFeedbackHits, 1u); EXPECT_EQ(stats.transformFeedbackMisses, 2u);
    EXPECT_EQ(stats.transformFeedbackInsertions, 1u); EXPECT_EQ(stats.transformFeedbackEntries, 1u);

    std::stringstream bytes{std::ios::in | std::ios::out | std::ios::binary};
    EXPECT_EQ(Save(bytes), 3u);
    const auto original = bytes.str();
    Clear();
    stats = GetStatistics();
    EXPECT_EQ(stats.clearCalls, 1u); EXPECT_EQ(stats.programEntries, 0u); EXPECT_EQ(stats.transformFeedbackEntries, 0u);
    EXPECT_EQ(stats.programInsertions, 2u); EXPECT_EQ(stats.savedEntries, 3u); EXPECT_EQ(stats.saveCalls, 1u);
    EXPECT_EQ(Load(bytes), 3u);
    std::stringstream same{original, std::ios::in | std::ios::out | std::ios::binary};
    EXPECT_EQ(Load(same), 3u); // merge existing keys, not another three current entries
    stats = GetStatistics();
    EXPECT_EQ(stats.loadCalls, 2u); EXPECT_EQ(stats.loadedEntries, 6u);
    EXPECT_EQ(stats.programEntries, 2u); EXPECT_EQ(stats.transformFeedbackEntries, 1u);
    EXPECT_EQ(stats.programInsertions, 2u); EXPECT_EQ(stats.transformFeedbackInsertions, 1u);
    std::stringstream resaved{std::ios::in | std::ios::out | std::ios::binary};
    EXPECT_EQ(Save(resaved), 3u);
    EXPECT_EQ(resaved.str(), original); // statistics never alter serialized cache bytes

    std::ostringstream badOutput; badOutput.setstate(std::ios::badbit);
    EXPECT_EQ(Save(badOutput), 3u); // existing API return value retained
    const uint32_t invalidVersion = 0;
    std::string invalidBytes(reinterpret_cast<const char*>(&invalidVersion), sizeof(invalidVersion));
    std::istringstream badVersion{invalidBytes};
    EXPECT_EQ(Load(badVersion), 0u);
    stats = GetStatistics();
    EXPECT_EQ(stats.saveCalls, 3u); EXPECT_EQ(stats.saveFailures, 1u); EXPECT_EQ(stats.savedEntries, 6u);
    EXPECT_EQ(stats.loadCalls, 3u); EXPECT_EQ(stats.loadFailures, 1u); EXPECT_EQ(stats.loadedEntries, 6u);
    EXPECT_EQ(stats.programEntries, 2u); EXPECT_EQ(stats.transformFeedbackEntries, 1u);
    Disable(); Enable();
    stats = GetStatistics();
    EXPECT_TRUE(stats.enabled); EXPECT_EQ(stats.programEntries, 0u); EXPECT_EQ(stats.programHits, 0u);
    EXPECT_EQ(stats.loadCalls, 0u); EXPECT_EQ(stats.clearCalls, 0u);
}

TEST(ShaderCache, StatisticsAndMapsSupportConcurrentWorkersWithStableLifetime)
{
    using namespace Babylon::Plugins::ShaderCache;
    Disable(); Enable();
    const auto disableCache = gsl::finally([] { Disable(); });
    constexpr size_t workerCount = 8, entriesPerWorker = 128;
    AddShader("shared", "fragment", {});
    std::atomic<bool> start{}, finished{}, invalidSnapshot{};
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < workerCount; ++worker)
    {
        workers.emplace_back([&, worker] {
            while (!start.load()) std::this_thread::yield();
            for (size_t entry = 0; entry < entriesPerWorker; ++entry)
            {
                const auto key = std::to_string(worker) + ":" + std::to_string(entry);
                if (GetShader(key, "f") || GetTransformFeedbackShader(key, {"out"})) invalidSnapshot = true;
                AddShader(key, "f", {}); AddTransformFeedbackShader(key, {"out"}, {});
                if (!GetShader(key, "f") || !GetShader("shared", "fragment") || !GetTransformFeedbackShader(key, {"out"})) invalidSnapshot = true;
            }
        });
    }
    std::thread reader([&] {
        uint64_t previousHits = 0;
        while (!finished.load())
        {
            const auto snapshot = GetStatistics();
            // Cross-field equality is intentionally not required while workers mutate counters.
            if (!snapshot.enabled || snapshot.programHits < previousHits || snapshot.programEntries > 1 + workerCount * entriesPerWorker) invalidSnapshot = true;
            previousHits = snapshot.programHits;
            std::this_thread::yield();
        }
    });
    start = true;
    for (auto& worker : workers) worker.join();
    finished = true; reader.join();
    EXPECT_FALSE(invalidSnapshot.load());
    const auto stats = GetStatistics();
    const uint64_t entries = workerCount * entriesPerWorker;
    EXPECT_EQ(stats.programMisses, entries); EXPECT_EQ(stats.programHits, 2 * entries);
    EXPECT_EQ(stats.programInsertions, entries + 1); EXPECT_EQ(stats.programEntries, entries + 1);
    EXPECT_EQ(stats.transformFeedbackMisses, entries); EXPECT_EQ(stats.transformFeedbackHits, entries);
    EXPECT_EQ(stats.transformFeedbackInsertions, entries); EXPECT_EQ(stats.transformFeedbackEntries, entries);
}

TEST(ShaderCache, StatisticsCountThrowingStreamFailuresWithoutReportingPersistedEntries)
{
    using namespace Babylon::Plugins::ShaderCache;
    Disable(); Enable();
    const auto disableCache = gsl::finally([] { Disable(); });
    class RejectWrites final : public std::streambuf
    {
        std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
        int_type overflow(int_type) override { return traits_type::eof(); }
    } rejectedWrites;
    std::ostream output{&rejectedWrites}; output.exceptions(std::ios::badbit | std::ios::failbit);
    EXPECT_THROW(Save(output), std::ios_base::failure);
    std::istringstream input; input.exceptions(std::ios::badbit | std::ios::failbit);
    EXPECT_THROW(Load(input), std::ios_base::failure);
    const auto stats = GetStatistics();
    EXPECT_EQ(stats.saveCalls, 1u); EXPECT_EQ(stats.saveFailures, 1u); EXPECT_EQ(stats.savedEntries, 0u);
    EXPECT_EQ(stats.loadCalls, 1u); EXPECT_EQ(stats.loadFailures, 1u); EXPECT_EQ(stats.loadedEntries, 0u);
    EXPECT_EQ(stats.programEntries, 0u); EXPECT_EQ(stats.transformFeedbackEntries, 0u);
}
