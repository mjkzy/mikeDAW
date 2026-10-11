#pragma once
#include <JuceHeader.h>
#include "../Model/ProjectModel.h"
#include "../Engine/AudioEngine.h"
#include "TransportBar.h"
#include "WheelComboBox.h"
#include "ZoomScrollBar.h"

class Playlist : public juce::Component,
                 public juce::DragAndDropTarget,
                 public juce::FileDragAndDropTarget,
                 private juce::ValueTree::Listener,
                 private juce::Timer
{
public:
    Playlist (juce::ValueTree project, AudioEngine&);
    ~Playlist() override;

    std::function<int()> getCurrentPatternId;
    std::function<juce::File()> getBrowserFile;
    std::function<void (int patternId)> onPatternSelected;
    std::function<void (int patternId)> onPatternCreated;
    std::function<void (int patternId)> onOpenPianoRoll;
    std::function<void (juce::ValueTree clip)> onOpenAudioClip;
    std::function<void (const juce::String& targetName, bool on)> onSoloToggled;

    struct PeerAvatar { juce::String name; juce::Image avatar; juce::Colour colour; };
    std::function<std::vector<PeerAvatar> (int patternId)> getPeersOnPattern;
    void repaintCanvas();

    std::function<int (int mixerTrack)> getRecordingLane;
    std::function<juce::String()> getLocalPeerIdString;
    juce::ValueTree addRecordedClip (const juce::File&, int trackIndex, double startBeats,
                                     double lengthBeats, bool pending, const juce::String& recordedBy);

    void setSelectedPattern (int patternId);
    void duplicateSelection();
    void selectAll();
    void deleteSelection();
    void moveSelectionTracks (int delta);

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    class Canvas;
    class CanvasViewport;
    class TrackHeaders;
    class PatternPicker;

    enum class Tool { draw, select, slice };
    enum class ResourceView { patterns, audio, midi };

    void syncScroll();
    void syncZoomBar();
    void updateZoomBarSpanLimits();
    double minZoomPxPerBeat() const;
    void setHZoom (double newPxPerBeat, double anchorBeat, int anchorViewX);
    void timerCallback() override;
    double getSnapBeats() const;
    void refreshPatternBox();
    void addAudioClip (const juce::File&, int trackIndex, double startBeats);
    void addMidiFileAsPatternClip (const juce::File&, int trackIndex, double startBeats);
    void growPatternToBeats (juce::ValueTree pattern, double beats);
    int numTracks() const;
    void setTool (Tool);
    void setResourceView (ResourceView);
    void refreshResourcePanel();
    void setAudioBrush (const juce::String& path);
    void setAudioBrushFromClip (const juce::ValueTree& clip);
    void clearAudioBrush();
    void paintAudioBrush (const juce::File&, int trackIndex, double startBeats);
    void queueEnsureTracks();
    void queueFullRefresh();
    void splitPatternByChannel (int patternId);
    void makeClipUnique (juce::ValueTree clip);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}
    void valueTreeRedirected (juce::ValueTree&) override {}

    juce::ValueTree project;
    AudioEngine& engine;

    double pxPerBeat = 26.0;
    static constexpr double minPxPerBeat = 2.0;
    static constexpr double maxPxPerBeat = 200.0;
    int trackHeight = 64;
    static constexpr int extraLanes = 3;
    double lastClipLenBeats = 0.0;
    Tool tool = Tool::draw;
    bool stretchEnabled = false;
    static constexpr int rulerHeight = 22;
    static constexpr int zoomBarHeight = ZoomScrollBar::barHeight;
    static constexpr int toolRowHeight = 28;
    static constexpr int toolStripWidth = 176;
    static constexpr int headerWidth = 110;
    static constexpr int patternPickerWidth = 170;
    static constexpr int pickerGroupGap = 6;

    juce::ComboBox snapBox;
    int selectedPatternId = 0;
    juce::String audioBrushPath;
    juce::ValueTree audioBrushTemplate;
    ResourceView resourceView = ResourceView::patterns;
    juce::TextButton patternFilterBtn { "PAT" };
    juce::TextButton audioFilterBtn { "AUD" };
    juce::TextButton midiFilterBtn { "MIDI" };
    struct PlusButton : public juce::Button
    {
        PlusButton() : juce::Button ("+") {}
        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            getLookAndFeel().drawButtonBackground (g, *this, findColour (juce::TextButton::buttonColourId), over, down);
            auto c = getLocalBounds().toFloat().getCentre();
            g.setColour (over ? Colors::text : Colors::textDim);
            g.fillRect (c.x - 4.0f, c.y - 0.75f, 8.0f, 1.5f);
            g.fillRect (c.x - 0.75f, c.y - 4.0f, 1.5f, 8.0f);
        }
    };
    PlusButton addPatternBtn;
    juce::Component toolStrip;
    IconButton snapBtn { "snap", IconButton::Icon::magnet, "Snap" };
    IconButton drawBtn { "draw", IconButton::Icon::pencil, "Draw tool: paint patterns, drag edges to resize" };
    IconButton selectBtn { "select", IconButton::Icon::cursor, "Select tool: rubber-band select and move clips" };
    IconButton sliceBtn { "slice", IconButton::Icon::blade, "Slice tool: drag a vertical cut through clips to split them" };
    IconButton stretchBtn { "stretch", IconButton::Icon::stretch,
                            "Stretch: resizing an audio clip time-stretches it instead of trimming" };
    ZoomScrollBar zoomBar;
    std::unique_ptr<CanvasViewport> viewport;
    std::unique_ptr<Canvas> canvas;
    std::unique_ptr<TrackHeaders> headers;
    std::unique_ptr<PatternPicker> patternPicker;
    juce::Component headerClip;
    juce::Viewport patternPickerViewport;
    bool ensureTracksQueued = false;
    bool fullRefreshQueued = false;
    bool suppressZoomBarSync = false;
    bool wasPlaying = false;

    juce::AudioThumbnailCache thumbCache { 64 };
    std::map<juce::String, std::unique_ptr<juce::AudioThumbnail>> thumbnails;
    juce::AudioThumbnail* getThumbnail (const juce::String& path);
    void prewarmThumbnail (const juce::ValueTree& clip);
    void prewarmAllThumbnails();

    struct TrimCacheEntry { juce::String path; AudioEngine::SampleTrimBounds bounds; };
    std::map<int, TrimCacheEntry> trimCache;

    int treeGen = 0;
    bool getClipTrimBounds (const juce::ValueTree& clip, const juce::String& path,
                            AudioEngine::SampleTrimBounds& out);

    friend class Canvas;
    friend class TrackHeaders;
    friend class PatternPicker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Playlist)
};
