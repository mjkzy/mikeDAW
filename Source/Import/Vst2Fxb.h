#pragma once
#include <JuceHeader.h>
#include <cstring>

namespace Vst2Fxb
{
    constexpr size_t fxBankHeaderSize = 160;
    constexpr size_t fxBankChunkSizeOffset = 156;

    inline bool magicMatches (const juce::uint8* p, const char* name)
    {
        auto n = reinterpret_cast<const juce::uint8*> (name);
        return (p[0] == n[0] && p[1] == n[1] && p[2] == n[2] && p[3] == n[3])
            || (p[0] == n[3] && p[1] == n[2] && p[2] == n[1] && p[3] == n[0]);
    }

    inline bool isFxBank (const void* data, size_t size)
    {
        if (data == nullptr || size < fxBankHeaderSize)
            return false;
        auto* p = static_cast<const juce::uint8*> (data);
        return magicMatches (p, "CcnK") && magicMatches (p + 8, "FBCh");
    }

    inline juce::MemoryBlock extractChunk (const void* data, size_t size)
    {
        if (! isFxBank (data, size))
            return {};

        auto* p = static_cast<const juce::uint8*> (data);
        auto chunkSize = (size_t) juce::ByteOrder::bigEndianInt (p + fxBankChunkSizeOffset);
        if (chunkSize == 0 || chunkSize > size - fxBankHeaderSize)
            return {};

        return juce::MemoryBlock (p + fxBankHeaderSize, chunkSize);
    }

    inline juce::MemoryBlock wrapChunk (const void* chunk, size_t size, juce::uint32 fxId)
    {
        juce::MemoryBlock mb (fxBankHeaderSize + size, true);
        auto* p = static_cast<juce::uint8*> (mb.getData());

        auto putBigEndian = [p] (size_t offset, juce::uint32 v)
        {
            p[offset]     = (juce::uint8) ((v >> 24) & 0xff);
            p[offset + 1] = (juce::uint8) ((v >> 16) & 0xff);
            p[offset + 2] = (juce::uint8) ((v >> 8) & 0xff);
            p[offset + 3] = (juce::uint8) (v & 0xff);
        };

        memcpy (p, "CcnK", 4);
        memcpy (p + 8, "FBCh", 4);
        putBigEndian (12, 1);
        putBigEndian (16, fxId);
        putBigEndian (20, 1);
        putBigEndian (24, 1);
        putBigEndian (fxBankChunkSizeOffset, (juce::uint32) size);

        if (size > 0)
            memcpy (p + fxBankHeaderSize, chunk, size);

        return mb;
    }
}
