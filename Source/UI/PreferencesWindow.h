#pragma once
#include <JuceHeader.h>
#include "AppLookAndFeel.h"
#include "AppSettings.h"
#include "ThemeManager.h"
#include "ThemeWindow.h"
#include "Dialogs.h"
#include "DarkTitleBar.h"

class PreferencesContent : public juce::Component
{
public:
    PreferencesContent (juce::AudioDeviceManager& adm,
                        juce::PropertiesFile& props,
                        std::function<void()> onAudioChanged)
        : deviceManager (adm), settings (props), audioChanged (std::move (onAudioChanged))
    {
        audioTitle.setText ("Audio Device", juce::dontSendNotification);
        audioTitle.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        addAndMakeVisible (audioTitle);

        followDefaultToggle.setButtonText ("Windows Default Output (follow system default device)");
        followDefaultToggle.setToggleState (AppSettings::followDefaultOutput (settings),
                                            juce::dontSendNotification);
        followDefaultToggle.onClick = [this]
        {
            const bool on = followDefaultToggle.getToggleState();
            AppSettings::setFollowDefaultOutput (settings, on);
            settings.saveIfNeeded();
            if (onFollowDefaultOutputChanged)
                onFollowDefaultOutputChanged (on);
        };
        addAndMakeVisible (followDefaultToggle);

        selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
            adm, 0, 8, 0, 2, false, false, true, false);
        addAndMakeVisible (*selector);

        asioPanelBtn.setButtonText ("Show ASIO panel");
        asioPanelBtn.onClick = [this]
        {
            if (auto* d = deviceManager.getCurrentAudioDevice())
                if (d->hasControlPanel() && d->showControlPanel())
                    deviceManager.restartLastAudioDevice();
        };
        addAndMakeVisible (asioPanelBtn);
        updateAsioButton();

        priorityLbl.setText ("Priority", juce::dontSendNotification);
        priorityLbl.setFont (juce::Font (juce::FontOptions (15.0f)));
        addAndMakeVisible (priorityLbl);

        priorityBox.addItem ("Normal", 1);
        priorityBox.addItem ("High", 2);
        priorityBox.addItem ("Highest", 3);
        priorityBox.setSelectedId (AppSettings::processPriority (settings) + 1,
                                   juce::dontSendNotification);
        priorityBox.onChange = [this]
        {
            int level = priorityBox.getSelectedId() - 1;
            AppSettings::setProcessPriority (settings, level);
            settings.saveIfNeeded();
            AppSettings::applyProcessPriority (level);
        };
        addAndMakeVisible (priorityBox);

        smartDisableToggle.setButtonText ("Smart disable (skip silent effect chains)");
        smartDisableToggle.setToggleState (AppSettings::smartDisable (settings),
                                           juce::dontSendNotification);
        smartDisableToggle.onClick = [this]
        {
            AppSettings::setSmartDisable (settings, smartDisableToggle.getToggleState());
            settings.saveIfNeeded();
            if (onSmartDisableChanged)
                onSmartDisableChanged (smartDisableToggle.getToggleState());
        };
        addAndMakeVisible (smartDisableToggle);

        multicoreToggle.setButtonText ("Multicore rendering (instruments across CPU cores)");
        multicoreToggle.setToggleState (AppSettings::multicoreRendering (settings),
                                        juce::dontSendNotification);
        multicoreToggle.onClick = [this]
        {
            AppSettings::setMulticoreRendering (settings, multicoreToggle.getToggleState());
            settings.saveIfNeeded();
            if (onMulticoreChanged)
                onMulticoreChanged (multicoreToggle.getToggleState());
        };
        addAndMakeVisible (multicoreToggle);

        generalTitle.setText ("General", juce::dontSendNotification);
        generalTitle.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        addAndMakeVisible (generalTitle);

