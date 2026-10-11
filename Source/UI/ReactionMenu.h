#pragma once
#include <JuceHeader.h>
#include <functional>
#include <vector>

namespace ReactionMenu
{
    struct Entry { const char* label; const char* emoji; };

    inline const std::vector<Entry>& entries()
    {
        static const std::vector<Entry> e = {
            { "Fire", "\xf0\x9f\x94\xa5" },
            { "Nice", "\xf0\x9f\x91\x8d" },
            { "Love", "\xe2\x9d\xa4" },
            { "Haha", "\xf0\x9f\x98\x82" },
            { "Huh?", "\xe2\x9d\x93" },
        };
        return e;
    }

    inline void show (juce::PopupMenu::Options options,
                      std::function<void (const juce::String& emoji)> onPick)
    {
        juce::PopupMenu m;
        const auto& es = entries();
        for (int i = 0; i < (int) es.size(); ++i)
            m.addItem (i + 1, juce::String (juce::CharPointer_UTF8 (es[(size_t) i].emoji))
                                  + "   " + es[(size_t) i].label);

        m.showMenuAsync (options,
            [pick = std::move (onPick)] (int result)
            {
                const auto& list = entries();
                if (result >= 1 && result <= (int) list.size() && pick)
                    pick (juce::String (juce::CharPointer_UTF8 (list[(size_t) (result - 1)].emoji)));
            });
    }
}
