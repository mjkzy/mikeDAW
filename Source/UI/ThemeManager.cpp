#include "ThemeManager.h"
#include <set>

const std::vector<ThemeManager::Slot>& ThemeManager::registry()
{
    static const std::vector<Slot> table = []
    {
        std::vector<Slot> t;
        auto add = [&t] (const char* key, const char* name, const char* group,
                         juce::Colour* slot)
        {
            t.push_back ({ key, name, group, slot, *slot });
        };

        add ("windowBg",        "Window Background",   "Window",   &Colors::windowBg);

        add ("panelBg",         "Panel",               "Panels",   &Colors::panelBg);
        add ("panelDark",       "Panel (dark)",        "Panels",   &Colors::panelDark);
        add ("knobBg",          "Knob Well",           "Panels",   &Colors::knobBg);
        add ("raised",          "Raised Control",      "Panels",   &Colors::raised);
        add ("header",          "Header",              "Panels",   &Colors::header);
        add ("card",            "Card Surface",        "Panels",   &Colors::card);
        add ("outline",         "Outline",             "Panels",   &Colors::outline);
        add ("popupBg",         "Popup / Row Stripe",  "Panels",   &Colors::popupBg);

        add ("text",            "Text",                "Text",     &Colors::text);
        add ("textDim",         "Text (dim)",          "Text",     &Colors::textDim);

        add ("accent",          "Accent",              "Accents",  &Colors::accent);
        add ("accent2",         "Accent 2",            "Accents",  &Colors::accent2);

        add ("stepDim",         "Step (dim)",          "Steps & Grid", &Colors::stepDim);
        add ("stepDim2",        "Step (dim alt)",      "Steps & Grid", &Colors::stepDim2);
        add ("gridLine",        "Grid Line",           "Steps & Grid", &Colors::gridLine);
        add ("gridBar",         "Grid Bar Line",       "Steps & Grid", &Colors::gridBar);
        add ("stepOn",          "Step On (LED)",       "Steps & Grid", &Colors::stepOn);
        add ("stepOff",         "Step Off (LED)",      "Steps & Grid", &Colors::stepOff);

        add ("noteGreen",       "Note",                "Piano Roll", &Colors::noteGreen);
        add ("scaleRow",        "In-Scale Row",        "Piano Roll", &Colors::scaleRow);
        add ("ghostNote",       "Ghost Note",          "Piano Roll", &Colors::ghostNote);
        add ("pianoWhiteKey",   "White Key",           "Piano Roll", &Colors::pianoWhiteKey);
        add ("pianoBlackKey",   "Black Key",           "Piano Roll", &Colors::pianoBlackKey);
        add ("pianoKeyText",    "Key Label",           "Piano Roll", &Colors::pianoKeyText);
        add ("pianoRowWhite",   "White-Key Row",       "Piano Roll", &Colors::pianoRowWhite);
        add ("pianoRowBlack",   "Black-Key Row",       "Piano Roll", &Colors::pianoRowBlack);
        add ("pianoRowFlat",    "Flat Row",            "Piano Roll", &Colors::pianoRowFlat);
        add ("lassoTint",       "Lasso Tint",          "Piano Roll", &Colors::lassoTint);
        add ("dangerTint",      "Out-of-Range Tint",   "Piano Roll", &Colors::dangerTint);

        add ("meterMid",        "Meter (mid)",         "Mixer & Meters", &Colors::meterMid);
        add ("meterHot",        "Meter (hot)",         "Mixer & Meters", &Colors::meterHot);
        add ("stereoMerged",    "Stereo Merged",       "Mixer & Meters", &Colors::stereoMerged);
        add ("stereoSeparated", "Stereo Separated",    "Mixer & Meters", &Colors::stereoSeparated);
        add ("toolSelect",      "Select Tool",         "Mixer & Meters", &Colors::toolSelect);
        add ("toolTrim",        "Slice / Trim Tool",   "Mixer & Meters", &Colors::toolTrim);

        add ("danger",          "Danger / Error",      "Status",   &Colors::danger);
        add ("record",          "Record",              "Status",   &Colors::record);
        add ("success",         "Success",             "Status",   &Colors::success);
        add ("warning",         "Warning",             "Status",   &Colors::warning);

        add ("clipDefault",     "Default Clip",        "Controls & Misc", &Colors::clipDefault);
        add ("automationClip",  "Automation Clip",     "Controls & Misc", &Colors::automationClip);
        add ("refreshGreen",    "Refresh Button",      "Controls & Misc", &Colors::refreshGreen);
        add ("scrollThumb",     "Scrollbar Thumb",     "Controls & Misc", &Colors::scrollThumb);
        add ("scrollThumbHover","Scrollbar Thumb Hover","Controls & Misc",&Colors::scrollThumbHover);
        add ("eqGraphBg",       "EQ Graph",            "Controls & Misc", &Colors::eqGraphBg);
        add ("eqGraphGrid",     "EQ Graph Grid",       "Controls & Misc", &Colors::eqGraphGrid);
        add ("eqPanelBg",       "EQ Panel",            "Controls & Misc", &Colors::eqPanelBg);
        add ("eqCurve",         "EQ Curve",            "Controls & Misc", &Colors::eqCurve);
        add ("eqHeat",          "EQ Heatmap",          "Controls & Misc", &Colors::eqHeat);
        add ("eqHeatHot",       "EQ Heatmap Peak",     "Controls & Misc", &Colors::eqHeatHot);
        add ("gbPanel",         "Nasty Beat Panel",    "Controls & Misc", &Colors::gbPanel);
        add ("gbEnvBg",         "Nasty Beat Envelope", "Controls & Misc", &Colors::gbEnvBg);
        add ("gbGrid",          "Nasty Beat Grid",     "Controls & Misc", &Colors::gbGrid);
        add ("gbTime",          "Nasty Beat Time",     "Controls & Misc", &Colors::gbTime);
        add ("gbVolume",        "Nasty Beat Volume",   "Controls & Misc", &Colors::gbVolume);

        return t;
    }();
    return table;
}

