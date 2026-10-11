#include "BrowserPanel.h"
#include "AppLookAndFeel.h"

namespace
{
    struct FileSorter
    {
        static int compareElements (const juce::File& a, const juce::File& b)
        {
            return a.getFileName().compareIgnoreCase (b.getFileName());
        }
    };
}

void BrowserPanel::ToolbarButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);

    auto tint = icon == Icon::refresh ? Colors::refreshGreen : Colors::textDim;
    if (icon == Icon::close && (highlighted || down))
        tint = Colors::accent2;
    else if (down)
        tint = tint.darker (0.15f);
    else if (highlighted)
        tint = tint.brighter (0.25f);

    g.setColour (tint);
    auto iconArea = r.withSizeKeepingCentre (15.0f, 15.0f);
    if (icon == Icon::close)
    {
        auto x = iconArea.withSizeKeepingCentre (11.0f, 11.0f);
        g.drawLine (x.getX(), x.getY(), x.getRight(), x.getBottom(), 1.6f);
        g.drawLine (x.getX(), x.getBottom(), x.getRight(), x.getY(), 1.6f);
        return;
    }
    if (icon == Icon::add)
    {
        g.drawLine (iconArea.getCentreX(), iconArea.getY() + 2.0f,
                    iconArea.getCentreX(), iconArea.getBottom() - 2.0f, 1.8f);
        g.drawLine (iconArea.getX() + 2.0f, iconArea.getCentreY(),
                    iconArea.getRight() - 2.0f, iconArea.getCentreY(), 1.8f);
        return;
    }

    juce::Path arc;
    arc.addCentredArc (iconArea.getCentreX(), iconArea.getCentreY(), 5.4f, 5.4f, 0.0f,
                       -juce::MathConstants<float>::pi * 0.8f,
                        juce::MathConstants<float>::pi * 0.72f, true);
    g.strokePath (arc, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));
    juce::Path arrow;
    arrow.addTriangle (iconArea.getRight() - 1.2f, iconArea.getY() + 4.2f,
                       iconArea.getRight() - 5.4f, iconArea.getY() + 3.6f,
                       iconArea.getRight() - 2.8f, iconArea.getY() + 7.2f);
    g.fillPath (arrow);
}

class BrowserPanel::RootItem : public juce::TreeViewItem
{
public:
    bool mightContainSubItems() override { return true; }
    void paintItem (juce::Graphics&, int, int) override {}
};

class BrowserPanel::FolderItem : public juce::TreeViewItem
{
public:
    FolderItem (BrowserPanel& panel, const juce::File& dir, const juce::String& displayName = {})
        : owner (panel), folder (dir),
          name (displayName.isNotEmpty() ? displayName : dir.getFileName()),
          nfo (BrowserPanel::readFolderNfo (dir))
    {}

    ~FolderItem() override
    {
        if (scanAlive)
            *scanAlive = false;
    }

    bool mightContainSubItems() override { return true; }
    int getItemHeight() const override { return juce::roundToInt (17.0f * owner.getZoom()); }

    juce::String getTooltip() override { return nfo.valid ? nfo.tip : juce::String(); }

    juce::String getUniqueName() const override { return folder.getFullPathName(); }

    void itemOpennessChanged (bool isNowOpen) override
    {
        if (isNowOpen)
        {
            owner.rememberedOpenPaths.erase (folder.getFullPathName());

            if (! scanned)
            {
                scanned = true;
                if (owner.restoringOpenState)
                    scan();
                else
                    scanAsync();
            }
        }
        else
        {
            rememberOpenDescendants();

            if (scanAlive)
                *scanAlive = false;
            clearSubItems();
            scanned = false;
        }
    }

    void paintItem (juce::Graphics& g, int width, int height) override
    {
        float z = owner.getZoom();

        const float alpha = owner.isIgnoredOrAncestor (folder) ? 0.30f : 1.0f;

        juce::Colour folderCol = (nfo.valid && nfo.hasColour) ? nfo.colour
                                                              : Colors::accent2.withAlpha (0.8f);

        auto iconArea = juce::Rectangle<float> (4.0f, 2.0f * z, 14.0f * z, (float) height - 4.0f * z);
        float textX = 22.0f * z;

        if (nfo.image.isValid())
        {
            float side = (float) height - 3.0f * z;
            juce::Rectangle<float> thumb (4.0f, ((float) height - side) * 0.5f, side, side);
            g.setOpacity (alpha);
            g.drawImage (nfo.image, thumb, juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
            g.setOpacity (1.0f);
            textX = thumb.getRight() + 5.0f * z;
        }
        else
        {
            g.setColour (folderCol.withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (iconArea.getX(), iconArea.getY(),
                                    iconArea.getWidth() * 0.5f, 4.5f * z, 1.0f);
            g.fillRoundedRectangle (iconArea.getX(), iconArea.getY() + 3.5f * z,
                                    iconArea.getWidth(), iconArea.getHeight() - 3.5f * z, 1.5f);
        }

        auto textCol = (nfo.valid && nfo.hasColour) ? nfo.colour.brighter (0.4f) : Colors::text;
        g.setColour (textCol.withMultipliedAlpha (alpha));
        g.setFont (nfo.valid ? Typography::bold (12.0f * z) : Typography::ui (12.0f * z));
        g.drawText (name, (int) textX, 0, width - (int) textX - 2, height, juce::Justification::centredLeft);
    }

    void itemClicked (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            owner.showFolderContextMenu (folder);
            return;
        }
        setOpen (! isOpen());
    }

    void itemDoubleClicked (const juce::MouseEvent&) override {}

    const juce::File& getFolder() const noexcept { return folder; }

    void scan();
    void scanAsync();
    void syncChildren();
    void rememberOpenDescendants();
    void restoreRememberedChildren();

private:
    BrowserPanel& owner;
    juce::File folder;
    juce::String name;
    BrowserPanel::NfoInfo nfo;
    bool scanned = false;
    std::shared_ptr<bool> scanAlive;
};

class BrowserPanel::FileItem : public juce::TreeViewItem
{
public:
    FileItem (BrowserPanel& panel, const juce::File& f)
        : owner (panel), file (f)
    {}

