#pragma once

#include <cstdint>
#include <fstream>

namespace Babylon::Plugins::ShaderCache
{
    struct Statistics
    {
        bool enabled{};
        uint64_t programHits{}, programMisses{}, programInsertions{}, programEntries{};
        uint64_t transformFeedbackHits{}, transformFeedbackMisses{}, transformFeedbackInsertions{}, transformFeedbackEntries{};
        uint64_t loadCalls{}, loadFailures{}, loadedEntries{};
        uint64_t saveCalls{}, saveFailures{}, savedEntries{};
        uint64_t clearCalls{};
    };

    // Nonblocking, per-field atomic snapshot; fields need not be mutually coherent while cache work is active.
    // Lookup hits mean cached compiler output was found, not that bgfx/driver program creation was avoided.
    // Insertions count newly added compiled keys, not duplicate Add calls or entries read by Load.
    // loadedEntries/savedEntries sum entries processed by successful calls (a repeated Load can merge keys).
    // Clear/Load/Save preserve lifetime counters. Disable->Enable begins a new lifetime; disabled returns zeros.
    // As before, Enable/Disable require a quiescent cache lifetime; this does not make concurrent destruction safe.
    Statistics GetStatistics();

    // Enables the shader cache.
    void Enable();

    // Disables the shader cache.
    void Disable();

    // Returns whether the shader cache is enabled.
    bool IsEnabled();

    // Clears the shader cache.
    void Clear();

    // Saves the shader cache to an output file stream.
    // Returns the number of entries in the shader cache.
    uint32_t Save(std::ostream& stream);

    // Loads the shader cache from an input file stream.
    // Returns the number of entries in the shader cache.
    uint32_t Load(std::istream& stream);
}