        auto styleLabel = [this] (juce::Label& l, const juce::String& t)
        {
            l.setText (t, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (15.0f)));
            addAndMakeVisible (l);
        };

        styleLabel (uiScaleLbl, "UI scale");
        for (int i = 0; i < numScaleOptions; ++i)
            uiScaleBox.addItem (juce::String (juce::roundToInt (scaleOptions[i] * 100.0)) + "%", i + 1);
        {
            double cur = AppSettings::uiScale (settings);
            int sel = 3;
            for (int i = 0; i < numScaleOptions; ++i)
                if (std::abs (scaleOptions[i] - cur) < 0.001) { sel = i + 1; break; }
            uiScaleBox.setSelectedId (sel, juce::dontSendNotification);
        }
        uiScaleBox.onChange = [this]
        {
            int idx = uiScaleBox.getSelectedId() - 1;
            if (idx < 0 || idx >= numScaleOptions)
                return;
            AppSettings::setUiScale (settings, scaleOptions[idx]);
            settings.saveIfNeeded();
            updateUiScaleHint();
        };
        addAndMakeVisible (uiScaleBox);

        uiScaleHint.setText ("applies after restart", juce::dontSendNotification);
        uiScaleHint.setFont (juce::Font (juce::FontOptions (13.0f)));
        uiScaleHint.setColour (juce::Label::textColourId, Colors::textDim);
        addChildComponent (uiScaleHint);
        updateUiScaleHint();

        dockedLayoutToggle.setButtonText ("Docked layout (experimental, applies after restart)");
        dockedLayoutToggle.setToggleState (AppSettings::dockedLayout (settings), juce::dontSendNotification);
        dockedLayoutToggle.onClick = [this]
        {
            AppSettings::setDockedLayout (settings, dockedLayoutToggle.getToggleState());
            settings.saveIfNeeded();
        };
        addAndMakeVisible (dockedLayoutToggle);

        styleLabel (nameLbl, "Display name");
        nameBox.setText (AppSettings::displayName (settings), juce::dontSendNotification);
        nameBox.setTextToShowWhenEmpty ("Your name", Colors::textDim);
        auto commitName = [this]
        {
            AppSettings::setDisplayName (settings, nameBox.getText());
            settings.saveIfNeeded();
            if (onDisplayNameChanged) onDisplayNameChanged();
        };
        nameBox.onReturnKey = commitName;
        nameBox.onFocusLost = commitName;
        addAndMakeVisible (nameBox);

        styleLabel (avatarLbl, "Profile avatar");
        avatarBtn.setButtonText ("Set Avatar...");
        avatarBtn.onClick = [this] { chooseAvatar(); };
        addAndMakeVisible (avatarBtn);

        avatarClearBtn.setButtonText ("Clear");
        avatarClearBtn.onClick = [this] { clearAvatar(); };
        addAndMakeVisible (avatarClearBtn);

        loadAvatarPreview();

        styleLabel (templateLbl, "New project template");
        templateBox.addItem ("Default (drum kit)", 1);
        templateBox.addItem ("Empty (blank Sampler)", 2);
        templateBox.setSelectedId (AppSettings::startupTemplate (settings) == "blank" ? 2 : 1,
                                   juce::dontSendNotification);
        templateBox.onChange = [this]
        {
            AppSettings::setStartupTemplate (settings, templateBox.getSelectedId() == 2 ? "blank" : "drums");
            settings.saveIfNeeded();
        };
        addAndMakeVisible (templateBox);

        styleLabel (bpmLbl, "Default BPM");
        bpmSlider.setRange (20.0, 999.0, 1.0);
        bpmSlider.setValue (AppSettings::defaultBpm (settings), juce::dontSendNotification);
        bpmSlider.setSliderStyle (juce::Slider::IncDecButtons);
        bpmSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 70, 22);
        bpmSlider.onValueChange = [this]
        {
            AppSettings::setDefaultBpm (settings, bpmSlider.getValue());
            settings.saveIfNeeded();
        };
        addAndMakeVisible (bpmSlider);

        styleLabel (projectsLbl, "Projects folder");
        projectsValue.setColour (juce::Label::textColourId, Colors::textDim);
        projectsValue.setFont (juce::Font (juce::FontOptions (13.0f)));
        projectsValue.setText (AppSettings::projectsFolder (settings).getFullPathName(),
                               juce::dontSendNotification);
        addAndMakeVisible (projectsValue);

        projectsBtn.setButtonText ("Choose...");
        projectsBtn.onClick = [this] { chooseProjectsFolder(); };
        addAndMakeVisible (projectsBtn);

        projectsOpenBtn.setButtonText ("Open");
        projectsOpenBtn.onClick = [this] { AppSettings::projectsFolder (settings).startAsProcess(); };
        addAndMakeVisible (projectsOpenBtn);

        autoSaveToggle.setButtonText ("Enable auto-save");
        autoSaveToggle.setToggleState (AppSettings::autoSaveEnabled (settings), juce::dontSendNotification);
        autoSaveToggle.onClick = [this]
        {
            AppSettings::setAutoSaveEnabled (settings, autoSaveToggle.getToggleState());
            settings.saveIfNeeded();
            if (onAutoSaveChanged) onAutoSaveChanged();
        };
        addAndMakeVisible (autoSaveToggle);

        styleLabel (intervalLbl, "Auto-save every (minutes)");
        intervalSlider.setRange (1.0, 120.0, 1.0);
        intervalSlider.setValue (AppSettings::autoSaveMinutes (settings), juce::dontSendNotification);
        intervalSlider.setSliderStyle (juce::Slider::IncDecButtons);
        intervalSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 60, 22);
        intervalSlider.onValueChange = [this]
        {
            AppSettings::setAutoSaveMinutes (settings, (int) intervalSlider.getValue());
            settings.saveIfNeeded();
            if (onAutoSaveChanged) onAutoSaveChanged();
        };
        addAndMakeVisible (intervalSlider);

        styleLabel (folderLbl, "Auto-save folder");
        folderValue.setColour (juce::Label::textColourId, Colors::textDim);
        folderValue.setFont (juce::Font (juce::FontOptions (13.0f)));
        folderValue.setText (AppSettings::autoSaveFolder (settings).getFullPathName(),
                             juce::dontSendNotification);
        addAndMakeVisible (folderValue);

        folderBtn.setButtonText ("Choose...");
        folderBtn.onClick = [this] { chooseFolder(); };
        addAndMakeVisible (folderBtn);

        folderOpenBtn.setButtonText ("Open");
        folderOpenBtn.onClick = [this]
        {
            auto dir = AppSettings::autoSaveDir (settings);
            dir.createDirectory();
            dir.startAsProcess();
        };
        addAndMakeVisible (folderOpenBtn);

        autoSaveSizeLbl.setColour (juce::Label::textColourId, Colors::textDim);
        autoSaveSizeLbl.setFont (juce::Font (juce::FontOptions (13.0f)));
        addAndMakeVisible (autoSaveSizeLbl);
        refreshAutoSaveSize();

        styleLabel (tempLbl, "Temp data");
        tempSizeLbl.setColour (juce::Label::textColourId, Colors::textDim);
        tempSizeLbl.setFont (juce::Font (juce::FontOptions (13.0f)));
        tempSizeLbl.setJustificationType (juce::Justification::centredLeft);
        tempSizeLbl.setText ("...", juce::dontSendNotification);
        addAndMakeVisible (tempSizeLbl);

        clearTempBtn.setButtonText ("Clear Temp Data");
        clearTempBtn.onClick = [this] { clearTempData(); };
        addAndMakeVisible (clearTempBtn);

        refreshTempSize();

       #if ! OPENDAW_PUBLIC_BUILD
        onlineTitle.setText ("Online", juce::dontSendNotification);
        onlineTitle.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        addAndMakeVisible (onlineTitle);

        relayToggle.setButtonText ("Enable online friends server");
        relayToggle.setToggleState (AppSettings::relayEnabled (settings), juce::dontSendNotification);
        relayToggle.onClick = [this]
        {
            AppSettings::setRelayEnabled (settings, relayToggle.getToggleState());
            settings.saveIfNeeded();
            if (onRelayChanged) onRelayChanged();
        };
        addAndMakeVisible (relayToggle);

        styleLabel (relayHostLbl, "Server address");
        relayHostBox.setText (AppSettings::relayHost (settings), juce::dontSendNotification);
        relayHostBox.setTextToShowWhenEmpty ("host or IP", Colors::textDim);
        auto commitRelay = [this]
        {
            AppSettings::setRelayHost (settings, relayHostBox.getText());
            settings.saveIfNeeded();
            if (onRelayChanged) onRelayChanged();
        };
        relayHostBox.onReturnKey = commitRelay;
        relayHostBox.onFocusLost = commitRelay;
        addAndMakeVisible (relayHostBox);

        licenseTitle.setText ("License", juce::dontSendNotification);
        licenseTitle.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        addAndMakeVisible (licenseTitle);

        licenseStatusLbl.setColour (juce::Label::textColourId, Colors::text);
        licenseStatusLbl.setText ("Checking license...", juce::dontSendNotification);
        addAndMakeVisible (licenseStatusLbl);

        licenseHwidLbl.setColour (juce::Label::textColourId, Colors::textDim);
        licenseHwidLbl.setFont (juce::Font (juce::FontOptions (12.0f)));
        addAndMakeVisible (licenseHwidLbl);
       #endif

        deviceManager.addChangeListener (&changeRelay);
        changeRelay.onChange = [this]
        {
            updateAsioButton();
            if (audioChanged)
                audioChanged();
            juce::Component::SafePointer<PreferencesContent> safe (this);
            juce::MessageManager::callAsync ([safe] { if (auto* s = safe.getComponent()) s->resized(); });
        };

        setSize (512, 1296);
    }

    ~PreferencesContent() override
    {
        deviceManager.removeChangeListener (&changeRelay);
    }

    std::function<void()> onAutoSaveChanged;
    std::function<void()> onDisplayNameChanged;
    std::function<void()> onAvatarChanged;
    std::function<void()> onRelayChanged;
    std::function<void (bool)> onSmartDisableChanged;
    std::function<void (bool)> onMulticoreChanged;
    std::function<void (bool)> onFollowDefaultOutputChanged;
    std::function<juce::File()> getActiveSessionFolder;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::panelBg);

        if (! avatarPreviewBounds.isEmpty())
        {
            auto b = avatarPreviewBounds;
            juce::Path clip;
            clip.addEllipse (b);
            g.setColour (Colors::panelDark);
            g.fillEllipse (b);

            if (avatarPreview.isValid())
            {
                juce::Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (clip);
                g.drawImage (avatarPreview, b, juce::RectanglePlacement::centred
                                                 | juce::RectanglePlacement::fillDestination);
            }
            else
            {
                g.setColour (Colors::textDim);
                g.setFont (juce::Font (juce::FontOptions (8.0f)));
                g.drawText ("none", b, juce::Justification::centred);
            }
            g.setColour (Colors::outline);
            g.drawEllipse (b, 1.0f);
        }
    }

    void resized() override
    {
        constexpr int lblW = 210;
        auto r = getLocalBounds().reduced (12);
        audioTitle.setBounds (r.removeFromTop (26));
        followDefaultToggle.setBounds (r.removeFromTop (26));
        r.removeFromTop (4);
        selector->setBounds (r.removeFromTop (measureSelectorHeight (r.getWidth())));
        r.removeFromTop (4);

        auto row = [&r] () { auto x = r.removeFromTop (30); r.removeFromTop (6); return x; };

        if (asioPanelBtn.isVisible())
            { auto x = row(); asioPanelBtn.setBounds (x.removeFromLeft (170)); }
        { auto x = row(); priorityLbl.setBounds (x.removeFromLeft (lblW)); priorityBox.setBounds (x.removeFromLeft (150)); }
        { auto x = row(); smartDisableToggle.setBounds (x.removeFromLeft (380)); }
        { auto x = row(); multicoreToggle.setBounds (x.removeFromLeft (380)); }

        r.removeFromTop (8);
        generalTitle.setBounds (r.removeFromTop (26));
        r.removeFromTop (4);

        { auto x = row(); uiScaleLbl.setBounds (x.removeFromLeft (lblW)); uiScaleBox.setBounds (x.removeFromLeft (150)); x.removeFromLeft (8); uiScaleHint.setBounds (x); }
        { auto x = row(); dockedLayoutToggle.setBounds (x.removeFromLeft (480)); }
        { auto x = row(); nameLbl.setBounds (x.removeFromLeft (lblW)); nameBox.setBounds (x.removeFromLeft (220)); }
        {
            auto x = r.removeFromTop (46); r.removeFromTop (6);
            avatarLbl.setBounds (x.removeFromLeft (lblW));
            avatarPreviewBounds = x.removeFromLeft (44).reduced (3).toFloat();
            x.removeFromLeft (8);
            avatarBtn.setBounds (x.removeFromLeft (110).withSizeKeepingCentre (110, 30));
            x.removeFromLeft (6);
            avatarClearBtn.setBounds (x.removeFromLeft (70).withSizeKeepingCentre (70, 30));
        }
        { auto x = row(); templateLbl.setBounds (x.removeFromLeft (lblW)); templateBox.setBounds (x.removeFromLeft (220)); }
        { auto x = row(); bpmLbl.setBounds (x.removeFromLeft (lblW)); bpmSlider.setBounds (x.removeFromLeft (180)); }
        {
            auto x = row();
            projectsLbl.setBounds (x.removeFromLeft (lblW));
            projectsBtn.setBounds (x.removeFromLeft (90));
            x.removeFromLeft (6);
            projectsOpenBtn.setBounds (x.removeFromLeft (70));
        }
        projectsValue.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);
        { auto x = row(); autoSaveToggle.setBounds (x.removeFromLeft (220)); }
        { auto x = row(); intervalLbl.setBounds (x.removeFromLeft (lblW)); intervalSlider.setBounds (x.removeFromLeft (150)); }
        {
            auto x = row();
            folderLbl.setBounds (x.removeFromLeft (lblW));
            folderBtn.setBounds (x.removeFromLeft (90));
            x.removeFromLeft (6);
            folderOpenBtn.setBounds (x.removeFromLeft (70));
        }
        folderValue.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        autoSaveSizeLbl.setBounds (r.removeFromTop (20));
        r.removeFromTop (6);
        {
            auto x = row();
            tempLbl.setBounds (x.removeFromLeft (lblW));
            clearTempBtn.setBounds (x.removeFromLeft (150));
            x.removeFromLeft (8);
            tempSizeLbl.setBounds (x);
        }

       #if ! OPENDAW_PUBLIC_BUILD
        r.removeFromTop (8);
        onlineTitle.setBounds (r.removeFromTop (26));
        r.removeFromTop (4);
        { auto x = row(); relayToggle.setBounds (x.removeFromLeft (300)); }
        { auto x = row(); relayHostLbl.setBounds (x.removeFromLeft (lblW)); relayHostBox.setBounds (x.removeFromLeft (220)); }

        r.removeFromTop (8);
        licenseTitle.setBounds (r.removeFromTop (26));
        r.removeFromTop (4);
        licenseStatusLbl.setBounds (r.removeFromTop (24));
        licenseHwidLbl.setBounds (r.removeFromTop (20));
       #endif
    }

    void setLicenseInfo (const juce::String& status, const juce::String& hwid)
    {
        licenseStatusLbl.setText (status, juce::dontSendNotification);
        licenseHwidLbl.setText ("Machine ID: " + hwid.substring (0, 24), juce::dontSendNotification);
    }

    void updateUiScaleHint()
    {
        const float running = juce::Desktop::getInstance().getGlobalScaleFactor();
        int idx = uiScaleBox.getSelectedId() - 1;
        const bool differs = idx >= 0 && idx < numScaleOptions
                             && std::abs ((float) scaleOptions[idx] - running) > 0.001f;
        uiScaleHint.setVisible (differs);
    }

    void refreshFollowDefaultOutput()
    {
        followDefaultToggle.setToggleState (AppSettings::followDefaultOutput (settings),
                                            juce::dontSendNotification);
    }

    void lookAndFeelChanged() override
    {
        uiScaleHint    .setColour (juce::Label::textColourId, Colors::textDim);
        folderValue    .setColour (juce::Label::textColourId, Colors::textDim);
        projectsValue  .setColour (juce::Label::textColourId, Colors::textDim);
        autoSaveSizeLbl.setColour (juce::Label::textColourId, Colors::textDim);
        tempSizeLbl    .setColour (juce::Label::textColourId, Colors::textDim);
        nameBox     .setTextToShowWhenEmpty ("Your name",  Colors::textDim);
        relayHostBox.setTextToShowWhenEmpty ("host or IP", Colors::textDim);
        repaint();
    }

