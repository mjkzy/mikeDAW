#pragma once
#include <JuceHeader.h>
#include <vector>
#include "../Model/ProjectModel.h"
#include "../Engine/AudioEngine.h"
#include "../Engine/PluginHost.h"
#include "WheelComboBox.h"

class ChannelRack : public juce::Component,
                    public juce::DragAndDropTarget,
                    public juce::FileDragAndDropTarget,
                    private juce::ValueTree::Listener,
                    private juce::Timer
{
public:
    ChannelRack (juce::ValueTree project, AudioEngine&, PluginHost&);
    ~ChannelRack() override;

    std::function<void (int patternId, int channelId)> onOpenPianoRoll;
    std::function<void (const juce::String& key, const juce::String& title)> onOpenPluginEditor;
    std::function<void (const juce::String& key, const juce::String& name)> onLocalVstAdded;
    std::function<bool (const juce::PluginDescription&)> isPluginEnabled;
    std::function<bool (const juce::PluginDescription&)> isPluginNew;
    std::function<void (const juce::String& targetName, bool on)> onSoloToggled;
    std::function<void (const juce::String& name, const juce::String& kind)> onChannelRemoved;
    std::function<void (juce::ValueTree channel)> onOpenSamplerSettings;
    std::function<juce::File()> getBrowserFile;
    std::function<void()> onPatternChanged;

    int getSelectedPatternId() const;
    void setSelectedPattern (int patternId);

    juce::Array<int> getSelectedChannelIds() const;
    void selectChannel (int channelId, bool additive);
    bool isChannelSelected (int channelId) const;
    int getLastSelectedChannelId() const { return lastSelectedChannel; }
    void selectAtContainerPoint (juce::Point<int> containerPos);

    int preferredContentHeight() const;
    int preferredContentWidth() const;
    int compactContentWidth() const;
    std::function<void()> onContentHeightChanged;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragMove (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;

private:
    class Row;

    int widthForSteps (int numSteps) const;
    void rebuildRows();
    void queueRebuild();
    void timerCallback() override;
    void refreshPatternBox();
    void showAddChannelMenu();
    void addSamplerFromFile (const juce::File&);
    void insertSamplerAt (const juce::File&, int index);
    void pickSampleFor (juce::ValueTree channel);
    void toggleChannelSolo (int channelId);
    int soloedChannelId = 0;
    void importMidiToChannel (const juce::File& midi, juce::ValueTree channel);
    void replaceInstrument (juce::ValueTree channel, const juce::PluginDescription& desc);
    void transposeChannels (const juce::Array<int>& channelIds, int semitones);
    void cloneChannel (juce::ValueTree channel);
    juce::ValueTree currentPattern() const;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}
    void valueTreeRedirected (juce::ValueTree&) override;

    juce::ValueTree project;
    AudioEngine& engine;
    PluginHost& host;

    int selectedPatternId = 0;
    static constexpr int footerH = 30;
    juce::TextButton addChannelBtn { "+" };
    struct SmoothViewport : public juce::Viewport,
                            private juce::Timer
    {
        bool wheelScroll (const juce::MouseWheelDetails&);
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void visibleAreaChanged (const juce::Rectangle<int>&) override;

    private:
        void timerCallback() override;
        int targetY = 0;
        bool animating = false;
    };

    SmoothViewport viewport;
    juce::Component rowContainer;
    juce::OwnedArray<Row> rows;
    std::unique_ptr<juce::FileChooser> chooser;
    bool rebuildQueued = false;

    juce::Array<int> selectedChannels;
    int lastSelectedChannel = 0;

    juce::uint32 lastGlowMs = 0;
    int timerRate = 30;

    struct SeqClipboard
    {
        bool valid = false;
        juce::String steps;
        bool usePianoRoll = false;
        juce::ValueTree notes { IDs::ChannelSeq };
    };
    static SeqClipboard seqClipboard;

    void updateDragHover (juce::Point<int> posInRack, bool isMidi, bool isAudio);
    void clearDragHover();
    int dragHoverChannelId = 0;
    bool dragHoverNewChannel = false;
    int dragInsertIndex = -1;

    struct InsertLine : public juce::Component
    {
        void paint (juce::Graphics&) override;
    };
    InsertLine insertLine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelRack)
};
