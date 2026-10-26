#include "ResourceStatisticsInternal.h"

namespace Babylon::Plugins::NativeEngine::Detail
{
    ResourceCounter& Counter(ResourcePhase phase) noexcept
    {
        static ResourceCounter parseImage, prepareImage, textureSubmit, imageFlip, formatConvert, mipGenerate;
        switch (phase)
        {
        case ResourcePhase::ParseImage: return parseImage;
        case ResourcePhase::PrepareImage: return prepareImage;
        case ResourcePhase::TextureSubmit: return textureSubmit;
        case ResourcePhase::ImageFlip: return imageFlip;
        case ResourcePhase::FormatConvert: return formatConvert;
        case ResourcePhase::MipGenerate: return mipGenerate;
        }
        return parseImage; // internal enum only; all declared phases are covered
    }
}

namespace Babylon::Plugins::NativeEngine
{
    ResourceStatistics GetResourceStatistics() noexcept
    {
        return {Detail::Counter(Detail::ResourcePhase::ParseImage).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::PrepareImage).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::TextureSubmit).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::ImageFlip).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::FormatConvert).Snapshot(),
            Detail::Counter(Detail::ResourcePhase::MipGenerate).Snapshot()};
    }
}