const std::vector<ThemeManager::FloatSlot>& ThemeManager::effectRegistry()
{
    static const std::vector<FloatSlot> table = {
        { "pianoGlowIntensity",    "Piano roll glow",         &Fx::pianoGlowIntensity,    1.0f, 0.0f, 2.0f },
        { "pianoGlowOpacity",      "Piano roll glow opacity", &Fx::pianoGlowOpacity,      1.0f, 0.0f, 2.0f },
        { "playlistGlowIntensity", "Playlist playhead glow",  &Fx::playlistGlowIntensity, 1.0f, 0.0f, 2.0f },
    };
    return table;
}

ThemeManager::ThemeManager (juce::PropertiesFile& s) : settings (s)
{
    for (auto& slot : registry())
        baseColours[slot.key] = slot.defaultValue;
    for (auto& fx : effectRegistry())
        effectValues[fx.key] = fx.defaultValue;

    Fx::glowEnabled = settings.getBoolValue ("fxGlowEnabled", true);

    load();
    if (! settings.getBoolValue ("studioThemeApplied", false))
    {
        settings.setValue ("studioThemeApplied", true);
        resetToDefault();
    }
    startTimer (500);
}

ThemeManager::~ThemeManager()
{
    stopTimer();
    if (saveCountdown >= 0)
        save();
}

juce::Colour ThemeManager::effectiveFor (const Slot& s) const
{
    auto it = baseColours.find (s.key);
    juce::Colour base = it != baseColours.end() ? it->second : s.defaultValue;

    float h = base.getHue();
    float sat = base.getSaturation();
    float b = base.getBrightness();
    float a = base.getFloatAlpha();

    h = std::fmod (h + adj.hue / 360.0f + 1.0f, 1.0f);
    sat = juce::jlimit (0.0f, 1.0f, sat * adj.saturation);
    b   = juce::jlimit (0.0f, 1.0f, b * adj.brightness);
    b   = juce::jlimit (0.0f, 1.0f, 0.5f + (b - 0.5f) * adj.contrast);

    if (s.group == "Text")
        b = juce::jlimit (0.0f, 1.0f, b * adj.text);

    return juce::Colour::fromHSV (h, sat, b, a);
}

void ThemeManager::apply()
{
    for (auto& slot : registry())
        *slot.slot = effectiveFor (slot);

    for (auto& fx : effectRegistry())
    {
        auto it = effectValues.find (fx.key);
        *fx.slot = it != effectValues.end() ? it->second : fx.defaultValue;
    }

    if (onThemeChanged)
        onThemeChanged();

    sendChangeMessage();
}

