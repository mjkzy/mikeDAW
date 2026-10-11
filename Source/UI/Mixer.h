#pragma once
#include <JuceHeader.h>
#include "../Model/ProjectModel.h"
#include "../Engine/AudioEngine.h"
#include "../Engine/PluginHost.h"

class MixerPanel : public juce::Component,
                   private juce::ValueTree::Listener,
                   private juce::Timer
{
public:
    MixerPanel (juce::ValueTree project, AudioEngine&, PluginHost&);
    ~MixerPanel() override;

    void setSettings (juce::PropertiesFile*);

    std::function<void (const juce::String& key, const juce::String& title)> onOpenPluginEditor;
    std::function<void (const juce::String& targetName, bool on)> onSoloToggled;
    std::function<void (const juce::String& name, int trackIndex)> onEffectRemoved;
    std::function<void (const juce::String& key, const juce::String& name)> onLocalVstAdded;
    std::function<bool (const juce::PluginDescription&)> isPluginEnabled;
    std::function<bool (const juce::PluginDescription&)> isPluginNew;
    std::function<void (const juce::String& key, int paramIndex, float oldValue, float newValue)> onParamEdit;

    std::function<juce::Array<int>()> getSelectedChannels;
    std::function<void (int channelId, int insertIndex)> routeChannelToInsert;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class RackMode { slotRack, deviceChain };

    bool replacingChain = false;

    class Strip;
    class AddStripButton;
    class EffectRack;
    class DeviceChain;
    class ModeChip;
    class TopScrollBar;
    class TimecodeBar;

    class ScrollingViewport : public juce::Viewport
    {
    public:
        std::function<void()> onVisibleAreaChanged;
        void visibleAreaChanged (const juce::Rectangle<int>&) override
        {
            if (onVisibleAreaChanged)
                onVisibleAreaChanged();
        }
    };

    int numTracksInTree() const;
    juce::ValueTree trackTree (int index) const;
    juce::String trackName (int index) const;
    juce::String routeLabelFor (int index) const;

    void rebuildStrips();
    void updateScrollBar();
    void selectTrack (int index);
    void showRoutingMenuFor (int index, juce::Component* target);
    void showAddEffectMenu (int trackIndex, int slotIndex, juce::Component* target);
    void openSlotEditor (int trackIndex, juce::ValueTree slot);
    void refreshAll();

    void setRackMode (RackMode);
    void toggleRackMode();

    void exportMixerState (int trackIndex);
    void importMixerState (int trackIndex);
    void applyMixerState (int trackIndex, const juce::ValueTree& state);

    void timerCallback() override;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}
    void valueTreeRedirected (juce::ValueTree&) override {}

    juce::ValueTree project;
    AudioEngine& engine;
    PluginHost& host;

    std::unique_ptr<juce::LookAndFeel> stripLnf;

    ScrollingViewport viewport;
    juce::Component stripContainer;
    juce::OwnedArray<Strip> strips;
    std::unique_ptr<AddStripButton> addStripBtn;
    std::unique_ptr<EffectRack> rack;
    std::unique_ptr<DeviceChain> deviceChain;
    std::unique_ptr<ModeChip> modeChip;
    std::unique_ptr<TopScrollBar> scrollBar;
    std::unique_ptr<TimecodeBar> timecode;
    double lastTimecodeTicks = -1.0;
    std::unique_ptr<juce::FileChooser> fileChooser;

    juce::PropertiesFile* settings = nullptr;
    RackMode rackMode = RackMode::slotRack;

    int selectedTrack = 0;
    bool rebuildPending = false;

    friend class Strip;
    friend class EffectRack;
    friend class DeviceChain;
    friend class ModeChip;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerPanel)
};
