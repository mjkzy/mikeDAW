#pragma once
#include <JuceHeader.h>
#include "AppLookAndFeel.h"
#include <vector>
#include <map>

class ThemeManager : private juce::Timer,
                     public juce::ChangeBroadcaster
{
public:
    struct Slot
    {
        juce::String  key;
        juce::String  displayName;
        juce::String  group;
        juce::Colour* slot;
        juce::Colour  defaultValue;
    };

    struct FloatSlot
    {
        juce::String key;
        juce::String displayName;
        float*       slot;
        float        defaultValue;
        float        minValue;
        float        maxValue;
    };

    struct Adjustments
    {
        float hue        = 0.0f;
        float saturation = 1.0f;
        float brightness = 1.0f;
        float contrast   = 1.0f;
        float text       = 1.0f;
    };

    explicit ThemeManager (juce::PropertiesFile& settings);
    ~ThemeManager() override;

    std::function<void()> onThemeChanged;

    static const std::vector<Slot>& registry();

    static const std::vector<FloatSlot>& effectRegistry();

    void apply();

    void setColour (const juce::String& key, juce::Colour c);
    void setEffectValue (const juce::String& key, float v);
    void setAdjustments (const Adjustments& a);
    void resetToDefault();
    void setName (const juce::String& n);
    void applyPresetVar (const juce::var& preset);

    bool isGlowEnabled() const { return Fx::glowEnabled; }
    void setGlowEnabled (bool on);

    Adjustments  getAdjustments()   const { return adj; }
    juce::String getName()          const { return themeName; }
    juce::Colour getBaseColour (const juce::String& key) const;
    float        getEffectValue (const juce::String& key) const;

    juce::var toVar() const;
    void      fromVar (const juce::var& v, bool doApply);

    bool exportTheme (const juce::File& dest) const;
    bool importTheme (const juce::File& src);
    juce::File getPresetDirectory() const;
    void save();
    void load();

    static std::vector<std::pair<juce::String, juce::var>> builtInPresets();

private:
    void timerCallback() override;
    void scheduleSave();
    juce::File themeFile() const;
    static juce::Colour parseColour (const juce::String& s, juce::Colour fallback);
    juce::Colour effectiveFor (const Slot& s) const;

    juce::PropertiesFile& settings;
    juce::String themeName { "Custom" };
    std::map<juce::String, juce::Colour> baseColours;
    std::map<juce::String, float> effectValues;
    Adjustments adj;

    juce::int64 expectedMtime = 0;
    int  saveCountdown = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThemeManager)
};
