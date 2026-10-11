# mikeDAW
a free &amp; cross platform DAW that is easy for beginners to use with features like real-time Multiplayer

<img width="1914" height="1039" alt="image" src="https://github.com/user-attachments/assets/b5deb3ed-5481-4688-8f54-eab4a7f5d143" />

## Download
to test it out, [you can download the pre-release of mikeDAW here](https://github.com/mjkzy/mikeDAW/releases/tag/v0.1.0).

## Features
- **real-time multiplayer** - up to 4 people edit the same project live over LAN, direct IP or invite codes (UPnP / NAT-PMP). notes, patterns, playlist, mixer, plugin parameters and transport all stay in sync. every peer renders their own project locally, just synced.
- **multiplayer social** - chat (with images), reactions, peer cursors, activity feed, friends list and per-peer listen/deafen.
- **VST2 & VST3 hosting** - instruments in the channel rack, effects in the mixer, native plugin editors in a plugin wrapper, full plugin state save/restore and presets.
- **32-bit VST2 bridge** - run old 32-bit VST2 plugins inside the 64-bit app through a separate bridge process.
- **Familiar feeling workflow** - channel rack with step sequencer, piano roll, playlist, mixer and file browser.
- **piano roll** - draw/move/resize/slice tools, velocity lane, slide notes, ghost notes, scale highlight with key detection, arpeggiator, strumizer, MIDI import/export.
- **playlist** - pattern, audio and automation clips, waveforms, clip slicing and per-track lanes.
- **automation clips** - automate mixer track and channel volume/pan with editable curves.
- **mixer** - master + up to 64 inserts, 10 effect slots per insert, routing, stereo separation, peak meters, with a slot rack or a device chain view.
- **built-in effects** - EQ, parametric EQ, delay, delay bank, reverb, convolution reverb, limiter, multiband compressor, soft clipper, a Gross Beat-style effect and more.
- **sampler** - sample start/length, fades, declick, pitch/fine tune, root note, reverse, normalize and pitch detection.
- **audio recording** - record audio inputs into the playlist with latency compensation, and record notes from the typing keyboard.
- **easyFLP project import/export** - open `.flp` files and export projects back to an FL zip.
- **undo/redo** that works in multiplayer without wiping other people's edits.
- **rendering** - export to WAV, MP3, FLAC and Ogg.
- **themes** - fully themeable colours with hot-reload.

## Building
requirements: CMake 3.22+ and Visual Studio 2022+ with the C++ workload.

```bat
git clone https://github.com/mjkzy/mikeDAW.git
cd mikeDAW
git clone --depth 1 --branch 8.0.8 https://github.com/juce-framework/JUCE.git external/JUCE
git clone --depth 1 --branch v1.91.9b https://github.com/ocornut/imgui.git external/imgui
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
powershell -ExecutionPolicy Bypass -File run.ps1
```

VST3 hosting always works because JUCE bundles the VST3 interface headers. VST2 and ASIO are optional, see below.

## Steinberg SDKs & trademarks
ASIO, VST and VST3 are trademarks of Steinberg Media Technologies GmbH. mikeDAW is not affiliated with Steinberg.

i do not provide the VST2 SDK or the ASIO SDK in this repository. mikeDAW supports these features if you have the SDK yourself:

- **VST2** - point CMake at the VST2 SDK with `-DOPENDAW_VST2_SDK_PATH=<path>`. without it the build is VST3-only.
- **ASIO** - point CMake at the ASIO SDK with `-DOPENDAW_ASIO_SDK_PATH=<path>`. without it the build uses WASAPI/DirectSound.

## Open Source
the code is now open source. i do not plan to actively maintain this and will only fix bugs for a while.

this was assisted with AI, so there is slop everything as I did not start a final cleanup stage to make it look less sloppy. i apologize.

## License
mikeDAW is licensed under the GNU Affero General Public License v3.0. See [LICENSE](LICENSE).
Third-party components and their terms are listed in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
