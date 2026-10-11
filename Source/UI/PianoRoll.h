#pragma once
#include <JuceHeader.h>
#include "../Model/ProjectModel.h"
#include "../Engine/AudioEngine.h"
#include "TransportBar.h"
#include "WheelComboBox.h"
#include "ZoomScrollBar.h"

class PianoRoll : public juce::Component,
                  public juce::DragAndDropTarget,
                  public juce::FileDragAndDropTarget,
                  private juce::ValueTree::Listener,
                  private juce::Timer
{
public:
    PianoRoll (juce::ValueTree project, AudioEngine&, juce::PropertiesFile* settings = nullptr);
    ~PianoRoll() override;

    void setTarget (int patternId, int channelId);
    int getChannelId() const noexcept { return channelId; }
    int getPatternId() const noexcept { return patternId; }
    juce::String getTargetDescription() const;
    std::function<void (const juce::String&)> onTargetChanged;
    std::function<void (int patternId)> onPatternSelected;

    std::function<void()> onBeginUndoTransaction;
    std::function<void()> onEndUndoTransaction;

    void duplicateSelection();
    void jumpPlaybackToLocalTick (int tick);
    void copySelection();
    void pasteClipboard();
    void selectAll();
    void deleteSelection();
    void transposeSelection (int semitones);
    void legatoSelection();
    void randomizeNotesWeirdly();
    void showArpeggiatorDialog();
    void showStrumizerDialog();
    void exportMidi();
    void clearSelection();

    void setKeyHighlight (int midiNote, bool on);
    void clearKeyHighlights();

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

    std::function<juce::File()> getBrowserFile;

private:
    class Grid;
    class Keyboard;
    class VelocityLane;
    class GridViewport;
    class LaneDivider;
    class Ruler;
    class ArpDialog;
    class ArpBlocker;
    class StrumDialog;
    class StrumBlocker;

    enum class Tool { draw, select, slice };

    juce::ValueTree getChannelSeq (bool createIfMissing) const;
    juce::ValueTree getPattern() const;
    int getSnapTicks() const;
    int getPatternTicks() const;
    int getGridTicks() const;
    double playheadTicks() const;
    void ensurePatternCovers (int tick);
    void gridRepaint();
    void velocityLaneRepaint();
    void importMidiFile (const juce::File&);
    void syncScroll();
    void syncZoomBar();
    void timerCallback() override;
    void refreshTargetBoxes();
    void refreshScaleBoxes();
    void setVerticalZoom (int newRowHeight, int anchorGridY, int anchorViewportY);
    void setHZoom (double newPxPerTick, double anchorTick, int anchorViewX);
    void layoutForGridWidth();
    void setTool (Tool);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}
    void valueTreeRedirected (juce::ValueTree&) override {}

    juce::ValueTree project;
    AudioEngine& engine;
    juce::PropertiesFile* settings = nullptr;

    int patternId = 0, channelId = 0;
    double pxPerTick = 2.0;
    static constexpr double minPxPerTick = 0.25, maxPxPerTick = 4.0;
    bool suppressZoomBarSync = false;
    int rowHeight = 22;
    static constexpr int minGridBars = 8;
    int lastNoteLength = Model::ticksPerStep;
    int lastNoteVelocity = 100;
    int rangeStartTick = -1, rangeEndTick = -1;
    Tool tool = Tool::draw;
    juce::Array<juce::ValueTree> selection;

    struct ClipNote { int key; int start; int length; int velocity; bool slide; };
    static std::vector<ClipNote> clipboard;
    static int clipboardOrigin;

    juce::ComboBox snapBox;
    WheelComboBox channelBox;
    WheelComboBox rootBox, scaleBox;
    juce::TextButton detectBtn { "Detect" };
    juce::String detectBtnLabel { "Detect" };
    juce::TextButton scaleHiliteBtn { "Scale" };
    juce::TextButton menuBtn { juce::String::charToString ((juce::juce_wchar) 0x25be) };
    std::unique_ptr<juce::FileChooser> chooser;
    IconButton drawBtn { "draw", IconButton::Icon::pencil, "Draw tool" };
    IconButton selectBtn { "select", IconButton::Icon::cursor, "Select tool" };
    IconButton sliceBtn { "slice", IconButton::Icon::blade, "Slice tool: drag a vertical cut through notes to split them" };
    IconButton slideBtn { "slide", IconButton::Icon::slide, "Slide notes: new notes glide sounding notes to their pitch" };
    bool slideMode = false;
    IconButton snapBtn { "snap", IconButton::Icon::magnet, "Snap" };
    std::unique_ptr<GridViewport> viewport;
    std::unique_ptr<Grid> grid;
    std::unique_ptr<Keyboard> keyboard;
    std::unique_ptr<VelocityLane> velocityLane;
    std::unique_ptr<LaneDivider> laneDivider;
    std::unique_ptr<Ruler> ruler;
    ZoomScrollBar zoomBar;
    juce::Component keyboardClip, laneClip, rulerClip;

    bool wasPlayingForReturn = false;
    double returnOnStopTick = -1.0;

    int laneHeight = 70;
    static constexpr int keyboardWidth = 64;
    static constexpr int dividerHeight = 6;
    static constexpr int toolbarHeight = 28;
    static constexpr int zoomBarHeight = ZoomScrollBar::barHeight;
    static constexpr int rulerHeight = 18;

    friend class Grid;
    friend class Keyboard;
    friend class VelocityLane;
    friend class LaneDivider;
    friend class Ruler;
    friend class ArpDialog;
    friend class ArpBlocker;
    friend class StrumDialog;
    friend class StrumBlocker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRoll)
};
