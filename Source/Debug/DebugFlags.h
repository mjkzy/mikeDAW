#pragma once
#include <atomic>

namespace DebugFlags
{
    inline std::atomic<bool> disableSpectrum { false };
    inline std::atomic<bool> disableWaveformThumbnails { false };
    inline std::atomic<bool> logHitchesToFile { false };
}
