# AGENTS.md

Guidance for AI agents working in this repository. Read before touching code — the architecture
has load-bearing invariants that break silently. The **Repository map** says which file to open
for any feature; for a bug report, **Locating problems fast** maps the symptom straight to the
first file to open. The sections after them are the invariants. Facts here carry exact
identifiers (class names, type strings, constants, ports) on purpose: trust them over guessing,
and verify against the named file if something looks off.

## What this is

A **native Windows DAW** in C++17/JUCE 8: VST2/VST3 hosting, classic DAW UI
(Channel Rack / Piano Roll / Playlist / Mixer / Browser / transport bar), automation clips, and
**P2P multiplayer** (up to 4 peers, `Session::maxPeople`, live TCP editing with a relay-server
fallback). Spec: `CREATE-DAW-TASK.md`. Docs: `README.md`.

Hard constraint: **single native process** — no browser, no Electron, no WASM, no IPC between
UI and audio. Never introduce any of these.

## Build / run / verify

```bat
:: Dependencies are NOT vendored in git - clone once if external/ pieces are missing:
git clone --depth 1 --branch 8.0.8 https://github.com/juce-framework/JUCE.git external/JUCE
git clone --depth 1 --branch v1.91.9b https://github.com/ocornut/imgui.git external/imgui

:: configure once (or after CMakeLists.txt changes) - MUST be this generator:
cmake -S . -B build -G "Visual Studio 18 2026" -A x64

:: build+verify loop - run after every change:
powershell -ExecutionPolicy Bypass -File run.ps1
:: relaunch without rebuilding:
powershell -ExecutionPolicy Bypass -File run.ps1 -NoBuild
```

`run.ps1` is the whole verify cycle: kills any running mikeDAW.exe (a live exe blocks the
linker), builds Release (output trimmed to last 25 lines — read them for errors), relaunches,
prints `alive PID=...` on success or `BUILD FAILED` / `NOT RUNNING (crashed on startup?)`.
No unit tests exist: clean build + live process is the baseline; the user verifies by hand in
the app. Slash command: **/build** runs this loop and interprets the result.

- First build compiles all JUCE modules (~4 min); incremental builds are fast.
- `run.ps1` also builds `bridge32/` (the 32-bit VST2 bridge, own tiny JUCE-free project) into
  `build32/` with `-A Win32` and copies `mikeDAW-Bridge32.exe` next to mikeDAW.exe. Bridge
  build failure is non-fatal (`BRIDGE32 BUILD FAILED` + app runs without 32-bit support).
- VST2 SDK `C:/GitHuhb/vst_sdk2_4_rev2/vstsdk2.4` (`OPENDAW_VST2_SDK_PATH`), ASIO SDK
  `C:/GitHuhb/asiosdk` (`OPENDAW_ASIO_SDK_PATH`); either missing = graceful downgrade. VST3 SDK
  is bundled in JUCE. Dear ImGui (debug overlay only) compiles from `external/imgui`;
  `PerfMonitor.cpp` links `dbghelp.lib` via pragma.
- IDE clangd errors ("JuceHeader.h not found") are **false positives** — the header is
  generated into `build/`. MSVC output is the only truth.
- `JUCE_MODAL_LOOPS_PERMITTED=0`: all dialogs/menus **must be async** (`launchAsync`,
  `showMenuAsync`, `enterModalState`). Keep `FileChooser`s alive in a member
  `std::unique_ptr` for the callback's duration. Helpers: `UI/Dialogs.h`.
- JUCE 8 APIs: `FontOptions` for fonts, `audioDeviceIOCallbackWithContext`,
  `juce::Optional<PositionInfo> getPosition()` for playheads.
- Never screenshot the running app to verify a UI change — the user rejects screen capture.
  Verify via `run.ps1` (`alive PID`), by reasoning through the code path, or with temporary
  diagnostics; runtime behaviour is confirmed by the user in the app.
- Parallel agents must NOT run `run.ps1`/cmake at the same time — one shared `build/` tree plus
  the kill-running-exe step corrupt concurrent builds. Batch multi-agent work into waves of
  disjoint files (one owner per shared file) and build once between waves.

## Comments — the rule

**Default: no comment.** Code must explain itself through naming and structure. This codebase
still contains legacy comment slop — restating code, narrating edits, section banners. That
density is NOT the house style and is NOT license to match it. When you touch a function,
deleting slop comments in it is welcome. Never add new ones.

A comment is justified only when it carries information *unrecoverable from the code* that a
competent reader would otherwise get wrong:

- A non-obvious external constraint (OS/JUCE/VST-spec behaviour, hardware quirk) forcing a
  shape that otherwise looks wrong.
- A deliberate deviation — why the obvious approach was rejected, where a reader would want to
  "fix" it back (lock discipline, protocol ordering, intentional asymmetries).
- A reference that saves a research session (spec section, MSDN page, known-bug link).

Never write: restatements (`// increment the counter`), section banners (`// --- setup ---`),
edit narration (`// changed to fix X`, `// this is safe because...`), docstrings that re-spell
the signature. **If you are unsure whether a comment qualifies, it does not. Leave it out.**

## Working rules

- **Extend existing code; no parallel implementations.** Check the Repository map first —
  knobs, popups, toasts, async dialogs, zoom bars, file caches all already exist.
- **New state goes in the ValueTree** — never ad-hoc members needing manual save/load/sync.
- All colours from `Colors` (`UI/AppLookAndFeel.h`) — never hardcode a `juce::Colour`. New
  slots must be registered in `ThemeManager::registry()`.
- **Source stays ASCII-only.** CMake passes no `/utf-8` and sources have no BOM, so non-ASCII
  bytes in a string literal (em-dash, arrows, checkmarks, emoji) render as mojibake in the UI.
  Draw glyphs (X, chevrons, arrows) as `juce::Path` strokes and use plain hyphens in
  user-facing strings. Runtime emoji are fine — they come from data, not source literals
  (`Colors::emojiFont`). After bulk UI edits, grep changed sources for non-ASCII bytes
  before building.

## Locating problems fast (symptom → first place to look)

Each row names the first symbol to open for a symptom (for features: the Repository map). If
a row disagrees with the code, trust the code and fix the row.

**Audio / engine**

- One channel silent, others play → channel `mute`/`solo`, `mixerTrack` routing,
  `engine.isChannelPlaceholder` (missing VST fell back to `PlaceholderSynth`); rebuild path
  `AudioEngine::rebuildChannels`.
- Crackles / dropouts → xruns + lock-wait in `engine.getAudioPerfStats()` (overlay Audio tab);
  something held `engineLock` across slow work — threading invariants below.
- Mixer or channel edits not heard → structural change went around `markDirty` + the 3-phase
  rebuild, or a builtin slot `type` string fell through to the VST path in `rebuildMixer`.
- 32-bit plugin silent → bridge process died (`Bridge32Host`); an RPC timeout outputs silence
  but never kills the bridge; reload is the only recovery (Known gaps).
- 32-bit plugin editor stays on "Loading 32-bit plugin editor..." → the bridge GUI thread never
  published `editorHwnd` (`showEditor` in `bridge32/main.cpp`); check `uiShow` reached it and
  that `CreateWindowExW` with the host peer as parent succeeded (DPI awareness must match).
- FLP import picked the 32-bit copy of a plugin that also exists as x64 → `formatRank` ranks
  bridged descs below native ones per tier, and `preferNativeTwin` swaps a bridged uid/name
  match for its x64 sibling file (both in `FLPImport.cpp`). An explicit wrapper path wins
  over both - it names the exact build FL loaded.
- Automation clip does nothing → playback must be SONG mode (`applyAutomation`); check the
  sentinel values before treating an override as a real value.
- Audition vs sequencer confusion (stuck notes, double triggers) → `auditionNote` rides MIDI
  ch 2, the sequencer ch 1; played-back notes come only from `recompileSequences` output.

**Build / launch**

- Linker cannot write mikeDAW.exe → a previous instance is still running; `run.ps1` kills it.
- `JuceHeader.h not found` in the IDE → false positive; the header is generated into `build/`.
  MSVC output is the only truth.
- App dies at startup → `run.ps1` prints `NOT RUNNING (crashed on startup?)`; minidumps land in
  `%APPDATA%/mikeDAW/CrashLogs`, hang reports in `PerfReports/`.
- New UI text renders as mojibake → non-ASCII literal in source (Working rules).

**UI**

- Widget doesn't follow model changes → it isn't a `ValueTree::Listener`, or the project was
  reassigned instead of mutated in place (Gotchas).
- Dialog/menu asserts or never opens → a modal call slipped in; `JUCE_MODAL_LOOPS_PERMITTED=0`,
  use `launchAsync`/`showMenuAsync` (`UI/Dialogs.h`).
- DeviceChain param edit desyncs peers or floods undo → missing
  `beginChangeGesture/endChangeGesture` + `chain.recordParamEdit` bracket.
- Mixer fader moves by itself during playback → automation follow (`Strip::updateAutoFollow`)
  is by design and must NOT write the tree; if it writes the tree, that is the bug.
- VST editor takes seconds to appear → deliberate deferred build (`scheduleEditorBuild`); never
  construct editors synchronously in `open()` paths.
- A panel won't show → `togglePanel(name)` in MainComponent; the debug overlay is NOT a desk
  panel (own DocumentWindow + GL context, Ctrl+Shift+D).

**Plugins**

- Plugin never reappears after a scan crash → pedal/blacklist files `scan-inflight.txt` /
  `scan-crashed-plugins.txt` (path+mtime keyed, so reinstalls retry).
- Plugin from a peer's project won't load → `createInstance` fallbacks `resolveLocalMatch` /
  `resolveUnverifiedMatch`; picker gating `isPluginCompatible` (Smart VST Match). Matches
  never cross formats except VST2 x86 <-> x64 (`Bridge32::formatsInterchangeable`, same
  chunk format); VST2 <-> VST3 state is not interchangeable, so a peer holding only the
  other format gets the placeholder.
- Builtin effect recalls wrong values after your edit → `addParameter` order changed; state =
  raw floats in parameter order, append only.
- FL Studio VSTi editor crashes ("Execution of address 0") → its own registry engine path
  (Gotchas), reproduces in any host — not our code.
- 32-bit plugin editor changes don't reach peers → the shm ring is drained via
  `sendValueChangedMessageToListeners`; using `setValue` creates the echo loop.

**Multiplayer / sync / undo**

- Echo/feedback loop after a remote apply → missing suppress guard: `applyingRemote`,
  `engine.suppressParamBroadcast`, `suppressTransportBroadcast`, History `suppress`.
- A peer's edit lands on the wrong note/clip, or a note vanishes from the roll but still
  plays → a node type without an identity property (see `Model::identityKeyFor`) or a
  Note/Point/Clip that escaped uid stamping (`Model::ensureIdentities`, run by History on
  every local add and by TreeSync/snapshot). Deltas that cannot resolve are dropped and
  logged as `delta DROP` in sync-debug.log.
- Undo wiped a peer's concurrent edit → snapshot-style undo crept in; `History` is op-based and
  remote edits fold into the baseline unrecorded.