juce::Colour ThemeManager::getBaseColour (const juce::String& key) const
{
    auto it = baseColours.find (key);
    return it != baseColours.end() ? it->second : juce::Colours::black;
}

void ThemeManager::setColour (const juce::String& key, juce::Colour c)
{
    baseColours[key] = c;
    themeName = "Custom";
    apply();
    scheduleSave();
}

float ThemeManager::getEffectValue (const juce::String& key) const
{
    auto it = effectValues.find (key);
    if (it != effectValues.end())
        return it->second;
    for (auto& fx : effectRegistry())
        if (fx.key == key)
            return fx.defaultValue;
    return 1.0f;
}

void ThemeManager::setEffectValue (const juce::String& key, float v)
{
    for (auto& fx : effectRegistry())
        if (fx.key == key)
        {
            effectValues[key] = juce::jlimit (fx.minValue, fx.maxValue, v);
            themeName = "Custom";
            apply();
            scheduleSave();
            return;
        }
}

void ThemeManager::setGlowEnabled (bool on)
{
    Fx::glowEnabled = on;
    settings.setValue ("fxGlowEnabled", on);
    settings.saveIfNeeded();
    apply();
}

void ThemeManager::setAdjustments (const Adjustments& a)
{
    adj = a;
    apply();
    scheduleSave();
}

void ThemeManager::setName (const juce::String& n)
{
    themeName = n;
    scheduleSave();
}

void ThemeManager::resetToDefault()
{
    for (auto& slot : registry())
        baseColours[slot.key] = slot.defaultValue;
    for (auto& fx : effectRegistry())
        effectValues[fx.key] = fx.defaultValue;
    adj = {};
    themeName = "Studio";
    apply();
    scheduleSave();
}

void ThemeManager::applyPresetVar (const juce::var& preset)
{
    fromVar (preset, true);
    scheduleSave();
}

juce::var ThemeManager::toVar() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("name", themeName);

    auto* aj = new juce::DynamicObject();
    aj->setProperty ("hue",        adj.hue);
    aj->setProperty ("saturation", adj.saturation);
    aj->setProperty ("brightness", adj.brightness);
    aj->setProperty ("contrast",   adj.contrast);
    aj->setProperty ("text",       adj.text);
    root->setProperty ("adjustments", juce::var (aj));

    auto* cols = new juce::DynamicObject();
    for (auto& slot : registry())
    {
        auto it = baseColours.find (slot.key);
        auto c = it != baseColours.end() ? it->second : slot.defaultValue;
        cols->setProperty (slot.key, "#" + c.toDisplayString (true));
    }
    root->setProperty ("colours", juce::var (cols));

    auto* fx = new juce::DynamicObject();
    for (auto& s : effectRegistry())
    {
        auto it = effectValues.find (s.key);
        fx->setProperty (s.key, it != effectValues.end() ? it->second : s.defaultValue);
    }
    root->setProperty ("effects", juce::var (fx));

    return juce::var (root);
}

juce::Colour ThemeManager::parseColour (const juce::String& s, juce::Colour fallback)
{
    auto hex = s.trim();
    if (hex.startsWithChar ('#'))
        hex = hex.substring (1);
    if (hex.isEmpty())
        return fallback;
    return juce::Colour ((juce::uint32) hex.getHexValue64());
}

void ThemeManager::fromVar (const juce::var& v, bool doApply)
{
    if (! v.isObject())
        return;

    themeName = v.getProperty ("name", themeName).toString();

    if (auto adjV = v.getProperty ("adjustments", {}); adjV.isObject())
    {
        adj.hue        = (float) (double) adjV.getProperty ("hue",        adj.hue);
        adj.saturation = (float) (double) adjV.getProperty ("saturation", adj.saturation);
        adj.brightness = (float) (double) adjV.getProperty ("brightness", adj.brightness);
        adj.contrast   = (float) (double) adjV.getProperty ("contrast",   adj.contrast);
        adj.text       = (float) (double) adjV.getProperty ("text",       adj.text);
    }

    auto colsV = v.getProperty ("colours", {});
    auto* cols = colsV.getDynamicObject();
    for (auto& slot : registry())
    {
        juce::Colour value = slot.defaultValue;
        if (cols != nullptr && cols->hasProperty (slot.key))
            value = parseColour (cols->getProperty (slot.key).toString(), slot.defaultValue);
        baseColours[slot.key] = value;
    }

    auto fxV = v.getProperty ("effects", {});
    auto* fx = fxV.getDynamicObject();
    for (auto& s : effectRegistry())
    {
        float value = s.defaultValue;
        if (fx != nullptr && fx->hasProperty (s.key))
            value = juce::jlimit (s.minValue, s.maxValue,
                                  (float) (double) fx->getProperty (s.key));
        effectValues[s.key] = value;
    }

    if (doApply)
        apply();
}