    bool mightContainSubItems() override { return false; }
    int getItemHeight() const override
    {
        return juce::roundToInt ((BrowserPanel::isAudioFile (file) ? 15.0f : 17.0f)
                                 * owner.getZoom());
    }

    juce::String getTooltip() override { return file.getFullPathName(); }
    juce::String getUniqueName() const override { return file.getFullPathName(); }

    const juce::File& getFile() const noexcept { return file; }

    void paintItem (juce::Graphics& g, int width, int height) override
    {
        float z = owner.getZoom();
        auto mediaFile = BrowserPanel::resolveLinkedMediaFile (file);
        bool audio = BrowserPanel::isAudioFile (file);
        bool midi  = BrowserPanel::isMidiFile (file);

        const float alpha = owner.isIgnoredOrAncestor (file) ? 0.30f : 1.0f;

        if (audio)
        {
            static constexpr float amps[] = { 0.35f, 0.8f, 1.0f, 0.5f, 0.85f, 0.4f };
            g.setColour (Colors::accent.withMultipliedAlpha (alpha));
            float x0 = 4.0f * z, cy = (float) height * 0.5f, bw = 1.8f * z;
            for (int i = 0; i < 6; ++i)
            {
                float h = amps[i] * (float) height * 0.28f;
                g.fillRoundedRectangle (x0 + (float) i * bw, cy - h, bw * 0.65f, h * 2.0f, 0.6f);
            }
        }
        else if (midi)
        {
            g.setColour (Colors::accent2.withMultipliedAlpha (alpha));
            float cx = 8.0f * z, cy = (float) height * 0.5f;
            float noteR = 2.8f * z;
            float stemH = 8.0f * z;
            g.fillEllipse (cx - noteR, cy + stemH * 0.15f - noteR, noteR * 2.0f, noteR * 1.6f);
            g.fillRect (cx + noteR - 1.2f * z, cy - stemH * 0.55f, 1.2f * z, stemH * 0.7f);
            float flagW = 3.5f * z, flagH = 3.0f * z;
            juce::Path flag;
            float fTop = cy - stemH * 0.55f;
            flag.startNewSubPath (cx + noteR - 1.2f * z + 1.2f * z, fTop);
            flag.quadraticTo (cx + noteR + flagW, fTop + flagH * 0.3f,
                              cx + noteR, fTop + flagH);
            flag.lineTo (cx + noteR - 1.2f * z + 1.2f * z, fTop + flagH * 0.7f);
            flag.closeSubPath();
            g.fillPath (flag);
        }
        else
        {
            float dotSize = 5.0f * z;
            g.setColour (Colors::textDim.withMultipliedAlpha (alpha));
            g.fillEllipse (6.0f * z, ((float) height - dotSize) / 2.0f, dotSize, dotSize);
        }

        auto name = owner.showFileExtensions ? mediaFile.getFileName()
                                             : mediaFile.getFileNameWithoutExtension();
        float textX = 18.0f * z;
        g.setColour ((isSelected() ? juce::Colours::white : Colors::text).withMultipliedAlpha (alpha));
        g.setFont (Typography::ui (12.0f * z));
        g.drawText (name, (int) textX, 0, width - (int) textX - 4, height,
                    juce::Justification::centredLeft);
    }

    void itemClicked (const juce::MouseEvent& e) override
    {
        owner.selectedFile = BrowserPanel::resolveLinkedMediaFile (file);
        if (e.mods.isPopupMenu())
            owner.showFileContextMenu (file);
        else if (BrowserPanel::isAudioFile (file))
            owner.engine.previewFile (owner.selectedFile);
    }

    void itemSelectionChanged (bool isNowSelected) override
    {
        if (isNowSelected)
            owner.selectedFile = BrowserPanel::resolveLinkedMediaFile (file);
    }

