#include <JuceHeader.h>
#include "CrashHandler.h"
#include "UI/MainComponent.h"
#include "UI/DarkTitleBar.h"
#include "UI/AppSettings.h"
#include "Import/FLPExport.h"
#include "Import/FLPImport.h"
#include "Engine/PluginHost.h"

#include <cstdio>

#if JUCE_WINDOWS
 #include <shlobj.h>
#endif

class mikeDAWApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "mikeDAW"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override
    {
        return getCommandLineParameters().contains (headlessExportFlag)
            || getCommandLineParameters().contains (headlessImportFlag);
    }

    void initialise (const juce::String& commandLine) override
    {
        CrashHandler::install();

        if (runHeadlessFlZipExport (commandLine) || runHeadlessFlpImport (commandLine))
        {
            quit();
            return;
        }

        migrateLegacyProfileIfNeeded();

        {
            juce::PropertiesFile::Options opts;
            opts.applicationName = "mikeDAW";
            opts.filenameSuffix = ".settings";
            opts.folderName = "mikeDAW";
            opts.osxLibrarySubFolder = "Application Support";
            juce::PropertiesFile props (opts);
            juce::Desktop::getInstance().setGlobalScaleFactor ((float) AppSettings::uiScale (props));
        }

        registerFileAssociationIfNeeded();

        watchdog = std::make_unique<CrashHandler::HangWatchdog>();
        mainWindow = std::make_unique<MainWindow> (getApplicationName());

        if (auto file = projectFileFromCommandLine (commandLine); file.existsAsFile())
        {
            juce::MessageManager::callAsync (
                [safe = juce::Component::SafePointer<MainWindow> (mainWindow.get()), file]
                {
                    if (safe != nullptr)
                        if (auto* main = dynamic_cast<MainComponent*> (safe->getContentComponent()))
                            main->openProjectFileFromOS (file);
                });
        }
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (mainWindow == nullptr)
            return;

        mainWindow->setMinimised (false);
        mainWindow->toFront (true);

        if (auto file = projectFileFromCommandLine (commandLine); file.existsAsFile())
            if (auto* main = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                main->openProjectFileFromOS (file);
    }

    void shutdown() override
    {
        raiseShutdownVeil();

        if (watchdog != nullptr)
            watchdog->setShuttingDown();

        if (mainWindow != nullptr)
            if (auto* main = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                main->prepareToQuit();

        mainWindow = nullptr;
        watchdog = nullptr;
        veil = nullptr;
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
        {
            if (auto* main = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
            {
                if (! main->tryClose())
                    return;
                raiseShutdownVeil();
                main->prepareToQuit();
            }
        }
        quit();
    }

    void raiseShutdownVeil()
    {
        if (veil != nullptr || mainWindow == nullptr || ! mainWindow->isShowing())
            return;

        auto* content = mainWindow->getContentComponent();
        if (content == nullptr)
            return;

        auto frame = content->createComponentSnapshot (content->getLocalBounds(), false);
        if (! frame.isValid())
            return;

        veil = std::make_unique<ShutdownVeil> (std::move (frame), content->getScreenBounds());

        if (auto* peer = veil->getPeer())
            peer->performAnyPendingRepaintsNow();

        juce::Thread::sleep (50);
    }

    class ShutdownVeil : public juce::Component
    {
    public:
        ShutdownVeil (juce::Image frame, juce::Rectangle<int> screenBounds)
            : snapshot (std::move (frame))
        {
            setOpaque (false);
            setBounds (screenBounds);
            addToDesktop (juce::ComponentPeer::windowIsTemporary
                          | juce::ComponentPeer::windowIgnoresKeyPresses
                          | juce::ComponentPeer::windowIsSemiTransparent);
            setAlwaysOnTop (true);
            setVisible (true);
            toFront (false);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colors::windowBg);
            if (snapshot.isValid())
                g.drawImage (snapshot, getLocalBounds().toFloat());
        }

    private:
        juce::Image snapshot;
    };

    static juce::File projectFileFromCommandLine (const juce::String& commandLine)
    {
        juce::ArgumentList args ("mikeDAW", commandLine);
        for (auto& a : args.arguments)
        {
            auto f = a.resolveAsFile();
            if (f.existsAsFile()
                && (f.hasFileExtension ("odaw") || f.hasFileExtension ("flp") || f.hasFileExtension ("zip")))
                return f;
        }
        return {};
    }

    static constexpr const char* headlessExportFlag = "--export-flzip";

    bool runHeadlessFlZipExport (const juce::String& commandLine)
    {
        juce::ArgumentList args ("mikeDAW", commandLine);
        const int flagIndex = args.indexOfOption (headlessExportFlag);

        if (flagIndex < 0)
            return false;

        const bool haveBothPaths = flagIndex + 2 < args.size();
        const auto input  = haveBothPaths ? args[flagIndex + 1].resolveAsFile() : juce::File();
        const auto output = haveBothPaths ? args[flagIndex + 2].resolveAsFile() : juce::File();

        FLPExport::Result result;

        if (! haveBothPaths)
            result.error = "Usage: mikeDAW " + juce::String (headlessExportFlag)
                             + " <input.odaw> <output.zip>";
        else if (! input.existsAsFile())
            result.error = "Input project not found: " + input.getFullPathName();
        else if (auto xml = juce::parseXML (input))
        {
            auto tree = juce::ValueTree::fromXml (*xml);

            if (tree.isValid())
                result = FLPExport::exportAsZip (tree, output);
            else
                result.error = "Not a project tree: " + input.getFullPathName();
        }
        else
            result.error = "Could not parse " + input.getFullPathName();

        const auto report = juce::String (result.success ? "OK\n" : "FAIL\n")
                              + (result.success ? result.summary : result.error);

        std::fprintf (stderr, "%s\n", report.toRawUTF8());
        std::fflush (stderr);

        if (output != juce::File())
            output.getSiblingFile (output.getFileName() + ".result.txt")
                  .replaceWithText (report + "\n");

        if (! result.success)
            setApplicationReturnValue (1);

        return true;
    }

    static constexpr const char* headlessImportFlag = "--import-flp";

    bool runHeadlessFlpImport (const juce::String& commandLine)
    {
        juce::ArgumentList args ("mikeDAW", commandLine);
        const int flagIndex = args.indexOfOption (headlessImportFlag);

        if (flagIndex < 0)
            return false;

        const bool haveBothPaths = flagIndex + 2 < args.size();
        const auto input  = haveBothPaths ? args[flagIndex + 1].resolveAsFile() : juce::File();
        const auto output = haveBothPaths ? args[flagIndex + 2].resolveAsFile() : juce::File();

        juce::String report;
        bool ok = false;

        if (! haveBothPaths)
            report = "FAIL\nUsage: mikeDAW " + juce::String (headlessImportFlag)
                       + " <input.flp|zip> <output.odaw>";
        else if (! input.existsAsFile())
            report = "FAIL\nInput not found: " + input.getFullPathName();
        else
        {
            PluginHost host;
            auto result = FLPImport::importFile (input, host);
            if (result.ok())
            {
                ok = output.replaceWithText (result.project.toXmlString());
                report = juce::String (ok ? "OK\n" : "FAIL\ncould not write output\n")
                           + result.summary + "\n" + result.warnings.joinIntoString ("\n");
            }
            else
                report = "FAIL\n" + result.error;
        }

        std::fprintf (stderr, "%s\n", report.toRawUTF8());
        std::fflush (stderr);

        if (output != juce::File())
            output.getSiblingFile (output.getFileName() + ".result.txt")
                  .replaceWithText (report + "\n");

        if (! ok)
            setApplicationReturnValue (1);

        return true;
    }

    static void migrateLegacyProfileIfNeeded()
    {
        auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
        auto oldDir = appData.getChildFile ("OpenDAW");
        auto newDir = appData.getChildFile ("mikeDAW");

        if (oldDir.getChildFile ("OpenDAW.settings").existsAsFile()
            && ! newDir.getChildFile ("mikeDAW.settings").existsAsFile())
        {
            newDir.createDirectory();
            for (auto& f : oldDir.findChildFiles (juce::File::findFilesAndDirectories, false))
            {
                auto name = f.getFileName();
                if (name.endsWithIgnoreCase (".TMP") || name == "sync-debug.log"
                    || name.startsWithIgnoreCase ("OpenDAW_temp"))
                    continue;
                if (name == "OpenDAW.settings")
                    f.copyFileTo (newDir.getChildFile ("mikeDAW.settings"));
                else if (f.isDirectory())
                    f.copyDirectoryTo (newDir.getChildFile (name));
                else
                    f.copyFileTo (newDir.getChildFile (name));
            }
        }

        auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        auto oldDocs = docs.getChildFile ("OpenDAW");
        auto newDocs = docs.getChildFile ("mikeDAW");
        if (oldDocs.isDirectory() && ! newDocs.exists())
            oldDocs.copyDirectoryTo (newDocs);
    }

    static void registerFileAssociationIfNeeded()
    {
       #if JUCE_WINDOWS
        auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        auto expected = exe.getFullPathName() + " \"%1\"";

        auto registered = juce::WindowsRegistry::getValue (
            "HKEY_CURRENT_USER\\Software\\Classes\\mikeDAW.Project\\shell\\open\\command\\");

        if (registered.equalsIgnoreCase (expected))
            return;

        const bool ok = juce::WindowsRegistry::registerFileAssociation (
            ".odaw", "mikeDAW.Project", "mikeDAW Project", exe,
            0, true);

        if (ok)
            SHChangeNotify (SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
       #endif
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, Colors::windowBg, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);

            auto area = juce::Desktop::getInstance().getDisplays()
                            .getPrimaryDisplay()->userArea;
            setResizeLimits (juce::jmin (1100, area.getWidth()  - 80),
                             juce::jmin (700,  area.getHeight() - 80), 10000, 10000);
            centreWithSize (juce::jmin (1600, area.getWidth()  - 60),
                            juce::jmin (900,  area.getHeight() - 60));
            setVisible (true);
            DarkTitleBar::apply (*this);
            DarkTitleBar::applyThemeColours (*this, Colors::header, Colors::text);
            setFullScreen (true);

            juce::Process::makeForegroundProcess();
            toFront (true);
            setAlwaysOnTop (true);
            juce::Timer::callAfterDelay (500,
                [safe = juce::Component::SafePointer<MainWindow> (this)]
                {
                    if (safe != nullptr)
                        safe->setAlwaysOnTop (false);
                });
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        void activeWindowStatusChanged() override
        {
            DocumentWindow::activeWindowStatusChanged();
            if (isActiveWindow())
                if (auto* main = dynamic_cast<MainComponent*> (getContentComponent()))
                    main->noteWindowActivated();
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<CrashHandler::HangWatchdog> watchdog;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<ShutdownVeil> veil;
};

START_JUCE_APPLICATION (mikeDAWApplication)
