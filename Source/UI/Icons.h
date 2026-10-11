#pragma once
#include <JuceHeader.h>
#include <array>

namespace Icons
{
    enum class Id
    {
        play, pause, stop, skipBack, loop, plus, minus, chevronDown, chevronUp, chevronRight,
        chevronLeft, close, moreVertical, search, panelLeft, panelRight, panelBottom, maximize,
        volume, exportIcon, cloud, users, cpu, activity, sliders, folder, music, waveform,
        piano, grid, mixer, rack, arrowsLeftRight, undo, redo, save, chat, metronome,
        numIcons
    };

    inline const char* svgFor (Id id)
    {
        switch (id)
        {
            case Id::play:            return "M6 3 L20 12 L6 21 Z";
            case Id::pause:           return "M6 4 H10 V20 H6 Z M14 4 H18 V20 H14 Z";
            case Id::stop:            return "M7 5 H17 A2 2 0 0 1 19 7 V17 A2 2 0 0 1 17 19 H7 A2 2 0 0 1 5 17 V7 A2 2 0 0 1 7 5 Z";
            case Id::skipBack:        return "M19 20 L9 12 L19 4 Z M5 19 V5";
            case Id::loop:            return "M17 2 L21 6 L17 10 M3 11 V10 A4 4 0 0 1 7 6 H21 M7 22 L3 18 L7 14 M21 13 V14 A4 4 0 0 1 17 18 H3";
            case Id::plus:            return "M5 12 H19 M12 5 V19";
            case Id::minus:           return "M5 12 H19";
            case Id::chevronDown:     return "M6 9 L12 15 L18 9";
            case Id::chevronUp:       return "M18 15 L12 9 L6 15";
            case Id::chevronRight:    return "M9 18 L15 12 L9 6";
            case Id::chevronLeft:     return "M15 18 L9 12 L15 6";
            case Id::close:           return "M18 6 L6 18 M6 6 L18 18";
            case Id::moreVertical:    return "M12 11 A1 1 0 1 1 12 13 A1 1 0 1 1 12 11 Z M12 4 A1 1 0 1 1 12 6 A1 1 0 1 1 12 4 Z M12 18 A1 1 0 1 1 12 20 A1 1 0 1 1 12 18 Z";
            case Id::search:          return "M11 3 A8 8 0 1 1 11 19 A8 8 0 1 1 11 3 Z M21 21 L16.7 16.7";
            case Id::panelLeft:       return "M5 3 H19 A2 2 0 0 1 21 5 V19 A2 2 0 0 1 19 21 H5 A2 2 0 0 1 3 19 V5 A2 2 0 0 1 5 3 Z M9 3 V21";
            case Id::panelRight:      return "M5 3 H19 A2 2 0 0 1 21 5 V19 A2 2 0 0 1 19 21 H5 A2 2 0 0 1 3 19 V5 A2 2 0 0 1 5 3 Z M15 3 V21";
            case Id::panelBottom:     return "M5 3 H19 A2 2 0 0 1 21 5 V19 A2 2 0 0 1 19 21 H5 A2 2 0 0 1 3 19 V5 A2 2 0 0 1 5 3 Z M3 15 H21";
            case Id::maximize:        return "M15 3 H21 V9 M9 21 H3 V15 M21 3 L14 10 M3 21 L10 14";
            case Id::volume:          return "M11 5 L6 9 H2 V15 H6 L11 19 Z M15.5 8.5 A5 5 0 0 1 15.5 15.5 M19 5 A10 10 0 0 1 19 19";
            case Id::exportIcon:      return "M21 15 V19 A2 2 0 0 1 19 21 H5 A2 2 0 0 1 3 19 V15 M17 8 L12 3 L7 8 M12 3 V15";
            case Id::cloud:           return "M17.5 19 H9 A7 7 0 1 1 15.7 10 H17.5 A4.5 4.5 0 1 1 17.5 19 Z";
            case Id::users:           return "M16 21 V19 A4 4 0 0 0 12 15 H6 A4 4 0 0 0 2 19 V21 M9 3 A4 4 0 1 1 9 11 A4 4 0 1 1 9 3 Z M22 21 V19 A4 4 0 0 0 19 15.1 M16 3.1 A4 4 0 0 1 16 10.9";
            case Id::cpu:             return "M6 4 H18 A2 2 0 0 1 20 6 V18 A2 2 0 0 1 18 20 H6 A2 2 0 0 1 4 18 V6 A2 2 0 0 1 6 4 Z M9 9 H15 V15 H9 Z M15 2 V4 M15 20 V22 M2 15 H4 M2 9 H4 M20 15 H22 M20 9 H22 M9 2 V4 M9 20 V22";
            case Id::activity:        return "M22 12 H18 L15 21 L9 3 L6 12 H2";
            case Id::sliders:         return "M21 4 H14 M10 4 H3 M21 12 H12 M8 12 H3 M21 20 H16 M12 20 H3 M14 2 V6 M8 10 V14 M16 18 V22";
            case Id::folder:          return "M20 20 A2 2 0 0 0 22 18 V8 A2 2 0 0 0 20 6 H12.1 A2 2 0 0 1 10.4 5.1 L9.6 3.9 A2 2 0 0 0 7.9 3 H4 A2 2 0 0 0 2 5 V18 A2 2 0 0 0 4 20 Z";
            case Id::music:           return "M9 18 V5 L21 3 V16 M6 15 A3 3 0 1 1 6 21 A3 3 0 1 1 6 15 Z M18 13 A3 3 0 1 1 18 19 A3 3 0 1 1 18 13 Z";
            case Id::waveform:        return "M2 13 A2 2 0 0 0 4 11 V7 A2 2 0 0 1 8 7 V20 A2 2 0 0 0 12 20 V4 A2 2 0 0 1 16 4 V17 A2 2 0 0 0 20 17 V13 A2 2 0 0 1 22 11";
            case Id::piano:           return "M4 4 H20 A2 2 0 0 1 22 6 V18 A2 2 0 0 1 20 20 H4 A2 2 0 0 1 2 18 V6 A2 2 0 0 1 4 4 Z M2 12 H22 M6 12 V16 M10 12 V16 M14 12 V16 M18 12 V16";
            case Id::grid:            return "M3 3 H10 V10 H3 Z M14 3 H21 V10 H14 Z M14 14 H21 V21 H14 Z M3 14 H10 V21 H3 Z";
            case Id::mixer:           return "M4 21 V14 M4 10 V3 M12 21 V12 M12 8 V3 M20 21 V16 M20 12 V3 M2 14 H6 M10 8 H14 M18 16 H22";
            case Id::rack:            return "M3 5 H21 M3 12 H21 M3 19 H21";
            case Id::arrowsLeftRight: return "M8 3 L4 7 L8 11 M4 7 H20 M16 21 L20 17 L16 13 M20 17 H4";
            case Id::undo:            return "M9 14 L4 9 L9 4 M4 9 H14.5 A5.5 5.5 0 0 1 14.5 20 H11";
            case Id::redo:            return "M15 14 L20 9 L15 4 M20 9 H9.5 A5.5 5.5 0 0 0 9.5 20 H13";
            case Id::save:            return "M15.2 3 A2 2 0 0 1 16.6 3.6 L20.4 7.4 A2 2 0 0 1 21 8.8 V19 A2 2 0 0 1 19 21 H5 A2 2 0 0 1 3 19 V5 A2 2 0 0 1 5 3 Z M17 21 V14 H7 V21 M7 3 V8 H15";
            case Id::chat:            return "M21 15 A2 2 0 0 1 19 17 H7 L3 21 V5 A2 2 0 0 1 5 3 H19 A2 2 0 0 1 21 5 Z";
            case Id::metronome:       return "M12 11.4 V9.1 M10.5 2 H13.5 L18.5 21 H5.5 Z M12 11.4 L17 5 M7.5 15 H16.5";
            case Id::numIcons:        break;
        }
        return "";
    }