    juce::var getDragSourceDescription() override
    {
        owner.selectedFile = BrowserPanel::resolveLinkedMediaFile (file);
        return BrowserPanel::dragDescription;
    }

private:
    BrowserPanel& owner;
    juce::File file;
};

void BrowserPanel::FolderItem::scan()
{
    if (! folder.isDirectory())
        return;

    juce::Array<juce::File> dirs, files;
    for (auto& entry : juce::RangedDirectoryIterator (folder, false, "*",
             juce::File::findFilesAndDirectories))
    {
        auto f = entry.getFile();
        if (f.isHidden())
            continue;
        if (f.isDirectory())
            dirs.add (f);
        else if (BrowserPanel::isAudioFile (f) || BrowserPanel::isMidiFile (f))
            files.add (f);
    }

    FileSorter sorter;
    dirs.sort (sorter);
    files.sort (sorter);

    for (auto& d : dirs)
        addSubItem (new FolderItem (owner, d));
    for (auto& f : files)
        addSubItem (new FileItem (owner, f));

    owner.engine.prefetchFiles (files);
    restoreRememberedChildren();
}

void BrowserPanel::FolderItem::rememberOpenDescendants()
{
    for (int i = 0; i < getNumSubItems(); ++i)
        if (auto* fi = dynamic_cast<FolderItem*> (getSubItem (i)))
            if (fi->isOpen())
            {
                owner.rememberedOpenPaths.insert (fi->folder.getFullPathName());
                fi->rememberOpenDescendants();
            }
}

void BrowserPanel::FolderItem::restoreRememberedChildren()
{
    if (owner.rememberedOpenPaths.empty())
        return;

    for (int i = 0; i < getNumSubItems(); ++i)
        if (auto* fi = dynamic_cast<FolderItem*> (getSubItem (i)))
            if (owner.rememberedOpenPaths.count (fi->folder.getFullPathName()) > 0)
                fi->setOpen (true);
}

void BrowserPanel::FolderItem::syncChildren()
{
    if (! scanned || ! folder.isDirectory())
        return;

    juce::Array<juce::File> dirs, files;
    for (auto& entry : juce::RangedDirectoryIterator (folder, false, "*",
             juce::File::findFilesAndDirectories))
    {
        auto f = entry.getFile();
        if (f.isHidden())
            continue;
        if (f.isDirectory())
            dirs.add (f);
        else if (BrowserPanel::isAudioFile (f) || BrowserPanel::isMidiFile (f))
            files.add (f);
    }

    FileSorter sorter;
    dirs.sort (sorter);
    files.sort (sorter);

    juce::Array<juce::File> desired;
    desired.addArray (dirs);
    desired.addArray (files);

    auto fileOf = [] (juce::TreeViewItem* item) -> juce::File
    {
        if (auto* fi = dynamic_cast<FolderItem*> (item)) return fi->folder;
        if (auto* fl = dynamic_cast<FileItem*> (item))   return fl->getFile();
        return {};
    };

    for (int i = getNumSubItems(); --i >= 0;)
        if (! desired.contains (fileOf (getSubItem (i))))
            removeSubItem (i);

    juce::Array<juce::File> addedFiles;
    for (int i = 0; i < desired.size(); ++i)
    {
        auto f = desired.getReference (i);
        if (i < getNumSubItems() && fileOf (getSubItem (i)) == f)
            continue;

        if (f.isDirectory())
        {
            addSubItem (new FolderItem (owner, f), i);
        }
        else
        {
            addSubItem (new FileItem (owner, f), i);
            addedFiles.add (f);
        }
    }

    if (! addedFiles.isEmpty())
        owner.engine.prefetchFiles (addedFiles);

    for (int i = 0; i < getNumSubItems(); ++i)
        if (auto* fi = dynamic_cast<FolderItem*> (getSubItem (i)))
            fi->syncChildren();
}

void BrowserPanel::FolderItem::scanAsync()
{
    auto dir = folder;
    auto safe = juce::Component::SafePointer<BrowserPanel> (&owner);
    auto alive = std::make_shared<bool> (true);
    scanAlive = alive;

    juce::Thread::launch ([dir, safe, alive]
    {
        if (! dir.isDirectory())
            return;

        juce::Array<juce::File> dirs, files;
        for (auto& entry : juce::RangedDirectoryIterator (dir, false, "*",
                 juce::File::findFilesAndDirectories))
        {
            if (! *alive)
                return;
            auto f = entry.getFile();
            if (f.isHidden())
                continue;
            if (f.isDirectory())
                dirs.add (f);
            else if (BrowserPanel::isAudioFile (f) || BrowserPanel::isMidiFile (f))
                files.add (f);
        }

        FileSorter sorter;
        dirs.sort (sorter);
        files.sort (sorter);

        juce::MessageManager::callAsync ([safe, alive, dir, dirs, files]
        {
            if (safe == nullptr || ! *alive)
                return;

            std::function<FolderItem* (juce::TreeViewItem*)> find = [&] (juce::TreeViewItem* item) -> FolderItem*
            {
                if (item == nullptr) return nullptr;
                if (auto* fi = dynamic_cast<FolderItem*> (item))
                    if (fi->folder == dir)
                        return fi;
                for (int i = 0; i < item->getNumSubItems(); ++i)
                    if (auto* result = find (item->getSubItem (i)))
                        return result;
                return nullptr;
            };

            auto* self = find (safe->tree.getRootItem());
            if (self == nullptr)
                return;

            for (auto& d : dirs)
                self->addSubItem (new FolderItem (*safe, d));
            for (auto& f : files)
                self->addSubItem (new FileItem (*safe, f));

            safe->engine.prefetchFiles (files);
            self->restoreRememberedChildren();
        });
    });
}

BrowserPanel::BrowserPanel (AudioEngine& e, juce::PropertiesFile& s)
    : engine (e), settings (s)
{
    addAndMakeVisible (tree);
    tree.setMultiSelectEnabled (true);
    tree.setDefaultOpenness (false);
    tree.setOpenCloseButtonsVisible (true);
    tree.setColour (juce::TreeView::backgroundColourId, juce::Colours::transparentBlack);
    tree.getViewport()->addMouseListener (this, true);

    zoom = juce::jlimit (0.7f, 2.0f, (float) settings.getDoubleValue ("browserZoom", 1.0));
    browserWidth = juce::jlimit (minWidth, maxWidth, settings.getIntValue ("browserWidth", 250));
    showFileExtensions = settings.getBoolValue ("browserShowExt", false);
    ignoredList.addTokens (settings.getValue ("browserIgnored"), "|", {});
    ignoredList.removeEmptyStrings();
    setWantsKeyboardFocus (true);

    addAndMakeVisible (searchBox);
    searchBox.setTextToShowWhenEmpty ("Search...", Colors::textDim);
    searchBox.setFont (Typography::ui (14.0f));
    searchBox.setJustification (juce::Justification::centredLeft);
    searchBox.setIndents (14, 0);
    searchBox.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    searchBox.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    searchBox.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    searchBox.onTextChange = [this] { applySearch(); };
    searchBox.onEscapeKey = [this]
    {
        searchBox.setText ({}, juce::dontSendNotification);
        applySearch();
    };

    addAndMakeVisible (refreshBtn);
    refreshBtn.setTooltip ("Refresh browser");
    refreshBtn.onClick = [this]
    {
        rebuildTree();
        buildSearchIndexAsync();
    };

    addAndMakeVisible (closeBtn);
    closeBtn.setTooltip ("Close browser");
    closeBtn.onClick = [this] { if (onClose) onClose(); };

    addAndMakeVisible (addFolderBtn);
    addFolderBtn.setTooltip ("Add a folder to the browser");
    addFolderBtn.onClick = [this] { chooseCustomRoot(); };

    if (getCustomRoots().isEmpty() && settings.getValue ("browserRoots").isNotEmpty())
    {
        auto musicDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                            .getFullPathName();
        juce::StringArray oldRoots;
        oldRoots.addTokens (settings.getValue ("browserRoots"), ";", {});
        oldRoots.removeEmptyStrings();
        oldRoots.removeString (musicDir);
        if (oldRoots.size() > 0)
            settings.setValue ("browserCustomRoots", oldRoots.joinIntoString (";"));
    }

    rebuildTree();
    buildSearchIndexAsync();
    startTimer (2000);
    juce::Desktop::getInstance().addFocusChangeListener (this);
}

BrowserPanel::~BrowserPanel()
{
    juce::Desktop::getInstance().removeFocusChangeListener (this);
    stopTimer();
    saveOpenState();
    tree.getViewport()->removeMouseListener (this);
    tree.setRootItem (nullptr);
}

juce::StringArray BrowserPanel::getCustomRoots() const
{
    juce::StringArray roots;
    roots.addTokens (settings.getValue ("browserCustomRoots"), ";", {});
    roots.removeEmptyStrings();
    return roots;
}

void BrowserPanel::addCustomRoot (const juce::File& folder)
{
    auto roots = getCustomRoots();
    roots.addIfNotAlreadyThere (folder.getFullPathName());
    settings.setValue ("browserCustomRoots", roots.joinIntoString (";"));
    settings.saveIfNeeded();
    rebuildTree();
    buildSearchIndexAsync();
}

void BrowserPanel::setMultiplayerFolder (const juce::File& folder)
{
    if (multiplayerFolder == folder)
        return;

    auto previous = multiplayerFolder;
    multiplayerFolder = folder;

    auto* mpRoot = rootItem != nullptr && rootItem->getNumSubItems() > 0
                     ? dynamic_cast<FolderItem*> (rootItem->getSubItem (0)) : nullptr;

    if (rootItem != nullptr && previous == juce::File() && folder.isDirectory())
        rootItem->addSubItem (new FolderItem (*this, folder, "Multiplayer"), 0);
    else if (mpRoot != nullptr && folder == juce::File() && mpRoot->getFolder() == previous)
        rootItem->removeSubItem (0);
    else
        rebuildTree();

    buildSearchIndexAsync();
}

void BrowserPanel::refreshMultiplayerFolder()
{
    if (! multiplayerFolder.isDirectory() || rootItem == nullptr)
        return;

    for (int i = 0; i < rootItem->getNumSubItems(); ++i)
        if (auto* fi = dynamic_cast<FolderItem*> (rootItem->getSubItem (i)))
            if (fi->getFolder() == multiplayerFolder)
            {
                fi->syncChildren();
                buildSearchIndexAsync();
                return;
            }

    rebuildTree();
    buildSearchIndexAsync();
}

juce::Array<juce::File> BrowserPanel::rootFolders() const
{
    juce::Array<juce::File> out;

    if (multiplayerFolder.isDirectory())
        out.add (multiplayerFolder);

    auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    if (music.isDirectory())
        out.add (music);

    const juce::StringArray programDirs = { "C:\\Program Files", "C:\\Program Files (x86)" };
    const juce::StringArray flVersions  = { "20", "21", "2024", "2025", "2026" };

    for (auto& progDir : programDirs)
        for (auto& ver : flVersions)
        {
            auto path = juce::File (progDir)
                            .getChildFile ("Image-Line")
                            .getChildFile ("FL Studio " + ver)
                            .getChildFile ("Data").getChildFile ("Patches")
                            .getChildFile ("Packs");
            if (path.isDirectory())
                out.add (path);
        }

    for (auto& r : getCustomRoots())
    {
        juce::File f (r);
        if (f.isDirectory())
            out.addIfNotAlreadyThere (f);
    }
    return out;
}

void BrowserPanel::rebuildTree()
{
    tree.setRootItem (nullptr);
    rootItem.reset();
    searchCache.clear();
    selectedFile = juce::File();

    rootItem = std::make_unique<RootItem>();

    auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    for (auto& f : rootFolders())
    {
        juce::String display;
        if (f == multiplayerFolder)
            display = "Multiplayer";
        else if (f == music)
            display = "Music";
        else if (f.getFullPathName().containsIgnoreCase ("Image-Line")
                 && f.getFileName().equalsIgnoreCase ("Packs"))
            display = "FL Studio " + f.getParentDirectory().getParentDirectory().getParentDirectory()
                                  .getFileName().fromLastOccurrenceOf ("FL Studio ", false, true)
                            + " Drums";
        rootItem->addSubItem (new FolderItem (*this, f, display));
    }

    tree.setRootItem (rootItem.get());
    tree.setRootItemVisible (false);

    restoreOpenState();

    if (searchBox.getText().trim().isNotEmpty())
        applySearch();
}

void BrowserPanel::saveOpenState()
{
    if (restoringOpenState || tree.getRootItem() != rootItem.get())
        return;

    juce::StringArray openPaths;
    std::function<void (juce::TreeViewItem*)> collect = [&] (juce::TreeViewItem* item)
    {
        if (item == nullptr) return;
        if (item->isOpen())
            if (auto* fi = dynamic_cast<FolderItem*> (item))
                openPaths.add (fi->getFolder().getFullPathName());
        for (int i = 0; i < item->getNumSubItems(); ++i)
            collect (item->getSubItem (i));
    };
    collect (tree.getRootItem());

    settings.setValue ("browserOpenFolders", openPaths.joinIntoString ("|"));

    settings.setValue ("browserScrollY", tree.getViewport()->getViewPositionY());
    settings.save();
}

void BrowserPanel::restoreOpenState()
{
    auto paths = settings.getValue ("browserOpenFolders");
    if (paths.isEmpty())
        return;

    juce::StringArray openPaths;
    openPaths.addTokens (paths, "|", {});
    openPaths.removeEmptyStrings();
    if (openPaths.isEmpty())
        return;

    const juce::ScopedValueSetter<bool> guard (restoringOpenState, true);

    std::function<void (juce::TreeViewItem*)> restore = [&] (juce::TreeViewItem* item)
    {
        if (item == nullptr) return;
        if (auto* fi = dynamic_cast<FolderItem*> (item))
        {
            if (openPaths.contains (fi->getFolder().getFullPathName()))
                fi->setOpen (true);
        }
        for (int i = 0; i < item->getNumSubItems(); ++i)
            restore (item->getSubItem (i));
    };
    restore (tree.getRootItem());

    int scrollY = settings.getIntValue ("browserScrollY", 0);
    if (scrollY > 0)
        tree.getViewport()->setViewPosition (0, scrollY);
}

void BrowserPanel::timerCallback()
{
    saveOpenState();
}

void BrowserPanel::globalFocusChanged (juce::Component*)
{
    bool f = searchBox.hasKeyboardFocus (true);
    if (f != searchFocused)
    {
        searchFocused = f;
        repaint();
    }
}

void BrowserPanel::buildSearchIndexAsync()
{
    auto roots = rootFolders();
    juce::Thread::launch ([safe = juce::Component::SafePointer<BrowserPanel> (this), roots]
    {
        auto index = std::make_shared<std::vector<IndexEntry>>();
        for (auto& root : roots)
        {
            for (auto& entry : juce::RangedDirectoryIterator (root, true, "*", juce::File::findFiles))
            {
                auto f = entry.getFile();
                if (isAudioFile (f) || isMidiFile (f))
                    index->push_back ({ f.getFileName().toLowerCase(), f });
            }
        }

        std::sort (index->begin(), index->end(),
            [] (const IndexEntry& a, const IndexEntry& b)
            { return a.nameLower < b.nameLower; });

        juce::MessageManager::callAsync ([safe, index]
        {
            if (safe != nullptr)
            {
                safe->searchIndex = index;
                safe->searchCache.clear();
                if (safe->searchBox.getText().trim().isNotEmpty())
                    safe->applySearch();
            }
        });
    });
}

void BrowserPanel::applySearch()
{
    auto query = searchBox.getText().trim().toLowerCase();

    if (query.isEmpty())
    {
        if (tree.getRootItem() != rootItem.get())
        {
            tree.setRootItem (nullptr);
            searchRootItem.reset();
            tree.setRootItem (rootItem.get());
            tree.setRootItemVisible (false);
        }
        return;
    }

    auto results = std::make_unique<RootItem>();
    if (searchIndex != nullptr)
    {
        auto cacheIt = searchCache.find (query);
        if (cacheIt != searchCache.end())
        {
            int shown = 0;
            for (auto idx : cacheIt->second)
            {
                results->addSubItem (new FileItem (*this, (*searchIndex)[(size_t) idx].file));
                if (++shown >= maxSearchResults)
                    break;
            }
        }
        else
        {
            juce::StringArray terms;
            terms.addTokens (query, " ", {});
            terms.removeEmptyStrings();

            std::vector<int> hits;
            for (int i = 0; i < (int) searchIndex->size(); ++i)
            {
                auto& entry = (*searchIndex)[(size_t) i];
                bool match = true;
                for (auto& t : terms)
                    if (! entry.nameLower.contains (t))
                    {
                        match = false;
                        break;
                    }
                if (! match || isIgnoredOrAncestor (entry.file))
                    continue;
                hits.push_back (i);
            }
            searchCache[query] = hits;

            int shown = 0;
            for (auto idx : hits)
            {
                results->addSubItem (new FileItem (*this, (*searchIndex)[(size_t) idx].file));
                if (++shown >= maxSearchResults)
                    break;
            }
        }
    }

    tree.setRootItem (nullptr);
    searchRootItem = std::move (results);
    tree.setRootItem (searchRootItem.get());
    tree.setRootItemVisible (false);
}

void BrowserPanel::showOptionsMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Show file extensions", true, showFileExtensions);
    m.addSeparator();
    m.addItem (2, "Rebuild search index");

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
        [this] (int r)
        {
            if (r == 1)
            {
                showFileExtensions = ! showFileExtensions;
                settings.setValue ("browserShowExt", showFileExtensions);
                settings.saveIfNeeded();
                tree.repaint();
            }
            else if (r == 2)
                buildSearchIndexAsync();
        });
}

