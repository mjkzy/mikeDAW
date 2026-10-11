#pragma once
#include <JuceHeader.h>
#include <atomic>
#include "../Model/TreePath.h"

class TreeSync : private juce::ValueTree::Listener
{
public:
    explicit TreeSync (const juce::ValueTree& rootTree);
    ~TreeSync() override;

    virtual void stateChanged (const void* data, size_t size) = 0;

    static bool applyChange (juce::ValueTree& root, const void* data, size_t size, juce::String* why = nullptr);

    static constexpr juce::uint8 formatTag = 0x7f;
    static constexpr juce::uint8 removeChildOp = 4;

    static bool looksLikeDelta (const void* data, size_t size)
    {
        return size >= 2 && static_cast<const juce::uint8*> (data)[0] == formatTag;
    }

private:
    enum class Op : juce::uint8 { setProperty = 1, removeProperty, addChild, removeChild, moveChild };

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void valueTreeParentChanged (juce::ValueTree&) override {}
    void valueTreeRedirected (juce::ValueTree&) override {}

    bool beginDelta (juce::MemoryOutputStream& out, Op op, const juce::ValueTree& node) const;
    void send (const juce::MemoryOutputStream& out);

    static bool isApplying() { return applyDepth.load() > 0; }

    struct ScopedApply
    {
        ScopedApply()  { ++applyDepth; }
        ~ScopedApply() { --applyDepth; }
    };

    static const char* opName (Op op);

    static inline std::atomic<int> applyDepth { 0 };

    juce::ValueTree root;
    bool stamping = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TreeSync)
};
