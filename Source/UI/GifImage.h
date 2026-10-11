#pragma once
#include <JuceHeader.h>
#include <vector>

#if JUCE_WINDOWS
 #include <wincodec.h>
 #include <propidl.h>
 #include <wrl/client.h>
 #pragma comment (lib, "windowscodecs.lib")
 #pragma comment (lib, "ole32.lib")
#endif

struct GifImage
{
    std::vector<juce::Image> frames;
    std::vector<int> delaysMs;
    int width = 0, height = 0;

    bool isAnimated() const noexcept { return frames.size() > 1; }

    static bool looksLikeGif (const void* data, size_t size) noexcept
    {
        return size >= 4 && std::memcmp (data, "GIF8", 4) == 0;
    }

    static bool decode (const void* data, size_t size, GifImage& out,
                        size_t memoryBudget = 80u * 1024u * 1024u)
    {
       #if JUCE_WINDOWS
        using Microsoft::WRL::ComPtr;

        if (! looksLikeGif (data, size))
            return false;

        ComPtr<IWICImagingFactory> factory;
        if (FAILED (CoCreateInstance (CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS (&factory))))
            return false;

        ComPtr<IWICStream> stream;
        if (FAILED (factory->CreateStream (&stream))
            || FAILED (stream->InitializeFromMemory (
                           reinterpret_cast<BYTE*> (const_cast<void*> (data)), (DWORD) size)))
            return false;

        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED (factory->CreateDecoderFromStream (stream.Get(), nullptr,
                                                      WICDecodeMetadataCacheOnLoad, &decoder)))
            return false;

        UINT frameCount = 0;
        if (FAILED (decoder->GetFrameCount (&frameCount)) || frameCount == 0)
            return false;

        unsigned canvasW = 0, canvasH = 0;
        {
            ComPtr<IWICMetadataQueryReader> gr;
            if (SUCCEEDED (decoder->GetMetadataQueryReader (&gr)))
            {
                readMetaUInt (gr.Get(), L"/logscrdesc/Width", canvasW);
                readMetaUInt (gr.Get(), L"/logscrdesc/Height", canvasH);
            }
        }
        if (canvasW == 0 || canvasH == 0)
        {
            ComPtr<IWICBitmapFrameDecode> f0;
            if (FAILED (decoder->GetFrame (0, &f0)))
                return false;
            f0->GetSize (&canvasW, &canvasH);
        }
        if (canvasW == 0 || canvasH == 0 || canvasW > 8192 || canvasH > 8192)
            return false;

        const size_t perFrame = (size_t) canvasW * canvasH * 4u;
        UINT framesToDecode = frameCount;
        if (perFrame * frameCount > memoryBudget)
            framesToDecode = 1;

        out.width  = (int) canvasW;
        out.height = (int) canvasH;
        out.frames.clear();
        out.delaysMs.clear();

        juce::Image canvas (juce::Image::ARGB, (int) canvasW, (int) canvasH, true);
        juce::Image previous;

        for (UINT i = 0; i < framesToDecode; ++i)
        {
            ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED (decoder->GetFrame (i, &frame)))
                break;

            unsigned left = 0, top = 0, delay = 0, disposal = 0;
            {
                ComPtr<IWICMetadataQueryReader> mr;
                if (SUCCEEDED (frame->GetMetadataQueryReader (&mr)))
                {
                    readMetaUInt (mr.Get(), L"/imgdesc/Left", left);
                    readMetaUInt (mr.Get(), L"/imgdesc/Top", top);
                    readMetaUInt (mr.Get(), L"/grctlext/Delay", delay);
                    readMetaUInt (mr.Get(), L"/grctlext/Disposal", disposal);
                }
            }

            juce::Image frameImg = toImage (factory.Get(), frame.Get());
            if (! frameImg.isValid())
                break;

            if (disposal == 3)
                previous = canvas.createCopy();

            {
                juce::Graphics g (canvas);
                g.drawImageAt (frameImg, (int) left, (int) top);
            }

            out.frames.push_back (canvas.createCopy());

            int ms = (int) delay * 10;
            out.delaysMs.push_back (ms <= 0 ? 100 : juce::jmax (20, ms));

            if (disposal == 2)
                clearRect (canvas, { (int) left, (int) top, frameImg.getWidth(), frameImg.getHeight() });
            else if (disposal == 3 && previous.isValid())
                canvas = previous;
        }

        return ! out.frames.empty();
       #else
        juce::ignoreUnused (data, size, out, memoryBudget);
        return false;
       #endif
    }

private:
   #if JUCE_WINDOWS
    static bool readMetaUInt (IWICMetadataQueryReader* r, LPCWSTR query, unsigned& value) noexcept
    {
        PROPVARIANT pv;
        PropVariantInit (&pv);
        bool ok = false;
        if (SUCCEEDED (r->GetMetadataByName (query, &pv)))
        {
            switch (pv.vt)
            {
                case VT_UI1: value = pv.bVal;  ok = true; break;
                case VT_UI2: value = pv.uiVal; ok = true; break;
                case VT_UI4: value = pv.ulVal; ok = true; break;
                default: break;
            }
        }
        PropVariantClear (&pv);
        return ok;
    }

    static juce::Image toImage (IWICImagingFactory* factory, IWICBitmapFrameDecode* frame)
    {
        using Microsoft::WRL::ComPtr;

        ComPtr<IWICFormatConverter> conv;
        if (FAILED (factory->CreateFormatConverter (&conv)))
            return {};
        if (FAILED (conv->Initialize (frame, GUID_WICPixelFormat32bppBGRA,
                                      WICBitmapDitherTypeNone, nullptr, 0.0,
                                      WICBitmapPaletteTypeCustom)))
            return {};

        UINT w = 0, h = 0;
        conv->GetSize (&w, &h);
        if (w == 0 || h == 0)
            return {};

        const UINT stride = w * 4;
        std::vector<BYTE> buffer ((size_t) stride * h);
        if (FAILED (conv->CopyPixels (nullptr, stride, (UINT) buffer.size(), buffer.data())))
            return {};

        juce::Image img (juce::Image::ARGB, (int) w, (int) h, true);
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        for (UINT y = 0; y < h; ++y)
        {
            const BYTE* src = buffer.data() + (size_t) y * stride;
            juce::uint8* dst = bd.getLinePointer ((int) y);
            for (UINT x = 0; x < w; ++x)
            {
                const juce::uint8 b = src[0], gr = src[1], r = src[2], a = src[3];
                dst[0] = (juce::uint8) ((b * a + 127) / 255);
                dst[1] = (juce::uint8) ((gr * a + 127) / 255);
                dst[2] = (juce::uint8) ((r * a + 127) / 255);
                dst[3] = a;
                src += 4;
                dst += 4;
            }
        }
        return img;
    }

    static void clearRect (juce::Image& img, juce::Rectangle<int> rect)
    {
        auto clipped = rect.getIntersection (img.getBounds());
        if (clipped.isEmpty())
            return;
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        for (int y = clipped.getY(); y < clipped.getBottom(); ++y)
        {
            juce::uint8* line = bd.getLinePointer (y);
            std::memset (line + (size_t) clipped.getX() * 4, 0, (size_t) clipped.getWidth() * 4);
        }
    }
   #endif
};