void BrowserPanel::showBlankAreaContextMenu (juce::Point<int> screenPosition)
{
    juce::PopupMenu menu;
    menu.addItem (1, "Add Path");

    auto safe = juce::Component::SafePointer<BrowserPanel> (this);
    menu.showMenuAsync (
        juce::PopupMenu::Options().withTargetScreenArea (
            { screenPosition.x, screenPosition.y, 1, 1 }),
        [safe] (int result)
        {
            if (result == 1 && safe != nullptr)
                safe->chooseCustomRoot();
        });
}

void BrowserPanel::chooseCustomRoot()
{
    chooser = std::make_unique<juce::FileChooser> (
        "Add folder to browser",
        juce::File::getSpecialLocation (juce::File::userHomeDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectDirectories,
        [this] (const juce::FileChooser& fc)
        {
            auto folder = fc.getResult();
            if (folder.isDirectory())
                addCustomRoot (folder);
        });
}

juce::File BrowserPanel::getSelectedFile() const
{
    return selectedFile;
}

juce::File BrowserPanel::resolveLinkedMediaFile (const juce::File& f)
{
    if (f.isSymbolicLink())
    {
        auto target = f.getLinkedTarget();
        if (target.existsAsFile())
            return target;
    }

   #if JUCE_WINDOWS
    if (f.isShortcut())
    {
        auto target = f.getLinkedTarget();
        if (target.existsAsFile())
            return target;
    }
   #endif

    return f;
}

bool BrowserPanel::isAudioFile (const juce::File& f)
{
    return resolveLinkedMediaFile (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;wma");
}

bool BrowserPanel::isMidiFile (const juce::File& f)
{
    return resolveLinkedMediaFile (f).hasFileExtension ("mid;midi");
}

bool BrowserPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (juce::File (f).isDirectory())
            return true;
    return false;
}

void BrowserPanel::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& f : files)
    {
        juce::File folder (f);
        if (folder.isDirectory())
            addCustomRoot (folder);
    }
}