    inline const juce::Path& get (Id id)
    {
        static const auto cache = []
        {
            std::array<juce::Path, (size_t) Id::numIcons> paths;
            for (size_t i = 0; i < paths.size(); ++i)
                paths[i] = juce::Drawable::parseSVGPath (svgFor ((Id) i));
            return paths;
        }();
        return cache[(size_t) id];
    }

    inline juce::Path fitted (Id id, juce::Rectangle<float> area)
    {
        auto side = juce::jmin (area.getWidth(), area.getHeight());
        auto box = juce::Rectangle<float> (side, side).withCentre (area.getCentre());
        auto p = get (id);
        p.applyTransform (juce::AffineTransform::scale (side / 24.0f).translated (box.getX(), box.getY()));
        return p;
    }

    inline void draw (juce::Graphics& g, Id id, juce::Rectangle<float> area,
                      juce::Colour colour, float strokePx = 1.6f)
    {
        g.setColour (colour);
        g.strokePath (fitted (id, area), juce::PathStrokeType (strokePx, juce::PathStrokeType::curved,
                                                                juce::PathStrokeType::rounded));
    }

    inline void fill (juce::Graphics& g, Id id, juce::Rectangle<float> area, juce::Colour colour)
    {
        g.setColour (colour);
        g.fillPath (fitted (id, area));
    }
}