juce::File ThemeManager::themeFile() const
{
    return settings.getFile().getParentDirectory().getChildFile ("theme.json");
}

juce::File ThemeManager::getPresetDirectory() const
{
    auto dir = settings.getFile().getParentDirectory().getChildFile ("Themes");
    dir.createDirectory();
    return dir;
}

void ThemeManager::save()
{
    saveCountdown = -1;
    auto f = themeFile();
    f.replaceWithText (juce::JSON::toString (toVar(), true));
    expectedMtime = f.getLastModificationTime().toMilliseconds();
}

void ThemeManager::load()
{
    auto f = themeFile();
    if (! f.existsAsFile())
        return;
    auto v = juce::JSON::parse (f);
    if (v.isObject())
        fromVar (v, true);
    expectedMtime = f.getLastModificationTime().toMilliseconds();
}

void ThemeManager::scheduleSave()
{
    saveCountdown = 1;
}

bool ThemeManager::exportTheme (const juce::File& dest) const
{
    return dest.replaceWithText (juce::JSON::toString (toVar(), true));
}

bool ThemeManager::importTheme (const juce::File& src)
{
    if (! src.existsAsFile())
        return false;
    auto v = juce::JSON::parse (src);
    if (! v.isObject())
        return false;
    fromVar (v, true);
    scheduleSave();
    return true;
}

void ThemeManager::timerCallback()
{
    if (saveCountdown > 0)
    {
        if (--saveCountdown == 0)
            save();
    }

    auto f = themeFile();
    if (f.existsAsFile())
    {
        auto mt = f.getLastModificationTime().toMilliseconds();
        if (mt != expectedMtime && saveCountdown <= 0)
        {
            auto v = juce::JSON::parse (f);
            if (v.isObject())
                fromVar (v, true);
            expectedMtime = mt;
        }
    }
}