void BrowserPanel::setZoom (float newZoom)
{
    newZoom = juce::jlimit (0.7f, 2.0f, newZoom);
    if (std::abs (newZoom - zoom) < 1.0e-3f)
        return;
    zoom = newZoom;
    settings.setValue ("browserZoom", (double) zoom);
    settings.saveIfNeeded();

    if (auto* root = tree.getRootItem())
        root->treeHasChanged();
    tree.repaint();
}

bool BrowserPanel::keyPressed (const juce::KeyPress& key)
{
    if (key.getModifiers().isCtrlDown())
    {
        auto c = key.getTextCharacter();
        auto code = key.getKeyCode();
        if (c == '+' || c == '=' || code == juce::KeyPress::numberPadAdd)
        {
            setZoom (zoom * 1.1f);
            return true;
        }
        if (c == '-' || c == '_' || code == juce::KeyPress::numberPadSubtract)
        {
            setZoom (zoom * 0.9f);
            return true;
        }
        if (c == '0')
        {
            setZoom (1.0f);
            return true;
        }
    }
    return false;
}

BrowserPanel::NfoInfo BrowserPanel::readFolderNfo (const juce::File& folder)
{
    NfoInfo info;
    auto nfo = folder.getParentDirectory().getChildFile (folder.getFileName() + ".nfo");
    if (! nfo.existsAsFile())
        return info;

    auto text = nfo.loadFileAsString();
    juce::String bitmapRel;
    for (auto& rawLine : juce::StringArray::fromLines (text))
    {
        auto line = rawLine.trim();
        if (line.startsWithIgnoreCase ("Color="))
        {
            auto hex = line.fromFirstOccurrenceOf ("=", false, false).trim()
                           .removeCharacters ("$#");
            if (hex.isNotEmpty())
            {
                auto v = (juce::uint32) hex.getHexValue64();
                if (hex.length() <= 6)
                    v |= 0xff000000;
                info.colour = juce::Colour (v);
                info.hasColour = true;
            }
        }
        else if (line.startsWithIgnoreCase ("Tip="))
        {
            info.tip = line.fromFirstOccurrenceOf ("=", false, false).trim();
        }
        else if (line.startsWithIgnoreCase ("Bitmap="))
        {
            bitmapRel = line.fromFirstOccurrenceOf ("=", false, false).trim();
        }
    }

    if (bitmapRel.isNotEmpty())
    {
        auto imgFile = folder.getChildFile (bitmapRel);
        if (! imgFile.existsAsFile())
            imgFile = nfo.getParentDirectory().getChildFile (bitmapRel);
        if (imgFile.existsAsFile())
            info.image = juce::ImageFileFormat::loadFrom (imgFile);
    }

    info.valid = info.hasColour || info.tip.isNotEmpty() || info.image.isValid();
    return info;
}

