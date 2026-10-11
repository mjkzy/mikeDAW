#include "Mp3Writer.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

namespace
{
    template <typename T>
    void safeRelease (T*& p) { if (p != nullptr) { p->Release(); p = nullptr; } }

    int chooseOutputRate (double sourceRate)
    {
        auto r = (int) juce::roundToInt (sourceRate);
        return (r % 48000 == 0 || r == 96000 || r == 192000) ? 48000 : 44100;
    }
}

struct Mp3Writer::Impl
{
    IMFSinkWriter* sink = nullptr;
    DWORD streamIndex = 0;
};

Mp3Writer::Mp3Writer (const juce::File& destination, double sourceSampleRate,
                      int numChannels, int bitrateKbps)
    : juce::AudioFormatWriter (nullptr, "MP3", sourceSampleRate,
                               (unsigned int) numChannels, 16),
      dest (destination),
      bitrate (bitrateKbps),
      outputSampleRate (chooseOutputRate (sourceSampleRate)),
      impl (std::make_unique<Impl>())
{
}

Mp3Writer::~Mp3Writer()
{
    finish();
}

bool Mp3Writer::ensureOpen()
{
    if (opened) return true;
    if (failed) return false;

    auto fail = [this] { failed = true; return false; };

    auto hrCom = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    comInitialised = (hrCom == S_OK || hrCom == S_FALSE);
    if (FAILED (hrCom) && hrCom != RPC_E_CHANGED_MODE)
        return fail();

    if (FAILED (MFStartup (MF_VERSION, MFSTARTUP_LITE)))
        return fail();
    mfStarted = true;

    if (FAILED (MFCreateSinkWriterFromURL (dest.getFullPathName().toWideCharPointer(),
                                           nullptr, nullptr, &impl->sink)))
        return fail();

    const auto channels = (UINT32) numChannels;

    IMFMediaType* outType = nullptr;
    bool outOk = SUCCEEDED (MFCreateMediaType (&outType))
        && SUCCEEDED (outType->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio))
        && SUCCEEDED (outType->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_MP3))
        && SUCCEEDED (outType->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, channels))
        && SUCCEEDED (outType->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) outputSampleRate))
        && SUCCEEDED (outType->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) (bitrate * 1000 / 8)));
    if (outOk)
        outOk = SUCCEEDED (impl->sink->AddStream (outType, &impl->streamIndex));
    safeRelease (outType);
    if (! outOk)
        return fail();

    const UINT32 blockAlign = channels * 2;
    const UINT32 srcRate = (UINT32) juce::roundToInt (sampleRate);
    IMFMediaType* inType = nullptr;
    bool inOk = SUCCEEDED (MFCreateMediaType (&inType))
        && SUCCEEDED (inType->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio))
        && SUCCEEDED (inType->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_PCM))
        && SUCCEEDED (inType->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16))
        && SUCCEEDED (inType->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, channels))
        && SUCCEEDED (inType->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, srcRate))
        && SUCCEEDED (inType->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign))
        && SUCCEEDED (inType->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, srcRate * blockAlign));
    if (inOk)
        inOk = SUCCEEDED (impl->sink->SetInputMediaType (impl->streamIndex, inType, nullptr));
    safeRelease (inType);
    if (! inOk)
        return fail();

    if (FAILED (impl->sink->BeginWriting()))
        return fail();

    opened = true;
    return true;
}

void Mp3Writer::finish()
{
    if (opened && impl != nullptr && impl->sink != nullptr)
        impl->sink->Finalize();
    if (impl != nullptr)
        safeRelease (impl->sink);

    if (mfStarted)   { MFShutdown();     mfStarted = false; }
    if (comInitialised) { CoUninitialize(); comInitialised = false; }
    opened = false;
}

bool Mp3Writer::write (const int** samplesToWrite, int numSamples)
{
    if (failed)
        return false;
    if (! ensureOpen())
        return false;
    if (numSamples <= 0)
        return true;

    const auto channels = (int) numChannels;
    const auto byteLen = (DWORD) (numSamples * channels * 2);

    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    bool ok = SUCCEEDED (MFCreateSample (&sample))
           && SUCCEEDED (MFCreateMemoryBuffer (byteLen, &buffer));

    if (ok)
    {
        BYTE* raw = nullptr;
        if (SUCCEEDED (buffer->Lock (&raw, nullptr, nullptr)))
        {
            auto* out = reinterpret_cast<int16_t*> (raw);
            for (int i = 0; i < numSamples; ++i)
                for (int ch = 0; ch < channels; ++ch)
                {
                    const int* src = samplesToWrite[ch];
                    *out++ = (int16_t) ((src != nullptr ? src[i] : 0) >> 16);
                }
            buffer->Unlock();
            ok = SUCCEEDED (buffer->SetCurrentLength (byteLen))
              && SUCCEEDED (sample->AddBuffer (buffer));
        }
        else ok = false;
    }

    if (ok)
    {
        const auto rtStart    = (LONGLONG) (sampleCounter * 10000000LL / (juce::int64) juce::roundToInt (sampleRate));
        const auto rtDuration = (LONGLONG) ((juce::int64) numSamples * 10000000LL / (juce::int64) juce::roundToInt (sampleRate));
        ok = SUCCEEDED (sample->SetSampleTime (rtStart))
          && SUCCEEDED (sample->SetSampleDuration (rtDuration))
          && SUCCEEDED (impl->sink->WriteSample (impl->streamIndex, sample));
    }

    safeRelease (buffer);
    safeRelease (sample);

    if (! ok)
    {
        failed = true;
        return false;
    }

    sampleCounter += numSamples;
    return true;
}
