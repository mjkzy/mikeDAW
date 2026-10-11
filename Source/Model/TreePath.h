#pragma once
#include <JuceHeader.h>
#include "ProjectModel.h"

namespace Model
{
    inline juce::Identifier identityKeyFor (const juce::Identifier& type)
    {
        if (type == IDs::Channel || type == IDs::Pattern || type == IDs::AudioFile || type == IDs::MidiFile)
            return IDs::id;
        if (type == IDs::ChannelSeq)
            return IDs::channelId;
        if (type == IDs::Note || type == IDs::Point || type == IDs::Clip || type == IDs::Slot)
            return IDs::uid;
        if (type == IDs::Track || type == IDs::PlaylistTrack)
            return IDs::index;
        return {};
    }

    inline bool isSingletonContainer (const juce::Identifier& type)
    {
        return type == IDs::Channels || type == IDs::Patterns || type == IDs::Playlist
            || type == IDs::AudioFiles || type == IDs::MidiFiles || type == IDs::Mixer;
    }

    inline bool hasMintableUid (const juce::Identifier& type)
    {
        return type == IDs::Note || type == IDs::Point || type == IDs::Clip;
    }

    inline juce::String childIdentity (const juce::ValueTree& n)
    {
        auto key = identityKeyFor (n.getType());
        if (key.isValid() && n.hasProperty (key))
            return n.getType().toString() + ":" + n.getProperty (key).toString();
        if (isSingletonContainer (n.getType()))
            return n.getType().toString();
        return {};
    }

    inline bool uidClashesWithSibling (const juce::ValueTree& node, const juce::ValueTree& parent)
    {
        if (! parent.isValid() || ! node.hasProperty (IDs::uid))
            return false;
        auto uid = node.getProperty (IDs::uid);
        for (int i = 0; i < parent.getNumChildren(); ++i)
        {
            auto sib = parent.getChild (i);
            if (sib != node && sib.hasType (node.getType()) && sib.getProperty (IDs::uid) == uid)
                return true;
        }
        return false;
    }

    inline bool ensureIdentities (juce::ValueTree node, juce::UndoManager* undo = nullptr)
    {
        bool changed = false;
        if (hasMintableUid (node.getType())
            && (! node.hasProperty (IDs::uid) || uidClashesWithSibling (node, node.getParent())))
        {
            node.setProperty (IDs::uid, newId(), undo);
            changed = true;
        }
        for (int i = 0; i < node.getNumChildren(); ++i)
            changed = ensureIdentities (node.getChild (i), undo) || changed;
        return changed;
    }

    struct TreePath
    {
        enum class Kind : juce::uint8 { byType = 0, byIdentity = 1, byIndex = 2 };

        struct Step
        {
            juce::Identifier type;
            Kind kind = Kind::byIndex;
            juce::var identity;
            int index = 0;
        };

        std::vector<Step> steps;

        static Step stepFor (const juce::ValueTree& node, const juce::ValueTree& parent)
        {
            Step s;
            s.type = node.getType();
            auto key = identityKeyFor (s.type);
            if (key.isValid() && node.hasProperty (key))
            {
                s.kind = Kind::byIdentity;
                s.identity = node.getProperty (key);
            }
            else if (isSingletonContainer (s.type))
            {
                s.kind = Kind::byType;
            }
            else
            {
                s.kind = Kind::byIndex;
                s.index = parent.isValid() ? parent.indexOf (node) : 0;
            }
            return s;
        }

        static bool of (juce::ValueTree node, const juce::ValueTree& root, TreePath& out)
        {
            out.steps.clear();
            while (node.isValid() && node != root)
            {
                auto parent = node.getParent();
                if (! parent.isValid())
                    return false;
                out.steps.insert (out.steps.begin(), stepFor (node, parent));
                node = parent;
            }
            return node.isValid();
        }

        static juce::ValueTree resolveStep (const juce::ValueTree& parent, const Step& s)
        {
            if (! parent.isValid())
                return {};
            switch (s.kind)
            {
                case Kind::byType:
                    return parent.getChildWithName (s.type);
                case Kind::byIdentity:
                {
                    auto key = identityKeyFor (s.type);
                    for (int i = 0; i < parent.getNumChildren(); ++i)
                    {
                        auto c = parent.getChild (i);
                        if (c.hasType (s.type) && c.getProperty (key) == s.identity)
                            return c;
                    }
                    return {};
                }
                case Kind::byIndex:
                {
                    auto c = parent.getChild (s.index);
                    return c.hasType (s.type) ? c : juce::ValueTree();
                }
            }
            return {};
        }

        static constexpr int maxBacktrackCandidates = 8;

        juce::ValueTree resolve (const juce::ValueTree& root) const
        {
            return resolveFrom (root, 0);
        }

        juce::ValueTree resolveFrom (const juce::ValueTree& parent, size_t depth) const
        {
            if (depth >= steps.size())
                return parent;
            if (! parent.isValid())
                return {};

            const auto& s = steps[depth];
            if (s.kind != Kind::byIdentity || s.identity.isVoid())
                return resolveFrom (resolveStep (parent, s), depth + 1);

            auto key = identityKeyFor (s.type);
            int tried = 0;
            for (int i = 0; i < parent.getNumChildren() && tried < maxBacktrackCandidates; ++i)
            {
                auto c = parent.getChild (i);
                if (! c.hasType (s.type) || c.getProperty (key) != s.identity)
                    continue;
                ++tried;
                auto found = resolveFrom (c, depth + 1);
                if (found.isValid())
                    return found;
            }
            return {};
        }

        int resolvedDepth (const juce::ValueTree& root) const
        {
            juce::ValueTree node = root;
            int depth = 0;
            for (auto& s : steps)
            {
                auto next = resolveStep (node, s);
                if (! next.isValid())
                    break;
                node = next;
                ++depth;
            }
            return depth;
        }

        void write (juce::OutputStream& out) const
        {
            out.writeCompressedInt ((int) steps.size());
            for (auto& s : steps)
                writeStep (out, s);
        }

        static void writeStep (juce::OutputStream& out, const Step& s)
        {
            out.writeString (s.type.toString());
            out.writeByte ((char) s.kind);
            if (s.kind == Kind::byIdentity)      s.identity.writeToStream (out);
            else if (s.kind == Kind::byIndex)    out.writeCompressedInt (s.index);
        }

        static bool readStep (juce::InputStream& in, Step& s)
        {
            auto typeName = in.readString();
            if (typeName.isEmpty())
                return false;
            s.type = juce::Identifier (typeName);
            auto kind = (juce::uint8) in.readByte();
            if (kind > (juce::uint8) Kind::byIndex)
                return false;
            s.kind = (Kind) kind;
            if (s.kind == Kind::byIdentity)      s.identity = juce::var::readFromStream (in);
            else if (s.kind == Kind::byIndex)    s.index = in.readCompressedInt();
            return true;
        }

        bool read (juce::InputStream& in)
        {
            steps.clear();
            int n = in.readCompressedInt();
            if (n < 0 || n > 64)
                return false;
            steps.resize ((size_t) n);
            for (auto& s : steps)
                if (! readStep (in, s))
                    return false;
            return true;
        }

        juce::String toString() const
        {
            juce::String s;
            for (auto& st : steps)
            {
                s << "/" << st.type.toString();
                if (st.kind == Kind::byIdentity)   s << "=" << st.identity.toString();
                else if (st.kind == Kind::byIndex) s << "[" << st.index << "]";
            }
            return s.isEmpty() ? "/" : s;
        }
    };
}