void BrowserPanel::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelBg);
    g.setColour (Colors::text);
    g.setFont (Typography::caps (13.5f));
    g.drawText ("BROWSER", 10, 0, 120, headerH, juce::Justification::centredLeft);

    g.setColour (Colors::textDim.withAlpha (0.7f));
    g.setFont (Typography::ui (10.0f));
    g.drawText (juce::String (juce::roundToInt (zoom * 100.0f)) + "%",
                getWidth() - resizeEdgeW - 142, 0, 44, headerH, juce::Justification::centredRight);

    auto content = getLocalBounds().withTrimmedRight (resizeEdgeW).withTrimmedTop (headerH);
    auto searchBar = content.removeFromBottom (searchBarH);

    g.setColour (Colors::panelDark);
    g.fillRoundedRectangle (content.toFloat().reduced (2.0f, 0.0f), Metrics::rL);

    auto well = searchBar.reduced (searchBarInsetX, searchBarInsetY).toFloat();
    float pill = well.getHeight() * 0.5f;
    g.setColour (Colors::panelDark);
    g.fillRoundedRectangle (well, pill);

    if (searchBox.hasKeyboardFocus (true))
    {
        g.setColour (Colors::accent);
        g.drawRoundedRectangle (well.reduced (0.75f), pill, 1.5f);
    }

    if (resizing)
    {
        g.setColour (Colors::accent.withAlpha (0.30f));
        g.fillRect ((float) getWidth() - 3.0f, 0.0f, 3.0f, (float) getHeight());
    }
}

