#pragma once
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace PitchDetect
{
    struct Region
    {
        int64_t startSample = 0;
        int64_t endSample   = 0;
        int     midiNote    = 60;
        float   avgCents    = 0.0f;
    };

    struct Result
    {
        bool  valid    = false;
        int   midiNote = 60;
        float cents    = 0.0f;
        std::vector<Region> regions;
    };

    constexpr int    windowSize   = 4096;
    constexpr int    hopSize      = 2048;
    constexpr float  silenceGate  = 0.002f;
    constexpr float  yinThreshold = 0.15f;
    constexpr float  voicedMax    = 0.30f;
    constexpr float  minFreq      = 30.0f;
    constexpr float  maxFreq      = 1000.0f;
    constexpr int    minRegionWindows = 2;

    inline float estimateWindow (const float* x, int n, double sampleRate, float& aperiodicity)
    {
        aperiodicity = 1.0f;

        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
            peak = std::max (peak, std::abs (x[i]));
        if (peak < silenceGate)
            return 0.0f;

        const int maxTau = n / 2;
        std::vector<float> d ((size_t) maxTau, 0.0f);
        std::vector<float> dp ((size_t) maxTau, 0.0f);

        for (int tau = 1; tau < maxTau; ++tau)
        {
            float sum = 0.0f;
            for (int i = 0; i < maxTau; ++i)
            {
                float diff = x[i] - x[i + tau];
                sum += diff * diff;
            }
            d[(size_t) tau] = sum;
        }

        dp[0] = 1.0f;
        float running = 0.0f;
        for (int tau = 1; tau < maxTau; ++tau)
        {
            running += d[(size_t) tau];
            dp[(size_t) tau] = running > 0.0f ? d[(size_t) tau] * (float) tau / running : 1.0f;
        }

        int tauMin = std::max (1, (int) std::floor (sampleRate / maxFreq));
        int tauMax = std::min (maxTau - 1, (int) std::ceil (sampleRate / minFreq));
        if (tauMin >= tauMax)
            return 0.0f;

        int tau = -1;
        for (int t = tauMin; t <= tauMax; ++t)
        {
            if (dp[(size_t) t] < yinThreshold)
            {
                while (t + 1 <= tauMax && dp[(size_t) (t + 1)] < dp[(size_t) t])
                    ++t;
                tau = t;
                break;
            }
        }
        if (tau < 0)
        {
            int best = tauMin;
            for (int t = tauMin; t <= tauMax; ++t)
                if (dp[(size_t) t] < dp[(size_t) best])
                    best = t;
            tau = best;
        }

        aperiodicity = dp[(size_t) tau];
        if (aperiodicity > voicedMax)
            return 0.0f;

        float betterTau = (float) tau;
        if (tau > tauMin && tau < tauMax)
        {
            float s0 = dp[(size_t) (tau - 1)];
            float s1 = dp[(size_t) tau];
            float s2 = dp[(size_t) (tau + 1)];
            float denom = 2.0f * (2.0f * s1 - s2 - s0);
            if (std::abs (denom) > 1.0e-9f)
                betterTau = (float) tau + (s2 - s0) / denom;
        }

        float f0 = (float) (sampleRate / betterTau);
        if (f0 < minFreq || f0 > maxFreq)
            return 0.0f;
        return f0;
    }

    inline float freqToMidi (float f) { return 69.0f + 12.0f * std::log2 (f / 440.0f); }

    inline Result analyze (const float* mono, int64_t numSamples, double sampleRate)
    {
        Result r;
        if (mono == nullptr || numSamples < windowSize || sampleRate <= 0.0)
            return r;

        struct WinPitch { int64_t start; int64_t end; float midi; float rms; };
        std::vector<WinPitch> voiced;

        for (int64_t pos = 0; pos + windowSize <= numSamples; pos += hopSize)
        {
            const float* w = mono + pos;
            float aper = 1.0f;
            float f0 = estimateWindow (w, windowSize, sampleRate, aper);
            if (f0 <= 0.0f)
                continue;

            double sq = 0.0;
            for (int i = 0; i < windowSize; ++i)
                sq += (double) w[i] * w[i];
            float rms = (float) std::sqrt (sq / windowSize);

            voiced.push_back ({ pos, pos + windowSize, freqToMidi (f0), rms });
        }

        if (voiced.empty())
            return r;

        auto roundNote = [] (float midi) { return (int) std::lround (midi); };
        for (size_t i = 0; i < voiced.size();)
        {
            int note = roundNote (voiced[i].midi);
            size_t j = i;
            double centsSum = 0.0, rmsSum = 0.0;
            int count = 0;
            while (j < voiced.size() && roundNote (voiced[j].midi) == note)
            {
                float cents = (voiced[j].midi - (float) note) * 100.0f;
                centsSum += cents * voiced[j].rms;
                rmsSum   += voiced[j].rms;
                ++count;
                ++j;
            }
            if (count >= minRegionWindows)
            {
                Region reg;
                reg.startSample = voiced[i].start;
                reg.endSample   = voiced[j - 1].end;
                reg.midiNote    = note;
                reg.avgCents    = rmsSum > 0.0 ? (float) (centsSum / rmsSum) : 0.0f;
                r.regions.push_back (reg);
            }
            i = j;
        }

        std::vector<std::pair<int, double>> weight;
        for (auto& v : voiced)
        {
            int note = roundNote (v.midi);
            auto it = std::find_if (weight.begin(), weight.end(),
                                    [note] (auto& e) { return e.first == note; });
            if (it == weight.end())
                weight.push_back ({ note, (double) v.rms });
            else
                it->second += v.rms;
        }
        int bestNote = weight.front().first;
        double bestW = weight.front().second;
        for (auto& e : weight)
            if (e.second > bestW) { bestW = e.second; bestNote = e.first; }

        double centsSum = 0.0, rmsSum = 0.0;
        for (auto& v : voiced)
            if (roundNote (v.midi) == bestNote)
            {
                float cents = (v.midi - (float) bestNote) * 100.0f;
                centsSum += cents * v.rms;
                rmsSum   += v.rms;
            }

        r.valid    = true;
        r.midiNote = bestNote;
        r.cents    = rmsSum > 0.0 ? (float) (centsSum / rmsSum) : 0.0f;
        return r;
    }
}