- Peer dropped during a big file transfer → by design: keepalive-proven peers drop after 60s
  silence because transfers monopolise the sender FIFO. Do not lower the timeout.
- Relay join never completes → confirm with `onSnapshotApplied`, never the loopback connect;
  12s timeout in `MainComponent::beginRelayJoin`, server kills unpaired bridges at 20s.
- Relay session drops when the internet blips → by design: a relay leg that dies mid-stream
  loses in-flight bytes, and `InterprocessConnection` silently kills its reader on a bad
  frame header (no `connectionLost`), so `RelayTunnel` ends the bridge instead of re-pairing.
- `pluginState` loops between peers → the engine ignores `pluginState` tree changes on purpose;
  live state travels as param messages (Plugin state section).

**Files / FLP / recording**

- Save stalls the UI briefly → by design: capture + `toXmlString` on the message thread, file
  write on `DiskWriteQueue`.
- Saved/exported project misses samples → `bundleProjectSounds` copies only sounds that no
  longer resolve at their own path; full bundling is the zip export's job; loads run
  `relinkBundledSamples`.
- FL export/import bytes wrong → `.knowledge/` tooling: headless `--export-flzip`,
  `flpdump.py`, `verify_export.py` byte-matcher, catalog `FLP.md` (Import/export section).
- Recorded clip silent/dimmed for peers → `pending` stays set until the recorder's WAV is
  offered; deliberate.
- Recorded clip starts off-time → in+out latency compensation on stop (Recording section).

## The single ValueTree — everything hangs off it

**The entire project state is one `juce::ValueTree`** (`IDs::Project`, schema in
`Source/Model/ProjectModel.h`, properties documented inline):

- UI components are `ValueTree::Listener`s writing straight into the tree.
- `AudioEngine` listens and mirrors the tree into realtime-safe structures.
- Save = `project.toXmlString()` on the message thread, file write on the background
  `DiskWriteQueue` thread; load = in-place `copyPropertiesAndChildrenFrom` (`.odaw` XML).
- Multiplayer = identity-addressed `TreeSync` deltas (`Net/TreeSync.{h,cpp}`, paths from
  `Model/TreePath.h`); undo (`Model/History.h`) = diffs of it, addressed the same way.

**Any new feature = new property/child in the tree** — persistence, sync and undo come free.
Out-of-band state means hand-rolling all four; avoid it.

```
Project { name, bpm(140), scaleRoot(0..11), scaleType("major"...), songMode(false=PAT),
          createdEpoch, secondsWorked }
├─ Channels └─ Channel { id, type("sampler"|"plugin"|"builtin"), name,
│              samplePath(path or "builtin:kick"), pluginDesc(XML), pluginState(base64),
│              volume(0.8), pan, mute, solo, mixerTrack(1), colour, cutItself(choke),
│              declickMode(0..5), pitchSemis, pitchRange(2), rootNote(60), fineTune(cents),
│              smpStart(0..1), smpLength(0..1), fadeIn, fadeOut,
│              fxRemoveDC, fxNormalize, fxReverse, fxRevPolarity, fxFadeStereo, fxSwapStereo }
├─ Patterns └─ Pattern { id, name, lengthSteps(16), colour }
│              └─ ChannelSeq { channelId, steps("0"/"1" string), usePianoRoll }
│                  └─ Note { uid, key, start, length, velocity, slide }   (ticks)
├─ Playlist ├─ Clip { uid, type("pattern"|"audio"|"automation"), patternId, path, track,
│           │        startBeats, lengthBeats, offsetBeats, pending, recordedBy,
│           │        target(automation only) }
│           │   └─ Point { uid, pos(beats), val(0..1), curveMode(0/1/2), tension(-1..1) }
│           └─ PlaylistTrack { index, name, enabled }
├─ AudioFiles └─ AudioFile { id, name, audioPath }
├─ MidiFiles  └─ MidiFile  { id, name, midiPath }
└─ Mixer └─ Track { index, name, routeTo(0=Master), audioInput(""|"N"|"N,M"), recArm,
            stereoSep(-1..1), volume(1.0), pan, mute, solo }
            └─ Slot { uid, slotIndex, type, enabled, mix(0..1), genericUI,
                      pluginDesc, pluginState }
```

`namespace Model` constants: `PPQ=96` ticks/beat, `stepsPerBeat=4`, `ticksPerStep=24`,
`beatsPerBar=4`, `ticksPerBar=384`, 4/4 only; `numMixerTracks=20` default /
`maxMixerTracks=64`; `numSlotsPerTrack=10`; `numPlaylistTracks=9` default (lanes grow via
`ensurePlaylistTracks`); `defaultSteps=16`; `middleC=60`.

Factories/helpers (`ProjectModel.{h,cpp}`): `addSamplerChannel/addPluginChannel/
addBuiltinChannel`, `addPattern`, `addMixerTrack`, `addAudioResource/addMidiResource`,
`createDefaultProject/createEmptyProject/createBlankSamplerProject/createProjectFromTemplate`,
`findChannelById/findPatternById`, `getChannelSeq`, `getMixerTrack`, `getMaxMixerTrackIndex`,
`convertStepsToNotes`, `removeChannelSequences`, `mergeDuplicateChannelSeqs`,
`patternLengthTicks/patternContentEndTicks`, `uniqueChannelName`, `channelColour/patternColour`.

Identity is **random 30-bit ids** (`Model::newId()` = `nextInt(0x3fffffff)+1`) so concurrent
peer creates can't collide — never introduce a sequential id counter. Note: channel faders
default 0.8; mixer Master + inserts default **unity 1.0** — don't stack sub-unity defaults.

## Automation clips

Implemented in `Source/Model/Automation.{h,cpp}` (`namespace Automation`) + playlist UI +
engine playback. A playlist `Clip` with `type=="automation"` carries a `target` string and
`Point` children forming the curve.

- **Target grammar** (NOT the plugin-key format): `"track.<mixerIndex>.volume"`,
  `"track.<mixerIndex>.pan"`, `"channel.<channelId>.volume"`, `"channel.<channelId>.pan"`.
  Parse with `Automation::parseTarget` → `Target{Kind,index}`, Kind ∈
  {trackVolume, trackPan, channelVolume, channelPan, invalid}. Display via `targetName`.
- **Values are normalized 0..1** in the tree. `denormalize`: volume → 0..1.25
  (`Automation::volumeMax=1.25`, shared ceiling of mixer and channel faders — change in
  lockstep with the knob ranges or values desync), pan → -1..1.
- **Curve semantics**: a Point's `curveMode`/`tension` describe the segment coming INTO that
  point from the previous one (first point's fields inert). Modes: 0=single curve (tension
  pow-shaping), 1=hold, 2=smooth. `Automation::shape/evaluate` are the only evaluators.
- **Creation**: only via `ValueKnob` right-click → "Create automation clip"
  (`ValueKnob::onCreateAutomationClip`), wired on channel-rack vol/pan knobs and mixer strip
  fader/pan. `Automation::createClip` seeds a 2-point flat clip on the first empty lane.
- **Playback is SONG mode only**: `AudioEngine::applyAutomation` (audio thread, under
  `engineLock`) evaluates compiled lanes and writes per-track/per-channel override atomics.
  Sentinels: `autoVolume < 0` = no override (none=-1.0f), `autoPan < -1.5` = no override
  (none=-2.0f) — never treat the sentinels as real values. UI reads lock-free
  `engine.getTrackAutoVolume/getTrackAutoPan`; mixer `Strip::updateAutoFollow` moves faders
  to follow **without writing the tree** (writing would spam undo + multiplayer).
- Playlist editing (in `Playlist.cpp` `Canvas`): drag points, Ctrl+drag segment = tension,
  double-click = insert point, right-click point = menu. Slice tool skips automation clips.
  New clip ops must branch on all three type strings, not assume two.
- Adding a new automatable target = extend `Automation::Target/parseTarget` AND the
  `applyAutomation` dispatch together. Plugin parameters are NOT automatable yet.

## Repository map — where to change what

Start at `UI/MainComponent.{h,cpp}` — owns everything, wires every `onXxx` callback in its
constructor. Shortcuts land in `handleAction(KeyBindings::Action)`; panel visibility via
`togglePanel(name)` (`browser/rack/roll/playlist/mixer/chat/activity/friends`). Typing
keyboard (Ctrl+T), recording commit, project new/save/load/autosave, FLP/MIDI import, relay
join fallback, and the debug overlay toggle all live here.

