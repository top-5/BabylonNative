#include "ResourceStatisticsInternal.h"

namespace Babylon::Plugins::NativeEngine::Detail
{
    namespace { std::atomic<bool> commandEnabled{}; }
    bool CommandTimingEnabled() noexcept { return commandEnabled.load(std::memory_order_relaxed); }
    ResourceCounter& Counter(ResourcePhase phase) noexcept
    {
        static ResourceCounter parseImage, prepareImage, textureSubmit, imageFlip, formatConvert, mipGenerate;
        static ResourceCounter commandRead, commandScopeDispatch, commandExecute, vertexBufferCreate, indexBufferCreate, vertexBufferRecord, rafCallbacks;
        switch (phase)
        {
        case ResourcePhase::ParseImage: return parseImage;
        case ResourcePhase::PrepareImage: return prepareImage;
        case ResourcePhase::TextureSubmit: return textureSubmit;
        case ResourcePhase::ImageFlip: return imageFlip;
        case ResourcePhase::FormatConvert: return formatConvert;
        case ResourcePhase::MipGenerate: return mipGenerate;
        case ResourcePhase::CommandRead: return commandRead;
        case ResourcePhase::CommandScopeDispatch: return commandScopeDispatch;
        case ResourcePhase::CommandExecute: return commandExecute;
        case ResourcePhase::VertexBufferCreate: return vertexBufferCreate;
        case ResourcePhase::IndexBufferCreate: return indexBufferCreate;
        case ResourcePhase::VertexBufferRecord: return vertexBufferRecord;
        case ResourcePhase::RafCallbacks: return rafCallbacks;
        }
        return parseImage; // internal enum only; all declared phases are covered
    }
}

namespace Babylon::Plugins::NativeEngine
{
    void SetCommandTimingEnabled(bool enabled) noexcept { Detail::commandEnabled.store(enabled, std::memory_order_relaxed); }
    ResourceStatistics GetResourceStatistics() noexcept
    {
        return {Detail::Counter(Detail::ResourcePhase::ParseImage).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::PrepareImage).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::TextureSubmit).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::ImageFlip).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::FormatConvert).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::MipGenerate).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::CommandRead).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::CommandScopeDispatch).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::CommandExecute).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::VertexBufferCreate).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::IndexBufferCreate).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::VertexBufferRecord).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::RafCallbacks).Snapshot(), Detail::CommandTimingEnabled()};
    }
}
