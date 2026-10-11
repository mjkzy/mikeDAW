#include "TreeSync.h"
#include "SyncDebug.h"

using Model::TreePath;

TreeSync::TreeSync (const juce::ValueTree& rootTree) : root (rootTree)
{
    root.addListener (this);
}

TreeSync::~TreeSync()
{
    root.removeListener (this);
}

const char* TreeSync::opName (Op op)
{
    switch (op)
    {
        case Op::setProperty:    return "setProperty";
        case Op::removeProperty: return "removeProperty";
        case Op::addChild:       return "addChild";
        case Op::removeChild:    return "removeChild";
        case Op::moveChild:      return "moveChild";
    }
    return "?";
}

bool TreeSync::beginDelta (juce::MemoryOutputStream& out, Op op, const juce::ValueTree& node) const
{
    TreePath path;
    if (! TreePath::of (node, root, path))
        return false;
    if (SyncDebug::enabled())
        synclog (juce::String ("delta SEND   ") + opName (op) + " " + path.toString());
    out.writeByte ((char) formatTag);
    out.writeByte ((char) op);
    path.write (out);
    return true;
}

void TreeSync::send (const juce::MemoryOutputStream& out)
{
    stateChanged (out.getData(), out.getDataSize());
}

void TreeSync::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& prop)
{
    if (stamping)
        return;
    juce::MemoryOutputStream out;
    const bool has = tree.hasProperty (prop);
    if (! beginDelta (out, has ? Op::setProperty : Op::removeProperty, tree))
        return;
    out.writeString (prop.toString());
    if (has)
        tree.getProperty (prop).writeToStream (out);
    send (out);
}

void TreeSync::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (! isApplying())
    {
        const juce::ScopedValueSetter<bool> svs (stamping, true);
        Model::ensureIdentities (child);
    }
    juce::MemoryOutputStream out;
    if (! beginDelta (out, Op::addChild, parent))
        return;
    out.writeCompressedInt (parent.indexOf (child));
    TreePath::writeStep (out, TreePath::stepFor (child, parent));
    child.writeToStream (out);
    send (out);
}

void TreeSync::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int index)
{
    juce::MemoryOutputStream out;
    if (! beginDelta (out, Op::removeChild, parent))
        return;
    auto step = TreePath::stepFor (child, {});
    if (step.kind == TreePath::Kind::byIndex)
        step.index = index;
    TreePath::writeStep (out, step);
    send (out);
}

void TreeSync::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int newIndex)
{
    juce::MemoryOutputStream out;
    if (! beginDelta (out, Op::moveChild, parent))
        return;
    TreePath::writeStep (out, TreePath::stepFor (parent.getChild (newIndex), parent));
    out.writeCompressedInt (newIndex);
    send (out);
}

static void mergeChannelSeqInto (juce::ValueTree target, const juce::ValueTree& incoming)
{
    auto a = target.getProperty (IDs::steps).toString();
    auto b = incoming.getProperty (IDs::steps).toString();
    const int len = juce::jmax (a.length(), b.length());
    juce::String merged;
    merged.preallocateBytes ((size_t) len);
    for (int c = 0; c < len; ++c)
    {
        auto x = c < a.length() ? a[c] : (juce::juce_wchar) '0';
        auto y = c < b.length() ? b[c] : (juce::juce_wchar) '0';
        merged += (x == '1' || y == '1') ? '1' : '0';
    }
    if (merged != a)
        target.setProperty (IDs::steps, merged, nullptr);
    if ((bool) incoming.getProperty (IDs::usePianoRoll, false) && ! (bool) target.getProperty (IDs::usePianoRoll, false))
        target.setProperty (IDs::usePianoRoll, true, nullptr);

    for (int i = 0; i < incoming.getNumChildren(); ++i)
    {
        auto note = incoming.getChild (i);
        auto uid = note.getProperty (IDs::uid);
        if (! uid.isVoid() && target.getChildWithProperty (IDs::uid, uid).isValid())
            continue;

        auto copy = note.createCopy();
        if (Model::ensureIdentities (copy) && SyncDebug::enabled())
            synclog ("merge STAMP  uid-less " + copy.getType().toString()
                     + " -> " + copy.getProperty (IDs::uid).toString());
        if (Model::uidClashesWithSibling (copy, target))
        {
            synclog ("merge CLASH  " + copy.getType().toString()
                     + " uid=" + copy.getProperty (IDs::uid).toString() + " - sender's world differs");
        }
        target.appendChild (copy, nullptr);
    }
}

bool TreeSync::applyChange (juce::ValueTree& root, const void* data, size_t size, juce::String* why)
{
    auto fail = [why] (const juce::String& reason) { if (why) *why = reason; return false; };

    juce::MemoryInputStream in (data, size, false);
    if (size < 2 || (juce::uint8) in.readByte() != formatTag)
        return fail ("legacy/unknown delta format");

    const ScopedApply applying;

    const auto op = (Op) (juce::uint8) in.readByte();
    TreePath path;
    if (! path.read (in))
        return fail ("bad path");

    auto node = path.resolve (root);
    if (! node.isValid())
        return fail ("unresolved " + path.toString());

    auto applied = [op, &path] { if (SyncDebug::enabled())
                                     synclog (juce::String ("delta APPLY  ") + opName (op) + " " + path.toString());
                                 return true; };

    switch (op)
    {
        case Op::setProperty:
        {
            juce::Identifier prop (in.readString());
            node.setProperty (prop, juce::var::readFromStream (in), nullptr);
            return applied();
        }
        case Op::removeProperty:
        {
            juce::Identifier prop (in.readString());
            node.removeProperty (prop, nullptr);
            return applied();
        }
        case Op::addChild:
        {
            const int index = in.readCompressedInt();
            TreePath::Step step;
            if (! TreePath::readStep (in, step))
                return fail ("bad child step");
            auto child = juce::ValueTree::readFromStream (in);
            if (! child.isValid())
                return fail ("bad child payload");

            if (step.kind == TreePath::Kind::byIdentity)
            {
                auto existing = TreePath::resolveStep (node, step);
                if (existing.isValid())
                {
                    if (child.hasType (IDs::ChannelSeq))
                    {
                        mergeChannelSeqInto (existing, child);
                        return applied();
                    }
                    return fail ("duplicate identity " + path.toString() + "/" + Model::childIdentity (child));
                }
            }
            node.addChild (child, juce::jlimit (0, node.getNumChildren(), index), nullptr);
            return applied();
        }
        case Op::removeChild:
        {
            TreePath::Step step;
            if (! TreePath::readStep (in, step))
                return fail ("bad child step");
            auto child = TreePath::resolveStep (node, step);
            if (! child.isValid())
                return fail ("remove of missing child under " + path.toString());
            node.removeChild (child, nullptr);
            return applied();
        }
        case Op::moveChild:
        {
            TreePath::Step step;
            if (! TreePath::readStep (in, step))
                return fail ("bad child step");
            const int newIndex = in.readCompressedInt();
            auto child = TreePath::resolveStep (node, step);
            if (! child.isValid())
                return fail ("move of missing child under " + path.toString());
            const int from = node.indexOf (child);
            const int to = juce::jlimit (0, node.getNumChildren() - 1, newIndex);
            if (from != to)
                node.moveChild (from, to, nullptr);
            return applied();
        }
    }
    return fail ("unknown op");
}