Project lifecycle details: saves go through the `DiskWriteQueue` background thread (capture +
`toXmlString` on the message thread, write off-thread, completion via `SafePointer`; dtor
drains up to 20s). Every successful save runs `bundleProjectSounds`, which copies into a
`<name> samples` folder next to the .odaw **only** the sounds that no longer resolve at
their own path (a peer's file living in the session cache) — a sample still sitting on this
machine is left alone, `builtin:` paths are skipped, and full bundling is what the zip / FL Studio
exports are for. Loads run `relinkBundledSamples`. Save/open/export dialogs default to
`AppSettings::projectsFolder` (`Documents/mikeDAW/Projects`, overridable in Preferences).
`loadFile` handles `.odaw`, `.zip` (extract + relink), and `.flp`
(FLPImport). In a session only the host may replace the project (`newProject`/`loadFile`/
`revertToAutoSave` toast and return for clients); every host load re-baselines peers via
`session.resetProjectForPeers()`. `exportProjectZip` writes .odaw + samples/ (builtins baked via
`AudioEngine::renderBuiltinSampleToWav`). Double-clicked files enter via
`openProjectFileFromOS`.

### Main UI panels (`Source/UI/`)

- **ChannelRack** — `Row` per channel (name button, `ValueKnob` pan/vol, mute light). Steps
  are **painted cells**, not buttons: `paintSteps` draws, `mouseDown/mouseDrag` paint into
  `ChannelSeq.steps`. Add flows: `showAddChannelMenu` → `PluginPickerPopup` /
  `addSamplerFromFile`; also `cloneChannel`, `replaceInstrument`, `importMidiToChannel`,
  right-click `showChannelMenu` (channel delete also calls `Model::removeChannelSequences`).
  Channel **solo** via right-click on the mute light (`toggleChannelSolo`; unsolo unmutes
  everything — no pre-solo restore; mixer and playlist solos do the same).
  Multi-select API (`getSelectedChannelIds`, `selectChannel(id, additive)`); static
  `SeqClipboard` copies one
  channel's steps+notes (deliberately transient, not in the tree). Drag-hover insert
  (`insertSamplerAt`). Follows selected pattern via `setSelectedPattern`.
- **PianoRoll** — inner `Grid` (draw/move/resize/rubber-band/slice, `enum Tool`), `Keyboard`,
  `VelocityLane`, `Ruler`, `ArpDialog`, `StrumDialog`. Target = (patternId, channelId) via
  `setTarget`. Ops: `duplicateSelection`, `copySelection/pasteClipboard`,
  `transposeSelection`, `legatoSelection`, `showArpeggiatorDialog`, `showStrumizerDialog`,
  `randomizeNotesWeirdly`, `exportMidi`. Note **slide** mode (`slideBtn`, `ClipNote.slide`,
  pitch glide). Scale highlight `rootBox/scaleBox` + `detectBtn`
  (→ `Scales::detectKey`); ghost notes drawn faint.
- **Playlist** — inner `Canvas` (clips), `TrackHeaders`, `PatternPicker` (Ctrl+click
  multi-select, range select, multi-delete/recolour); tabs PAT/AUD/MIDI. Clips are
  `IDs::Clip` nodes; audio clips draw waveforms via shared `AudioThumbnailCache` (gated by
  `DebugFlags::disableWaveformThumbnails`); automation clips draw editable curves
  (`Colors::automationClip`). Ops: `duplicateSelection`, `moveSelectionTracks`,
  `splitPatternByChannel`, `makeClipUnique`. `ZoomScrollBar` horizontal, Alt+wheel track
  height; ruler drag scrubs. Recording lands via `addRecordedClip` / `getRecordingLane`.
- **Mixer** — `MixerPanel`, `Strip`s (Master + inserts, grow to 64). Strip = peak meter
  (`engine.getTrackPeak`), fader, stereo-separation knob (`IDs::stereoSep`), recArm +
  `audioInput` row, routing (`showRoutingMenuFor` → `routeTo`). **Two rack views**
  (`enum class RackMode { slotRack, deviceChain }`, persisted in `AppSettings::mixerRackMode`,
  toggled by `ModeChip`): `EffectRack` = 10-slot vertical rack (`showAddEffectMenu`,
  `openSlotEditor` key `"m<track>s<slotUid>"`, per-slot LED + `mix` knob); `DeviceChain` =
  horizontal cards with an inline editable param grid — every cell edit must
  bracket `beginChangeGesture/endChangeGesture` and report via
  `chain.recordParamEdit → MixerPanel::onParamEdit(key, paramIndex, old, new)` or peers
  desync. Mixer solo never mutes Master (the `i>0` guard). Chain presets:
  `exportMixerState/importMixerState` (`.odms`).
- **TransportBar** — FILE/EDIT/PLUGINS menu buttons, play/stop/record (`RecordButton`
  right-click = record options), `BpmBox`, metronome, PAT/SONG (`IDs::songMode`), `SpectrumBox`
  (right-click: spectrogram/oscilloscope), `PosReadout` (right-click cycles M:S:cs /
  bar:beat:step / H:M:S), master `playbackVolKnob`, panel-toggle `IconButton`s, typing-kb
  button, multiplayer cluster (`PeerStrip` with per-peer deafen/listen, chat/activity/social
  toggles). Reactions are NOT here — global middle-click opens the picker
  (`MainComponent::showReactionPickerAt`).
- **BrowserPanel** — file tree: home + Music + custom roots (`getCustomRoots`) + multiplayer
  download folder. Drag description `"browser-file"` (`BrowserPanel::dragDescription`);
  selected file via `getSelectedFile()` (MainComponent wires other panels' `getBrowserFile`
  to it). Selecting previews (`engine.previewFile`). Async search index, Ctrl+wheel zoom,
  draggable width, `onSendToSelectedChannel`, folder `.nfo` metadata (colour/tooltip/image,
  `readFolderNfo`), per-folder ignore list, show-extensions toggle.
- **SamplerSettingsPanel.h** — Channel-settings page for a sampler `Channel` or playlist
  audio `Clip` (`setTarget`). Fx LED toggles (fxRemoveDC/Normalize/Reverse/RevPolarity/
  FadeStereo/SwapStereo — baked into the sample by the engine), sample start/length,
  fadeIn/fadeOut, declick mode (0..5), pitch knob (± `pitchRange` semis), fine-tune,
  `RootKeyboard` (`rootNote`), YIN pitch detect (bg thread → `Engine/PitchDetect.h`).
- **FriendsPanel.h** — the SOCIAL floating panel (`togglePanel("friends")`): profile card,
  friend search, Host/Join/Leave, Requests / Online / Offline sections, per-row
  join/invite/accept/decline/remove. Reads **only** `FriendsManager` (1 Hz timer); all
  side-effects go through `std::function` callbacks wired in MainComponent. Panel visibility
  gates `lanDiscovery.setPresenceQuerying` — presence queries only run while it's open.

### Plugin UX

- **PluginWrapper.h** — `PluginWrapperComponent(processor, projectTree, pluginKey)`,
  plugin-wrapper chrome above every hosted editor: wrapper menu, param-list toggle
  (persisted per mixer slot in `IDs::genericUI`), knobs resolved from the plugin key by
  `resolveNode` — channel keys get VOL/PAN + ON(=mute) + TRACK box, mixer-slot keys get
  MIX + ON(=enabled); the two are not interchangeable. Preset prev/next arrows step the
  plugin's internal *programs*; `.odawpreset` save/load lives in the menus
  (`Documents/mikeDAW/Presets/<plugin>`, `setStateInformation` under `getCallbackLock()`,
  always fire `onStateChanged` after so peers resync). Editor build is deferred until after
  the `LoadingView` spinner's first paint (`scheduleEditorBuild`) — keep it that way, editor
  construction is slow.
- **PluginWindow / PluginWindowManager** (`Engine/PluginHost.h`) — one native window per
  plugin key; `open(key, proc, title)`, `close`, `closeAll`; closing rebroadcasts state in
  multiplayer (`onWindowClosed`); render-lock veil (`setWindowsRenderLocked`).
  `openLoading(key, title)` shows an spinner window before the instance exists;
  `open()` promotes it in place, keeping position. `keyForwarder`/`onKeyPress` forward
  unconsumed keys (e.g. Space=play) from focused plugin windows to app keybindings.
- **PluginManagerWindow.h** — scan UI: SCAN / RESCAN ALL / + FOLDER, "Verify plugins" toggle
  (full-load scan vs fast index; unverified `?` entries promoted on first use via
  `PluginHost::resolveForLoad`), FL Studio plugin scan toggle, failure/blacklist report.
- **PluginPickerPopup.h** — search-filterable picker used everywhere (`show`); `extraItems`
  for non-plugin rows, `isEnabled` greys plugins not on all peers (Smart VST Match), `isNew`
  tags fresh scans.

### Engine (`Source/Engine/`)

- **AudioEngine.{h,cpp}** — realtime engine mirroring the tree. API groups: transport
  (`play/stop/stopAt/pause/togglePlay/seekTicks`, `setBpm`, `setSongMode`,
  `setMetronomeEnabled`, `setCurrentPatternId`); lock-free UI getters (`getPositionTicks`,
  `isPlaying`, `getTrackPeak`, `getTrackAutoVolume/Pan`, `drainNoteOnChannelIds`, spectrum
  snapshot, `getAudioPerfStats`, `getPluginPerfStats`); local-only toggles (`setDeafened`,
  `setPlaybackVolume`, `setSmartDisableEnabled` — smart-disable of silent chains);
  audition/preview (`auditionNote` rides MIDI ch 2 so it never releases sequencer notes on
  ch 1); plugin state (`capturePluginStatesToTree`, `capturePluginState`,
  `applyPluginStateBlob`, `getProcessorFor/getPluginFor`, `setRemoteParam`,
  `onNonParamStateChanged` — fires when a plugin switches programs/patches internally so
  multiplayer resyncs immediately); recording (`setRecordArmed`, `getActiveRecordings`,
  `getRecordingPeaks` live waveform, `setAudioRecordingEnabled`); offline `renderToFile`;
  `setSamplePathResolver`. Rebuilds private: `markDirty` → `handleAsyncUpdate` →
  `rebuildChannels` / `rebuildMixer` / `recompileSequences`.
  Plugin loads are async with **epoch tagging** (`loadEpoch` stamped per entry and job;
  `finishPluginLoad` discards late instances whose target was swapped — keep the check):
  **VST3 goes to a message-thread queue** (`drainMessageThreadLoad`, one job per tick, first
  deferred ~40ms so the loading window paints — VST3 spec requires message-thread
  instantiation; off-thread creation caused MessageManagerLock ping-pong and 2.5-3x slower
  loads) and **all other formats to the background loader thread** (`pluginLoaderRun`).
  Keep that routing. Channel type dispatch in `rebuildChannels`: `"plugin"` = hosted VST,
  `"builtin"` = `PlaceholderSynth` (fixed pluginId `"builtin:piano"` for reuse-by-id),
  anything else = sampler. Sampler paths starting `builtin:` (`kick/snare/clap/hat`) are
  DSP-synthesized by `synthesizeBuiltin` with a **deterministic seed** (0x5eed + name hash)
  so every peer/render gets byte-identical audio — never reseed randomly.
- **PluginHost.{h,cpp}** — format manager + `ScanThread`. Crash safety: dead-man's-pedal
  file before each load (`scan-inflight.txt`); death mid-scan blacklists the plugin
  (`scan-crashed-plugins.txt`, path→mod-time keyed so reinstalls retry). `createInstance`
  falls back to `resolveLocalMatch` / `resolveUnverifiedMatch` (uid/name) so foreign
  projects load. Static PE probe `inspectDllExports` classifies VST DLLs (x64 `vst2`,
  x86 `vst2x86`, FL-native, not-a-plugin) without ever executing them.
- **32-bit VST2 bridge** — jBridge style: one `mikeDAW-Bridge32.exe` (x86, JUCE-free,
  source `bridge32/`, own CMake project built into `build32/` by run.ps1) **per plugin
  instance**, talking over one shared-memory mapping + two named events. Wire contract:
  `Engine/Bridge32Protocol.h` — compiled by BOTH binaries, `#pragma pack(4)`, fixed-width
  fields only, **never reorder/resize existing fields** (append only). Host side
  `Engine/Bridge32Host.{h,cpp}`: `Bridge32::createInstance` returns a
  `Bridge32PluginInstance : juce::AudioPluginInstance` so the whole engine/wrapper/
  multiplayer stack works unchanged; `Bridge32::scanPlugin` runs `--scan` as a child
  process (crash-isolated, no pedal needed). Bridged descs carry
  `pluginFormatName == Bridge32::formatName` ("VST2 (32-bit)") and must never reach the
  JUCE format manager. RPC is single-in-flight, mutex-serialized, served by a dedicated
  bridge RPC thread (audio keeps flowing while the editor is dragged); a process-RPC timeout
  outputs silence but does NOT kill the bridge — only bridge-process death is fatal. Param
  changes from the plugin editor flow back through a lock-free shm ring drained by a 30 Hz
  host timer via `sendValueChangedMessageToListeners` (never `setValue` — echo loop).
  **Editor**: jBridge style cross-process embedding. The host `BridgeEditor` sends
  `uiShow` + its peer HWND over the one-way `.ui` event channel (never waits); the bridge
  GUI thread creates the plugin editor as a `WS_CHILD` of that peer and publishes
  `editorHwnd`/`editorWidth`/`editorHeight`; the host timer adopts it via
  `juce::HWNDComponent` and resizes the editor to the published size. The bridge process
  matches the host's per-monitor-V2 DPI awareness for that reason. Host waits that run on
  the message thread go through `waitBridge` (services sent messages) — a plain
  `WaitForMultipleObjects` there can deadlock against the child window's thread. Kernel
  objects ride a job with KILL_ON_JOB_CLOSE — no orphan processes. The one-time `bridge32TsPurged` settings flag un-poisons timestamp-cache
  entries for x86 DLLs recorded before the bridge existed.
- **Builtin effects** — identified by mixer `Slot.type` **string**; the if/else chain in
  `rebuildMixer` instantiates (an unknown string silently falls through to the VST load
  path). The classes are named `<X>Processor` — the `Builtin*` names are the FILE names.
  Full table (type string → class; header = `Builtin<File>.h`):
  `gain`→`GainProcessor`, `eq`→`SimpleEQProcessor`, `softclip`→`SoftClipperProcessor`
  (all `BuiltinEffects.h`); `delay`→`DelayProcessor`;
  `delaybank`→`DelayBankProcessor` (8 chained delay/filter banks);
  `reverb`→`ReverbProcessor`; `limiter`→`LimiterProcessor` (comp + gate + lookahead
  limiter, reports latency); `multiband`→`MultibandProcessor` (shared `BandCompressor` +
  `ThreeBandSplitter` structs in `BuiltinMultiband.h`, also reused by limiter + soundbeast);
  `soundbeast`→`SoundbeastProcessor` (one-knob maximizer);
  `void`→`VoidProcessor` (shimmer-reverb FDN, freeze, ducking, presets);
  `lowlifter`→`LowLifterProcessor` (low-end enhancer);
  `parameq`→`ParamEQProcessor` (7-band parametric EQ, spectrum analyzer, optional
  linear-phase FIR designed on the message thread, never the audio thread);
  `convolver`→`ConvolverProcessor` (convolution reverb, IR presets + custom IR load/save);
  `parterre`→`ParterreProcessor` (4-bed resonant filter bank);
  `equivocal`→`EquivocalProcessor` (25-program glitch delay/reverb, editor in
  `BuiltinEquivocalEditor.cpp`); `grossbeat`→`GrossBeatProcessor` (Gross Beat-style effect:
  36 time + 36 volume envelope slots, 12 s ring buffer, playhead-synced). All derive from
  `BuiltinEffectBase`, stereo in/out.
  **State format = parameter order**: `getStateInformation` writes raw floats in
  `getParameters()` order — never reorder `addParameter` calls, append only, or every saved
  project corrupts and multiplayer desyncs. Exception: `GrossBeatProcessor` writes a
  `'GBT1'` magic, the param floats, then its slot envelopes as a ValueTree - slots are not
  parameters, so slot edits reach peers only through `notifySlotsEdited` (non-parameter
  state change). Custom editors bind `*ParameterAttachment`s and
  read `std::atomic<float>` visualizer fields written once per block on the audio thread.
  `PlaceholderSynth` (`BuiltinEffects.h`) = stock `"builtin"` instrument and missing-VST
  fallback.
- **Mp3Writer.{h,cpp}** — MP3 via Windows Media Foundation. COM/MF init is lazy on the render
  thread's **first `write()`** — do not move it to constructor or message thread.
- **PitchDetect.h** — header-only YIN (`analyze`), called only from SamplerSettingsPanel on a
  background thread.

### Model (`Source/Model/`)

- **ProjectModel** — schema + helpers (above). **Automation.{h,cpp}** — automation model
  (own section above). **History.h** — operation-based undo: `undo/redo/canUndo/canRedo`,
  `beginTransaction/endTransaction` (collapse bursts), `commitPending`, `clear`,
  `onChanged/onConflict`; plugin params (outside the tree) undo via explicit
  `recordParam(key, paramIndex, old, new)` + `applyParam` callback — `pluginState` is
  excluded from diffs entirely. **Scales.h** — 13 scale defs, `keyInScale`,
  Krumhansl-Schmuckler `detectKey`. **Leaderboard.h** — per-project time tracking
  (`Project.createdEpoch/secondsWorked` → `recordSave` → `leaderboard.xml`, "minutes
  wasted"; skips autosave paths).

### Networking (`Source/Net/`)

- **Session.{h,cpp}** — multiplayer core; the wire protocol is the `MsgType` enum in
  `Session.h`: **31 types**, `msgHello=1` … `msgRejected=31` (snapshot/delta/param/plugin-state/
  transport/cursor/presence, audio-file offer→request→transfer with persistent cache, chat +
  chat images, reactions, avatars, plugin inventory, render lock, session end, host kick).
  `msgPing` is retired but its slot is reserved — never renumber or reuse any slot. The joiner
  sends `Session::protocolVersion` in `msgHello`; a mismatch is answered with `msgRejected`
  (client `onJoinRejected`) — bump it whenever an existing payload format changes. TCP port
  is fixed `Session::defaultPort=41234`, magic `0xda3b71c4`. All socket writes drain through
  the single `SenderThread` FIFO — never send synchronously from the message thread or
  engine callbacks (`sendMessage` blocks and interleaves). Host-only `kickPeer(peerId)` →
  client `onKicked`.
- **RelayTunnel.{h,cpp}** — TURN-style fallback when a joiner can't reach a host directly:
  session bytes tunnel through the relay server. Control rides RelayClient's JSON connection
  (`joinreq` → `joinok{token}` to joiner / `bridgereq{token}` to host); bytes ride a
  **separate** raw TCP connection to the same relay port whose first line is
  `{"t":"bridge","token","role"}` — after that line it is a pure byte pipe, never re-enters
  the JSON parser. Host side: `RelayTunnel::startHostBridge(...)` dials
  `127.0.0.1:<sessionPort>`; joiner side: `RelayJoinTunnel` binds a loopback listener and
  the local `Session` connects to it as if it were the host. Wired in
  `MainComponent::beginRelayJoin` (relay path preferred, direct probe fallback, 12s timeout).
- **LanDiscovery.h** — UDP 41235: host beacons (`OPENDAW1|name|tcpPort|people`), broadcast
  query + /24 sweep, LAN presence (`ODPRES1`), friend requests/responses
  (`ODFREQ1`/`ODFRSP1`), invites (`ODINV1`). Sender IP from datagram source, not payload.
- **FriendsManager.{h,cpp}** — persistent friends + pending requests (friends.xml), LAN+relay
  presence merge (`viaRelay` flag; `lanAddressFor` never returns a relay-observed public
  IP), online hysteresis (online ≤7s beacon age, offline after 30s), transition callbacks.
  Message-thread only, no locks. Identity = `AppSettings::localUserId` (random int64,
  minted once, doubles as `Session::localPeerId` — never regenerate it).
- **RelayClient.{h,cpp}** — persistent client to the central relay (TCP 41236, newline JSON,
  protocol doc in header; presence every 5s — must stay under the 7s online window and the
  server's 75s idle timeout). Server source `server/opendaw-relay.py` (asyncio; state.json +
  avatars/ next to the script; offline friend-request mailbox; 20s unpaired-bridge timeout),
  deployed as systemd unit `opendaw-relay` on ubuntu@148.113.175.195 (redeploy: scp the .py to
  `/home/ubuntu/opendaw-relay/`, `sudo systemctl restart opendaw-relay`). Client and server are
  hand-mirrored JSON — any new message must be added to both.
- **InviteCode.h** — invite codes, UPnP mapping, `parseJoinText`, `probeEndpoint`.
  **DiscordPresence.h** — Rich Presence on a dedicated thread (named pipe discord-ipc-0..9).
  **SyncDebug.h** — opt-in sync logging, flag file `%APPDATA%/mikeDAW/sync-debug.enabled`
  → `sync-debug.log`; `synclog(msg)`, `SyncDebug::sum` blob checksum.
- **Updater.{h,cpp}** — authenticated auto-update: on authorized launch fetches `latest.json`
  from the update server (TCP 41238, token-gated) and prompts to download a newer build.
  Server `server/opendaw-update.py` (systemd `opendaw-update` on ubuntu@148.113.175.195);
  push via `push-update.ps1`. The installer download carries the cached license token
  (`?token=&sig=`); the server verifies the Ed25519 signature + allowlist before serving (403
  otherwise).

### Import / export (`Source/Import/`)

- **FLPImport** — `importFile(flp, host)`, FL 20.0.5–26: rack/patterns/playlist/mixer; VSTs
  matched against the cache; FL-native plugins → warnings, except the ones **FLNativeFx**
  converts both ways (Fruity Parametric EQ 2 <-> `parameq`, Fruity Soft Clipper <->
  `softclip`, Gross Beat <-> `grossbeat`; layouts in `.knowledge/FLP.md` "FL-native effect states"). **FLPExport** —
  `exportAsZip(project, zip)`; `Vst3ClassId::extractClassId` writes real VST3 TUIDs; VST2 state
  round-trips via `Vst2Fxb.h` (21-byte Fruity Wrapper frame + big-endian CcnK/FBCh fxBank).
  **MidiImport** — `applyToChannelSeq(file, pattern, seq)`. Iteration tooling (gitignored,
  local): headless `mikeDAW.exe --export-flzip <in.odaw> <out.zip>` (writes
  `<out>.zip.result.txt`), `mikeDAW.exe --import-flp <in.flp|zip> <out.odaw>` (writes
  `<out>.odaw.result.txt` with summary + warnings), `.knowledge/flpdump.py` (FLP event dumper),
  `.knowledge/verify_export.py <zip> [odaw]` (structural + state byte-match verifier, also
  accepts a raw truth `.flp`), `.knowledge/FLP.md` (byte-level catalog). Truth FLPs live under
  `C:/Users/mikey/Downloads` (`test/test.flp`, `holy sheet/`, `FL Studio FLP references/`).
- **easyFLP (child project)** — `D:\GitHub\easyflp`, born from the FLP
  research done here: a standalone Rust viewer and backporter (FL v21/24/25 → 20.8). Its
  `FORMAT.md` is the byte-level FLP format knowledge base. Any FLP format discovery in either
  repo (new opcodes, record layouts, version quirks) must be synced to the other — `FORMAT.md`
  there, FLPImport/FLPExport here.

### Debug subsystem (`Source/Debug/`)

Dear ImGui diagnostic overlay + process-wide perf monitor. Toggle: **Ctrl+Shift+D**
(`Action::toggleDebugOverlay` → `MainComponent::toggleDebugOverlay`, lazy-constructed). It is
a standalone native `DocumentWindow` (`DebugOverlayWindow`) with its own OpenGL context — NOT
a desk `FloatingPanel`, not part of `togglePanel`.

- **ImGuiCanvas.{h,cpp}** — `juce::Component` + `OpenGLRenderer` hosting real Dear ImGui
  (vendored `external/imgui`, OpenGL3 backend). ImGui draws on the **GL render thread**;
  input events cross from the message thread through a mutex-guarded queue. Never call ImGui
  from message-thread code.
- **DebugOverlayUI.{h,cpp}** — the tabs: Main (drawOverview) / Audio / Threads / Hitches /
  Memory / Flags / Report. Reads `PerfMonitor`, `engine.getAudioPerfStats()`,
  `engine.getPluginPerfStats()`, and a mutex-guarded 2s project snapshot. New tab = a
  `BeginTabItem` in `draw()` + a `drawX()` method.
- **PerfMonitor.{h,cpp}** — always-on (constructed in MainComponent ctor): 1s resource
  sampler thread (CPU/memory/handles/GDI/per-thread), 25ms heartbeat + watcher for
  message-thread **hitch detection** (>250ms gap captures a stack via StackWalk64; all
  dbghelp calls serialize through the single `dbghelpMutex`; nothing allocates between
  Suspend/ResumeThread — do not reorder). Reports → `%APPDATA%/mikeDAW/PerfReports/`.
- **DebugFlags.h** — global `inline std::atomic<bool>` runtime flags
  (`disableSpectrum`, `disableWaveformThumbnails`, `logHitchesToFile`), toggled in the Flags
  tab, consumed at the target sites. New flag = atomic here + checkbox in `drawFlags` +
  consume where needed.
- Engine probes: `AudioPerfStats` (callback load/xruns/lock-wait, 512-entry history) always
  published lock-free; per-plugin `ProcessorPerfStats` gated by
  `setPluginPerfMonitoringEnabled` — enabled only while the overlay is open (MainComponent
  toggles it in lockstep with visibility). Read via atomic getters, never under `engineLock`.

### Shared UI infrastructure

- **FloatingPanel.h** — draggable internal window on the `desk`; native-HWND clipping so
  overlapping VST editors don't fight; peer badges (`setPeerBadges`).
- **Theming** — `Colors` = 53 registered `inline juce::Colour` slots (`AppLookAndFeel.h`);
  `ThemeManager` writes them from `theme.json` (hot-reload, 500ms mtime check) via
  `registry()`, fires change events; global `Adjustments` (hue/sat/brightness/contrast/text)
  post-process every colour. Animated effects live in `Fx` floats registered in
  `effectRegistry()` (glow intensities); the master `Fx::glowEnabled` persists in
  mikeDAW.settings (key `fxGlowEnabled`), deliberately NOT in theme.json so it never travels
  with shared themes. `ThemeWindow.h` = Preferences tab; `.odawtheme` presets. Every Colors
  slot must appear exactly once in `registry()` or it silently won't theme. Text over
  channel/pattern colours uses `Colors::contrastText(bg)` (black/white by luminance);
  `clipTone`/`clipHeader` tame over-bright imported colours. Panel titles are
  `Typography::caps(13.5f)` — keep new headers at that size.
- **AppSettings.h** — typed accessors over `%APPDATA%/mikeDAW/mikeDAW.settings`. Current
  surface: startup template + default BPM, `projectsFolder`, autosave (enabled/minutes/folder), display name +
  avatar + `localUserId`, recent projects (max 8), process priority, `followDefaultOutput`
  (Windows default-device following via `DeviceChangeRelay`), `smartDisable`, `uiScale`
  (restart-to-apply), scale-highlight, recording flags (recNotes/recAudio/recStartsPlayback/
  recDisarmOnStop/recQuantizeStart/recQuantizeEnd), `discordRpc`, relay
  (enabled/host/port), `mixerRackMode`. New setting: add here, expose in
  `PreferencesWindow.h` (two tabs: Settings — Audio Device / General / Online sections — and
  Theme).
- **KeyBindings.h** + **KeyBindingsWindow.h** — `Action` enum + `Entry` table with defaults;
  user-rebindable, persisted. Actions include playPause(Space), stop(Return), save/saveAs,
  undo/redo, duplicate(Ctrl+B), copy/paste, select/deselect-all, delete, legato(Ctrl+L),
  arpeggiate(Alt+A), strumize(Alt+S), randomizeNotes(unbound), octave/semitone moves, panel
  toggles, fullscreen, typing keyboard(Ctrl+T), debug overlay(Ctrl+Shift+D). New shortcut:
  `Action` + `Entry` + case in `MainComponent::handleAction` (context-aware: some actions
  route to playlist vs piano roll by `activePanel`). Appears in the rebinding UI
  automatically.
- **Multiplayer social** — `ChatPanel`, `ActivityFeed`, `FriendsPanel`,
  `ReactionMenu`/`ReactionLayer` (middle-click anywhere), `ToastLayer`
  (`post(msg, Kind, holdMs)` — use for any transient warning), `PeerCursorLayer`; AFK
  detection (3 min idle broadcast), peer viewport badges on panels.
- **Small widgets** — `ValueKnob.h` (use for every bound knob; right-click
  copy/paste/set/reset + "Create automation clip" via `onCreateAutomationClip`),
  `WheelComboBox.h`, `ZoomScrollBar.h`, `SmoothValue.h`, `GifImage.h`, `DarkTitleBar.h`,
  `Dialogs.h`, `MissingFilesDialog.h`, `JoinSessionWindow.h`.
- **CrashHandler.h** — minidumps + 20s hang watchdog → `%APPDATA%/mikeDAW/CrashLogs`.

## Engine threading model (the invariants)

- The audio callback takes ONE `CriticalSection` (`engineLock`) for the whole block. Accepted
  tradeoff — but the **message thread must never hold `engineLock` during slow work** (no
  plugin create/destroy, no file I/O, no heavy allocation under it).
- Rebuilds are strictly 3-phase (`rebuildChannels` / `rebuildMixer`): **A** create/load
  OUTSIDE the lock (reuse existing entries by id/uid — recreating an instance loses state and
  glitches) → **B** swap vectors under the lock, stale entries to a graveyard → **C** notify
  (`onPluginRemoved` closes editors) and destroy OUTSIDE the lock. Keep this shape.
- Continuous params (volume/pan/mute/BPM/routing/stereoSep) mirror into `std::atomic`s
  directly in `valueTreePropertyChanged` — no lock, no rebuild. Structural changes:
  `markDirty()` + `AsyncUpdater`; sequences recompile into flat sorted event vectors.
  `recompileSequences` swaps note events, slides, audio clips AND automation lanes together
  under `engineLock` as one atomic group — never mutate them in place.
- UI never calls the engine from paint/mouse except lock-free atomic getters and queued
  requests (`auditionNote`, `seekTicks`, `previewFile`). Perf stats and automation overrides
  are read via atomic getters, never under the lock.
- Around any hosted `processBlock`: hold `proc.getCallbackLock()` and check `isSuspended()`.
  Prepare via `preparePlugin()` (suspend → setRateAndBufferSize → prepare → unsuspend).
- `renderToFile` takes `engineLock` **per block**, never across the render — holding it froze
  the app and starved the multiplayer TCP relay. It also force-disables the metronome for
  the pass (monitoring-only, never rendered).
- Recording jobs (`RecordJob*`) are published/nulled only under `engineLock`; the audio thread
  writes a lock-free FIFO that **drops whole blocks on overflow** — never block or allocate
  on the audio thread. Same for `noteOnFifo`, the spectrum snapshot, and the perf stats.
- `ParamListener` callbacks can fire on ANY thread — respect the `suppress*Broadcast` guards
  when applying remote changes.

## Plugin keys

Every hosted processor is addressed by string key: channel instrument = `"c<channelId>"`,
mixer effect = `"m<trackIndex>s<slotUid>"`. Resolve with `engine.getProcessorFor(key)`
(includes builtins) or `engine.getPluginFor(key)` (real instances only). Automation targets
use a different grammar (see Automation clips) — do not mix them up.

## Plugin state — deliberate asymmetry

`pluginState` (base64 blob in the tree) is applied **only at instance creation**
(load/join/rebuild). The engine's tree listener **ignores** `pluginState` changes — otherwise
capture-on-save would echo into the plugin and multiplayer would loop. Live state travels as
`(key, paramIndex, value)` param messages; full blobs re-broadcast via `msgPluginState` when
an editor closes (`PluginWindowManager::onWindowClosed`) or a plugin flips programs/patches
internally (`onNonParamStateChanged`). Call `engine.capturePluginStatesToTree()` before
serialising (save + snapshot already do). `History` never records `pluginState`; plugin-param
undo goes through the explicit `History::recordParam` path instead.

## Multiplayer (Session)

Host-authoritative star over `juce::InterprocessConnection` (TCP 41234, callbacks on message
thread; outbound drains through `SenderThread`). Flow: Hello → Snapshot (full tree XML incl.
fresh plugin states) → deltas/params/state/transport.

Tree deltas are **identity-addressed** (`TreeSync`, not `juce::ValueTreeSynchroniser`): every
path step names a node by its identity property (`Model::identityKeyFor`: Channel/Pattern/
AudioFile/MidiFile `id`, ChannelSeq `channelId`, Note/Point/Clip/Slot `uid`, Track/
PlaylistTrack `index`, section containers by type) and only falls back to an index for
nodes without one. Index paths desync the moment two peers append to the same parent.
Note/Point/Clip uids are minted lazily by `Model::ensureIdentities` (History stamps every
local add, TreeSync stamps before encoding, the host stamps the whole tree before any
snapshot). A remote add whose identity already exists is merged for ChannelSeq (steps OR,
notes by uid) and dropped otherwise. New node type with children edited concurrently = give
it an identity property and register it in `identityKeyFor`. Loop-freedom rules:

- `applyingRemote` (ScopedValueSetter) suppresses the local synchroniser while applying a
  remote delta; the **host relays** every message to all peers except the sender.
- `engine.suppressParamBroadcast` / `suppressTransportBroadcast` wrap remote applies;
  `Session::remoteApplyFlag` tells `History` to skip recording.
- `broadcastParam` may be called from the **audio thread** — it queues under a mutex and
  flushes via AsyncUpdater, coalescing per (key, index). Never send network messages
  synchronously from engine callbacks.
- Transport cmds: 0=stop, 1=play(+seek), 2=seek, 3=pause.
- Conflicts: last-writer-wins per property. No locking protocols.
- Files transfer by **offer → request-if-missing → data** with persistent cache
  (`cache-index.xml`); never push bytes unsolicited.
- Smart VST Match: peers exchange inventories (`msgPluginInventory`) so pickers grey out
  plugins not everyone has (`isPluginCompatible`).
- Identity: `localUserId` from AppSettings (= `localPeerId`) + `displayName`. Cursors,
  toasts, chat, reactions, avatars key off the peer id.
- Join paths: ALL joins probe direct TCP endpoints first; friend joins fall back to the
  relay tunnel only when no endpoint answers (see RelayTunnel). A relay join is confirmed
  by `onSnapshotApplied`, not by the loopback connect (which always succeeds); a 10s timer
  tears down an unpaired bridge. `msgKeepalive` heartbeats every ~2s; a peer that has
  proven it speaks keepalive is dropped after 60s silence (60s, not less - file transfers
  monopolise the sender FIFO and arrive as one framed message). Host can kick
  (`kickPeer`); host disband = `msgSessionEnded`.

## Undo (operation-based, NOT snapshot)

**Never** undo by restoring a whole-tree snapshot in a session — it wipes peers' concurrent
edits and desyncs everyone. `History` keeps a shadow baseline; after ~350ms of quiet it diffs
baseline vs live tree into minimal reversible ops (property old+new, child add/remove with
XML, children aligned by `Model::childIdentity`; pure reorders are not recorded). Ops address
nodes by `Model::TreePath` (identity), so a peer inserting a sibling cannot redirect an undo.
Undo/redo apply the inverse **as a normal edit**, so it flows through TreeSync. Remote edits
(applied while the `suppress` atomic is true — set it around any remote apply) are mirrored
into the baseline immediately, so a peer's edit landing mid-burst is never recorded as yours.
Conflicts are soft: undo runs, fires `onConflict` (toast), last-writer-wins. Wrap multi-edit
gestures in `beginTransaction()/endTransaction()`. Entry cap 200. History is cleared on every
load path and on `onSnapshotApplied`.

## Missing plugins / placeholder synth

A channel whose `pluginDesc` can't load here gets `PlaceholderSynth` so the part stays audible
on every peer. Engine sets per-channel `placeholder` flag (`isChannelPlaceholder`), fires
`onPluginMissing`; UI toasts once. `pluginDesc`/`pluginState` stay untouched in the tree so
the real VST loads for whoever has it. Never overwrite `pluginDesc` with the placeholder.
(Deliberate `"builtin"` channels also use PlaceholderSynth but with `placeholder=false` — no
warning.)

## Recording

Mixer inserts carry `audioInput` ("N" mono / "N,M" stereo / "" none) + `recArm`; a track with
input monitors live through its chain. Armed + record + playing = capture to 24-bit WAV via
lock-free FIFO + dedicated writer thread (never `engineLock` during file I/O); on stop the
take becomes a playlist audio clip, start compensated by in+out latency. Typing-keyboard notes
record through `engine.onAudition` into the current pattern. Record-button right-click = record
options (persisted in AppSettings; the Automation and Clips items are deliberately disabled —
not implemented). In multiplayer the clip node syncs instantly with `pending` + `recordedBy`;
the WAV is offered only after the recorder clears `pending`; peers render pending clips
dimmed/silent. Live mic mirroring is deliberately NOT implemented.

## Common recipes

- **Persistent/synced feature** → property/child in `ProjectModel.h`. UI writes the tree,
  tree listener updates the widget with `dontSendNotification`. Engine: continuous → atomic
  in `valueTreePropertyChanged`; structural → `markDirty`. Save/load/sync/undo automatic.
- **Builtin mixer effect** → class `<Name>Processor : BuiltinEffectBase` in
  `Builtin<Name>.{h,cpp}` (model on `BuiltinDelay`). Four coordinated edits or it
  half-works: (1) type-string branch in `AudioEngine::rebuildMixer` + header include;
  in `Mixer.cpp`: (2) `extras.push_back({id, "Add <Name>"})` in `showAddEffectMenu`,
  (3) the id→type-string mapping in the picker callback, (4) the `effectDisplayName`
  entry. Params append-only forever (state = param order).
- **Automatable target** → extend `Automation::Target`/`parseTarget`/`denormalize` AND the
  `AudioEngine::applyAutomation` dispatch, then wire a knob's `onCreateAutomationClip`.
- **Keyboard shortcut** → `Action` + `Entry` in `UI/KeyBindings.h`, case in
  `MainComponent::handleAction`. Appears in the rebinding UI automatically.
- **Network message** → append to `MsgType` in `Session.h` (**never renumber** — breaks
  cross-version sessions), send + handle, host relays, wrap remote apply in suppress guards.
  Changing an EXISTING payload format → bump `Session::protocolVersion`.
  Relay-server messages additionally need matching handling in `server/opendaw-relay.py`.
- **Theme colour** → `inline juce::Colour` in `Colors` + slot in `ThemeManager::registry()`.
  Animated effect intensity → float in `Fx` + slot in `effectRegistry()`.
- **Setting** → accessor in `UI/AppSettings.h`, control in `PreferencesWindow.h`.
- **Debug flag / overlay tab / engine metric** → see Debug subsystem section.
- **Transient warning** → `ToastLayer::post`; no dialogs for non-blocking info.
- **Ship an update / bump the version** → bump the version in `CMakeLists.txt` in TWO places,
  kept equal: `project(mikeDAW VERSION x.y.z ...)` (line 2) and the `VERSION x.y.z` arg in
  `juce_add_gui_app` (line 44). The `juce_add_gui_app` one is what drives
  `JUCE_APPLICATION_VERSION_STRING` (window title + the update check) and the exe
  `ProductVersion` (which `release-private.ps1` reads for the installer). Then run `release-private.ps1`
  (Release + Themida + Inno → `dist\mikeDAW-Setup.exe`), then
  `.\push-update.ps1 -Installer dist\mikeDAW-Setup.exe -Version x.y.z -Notes @("...","...")` from a
  PowerShell prompt, one string per change - the client bullets them (uploads the
  installer + rewrites `latest.json` on the update server). Clients on older builds prompt on
  their next authorized launch. See SECURITY-LICENSING.md "Auto-update".
- **Public release (default; no license, no Themida, no updater)** → `release.ps1`. Builds
  `build-public\` with `-DOPENDAW_PUBLIC_BUILD=ON` into `dist\mikeDAW-Setup.exe`. The option
  defaults ON, so `run.ps1` dev builds are public too; the licensed flow is
  `release-private.ps1` (forces OFF for its build, then restores ON). In the public build
  `License.cpp`/`Updater.cpp` are not compiled and `License::shouldDegrade()` is
  `constexpr false`. It also contacts no server: `RelayClient::start`/`enqueue` and
  `RelayTunnel::startHostBridge` are no-ops, `relayHost` is empty, and invite codes take the
  public IP only from the router's IGD (no ipify). LAN, direct IP, UPnP and NAT-PMP stay. New code that touches `LicenseManager`/`Updater` must sit under
  `#if ! OPENDAW_PUBLIC_BUILD` or the public build breaks.

## On-disk locations

`%APPDATA%/mikeDAW/`: `mikeDAW.settings`, `plugin-cache.xml` + `plugins-unverified.xml`,
`scan-crashed-plugins.txt` + `scan-inflight.txt`, `scan-timestamps.txt`, `theme.json` +
`Themes/`, `templates/`, `leaderboard.xml`, `friends.xml`, `avatar.png`, `CrashLogs/`,
`PerfReports/` (perf reports + hitches.log), `sync-debug.*`, multiplayer file cache +
`cache-index.xml`. Projects: `.odaw` (+ sibling `<name> samples/` bundle folder). Presets:
`Documents/mikeDAW/Presets/<plugin>/*.odawpreset`. Mixer chains: `.odms`. Themes:
`.odawtheme`. Recordings: `%TEMP%/mikeDAW Recordings`, overridden at runtime to
`<autosave folder>/Recorded`.

## Known gaps

Automation clips cover only track/channel volume + pan — no plugin-parameter automation, no
automation *recording* (the record-menu item is disabled). 4/4 only. Multiplayer has no
auth/encryption. In-process plugin scan can still crash on a hostile plugin (mitigated by
pedal/blacklist, not eliminated). FLP import converts only Parametric EQ 2 and Soft Clipper among FL-native effects
(Reeverb 2 field layout unknown). The 32-bit bridge covers VST2 only (no 32-bit VST3), and a
dead bridge process stays silent until the plugin is reloaded (no auto-restart).

## Code invariants (moved out of source comments)

Reasons the code alone does not show. Check these before changing the listed code.

**Engine/AudioEngine.cpp**
- No allocation or lock on the audio path. The only shared state is two atomics and the work list.
- Reserve headroom so a crossfade overlap never reallocates on the audio thread.
- Stop loader threads before plugin teardown. A late completion may touch a dead host.
- Explicit stop before destruction. Workers dereference channels declared after the pool.
- Hold the callback lock during plugin state serialisation. Otherwise plugin state races the audio thread.
- `engineLock` is taken per block, never for the whole pass. Otherwise the session starves.
- `setNonRealtime` must precede `prepare`. Otherwise streaming VSTs drop or delay onsets.
- `prepare` runs outside `engineLock` and the callback lock, because VST3 prepare takes the message lock.
- Disk IO stays outside `engineLock`. A filled block is render-thread local.
- Epoch mismatch means the target changed during load. Discard the late instance.
- Lock order is lifetime, then engine. The displaced instance dies after both are released.
- Built-in channels use a fixed id, so reuse by id keeps the synth instance across rebuilds.
- Create real VSTs async. A heavy plugin blocks for seconds and freezes every peer.
- A redundant write must not reach `setSongMode`. It arms a fade and rewinds the playhead.
- Browser bookkeeping only. A catch-all rebuild restarts every clip voice.
- Apply device changes outside `engineLock`. Preparing under it stalls the app.
- `auditionOnly` gates preview voices, so an audition note-off never silences a sequencer voice.
- Changed clips crossfade with a matching fade-in. Edits never stack copies and double loudness.
- After a discontinuous jump, scan from the floor of `startTick`. Otherwise events on the landed tick never fire.
- The realtime-safe path walks only the flat sorted note vector, never the tree.
- `recompileSequences` drops inner offs, so each open key is tracked once.
- Clips carry their own gain and pan. Channel volume and pan do not apply to them.
- Route only into a later insert. Tracks process in index order.
- Audition notes ride their own MIDI channel, so the channel 1 all-notes-off never releases them.
- Stop flush ramps output to silence, resets plugins, then ramps back. A hard cut clicks.
- Publish under `engineLock`, so the audio thread only sees a fully constructed job.

**Engine/AudioEngine.h**
- Program and patch changes skip parameter notifications, so the `onNonParamStateChanged` hook drives the resync.
- `pluginDesc` and `pluginState` stay untouched, so a peer with the plugin still loads it.
- Must stay in lockstep with the `configureVoicePlayback` rate math, or waveform and audio diverge.
- A volume below 0 means no override. A pan below -1.5 means no override.
- Per-channel scratch lets parallel workers touch only channel-owned state.
- `createInstance` blocks for seconds on heavy VSTs, so load off the message thread.
- VST3 instances must be created on the message thread. Off-thread creation stalls the lock.
- Reset takes each processor callback lock. Flush on the message thread, not under `engineLock`.
- The callback lock is always taken before `engineLock`, never on the audio thread.
- Dropping the last sample reference on the audio thread would free megabytes in the callback.
- Phase C can pump the message loop, so a rebuild must not re-enter through the `AsyncUpdater`.

**Engine/PluginHost.cpp**
- Plugin loading shares global state. Parallel scans deadlock the UI.
- Read the export table from file bytes, so no plugin code runs.
- PE32+ export directory sits at optional header offset 112. PE32 sits at 96.
- VST3 factories use COM during enumeration, so initialise the apartment on this thread.
- Stamp the buffer until the lists save. A crash may lose stamps but never orphan a plugin.
- Lists save before the compacted stamp log. Otherwise a crash orphans scanned plugins.
- Consume the pedal before the load. A crash in a constructor then falls back to the placeholder.
- Several loads in flight cannot say which one died, so only a lone entry earns a strike.
- `resolveForLoad` runs on the loader thread, so listeners must hop to the message thread.
- Bridged plugins never reach the JUCE format manager. Scan them out of process.
- A bridged copy lives under Program Files (x86). That path is the only signal that survives reinstall.
- Unverified entries all carry uid 0, so the filename stem is the only key.
- Cross-machine matches stay within one format. VST2 and VST3 state are not interchangeable.
- `resolveForLoad` can reclassify a 32-bit VST2 as bridged. That must never reach the JUCE format manager.
- A C++ exception surfaces as SEH 0xE06D7363 and is a plain load failure, not a fault.
- A stack overflow consumes the guard page. Restore it, or the next overflow kills the process.
- Keep the SEH wrapper apart from the C++ code. A function with `__try` cannot unwind C++ objects (C2712).
- One strike is only a warning, because the user may have killed a merely slow load.
- One lock covers the pedal and blacklist, because three threads write the same two files.
- The JUCE VST2 module list has no lock, so background threads must not instantiate VST2 concurrently.
- Locks are taken off the message thread only, because VST2 teardown uses `callSync` and would deadlock.

**Engine/PluginHost.h and CrashHandler.h**
- Plugins hit benign CRT parameter checks. Only a flood is fatal.

**Net/Session.cpp and Session.h**
- Host capacity is enforced at `msgHello`, so a ghost connection never blocks a rejoin.
- Must be set before hosting or joining, because a stable id keeps rejoin handling exact.
- juce posts `connectionMade` after the handler returns. The peer is registered before its callbacks.
- The weak reference master is created lazily and not thread-safely, so force it here.
- Stop the sender before any peer is freed, so it cannot send on a socket being torn down.
- Tell every client the session is disbanding, then give the sender a window to flush before teardown.
- Capture the peer id, not the peer pointer. A freed address may be reused.
- Calls from connection callbacks defer deletion.
- Release the render lock if the dropped peer held it, so nobody stays blocked.
- A same-launch reconnect neutralises the ghost identity, so its late `connectionLost` cannot wipe the rejoined state.
- `sessionFolder` is relative with forward slashes, so Recordings round-trip.
- `enqueue` returns immediately. No big load or send runs on the message thread.
- A recording awaiting confirm must never auto-share. Check it before the dedupe insert.
- A fresh full tree is deterministic where deltas are not, so the client replaces its project.
- Load and frame audio payloads off the message thread.
- Snapshot the live links, then send outside `linksLock`, so joins never wait on a socket write.
- Run outside the `applyingRemote` scope, so the merge flows back as a normal edit.
- Never hand the joiner a ghost of itself. It could receive stale state and purge the host.
- Only the first post-snapshot delta being unreadable means an older wire format.
- Cache the newest plugin blob before any apply, so the instance applies it when it appears.
- A relaunched rejoin gets a fresh id, so the stale entry is dropped by name.
- The stale plugin inventory is kept on purpose. Names can collide and it is never resent.
- Deferred teardown avoids deleting the connection from inside its own callback.
- `sendMessage` blocks and takes only a shared lock, so every socket write goes through one sender thread.
- A peer can be deleted asynchronously, so queue items never hold a raw peer pointer.
- The sender pointer is nulled under `sendLock`, so in-flight sends finish before teardown.
- Received audio files are written off the message thread, so big blobs do not stall the UI.
- Async param notifications echo the remote change, so outgoing broadcasts are dropped during the cooldown.
- A poll needs the same changed hash on two ticks, so a mid-load blob is not broadcast.
- The destructor runs after the UI members are gone, so UI callbacks must be dropped there.
- `volume` stays on the wire for framing compatibility and is unused.
- A key applied remotely and untouched locally only absorbs the new hash, so plugin fallout cannot ping-pong.
- Bump when an existing message format changes, so mismatched builds are refused.
- The lock can be force-released when that peer disconnects, so it is tracked.

**UI/MainComponent.cpp and MainComponent.h**
- Callbacks may fire on the audio or plugin UI thread, so hop to the message thread before touching the socket.
- A late-loaded instance applies the newest cached remote state, not the creation snapshot.
- Only broadcast state the user touched since the last remote apply.
- Pre-join history entries address a tree that no longer exists.
- Undo applies via `setValueNotifyingHost`, not `setRemoteParam`, so the change broadcasts like a local edit.
- Bracket the preview session, so undo treats it as one action.
- An edit made while the write is in flight legitimately re-dirties. Do not clear it here.
- Enter modal state takes keyboard focus, so Return and Escape fire save or cancel.
- Everything is async with no modal loops.
- The MP3 writer opens lazily on the render thread.
- Always release the session render lock first on every exit path.
- MP3 has no pre-flight, so this is where Media Foundation trouble shows.
- Lock everyone, including us, for the duration of the render.
- `joinHost` is not thread-safe. Probe on the message thread only.
- Comparing names first stops `setAudioDeviceSetup` own change from looping.
- A client never learns the host relay uid, so it has nothing for the join secret.
- Song mode is synced. Auto-flipping it from one peer's panel would ping-pong the transports.
- A lane claimed by a concurrently recording track is not in the tree yet, so the occupancy scan cannot see it.
- Open plugin editors are separate top-level windows, so veil them too.
- The destructor drains the queue, so a quit right after a save cannot truncate the file.
- Native plugin windows outlive the hidden main window and repaint white during teardown. Destroy them first.
- The unsaved-changes guard runs before loading, so a double click cannot discard live work.
- The lane cache is keyed by mixer track, so the lane chosen at start is reused at commit.
- Declaration order is construction order, and destruction runs in reverse, so plugin windows close first.
- The render lock grays out the window and swallows input, so nobody edits mid-render.
- Guards against a second join while endpoints are probed on a background thread.
- `gl` render thread reads these every frame. Declared last, so they are destroyed first.

**UI/Playlist.cpp and Playlist.h**
- A menu with no parent becomes its own desktop window. Parent it to the top level to skip the extra HWND.
- The gutter is always reserved, so rows do not reflow when the list grows past the viewport.
- Never smaller than the viewport, so wheel zoom works over empty space.
- Paint runs once per dirty rectangle. Anything that walks the tree per clip must be memoised.
- Enter the loop at the first line inside the dirty rect, rather than scanning the song.
- The cull margin covers the selection glow, which spills past the clip rect.
- Snap outward to a fixed grid. A cache window keyed on start time would miss every frame.
- The float rect keeps the clip half-pixel origin from rounding down.
- Repaint the union of the old and new comet x, widened to cover the trail.
- A loop wrap or big jump uses two cheap bands instead of one huge span.
- One rect covers every sparkle, because each dirty rect re-runs paint.
- Advance sparkles by real elapsed time, so motion is frame-rate independent.
- Captured once at mouse-down, because resolving it can decode the sample. Only ever call from mouse-down, never from paint.
- The old factors are already folded into `stretchBeats`. Leaving them set would scale twice.
- The record start chooses the lane once, so commit and overlay reuse it.
- Rescans the decoded buffer for silence, so it must not run per clip per paint.
- Bumped by the value-tree listeners, so no edit outlives a cached rect.

**UI/PianoRoll.cpp and PianoRoll.h**
- At least as big as the viewport, so wheel events over empty space zoom instead of scroll.
- Fixed for the whole zoom animation. Re-deriving it per step made the view slide.
- Bracketed by an undo transaction, so the whole session collapses to one action.
- Reinsert in ascending original index order, so each lands at its exact slot.
- A full-window overlay survives focus loss, which a callout box would not.
- Open the undo transaction before the dialog constructor removes the source notes.
- Matches the engine pattern loop exactly, or the dim and wrap disagree with the audible loop.
- A peer stop lands here too, so broadcasting this seek would tug their playhead back.
- Deferred, because `setTarget` can write the tree and this runs inside a tree callback.
- The arpeggiator preview churns the tree. Bracket it, so undo treats it as one action.
- `setViewPosition` re-enters `syncZoomBar` with half-applied state.
- The clipboard holds plain note data, not ValueTrees. Starts are relative to the origin.

**Main.cpp**
- A second instance would fight over the exclusive audio device, so double-clicks go to the running one.
- A headless export opens no device, so it runs alongside a live session.
- Apply the saved UI scale before any window exists. The settings file object does not exist yet.
- Plugin destructors are a common crash spot, so the watchdog outlives teardown.
- DWM composites off-thread, so let it pick up the frame before the message thread goes away.
- Layered, not opaque, because DWM composites an opaque window as a white frame before paint.
- The outcome is also written beside the zip, because a GUI binary has no attached stderr.
- Reads the registered command first and only rewrites when it differs, so an exe move self-heals.

**Remaining files (moved from source comments)**
- `TransportBar`: `isHost` labels the button End because the host disbands for everyone. Both listen prefs are local only and not broadcast. Route intent through the tree so it persists and syncs. Disarm on stop only on the playing-to-stopped edge. Do not gate on `isRecording`, because the edge fires once.
- `ThemeManager`: every inline colour in `Colors` must appear in `registry()` exactly once. Notify secondary observers without stealing `onThemeChanged`. Glow lives in settings, not the theme, so it never travels with a shared theme file.
- `LoadProgress`: not theme colours, because the theme writes on the message thread, which may be wedged. Atomic and not lifecycle-guarded, because `startWindow` joins this thread while holding that mutex. Double-buffered, because GDI text flickers at about 30 repaints per second.
- `LanDiscovery`: bind to `0.0.0.0` so subnet-directed broadcasts arrive on any adapter. Providers are message-thread only. Presence replies use the payload cached by `sendPresence`. Send one directed broadcast per interface, because the limited broadcast is unreliable on Windows. Answer discovery unicast.
- `PluginWrapper`: a click on the chrome gives JUCE key focus, so global shortcuts bubble up. A plain delay can fire before the first paint, so wait for the spinner to show. Keep the audio thread out of `processBlock` while state swaps.
- `DarkTitleBar`: works on Windows 10 1809 and later, no-op elsewhere. Attribute 20 is `DWMWA_USE_IMMERSIVE_DARK_MODE`. Builds before 20H1 used 19. `COLORREF` is `0x00BBGGRR`.
- `RelayTunnel`: `juce::read` returns 0 for EOF and error alike, so judge reconnectability by which socket died. Re-pairing would resume the pipe mid-frame and leave a zombie peer. Publish state before the blocking relay connect, so the destructor can unblock the sockets.
- `License`: a content change forces a recompile, so no two releases share a marker. No reachable server keeps a valid cached token alive for offline grace. Fresh online replies must echo the nonce.
- `DiscordPresence`: the rate limit is 5 updates per 20 seconds. Drain incoming frames so the pipe buffer never fills. A valid ready arrives as an op 1 dispatch with `evt READY`. Details and state must be at least 2 characters when present.
- `TreeSync`: every delta payload starts with `formatTag`, so legacy index-based deltas are rejected. The applying state is shared across instances. Stamp while parentless, so no property delta echoes back. Remote adds keep sender uids, because re-minting would make later sender edits unresolvable.
- `FriendsManager`: all methods run on the message thread, so no locking. LAN fresh address is never a relay-observed public IP.
- `RelayClient`: presence is pushed every 5 s and must stay under the 7 s online window. The server drops silent connections after 75 s, so the client reconnects with backoff.
- `ProjectModel`: inserts and master default to unity, so stacked sub-unity gains do not bury samples. A concurrent convert can leave notes present with steps cleared, so converting again would double them.
- `TreePath`: child-index addressing diverges when two peers append to the same parent, so each level names its node by identity. Duplicate identity siblings occur when peers add the same channel seq concurrently, so the match backtracks.
- `History`: remote edits fold into the baseline, so a later local diff cannot attribute them to us. Pure reorders are not recorded.
- `Scales`: display aid only, never constrains note placement. Krumhansl-Schmuckler correlates against all 24 rotated profiles.
- `Vst3ClassId`: the factory vtable is declared by hand, avoiding the Steinberg SDK on the include path. A VST3 may be a bundle, so resolve the inner module before loading.
- `Vst2Fxb`: JUCE state wraps the chunk in an fxBank container. Strip it on export and rebuild it on import.
- `Vst3ClassId.h`: read the real 16-byte TUID from the module factory. The plugin cache keeps only a hash.
- `BuiltinParamEQ`: FIR design state is shared across threads, so serialising here keeps it race-free. `BuiltinParamEQ.cpp`: clear only the run state, because a FIR redesign allocates and must never set `firDirty` on the audio thread. Master trim applies after the convolution, so tweaks do not force a kernel rebuild.
- `BuiltinVoid`: `addParameter` order is the serialisation format. Visualizer values are written once per block on the audio thread and read only by the editor timer. Damping keeps tracking input while frozen, so unfreezing engages the filter cleanly.
- `BuiltinMultiband` and `BuiltinLimiter`: clamp both ends. A single inf detector would make gain state absorbing. An inf in the buffer becomes NaN through zero gain and poisons the delay line.
- `BuiltinDelay`: attachment-driven, so multiplayer sync and undo apply to the LED toggle. Stateless, so the editor curve reuses the exact `processBlock` math.
- `BuiltinDelayBank`: LED toggle is renamed to avoid an ODR clash between translation units.
- `BuiltinGrossBeat`: the playhead is published after the sequencer advances, so PPQ is the block end.
- `BuiltinLowLifter`: button attachment keeps the lit state right for remote and undo edits.
- `Bridge32Protocol.h`: compiled by both binaries, so no JUCE, no pointers, no `size_t` or `long`, and pack 4 for matching layout. RPC runs on a dedicated thread, so a busy editor does not stall audio. Overflow drops the oldest automation hints, because the ring is not a state of record.
- `Bridge32Host.h`: the format string in the plugin cache is never a real JUCE format, so intercept it first. A VST2 chunk is bit-width independent, so x86 and x64 builds load each other's state.
- `Bridge32Host.cpp`: the editor is a child HWND of our peer, so service sent messages while blocked. A timed-out RPC may answer late, so drain the stale response before a new command. The exit code is ignored because teardown faults poison it. A complete result file is the proof.
- `FLPImport`: a native x64 match always beats the 32-bit bridged copy. Fast-scanned plugins live in unverified plugins with uid 0 until first use. `FLPExport`: the volume knob maps to amplitude as `knob^2.42`. Byte-count uses its own dot-chars alphabet, not JUCE base64.
- `FLNativeFx`: knob words are u32 in 1/128 units. Threshold, then post-gain.
- `MidiImport`: SMPTE files fall back to 96 PPQN, because only the common PPQN format is handled. It flattens all tracks onto one channel sequence, because each channel rack channel holds one instrument.
- `ChatPanel`, `GifImage`, `FloatingPanel`, `FriendsPanel`, `Dialogs`, `ZoomScrollBar`, `JoinSessionWindow`, `AppLookAndFeel`, `Icons`: see the code. Notes that matter: GIF decodes every frame through WIC, because JUCE decodes only the first frame. Lucide icons are ISC licensed.
- `InviteCode`: 718 conflict on a mapping means an identical mapping exists, so treat it as success. NAT-PMP covers consumer routers that speak neither UPnP variant. The COM UPnP API is independent of it.
- `ChannelRack`: anchor the popup at the pointer, because a component-targeted menu can jump monitors. Do not add audio resources here. A whole-project swap replaces container nodes, so catch those too, and defer the rebuild.
- `BrowserPanel`: the alpha is baked into each colour, because `setColour` resets it. `itemClicked` already toggles, so the default double-click handler would toggle again.
- `MissingFilesDialog`: built-in paths are skipped at the call site. A null undo manager matches the load path.
- `PreferencesWindow`: the stock selector cannot host a pseudo-item, so the default output sits above it. Restart to apply, because startup sets the factor before any window exists. The device manager broadcasts changes, but the selector does not. Returns the live session folder, so cleanup skips files in use. Measure on a throwaway thread and guard the result with a `SafePointer`.
- `AppSettings`: random peer id is reused forever, so it is a durable friend handle. Stored as a string, because `PropertiesFile` has no int64 accessor. JUCE maps HighPriority to HIGH_PRIORITY_CLASS, so use ABOVE_NORMAL for high. Per-user only, not in the project tree, so it never syncs. A manual Away survives restarts, unlike automatic idle AFK.
- `Mixer.cpp`: dim everything, then draw the meter above the overlay. Tracks process in index order. Coalesce bursts and never rebuild mid-swap.
- `CrashHandler`: keeps SEH intact, so `scanFileGuarded` still swallows plugin faults. The dump thread exists because `MiniDumpWriteDump` cannot run from a thread that ran out of stack. Plugins unhook handlers from module init, so rearm after each instantiation. Fail-fast statuses skip `SetUnhandledExceptionFilter`. The dump runs from a fresh full stack. The faulting thread must neither return nor park forever. Teardown blocks legitimately, so a quit is not reported as a hang.
- `Mp3Writer`: balance `s_false` with `CoUninitialize`, but skip it on `RPC_E_CHANGED_MODE`. COM and MF init run lazily on the first write.
- `PitchDetect.h`: header-only YIN analyzer, safe on a background thread.
- `FloatingPanel` and plugin HWNDs: clip plugin HWNDs around overlapping panels, because hiding the whole HWND blanks the editor. Match native child HWNDs to the exact editor rect. VST editors create child windows JUCE does not know, so walk the Win32 children. `GetWindowRect` is physical pixels while JUCE rects are logical.
- `ChatPanel`: loop back through the add methods, so the panel never shows an unreceived message. Keep original encoded bytes, because the session decides verbatim send or downscale. `drawImage` multiplies by fill opacity, so the hover wash would ghost the avatar.
- `PluginHost.h`: one strike is only a warning. One lock covers the pedal and blacklist. Background threads must not instantiate VST2 concurrently. Locks are taken off the message thread only, because VST2 teardown uses `callSync`.
- `SamplerSettingsPanel`: bound through get/set lambdas, so it can drive an inverted property. Kept local, so this panel does not pull in PianoRoll.
- `Playlist.h`, `PianoRoll.h`, `MainComponent.h`: covered in the invariants above.

## Gotchas learned the hard way

- `juce_add_gui_app` with `ICON_BIG ""` breaks configure — omit the arg.
- Scan progress callbacks run on a background `juce::Thread`; hop via
  `MessageManager::callAsync`.
- `Session::removePeer` defers deletion with `callAsync` (called from the connection's own
  callback).
- Replacing a whole project (load/snapshot): mutate in place
  (`copyPropertiesAndChildrenFrom`) — never reassign the `project` member, or every listener
  and the synchroniser silently detach. Every load path also calls `engine.stop()`,
  `history.clear()`, resets dirty/warned sets, and re-runs `checkMissingFiles()`.
- `MainComponent` member declaration order is load-bearing (reverse destruction closes editor
  windows before the engine destroys instances): project → pluginHost → themeManager →
  engine → perfMonitor → session → ... → history → keyBindings → pluginWindows → UI, and
  `debugOverlayUI` + `debugOverlay` declared LAST (destroyed first — the ImGui GL render
  thread reads them + engine every frame and must stop before those die). Don't reorder.
- VST editor construction is slow — `PluginWrapperComponent` defers it until after first
  paint; never build editors synchronously in `open()` paths.
- Builtin effect state has no keys — it is raw floats in parameter order. Reordering
  `addParameter` calls silently corrupts saves; append only.
- dbghelp (PerfMonitor stack capture) is single-threaded: serialize through `dbghelpMutex`,
  never allocate between SuspendThread/ResumeThread.
- FL Studio VSTi editor crashes (Delphi "Execution of address 0000000000000000", runtime error
  231) come from the plugin's own engine lookup, not this host: the registry value
  `FL Studio engine` under `HKCU\SOFTWARE\Image-Line\Shared\Paths` pointed at a stale FL
  version while the installed shim was newer. It reproduces in a minimal vanilla JUCE host —
  check the registry before touching hosting code. FL-native (Fruity) plugins can never load
  as VSTs at all; the scanner PE-filters them (`inspectDllExports`).

# How to respond

When you write technical text (documentation, READMEs, runbooks, procedures, error messages, release notes, reports), obey these rules from ASD-STE100 Simplified Technical English:

CLASSIFY FIRST. Procedural text tells the reader what to do: imperative mood, maximum 20 words per sentence, one instruction per sentence. Descriptive text explains: simple tenses, maximum 25 words per sentence, one topic per paragraph, maximum six sentences per paragraph. Never mix the two in one passage.

VERBS. Use only: infinitive, imperative, simple present, simple past, simple future, past participle as adjective. No present perfect ("has completed" → "completed"). No "-ing" verb forms ("making it easy" → new sentence). Active voice; passive only in descriptions when the agent is unknown. Approved modals: can, will, must. Banned: should, would, may, might, could. For "should": write "must" if required, delete if optional.

SENTENCES. Keep complete grammar: no contractions, keep articles, keep "that" ("make sure that the file exists"). Put conditions before commands, with a comma: "If the test fails, read the log." No semicolons — write two sentences. Use a vertical list for more than two items or steps.

WORDS. One word, one meaning, for the whole document: pick one of check/verify/confirm and keep it. Noun chains of maximum three words; break longer ones with prepositions ("the timeout value for the connection pool"). Delete words that carry no fact: simply, seamlessly, robust, powerful, comprehensive, leverage, "in order to", "it is worth noting". Replace: utilize → use, prior to → before, in the event that → if, e.g. → for example. American spelling.

WARNINGS. Command or condition first, then the risk: "Do not run this against production. The command deletes rows."

NEVER TOUCH. Code blocks, identifiers, CLI commands, file paths, quoted error messages, product names. Each counts as one word toward sentence limits.

SELF-CHECK before returning: scan for contractions, "has been", "should", ", making", semicolons. Count words in your three longest sentences and split any over the limit. Collapse synonym rotation.

Do not apply these rules to marketing copy or brand writing.