bool BrowserPanel::overResizeEdge (juce::Point<int> p) const
{
    return p.x >= getWidth() - resizeEdgeW && p.x <= getWidth();
}

void BrowserPanel::resized()
{
    auto r = getLocalBounds().withTrimmedRight (resizeEdgeW);
    auto topRow = r.removeFromTop (headerH).reduced (6, 3);
    closeBtn.setBounds (topRow.removeFromRight (28));
    topRow.removeFromRight (2);
    refreshBtn.setBounds (topRow.removeFromRight (28));
    topRow.removeFromRight (2);
    addFolderBtn.setBounds (topRow.removeFromRight (28));

    auto searchBar = r.removeFromBottom (searchBarH);
    searchBox.setBounds (searchBar.reduced (searchBarInsetX, searchBarInsetY));

    tree.setBounds (r.reduced (4, 2));
}

void BrowserPanel::setPreferredWidth (int w)
{
    w = juce::jlimit (minWidth, maxWidth, w);
    if (w == browserWidth)
        return;
    browserWidth = w;
    settings.setValue ("browserWidth", browserWidth);
    settings.saveIfNeeded();
    if (onWidthChanged)
        onWidthChanged();
}

void BrowserPanel::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (overResizeEdge (e.getEventRelativeTo (this).getPosition())
                    ? juce::MouseCursor::LeftRightResizeCursor
                    : juce::MouseCursor::NormalCursor);
}