private:
    struct ChangeRelay : public juce::ChangeListener
    {
        std::function<void()> onChange;
        void changeListenerCallback (juce::ChangeBroadcaster*) override { if (onChange) onChange(); }
    };

    int measureSelectorHeight (int width)
    {
        selector->setBounds (0, 0, width, 600);
        int maxBottom = 0;
        for (auto* c : selector->getChildren())
            if (c->isVisible())
                maxBottom = juce::jmax (maxBottom, c->getBottom());
        return juce::jlimit (220, 460, maxBottom + 8);
    }

    void updateAsioButton()
    {
        auto* d = deviceManager.getCurrentAudioDevice();
        const bool hasPanel = d != nullptr && d->hasControlPanel();
        const bool wasVisible = asioPanelBtn.isVisible();
        asioPanelBtn.setEnabled (hasPanel);
        asioPanelBtn.setVisible (hasPanel);
        if (wasVisible != hasPanel)
            resized();
    }

    static juce::Image squareAvatar (const juce::Image& src, int size)
    {
        if (! src.isValid())
            return {};
        int side = juce::jmin (src.getWidth(), src.getHeight());
        auto cropped = src.getClippedImage ({ (src.getWidth()  - side) / 2,
                                              (src.getHeight() - side) / 2,
                                              side, side });
        return cropped.rescaled (size, size, juce::Graphics::highResamplingQuality);
    }

    void loadAvatarPreview()
    {
        avatarPreview = AppSettings::hasAvatar (settings)
                          ? juce::ImageFileFormat::loadFrom (AppSettings::avatarFile (settings))
                          : juce::Image();
        avatarClearBtn.setEnabled (avatarPreview.isValid());
        repaint();
    }

    void chooseAvatar()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose an avatar image",
            juce::File::getSpecialLocation (juce::File::userPicturesDirectory),
            "*.png;*.jpg;*.jpeg;*.bmp");
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (! f.existsAsFile())
                    return;

                auto src = juce::ImageFileFormat::loadFrom (f);
                auto square = squareAvatar (src, 64);
                if (! square.isValid())
                    return;

                juce::MemoryOutputStream png;
                juce::PNGImageFormat fmt;
                if (! fmt.writeImageToStream (square, png))
                    return;

                auto dest = AppSettings::avatarFile (settings);
                if (! dest.replaceWithData (png.getData(), png.getDataSize()))
                    return;

                AppSettings::setHasAvatar (settings, true);
                settings.saveIfNeeded();
                avatarPreview = square;
                avatarClearBtn.setEnabled (true);
                repaint();
                if (onAvatarChanged) onAvatarChanged();
            });
    }

    void clearAvatar()
    {
        AppSettings::avatarFile (settings).deleteFile();
        AppSettings::setHasAvatar (settings, false);
        settings.saveIfNeeded();
        avatarPreview = juce::Image();
        avatarClearBtn.setEnabled (false);
        repaint();
        if (onAvatarChanged) onAvatarChanged();
    }

    static void deleteChildrenExcept (const juce::File& dir, const juce::File& keep)
    {
        for (auto& child : dir.findChildFiles (juce::File::findFilesAndDirectories
                                                 | juce::File::ignoreHiddenFiles, false))
        {
            if (child == keep)
                continue;

            if (keep.isAChildOf (child))
                deleteChildrenExcept (child, keep);
            else
                child.deleteRecursively();
        }
    }

    static juce::Array<juce::File> tempDataRoots()
    {
        auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory);
        juce::Array<juce::File> roots;
        if (auto openDawDir = temp.getChildFile ("mikeDAW"); openDawDir.isDirectory())
            roots.add (openDawDir);
        for (auto& sib : temp.findChildFiles (juce::File::findDirectories
                                               | juce::File::ignoreHiddenFiles, false, "mikeDAW_*"))
            roots.add (sib);
        return roots;
    }

    static juce::int64 tempDataBytes (const juce::File& liveFolder)
    {
        const bool haveLive = liveFolder.isDirectory();
        juce::int64 bytes = 0;
        for (auto& root : tempDataRoots())
            for (auto& f : root.findChildFiles (juce::File::findFiles, true))
                if (! (haveLive && (f == liveFolder || f.isAChildOf (liveFolder))))
                    bytes += f.getSize();
        return bytes;
    }

    void setTempSizeText (juce::int64 bytes)
    {
        tempSizeLbl.setText (bytes > 0 ? juce::File::descriptionOfSizeInBytes (bytes)
                                       : juce::String ("empty"),
                             juce::dontSendNotification);
    }

    void refreshTempSize()
    {
        tempSizeLbl.setText ("measuring...", juce::dontSendNotification);
        juce::File live;
        if (getActiveSessionFolder)
            live = getActiveSessionFolder();

        juce::Component::SafePointer<PreferencesContent> safe (this);
        juce::Thread::launch ([safe, live]
        {
            auto bytes = tempDataBytes (live);
            juce::MessageManager::callAsync ([safe, bytes]
            {
                if (auto* self = safe.getComponent())
                    self->setTempSizeText (bytes);
            });
        });
    }

    void clearTempData()
    {
        juce::File liveFolder;
        if (getActiveSessionFolder)
            liveFolder = getActiveSessionFolder();
        const bool haveLive = liveFolder.isDirectory();

        auto roots = tempDataRoots();

        juce::int64 bytes = tempDataBytes (liveFolder);

        for (auto& root : roots)
        {
            if (haveLive && (liveFolder == root || liveFolder.isAChildOf (root)))
                deleteChildrenExcept (root, liveFolder);
            else
                root.deleteRecursively();
        }

        double mb = bytes / (1024.0 * 1024.0);
        clearTempBtn.setButtonText ("Cleared " + juce::String (mb, mb < 10.0 ? 2 : 1) + " MB");
        refreshTempSize();
    }

    void refreshAutoSaveSize()
    {
        auto files = AppSettings::autoSaveDir (settings)
                         .findChildFiles (juce::File::findFiles, false, "*.odaw");
        juce::int64 bytes = 0;
        for (auto& f : files)
            bytes += f.getSize();

        autoSaveSizeLbl.setText ("Autosaves: " + juce::String (files.size())
                                     + (files.size() == 1 ? " file, " : " files, ")
                                     + juce::File::descriptionOfSizeInBytes (bytes),
                                 juce::dontSendNotification);
    }

    void chooseProjectsFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Projects folder",
            AppSettings::projectsFolder (settings));
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (! f.isDirectory())
                    return;
                AppSettings::setProjectsFolder (settings, f);
                settings.saveIfNeeded();
                projectsValue.setText (f.getFullPathName(), juce::dontSendNotification);
            });
    }

    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Auto-save folder",
            AppSettings::autoSaveFolder (settings));
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f.isDirectory())
                {
                    AppSettings::setAutoSaveFolder (settings, f);
                    settings.saveIfNeeded();
                    folderValue.setText (f.getFullPathName(), juce::dontSendNotification);
                    refreshAutoSaveSize();
                }
            });
    }

    juce::AudioDeviceManager& deviceManager;
    juce::PropertiesFile& settings;
    std::function<void()> audioChanged;

    juce::Label audioTitle, generalTitle, onlineTitle;
    juce::Label licenseTitle, licenseStatusLbl, licenseHwidLbl;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> selector;

    juce::Label nameLbl, avatarLbl, templateLbl, bpmLbl, intervalLbl, folderLbl, folderValue, autoSaveSizeLbl, priorityLbl, tempLbl, tempSizeLbl, uiScaleLbl, uiScaleHint, relayHostLbl;
    juce::Label projectsLbl, projectsValue;
    juce::TextEditor nameBox, relayHostBox;
    juce::ToggleButton relayToggle;
    juce::ComboBox templateBox, priorityBox, uiScaleBox;

    static constexpr int    numScaleOptions = 8;
    static constexpr double scaleOptions[numScaleOptions]
        = { 0.75, 0.90, 1.00, 1.10, 1.25, 1.50, 1.75, 2.00 };
    juce::Slider bpmSlider, intervalSlider;
    juce::ToggleButton autoSaveToggle, smartDisableToggle, multicoreToggle, followDefaultToggle, dockedLayoutToggle;
    juce::TextButton folderBtn, folderOpenBtn, asioPanelBtn, avatarBtn, avatarClearBtn, clearTempBtn;
    juce::TextButton projectsBtn, projectsOpenBtn;
    juce::Image avatarPreview;
    juce::Rectangle<float> avatarPreviewBounds;
    std::unique_ptr<juce::FileChooser> chooser;
    ChangeRelay changeRelay;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreferencesContent)
};

