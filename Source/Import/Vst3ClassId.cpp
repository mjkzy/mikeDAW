#include "Vst3ClassId.h"

#if JUCE_WINDOWS
#include <windows.h>

namespace
{

struct PClassInfoRaw
{
    char cid[16];
    int32_t cardinality;
    char category[32];
    char name[64];
};

struct IPluginFactoryRaw
{
    struct VTable
    {
        void* queryInterface;
        void* addRef;
        int32_t (__stdcall* release) (void* self);
        void* getFactoryInfo;
        int32_t (__stdcall* countClasses) (void* self);
        int32_t (__stdcall* getClassInfo) (void* self, int32_t index, PClassInfoRaw* info);
        void* createInstance;
    }* vtbl;
};

using GetFactoryProc = IPluginFactoryRaw* (__stdcall*) ();

}

juce::MemoryBlock FLVst3::extractClassId (const juce::File& vst3File)
{
    juce::MemoryBlock result;
    if (! vst3File.exists() || ! vst3File.getFileName().endsWithIgnoreCase (".vst3"))
        return result;

    juce::File module = vst3File;
    if (vst3File.isDirectory())
    {
        auto inner = vst3File.getChildFile ("Contents").getChildFile ("x86_64-win")
                             .getChildFile (vst3File.getFileNameWithoutExtension() + ".vst3");
        if (inner.existsAsFile())
            module = inner;
        else
            return result;
    }

    auto handle = LoadLibraryExW (module.getFullPathName().toWideCharPointer(), nullptr,
                                  LOAD_WITH_ALTERED_SEARCH_PATH);
    if (handle == nullptr)
        return result;

    if (auto getFactory = (GetFactoryProc) GetProcAddress (handle, "GetPluginFactory"))
    {
        if (auto* factory = getFactory())
        {
            const int count = factory->vtbl->countClasses (factory);
            for (int i = 0; i < count; ++i)
            {
                PClassInfoRaw info{};
                if (factory->vtbl->getClassInfo (factory, i, &info) == 0
                    && juce::String (info.category) == "Audio Module Class")
                {
                    result.append (info.cid, 16);
                    break;
                }
            }
            factory->vtbl->release (factory);
        }
    }

    FreeLibrary (handle);
    return result;
}

#else
juce::MemoryBlock FLVst3::extractClassId (const juce::File&) { return {}; }
#endif
