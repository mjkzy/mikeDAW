#pragma once
#include <JuceHeader.h>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include "../Engine/AudioEngine.h"

class BrowserPanel : public juce::Component,
                     public juce::FileDragAndDropTarget,
                     private juce::Timer,
                     private juce::FocusChangeListener
{
public:
    BrowserPanel (AudioEngine& engine, juce::PropertiesFile& settings);
    ~BrowserPanel() override;

    juce::File getSelectedFile() const;
    static juce::File resolveLinkedMediaFile (const juce::File&);
    static bool isAudioFile (const juce::File&);
    static bool isMidiFile (const juce::File&);

    static constexpr const char* dragDescription = "browser-file";

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    bool keyPressed (const juce::KeyPress&) override;

    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    float getZoom() const noexcept { return zoom; }
    void setZoom (float newZoom);

    int  getPreferredWidth() const noexcept { return browserWidth; }
    void setPreferredWidth (int w);
    std::function<void()> onWidthChanged;
    std::function<void()> onClose;

    void setMultiplayerFolder (const juce::File& folder);
    void refreshMultiplayerFolder();

    std::function<void (const juce::File&)> onSendToSelectedChannel;
    std::function<juce::String()> getSendToChannelLabel;

    bool isIgnored (const juce::File&) const;
    void setIgnored (const juce::File&, bool ignored);

    struct NfoInfo
    {
        bool valid = false;
        juce::Colour colour;
        bool hasColour = false;
        juce::String tip;
        juce::Image image;
    };
    static NfoInfo readFolderNfo (const juce::File& folder);

private:
    class RootItem;
    class FolderItem;
    class FileItem;

    class ToolbarButton : public juce::Button
    {
    public:
        enum class Icon { add, refresh, close };

        ToolbarButton (const juce::String& name, Icon iconToDraw)
            : juce::Button (name), icon (iconToDraw) {}

        void paintButton (juce::Graphics&, bool highlighted, bool down) override;

    private:
        Icon icon;
    };

    class ZoomTreeView : public juce::TreeView
    {
    public:
        explicit ZoomTreeView (BrowserPanel& p) : owner (p) {}
        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            if (e.mods.isCtrlDown())
            {
                owner.setZoom (owner.getZoom() * (w.deltaY > 0 ? 1.1f : 0.9f));
                return;
            }
            juce::TreeView::mouseWheelMove (e, w);
        }
        BrowserPanel& owner;
    };

    void rebuildTree();
    void addCustomRoot (const juce::File& folder);
    juce::StringArray getCustomRoots() const;
    juce::Array<juce::File> rootFolders() const;

    void buildSearchIndexAsync();
    void applySearch();
    static constexpr int maxSearchResults = 400;

    void showOptionsMenu();
    void showBlankAreaContextMenu (juce::Point<int> screenPosition);
    void chooseCustomRoot();

    void saveOpenState();
    void restoreOpenState();
    void timerCallback() override;
    void globalFocusChanged (juce::Component*) override;
    bool searchFocused = false;

    void showFolderContextMenu (const juce::File& clicked);
    void showFileContextMenu (const juce::File& clicked);
    juce::Array<juce::File> selectedFolders() const;
    bool isIgnoredOrAncestor (const juce::File&) const;

    static constexpr int resizeEdgeW = 6;
    static constexpr int headerH = 34;
    static constexpr int searchBarH = 42;
    static constexpr int searchBarInsetX = 2;
    static constexpr int searchBarInsetY = 6;
    bool overResizeEdge (juce::Point<int>) const;

    AudioEngine& engine;
    juce::PropertiesFile& settings;
    float zoom = 1.0f;
    bool showFileExtensions = false;
    bool restoringOpenState = false;
    std::set<juce::String> rememberedOpenPaths;
    juce::StringArray ignoredList;

    static constexpr int minWidth = 160, maxWidth = 900;
    int browserWidth = 250;
    bool resizing = false;
    int dragStartWidth = 0, dragStartX = 0;

    ZoomTreeView tree { *this };
    std::unique_ptr<RootItem> rootItem;
    std::unique_ptr<RootItem> searchRootItem;
    juce::TextEditor searchBox;
    ToolbarButton addFolderBtn { "add-folder", ToolbarButton::Icon::add };
    ToolbarButton refreshBtn { "refresh", ToolbarButton::Icon::refresh };
    ToolbarButton closeBtn { "close", ToolbarButton::Icon::close };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File selectedFile;
    juce::File multiplayerFolder;

    struct IndexEntry
    {
        juce::String nameLower;
        juce::File file;
    };
    std::shared_ptr<const std::vector<IndexEntry>> searchIndex;
    std::map<juce::String, std::vector<int>> searchCache;

    friend class FolderItem;
    friend class FileItem;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrowserPanel)
};
