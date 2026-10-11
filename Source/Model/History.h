#pragma once
#include <JuceHeader.h>
#include "ProjectModel.h"
#include "TreePath.h"

class History : private juce::ValueTree::Listener,
                private juce::Timer
{
public:
    explicit History (juce::ValueTree projectTree)
        : project (projectTree)
    {
        baseline = project.createCopy();
        project.addListener (this);
    }

    ~History() override { project.removeListener (this); }

    std::atomic<bool> suppress { false };

    std::function<void()> onChanged;
    std::function<void (const juce::String&)> onConflict;
    std::function<void (const juce::String& key, int paramIndex, float value)> applyParam;

    bool isApplying() const { return applyingHistory; }

    bool canUndo() const { return undoIndex > 0; }
    bool canRedo() const { return undoIndex < (int) entries.size(); }

    void undo()
    {
        if (inTransaction) return;
        commitPending();
        if (! canUndo())
            return;
        --undoIndex;
        applyEntry (entries[(size_t) undoIndex], false);
    }

    void redo()
    {
        if (inTransaction) return;
        commitPending();
        if (! canRedo())
            return;
        applyEntry (entries[(size_t) undoIndex], true);
        ++undoIndex;
    }

    void clear()
    {
        if (inTransaction) return;
        commitPending();
        entries.clear();
        undoIndex = 0;
        baseline = project.createCopy();
    }

    void beginTransaction()
    {
        commitPending();
        inTransaction = true;
    }

    void endTransaction()
    {
        inTransaction = false;
        stopTimer();
        pendingLocal = false;
        captureDiff();
    }

    void commitPending()
    {
        if (isTimerRunning())
        {
            stopTimer();
            if (pendingLocal)
                captureDiff();
            pendingLocal = false;
        }
    }

    void recordParam (const juce::String& key, int paramIndex, float oldV, float newV)
    {
        if (suppress.load() || applyingHistory || inTransaction)
            return;
        if (juce::approximatelyEqual (oldV, newV))
            return;
        commitPending();

        auto now = juce::Time::getMillisecondCounter();
        if (undoIndex == (int) entries.size() && ! entries.empty())
        {
            auto& last = entries.back().ops;
            if (last.size() == 1 && last[0].type == Op::Type::setParam
                && last[0].paramKey == key && last[0].paramIndex == paramIndex
                && now - lastParamMs < 350)
            {
                last[0].newValue = newV;
                lastParamMs = now;
                return;
            }
        }

        if ((int) entries.size() > undoIndex)
            entries.resize ((size_t) undoIndex);
        Op op;
        op.type = Op::Type::setParam;
        op.oldValue = oldV;
        op.newValue = newV;
        op.paramKey = key;
        op.paramIndex = paramIndex;
        entries.push_back ({ { std::move (op) } });
        if ((int) entries.size() > maxEntries)
            entries.erase (entries.begin());
        undoIndex = (int) entries.size();
        lastParamMs = now;
    }

private:
    using TreePath = Model::TreePath;
    static constexpr int maxEntries = 200;

    struct Op
    {
        enum class Type { setProp, addChild, removeChild, setParam };
        Type type = Type::setProp;
        TreePath path;
        juce::Identifier prop;
        juce::var oldValue, newValue;
        juce::String childXml;
        TreePath::Step childStep;
        int childIndex = 0;
        juce::String paramKey;
        int paramIndex = 0;
    };

    struct Entry { std::vector<Op> ops; };

    static Op propOp (const TreePath& path, const juce::Identifier& name, juce::var oldV, juce::var newV)
    {
        Op op;
        op.type = Op::Type::setProp;
        op.path = path;
        op.prop = name;
        op.oldValue = std::move (oldV);
        op.newValue = std::move (newV);
        return op;
    }

    static Op childOp (Op::Type type, const TreePath& parentPath, const juce::ValueTree& parent,
                       const juce::ValueTree& child, int index)
    {
        Op op;
        op.type = type;
        op.path = parentPath;
        op.childXml = child.toXmlString();
        op.childStep = TreePath::stepFor (child, parent);
        op.childIndex = index;
        return op;
    }

    static TreePath extend (const TreePath& path, const juce::ValueTree& child, const juce::ValueTree& parent)
    {
        auto p = path;
        p.steps.push_back (TreePath::stepFor (child, parent));
        return p;
    }

    void diffNode (juce::ValueTree oldNode, juce::ValueTree newNode,
                   const TreePath& path, std::vector<Op>& ops)
    {
        for (int i = 0; i < newNode.getNumProperties(); ++i)
        {
            auto name = newNode.getPropertyName (i);
            if (name == IDs::pluginState)
                continue;
            auto nv = newNode.getProperty (name);
            auto ov = oldNode.getProperty (name);
            if (! oldNode.hasProperty (name) || nv != ov)
                ops.push_back (propOp (path, name, ov, nv));
        }
        for (int i = 0; i < oldNode.getNumProperties(); ++i)
        {
            auto name = oldNode.getPropertyName (i);
            if (name == IDs::pluginState)
                continue;
            if (! newNode.hasProperty (name))
                ops.push_back (propOp (path, name, oldNode.getProperty (name), juce::var()));
        }

        if (diffChildrenByIdentity (oldNode, newNode, path, ops))
            return;

        int oldN = oldNode.getNumChildren();
        int newN = newNode.getNumChildren();

        if (oldN == newN)
        {
            for (int i = 0; i < newN; ++i)
                diffNode (oldNode.getChild (i), newNode.getChild (i),
                          extend (path, newNode.getChild (i), newNode), ops);
            return;
        }

        if (newN == oldN + 1)
        {
            int at = cleanInsertionIndex (oldNode, newNode);
            if (at >= 0)
            {
                ops.push_back (childOp (Op::Type::addChild, path, newNode, newNode.getChild (at), at));
                return;
            }
        }

        if (oldN == newN + 1)
        {
            int at = cleanInsertionIndex (newNode, oldNode);
            if (at >= 0)
            {
                ops.push_back (childOp (Op::Type::removeChild, path, oldNode, oldNode.getChild (at), at));
                return;
            }
        }

        for (int i = oldN; --i >= 0;)
            ops.push_back (childOp (Op::Type::removeChild, path, oldNode, oldNode.getChild (i), i));
        for (int i = 0; i < newN; ++i)
            ops.push_back (childOp (Op::Type::addChild, path, newNode, newNode.getChild (i), i));
    }

    static int cleanInsertionIndex (const juce::ValueTree& shorter, const juce::ValueTree& longer)
    {
        int n = shorter.getNumChildren();
        int at = n;
        for (int i = 0; i < n; ++i)
            if (! shorter.getChild (i).isEquivalentTo (longer.getChild (i)))
                { at = i; break; }

        for (int i = at; i < n; ++i)
            if (! shorter.getChild (i).isEquivalentTo (longer.getChild (i + 1)))
                return -1;
        return at;
    }

    bool diffChildrenByIdentity (juce::ValueTree oldNode, juce::ValueTree newNode,
                                 const TreePath& path, std::vector<Op>& ops)
    {
        const int oldN = oldNode.getNumChildren();
        const int newN = newNode.getNumChildren();

        juce::StringArray oldKeys, newKeys;
        for (int i = 0; i < oldN; ++i)
        {
            auto k = Model::childIdentity (oldNode.getChild (i));
            if (k.isEmpty() || oldKeys.contains (k)) return false;
            oldKeys.add (k);
        }
        for (int i = 0; i < newN; ++i)
        {
            auto k = Model::childIdentity (newNode.getChild (i));
            if (k.isEmpty() || newKeys.contains (k)) return false;
            newKeys.add (k);
        }

        for (int i = oldN; --i >= 0;)
            if (! newKeys.contains (oldKeys[i]))
                ops.push_back (childOp (Op::Type::removeChild, path, oldNode, oldNode.getChild (i), i));

        for (int i = 0; i < newN; ++i)
            if (! oldKeys.contains (newKeys[i]))
                ops.push_back (childOp (Op::Type::addChild, path, newNode, newNode.getChild (i), i));

        for (int i = 0; i < newN; ++i)
        {
            int oi = oldKeys.indexOf (newKeys[i]);
            if (oi >= 0)
                diffNode (oldNode.getChild (oi), newNode.getChild (i),
                          extend (path, newNode.getChild (i), newNode), ops);
        }
        return true;
    }

    void captureDiff()
    {
        std::vector<Op> ops;
        diffNode (baseline, project, {}, ops);
        if (ops.empty())
            return;

        if ((int) entries.size() > undoIndex)
            entries.resize ((size_t) undoIndex);
        entries.push_back ({ std::move (ops) });
        if ((int) entries.size() > maxEntries)
            entries.erase (entries.begin());
        undoIndex = (int) entries.size();

        baseline = project.createCopy();
    }

    static juce::ValueTree findChild (const juce::ValueTree& parent, const TreePath::Step& step, int fallbackIndex)
    {
        if (step.kind != TreePath::Kind::byIndex)
            return TreePath::resolveStep (parent, step);
        auto c = parent.getChild (fallbackIndex);
        return c.hasType (step.type) ? c : juce::ValueTree();
    }

    void applyEntry (const Entry& entry, bool forward)
    {
        const juce::ScopedValueSetter<bool> svs (applyingHistory, true);
        int conflicts = 0;

        auto addFromXml = [&] (juce::ValueTree parent, const Op& op)
        {
            if (! parent.isValid()) { ++conflicts; return; }
            if (findChild (parent, op.childStep, -1).isValid())
                return;
            if (auto xml = juce::parseXML (op.childXml))
            {
                auto child = juce::ValueTree::fromXml (*xml);
                if (child.isValid())
                    parent.addChild (child, juce::jmin (op.childIndex, parent.getNumChildren()), nullptr);
            }
        };
        auto removeByStep = [&] (juce::ValueTree parent, const Op& op)
        {
            auto child = findChild (parent, op.childStep, op.childIndex);
            if (child.isValid()) parent.removeChild (child, nullptr);
            else                 ++conflicts;
        };

        auto applyOne = [&] (const Op& op, bool fwd)
        {
            auto node = op.path.resolve (project);
            switch (op.type)
            {
                case Op::Type::setProp:
                {
                    if (! node.isValid()) { ++conflicts; return; }
                    auto expected = fwd ? op.oldValue : op.newValue;
                    auto target   = fwd ? op.newValue : op.oldValue;
                    if (node.getProperty (op.prop) != expected)
                        ++conflicts;
                    if (target.isVoid())
                        node.removeProperty (op.prop, nullptr);
                    else
                        node.setProperty (op.prop, target, nullptr);
                    break;
                }
                case Op::Type::addChild:
                    if (fwd) addFromXml (node, op);
                    else     removeByStep (node, op);
                    break;
                case Op::Type::removeChild:
                    if (fwd) removeByStep (node, op);
                    else     addFromXml (node, op);
                    break;
                case Op::Type::setParam:
                    if (applyParam)
                        applyParam (op.paramKey, op.paramIndex,
                                    (float) (double) (fwd ? op.newValue : op.oldValue));
                    else
                        ++conflicts;
                    break;
            }
        };

        if (forward)
            for (auto& op : entry.ops) applyOne (op, true);
        else
            for (auto it = entry.ops.rbegin(); it != entry.ops.rend(); ++it) applyOne (*it, false);

        baseline = project.createCopy();
        if (onChanged)
            onChanged();
        if (conflicts > 0 && onConflict)
            onConflict ("Undo/redo touched something a peer also changed - it may look different than expected.");
    }

    bool mirrorRemote (const std::function<bool (juce::ValueTree&)>& apply, const juce::ValueTree& liveNode)
    {
        TreePath path;
        if (! TreePath::of (liveNode, project, path))
            return false;
        auto target = path.resolve (baseline);
        return target.isValid() && apply (target);
    }

    void resyncBaseline()
    {
        if (pendingLocal) captureDiff();
        else              baseline = project.createCopy();
        pendingLocal = false;
        stopTimer();
    }

    void markLocalChanged()
    {
        pendingLocal = true;
        startTimer (350);
    }

    void timerCallback() override
    {
        stopTimer();
        if (inTransaction)
            return;
        if (pendingLocal)
            captureDiff();
        pendingLocal = false;
    }

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& p) override
    {
        if (p == IDs::pluginState)
            return;
        if (applyingHistory)
            return;
        if (suppress.load())
        {
            const bool ok = mirrorRemote ([&] (juce::ValueTree& b)
            {
                if (tree.hasProperty (p)) b.setProperty (p, tree.getProperty (p), nullptr);
                else                      b.removeProperty (p, nullptr);
                return true;
            }, tree);
            if (! ok) resyncBaseline();
            return;
        }
        markLocalChanged();
    }

    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        if (applyingHistory)
            return;
        if (suppress.load())
        {
            const bool ok = mirrorRemote ([&] (juce::ValueTree& b)
            {
                auto step = TreePath::stepFor (child, parent);
                if (step.kind == TreePath::Kind::byIdentity && TreePath::resolveStep (b, step).isValid())
                    return true;
                b.addChild (child.createCopy(), juce::jmin (parent.indexOf (child), b.getNumChildren()), nullptr);
                return true;
            }, parent);
            if (! ok) resyncBaseline();
            return;
        }
        Model::ensureIdentities (child);
        markLocalChanged();
    }

    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int index) override
    {
        if (applyingHistory)
            return;
        if (suppress.load())
        {
            const bool ok = mirrorRemote ([&] (juce::ValueTree& b)
            {
                auto step = TreePath::stepFor (child, {});
                auto target = findChild (b, step, index);
                if (! target.isValid())
                    return false;
                b.removeChild (target, nullptr);
                return true;
            }, parent);
            if (! ok) resyncBaseline();
            return;
        }
        markLocalChanged();
    }

    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int newIndex) override
    {
        if (applyingHistory)
            return;
        if (suppress.load())
        {
            const bool ok = mirrorRemote ([&] (juce::ValueTree& b)
            {
                auto moved = parent.getChild (newIndex);
                auto target = findChild (b, TreePath::stepFor (moved, parent), -1);
                if (! target.isValid())
                    return false;
                b.moveChild (b.indexOf (target), juce::jlimit (0, b.getNumChildren() - 1, newIndex), nullptr);
                return true;
            }, parent);
            if (! ok) resyncBaseline();
            return;
        }
        markLocalChanged();
    }
    void valueTreeParentChanged (juce::ValueTree&) override {}
    void valueTreeRedirected (juce::ValueTree&) override {}

    juce::ValueTree project, baseline;
    std::vector<Entry> entries;
    int undoIndex = 0;
    bool applyingHistory = false;
    bool pendingLocal = false;
    bool inTransaction = false;
    juce::uint32 lastParamMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (History)
};