std::vector<std::pair<juce::String, juce::var>> ThemeManager::builtInPresets()
{
    auto makeTheme = [] (const juce::String& name,
                         const std::map<juce::String, juce::uint32>& overrides,
                         ThemeManager::Adjustments a = {})
    {
        auto* root = new juce::DynamicObject();
        root->setProperty ("name", name);

        auto* aj = new juce::DynamicObject();
        aj->setProperty ("hue",        a.hue);
        aj->setProperty ("saturation", a.saturation);
        aj->setProperty ("brightness", a.brightness);
        aj->setProperty ("contrast",   a.contrast);
        aj->setProperty ("text",       a.text);
        root->setProperty ("adjustments", juce::var (aj));

        auto* cols = new juce::DynamicObject();
        for (auto& kv : overrides)
            cols->setProperty (kv.first, "#" + juce::Colour (kv.second).toDisplayString (true));
        root->setProperty ("colours", juce::var (cols));

        return juce::var (root);
    };

    static const std::set<juce::String> chassisKeys = {
        "windowBg", "panelBg", "panelDark", "knobBg", "raised", "header", "card", "outline", "popupBg",
        "stepDim", "stepDim2", "gridLine", "gridBar", "stepOff", "scaleRow", "ghostNote",
        "pianoWhiteKey", "pianoBlackKey", "pianoKeyText", "pianoRowWhite", "pianoRowBlack",
        "pianoRowFlat", "scrollThumb", "scrollThumbHover"
    };

    auto chassisTheme = [&makeTheme] (const juce::String& name, juce::Colour tint, float sat,
                                      std::map<juce::String, juce::uint32> overrides,
                                      ThemeManager::Adjustments a = {})
    {
        std::map<juce::String, juce::uint32> cols;
        for (auto& slot : ThemeManager::registry())
        {
            if (chassisKeys.count (slot.key) == 0)
                continue;
            auto def = slot.defaultValue;
            auto c = juce::Colour::fromHSV (tint.getHue(), sat, def.getBrightness(),
                                            def.getFloatAlpha());
            cols[slot.key] = c.getARGB();
        }
        for (auto& kv : overrides)
            cols[kv.first] = kv.second;
        return makeTheme (name, cols, a);
    };

    std::vector<std::pair<juce::String, juce::var>> presets;

    presets.emplace_back ("Studio", makeTheme ("Studio", {}));

    presets.emplace_back ("Nocturne", makeTheme ("Nocturne", {
        { "windowBg",         0xff0d0b12 },
        { "panelBg",          0xff16131e },
        { "panelDark",        0xff110f18 },
        { "knobBg",           0xff0a0812 },
        { "raised",           0xff262138 },
        { "header",           0xff1a1724 },
        { "card",             0xff1d1928 },
        { "outline",          0xff0a0810 },
        { "popupBg",          0xff191521 },
        { "text",             0xffe4dff0 },
        { "textDim",          0xff8d85a3 },
        { "accent",           0xff8b5cf6 },
        { "accent2",          0xffec4899 },
        { "stepDim",          0xff2a2438 },
        { "stepDim2",         0xff241f31 },
        { "gridLine",         0xff231e31 },
        { "gridBar",          0xff3a3452 },
        { "stepOn",           0xffa78bfa },
        { "stepOff",          0xff2e2840 },
        { "noteGreen",        0xffa78bfa },
        { "scaleRow",         0xff201b2e },
        { "ghostNote",        0xff6b6382 },
        { "pianoWhiteKey",    0xffd9d4e6 },
        { "pianoBlackKey",    0xff1a1724 },
        { "pianoKeyText",     0xff272138 },
        { "pianoRowWhite",    0xff171322 },
        { "pianoRowBlack",    0xff100d17 },
        { "pianoRowFlat",     0xff141020 },
        { "lassoTint",        0x2e8b5cf6 },
        { "dangerTint",       0x55ff3050 },
        { "meterMid",         0xffc084fc },
        { "meterHot",         0xffef4458 },
        { "stereoMerged",     0xffc084fc },
        { "stereoSeparated",  0xff60a5fa },
        { "toolSelect",       0xfff472b6 },
        { "toolTrim",         0xff818cf8 },
        { "danger",           0xffef4458 },
        { "record",           0xfff43f5e },
        { "success",          0xff34d399 },
        { "warning",          0xfffbbf24 },
        { "clipDefault",      0xff6d5bd0 },
        { "automationClip",   0xff2dd4bf },
        { "refreshGreen",     0xff7c5cf6 },
        { "scrollThumb",      0xff342c4c },
        { "eqGraphBg",        0xff20203a },
        { "eqGraphGrid",      0xff30304e },
        { "eqPanelBg",        0xff221f33 },
        { "eqCurve",          0xfff4f0ff },
        { "eqHeat",           0xffb03a8a },
        { "eqHeatHot",        0xffffa8e6 },
        { "gbPanel",          0xff262233 },
        { "gbEnvBg",          0xff17141f },
        { "gbGrid",           0xff352f45 },
        { "gbTime",           0xff9be07a },
        { "gbVolume",         0xfff0b060 },
        { "scrollThumbHover", 0xff484060 },
    }));

    presets.emplace_back ("Classic", makeTheme ("Classic", {
        { "windowBg",         0xff1b1f23 },
        { "panelBg",          0xff2a3036 },
        { "panelDark",        0xff21262b },
        { "knobBg",           0xff151a1e },
        { "raised",           0xff394047 },
        { "header",           0xff30373e },
        { "card",             0xff30373e },
        { "outline",          0xff14171a },
        { "popupBg",          0xff262b30 },
        { "text",             0xffc9d1d5 },
        { "textDim",          0xff7f8a91 },
        { "accent",           0xff62d0bd },
        { "accent2",          0xffef9c4d },
        { "stepDim",          0xff444d57 },
        { "stepDim2",         0xff3a424b },
        { "gridLine",         0xff3e454d },
        { "gridBar",          0xff59626b },
        { "stepOn",           0xff8fce4a },
        { "stepOff",          0xff4a5158 },
        { "noteGreen",        0xff7de08a },
        { "scaleRow",         0xff3c444d },
        { "ghostNote",        0xff8a929a },
        { "pianoWhiteKey",    0xffd7dde0 },
        { "pianoBlackKey",    0xff22262a },
        { "pianoKeyText",     0xff30373e },
        { "pianoRowWhite",    0xff23282d },
        { "pianoRowBlack",    0xff181c20 },
        { "pianoRowFlat",     0xff2b3138 },
        { "lassoTint",        0x22ff2020 },
        { "dangerTint",       0x55ff3030 },
        { "meterMid",         0xffcbe04a },
        { "meterHot",         0xffe0483e },
        { "stereoMerged",     0xffb79ce8 },
        { "stereoSeparated",  0xff5aa7e8 },
        { "toolSelect",       0xffef5350 },
        { "toolTrim",         0xff4da3ff },
        { "danger",           0xffd05555 },
        { "record",           0xffd05050 },
        { "success",          0xff5bbf5b },
        { "warning",          0xffe0a53a },
        { "clipDefault",      0xff5a7a9a },
        { "automationClip",   0xff7ec8a0 },
        { "refreshGreen",     0xff3a7a4a },
        { "scrollThumb",      0xff586069 },
        { "eqGraphBg",        0xff1f2a33 },
        { "eqGraphGrid",      0xff2f3d49 },
        { "eqPanelBg",        0xff242c35 },
        { "eqCurve",          0xfff2f5f8 },
        { "eqHeat",           0xffc8324e },
        { "eqHeatHot",        0xffff9aa8 },
        { "gbPanel",          0xff2a2d30 },
        { "gbEnvBg",          0xff1b1e21 },
        { "gbGrid",           0xff33383d },
        { "gbTime",           0xff7ccf4a },
        { "gbVolume",         0xffe8a33a },
        { "scrollThumbHover", 0xff6a737d },
    }));

    presets.emplace_back ("Ember", chassisTheme ("Ember",
        juce::Colour (0xffe97d94), 0.27f, {
        { "accent",     0xffe97d94 },
        { "accent2",    0xffef9c4d },
        { "noteGreen",  0xffe97d94 },
        { "stepOn",     0xff8fce4a },
        { "toolSelect", 0xffef5350 },
        { "toolTrim",   0xffe89a6a },
    }));

    presets.emplace_back ("Dark Orange", chassisTheme ("Dark Orange",
        juce::Colour (0xffe8863a), 0.28f, {
        { "accent",     0xffe8863a },
        { "accent2",    0xff5bbfae },
        { "toolTrim",   0xffe8a24a },
        { "noteGreen",  0xfff0b45a },
    }));

    presets.emplace_back ("Cherry", chassisTheme ("Cherry",
        juce::Colour (0xffd9506a), 0.30f, {
        { "accent",     0xffd9506a },
        { "accent2",    0xffef8a4d },
        { "toolTrim",   0xffe0788c },
        { "noteGreen",  0xffe97d94 },
    }));

    presets.emplace_back ("Light", makeTheme ("Light", {
        { "windowBg",       0xffe8eaed },
        { "panelBg",        0xfff2f4f6 },
        { "panelDark",      0xffe0e3e6 },
        { "raised",         0xffd2d7db },
        { "header",         0xffc8ced3 },
        { "card",           0xffe9ecef },
        { "outline",        0xffb0b6bc },
        { "popupBg",        0xffeef0f2 },
        { "text",           0xff2a3036 },
        { "textDim",        0xff5a636b },
        { "gridLine",       0xffc4cace },
        { "gridBar",        0xffa8b0b6 },
        { "stepDim",        0xffc6ccd1 },
        { "stepDim2",       0xffced3d8 },
        { "stepOff",        0xffbfc5cb },
        { "scaleRow",       0xffdcdfe3 },
        { "ghostNote",      0xff9098a0 },
        { "scrollThumb",    0xffb2b8be },
        { "scrollThumbHover",0xff9aa2aa },
        { "pianoWhiteKey",  0xfff4f6f8 },
        { "pianoBlackKey",  0xffcbd0d4 },
        { "pianoKeyText",   0xff2a3036 },
        { "pianoRowWhite",  0xffe6e9ec },
        { "pianoRowBlack",  0xffd8dce0 },
        { "pianoRowFlat",   0xffe1e4e8 },
    }));

    return presets;
}