void BrowserPanel::mouseExit (const juce::MouseEvent&)
{
    if (! resizing)
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

void BrowserPanel::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isMiddleButtonDown())
        return;

    auto localEvent = e.getEventRelativeTo (this);
    if (e.mods.isPopupMenu() && tree.getBounds().contains (localEvent.getPosition()))
    {
        auto treeEvent = e.getEventRelativeTo (&tree);
        if (tree.getItemAt (treeEvent.y) == nullptr)
        {
            showBlankAreaContextMenu (e.getScreenPosition());
            return;
        }
    }

    if (e.mods.isPopupMenu() && localEvent.y < headerH
        && ! overResizeEdge (localEvent.getPosition()))
    {
        showOptionsMenu();
        return;
    }
    if (overResizeEdge (localEvent.getPosition()))
    {
        resizing = true;
        dragStartWidth = browserWidth;
        dragStartX = e.getScreenX();
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        repaint();
    }
}

void BrowserPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isMiddleButtonDown())
        return;
    if (resizing)
        setPreferredWidth (dragStartWidth + (e.getScreenX() - dragStartX));
}

void BrowserPanel::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isMiddleButtonDown())
        return;
    if (resizing)
    {
        resizing = false;
        setMouseCursor (overResizeEdge (e.getEventRelativeTo (this).getPosition())
                        ? juce::MouseCursor::LeftRightResizeCursor
                        : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

bool BrowserPanel::isIgnored (const juce::File& f) const
{
    return ignoredList.contains (f.getFullPathName());
}

bool BrowserPanel::isIgnoredOrAncestor (const juce::File& f) const
{
    for (auto& path : ignoredList)
    {
        juce::File ignored (path);
        if (f == ignored || f.isAChildOf (ignored))
            return true;
    }
    return false;
}

void BrowserPanel::setIgnored (const juce::File& f, bool ignored)
{
    auto path = f.getFullPathName();
    if (ignored)
        ignoredList.addIfNotAlreadyThere (path);
    else
        ignoredList.removeString (path);
    settings.setValue ("browserIgnored", ignoredList.joinIntoString ("|"));
    settings.saveIfNeeded();
    tree.repaint();
}

juce::Array<juce::File> BrowserPanel::selectedFolders() const
{
    juce::Array<juce::File> out;
    for (int i = 0; i < tree.getNumSelectedItems(); ++i)
        if (auto* fi = dynamic_cast<FolderItem*> (tree.getSelectedItem (i)))
            out.addIfNotAlreadyThere (fi->getFolder());
    return out;
}

void BrowserPanel::showFileContextMenu (const juce::File& clicked)
{
    juce::PopupMenu m;
    if (isAudioFile (clicked))
    {
        juce::String label = getSendToChannelLabel ? getSendToChannelLabel() : juce::String();
        m.addItem (2, "Send to selected channel" + (label.isNotEmpty() ? " (" + label + ")" : juce::String()),
                   label.isNotEmpty());
        m.addSeparator();
    }
    m.addItem (1, "Open in Explorer");

    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
        [this, clicked] (int r)
        {
            if (r == 1)
                clicked.revealToUser();
            else if (r == 2 && onSendToSelectedChannel)
                onSendToSelectedChannel (clicked);
        });
}

void BrowserPanel::showFolderContextMenu (const juce::File& clicked)
{
    auto targets = selectedFolders();
    if (! targets.contains (clicked))
        targets = { clicked };

    bool allIgnored = true;
    for (auto& f : targets)
        allIgnored = allIgnored && isIgnored (f);

    juce::PopupMenu m;
    juce::String label = targets.size() > 1 ? (" (" + juce::String (targets.size()) + " folders)") : juce::String();
    m.addItem (1, (allIgnored ? "Show" : "Ignore") + label);
    if (! allIgnored)
        m.addItem (2, "Show" + label);
    m.addSeparator();
    m.addItem (10, "Open in Explorer");

    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
        [this, targets, allIgnored, clicked] (int r)
        {
            if (r == 1)
                for (auto& f : targets) setIgnored (f, ! allIgnored);
            else if (r == 2)
                for (auto& f : targets) setIgnored (f, false);
            else if (r == 10)
                clicked.revealToUser();
        });
}