class PreferencesTabs : public juce::TabbedComponent
{
public:
    PreferencesTabs (juce::AudioDeviceManager& adm, juce::PropertiesFile& props,
                     ThemeManager& theme, std::function<void()> onAudioChanged)
        : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop)
    {
        setTabBarDepth (32);
        setOutline (0);
        setColour (juce::TabbedComponent::backgroundColourId, Colors::panelBg);

        content = new PreferencesContent (adm, props, std::move (onAudioChanged));
        addTab ("Settings", Colors::panelBg, content, true);
        addTab ("Theme",    Colors::panelBg, new ThemeContent (theme), true);
    }

    void lookAndFeelChanged() override
    {
        juce::TabbedComponent::lookAndFeelChanged();
        setColour (juce::TabbedComponent::backgroundColourId, Colors::panelBg);
        for (int i = 0; i < getNumTabs(); ++i)
            setTabBackgroundColour (i, Colors::panelBg);
    }

    PreferencesContent* content = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreferencesTabs)
};

class PreferencesWindow : public juce::DocumentWindow
{
public:
    PreferencesWindow (juce::AudioDeviceManager& adm, juce::PropertiesFile& props,
                       ThemeManager& theme, std::function<void()> onAudioChanged)
        : juce::DocumentWindow ("Preferences", Colors::panelBg,
                                juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        auto* tabs = new PreferencesTabs (adm, props, theme, std::move (onAudioChanged));
        content = tabs->content;
        Dialogs::setContentFitted (*this, tabs, 512, 1232);
        setResizable (false, false);
    }

    void closeButtonPressed() override { setVisible (false); }

    void lookAndFeelChanged() override
    {
        juce::DocumentWindow::lookAndFeelChanged();
        setBackgroundColour (Colors::panelBg);
        DarkTitleBar::applyThemeColours (*this, Colors::header, Colors::text);
    }

    PreferencesContent* content = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreferencesWindow)
};
