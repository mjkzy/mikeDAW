---
name: "source-command-build"
description: "Build Release, relaunch mikeDAW, and report the result"
---

# source-command-build

Use this skill when the user asks to run the migrated source command `build`.

## Command Template

Run the project's build+verify loop and interpret the outcome:

1. Run: `powershell -ExecutionPolicy Bypass -File run.ps1` (from the repo root; timeout 600000ms).
2. Read the trimmed output (last 25 lines of the MSVC build):
   - `alive PID=...` → success. Report it; the user tests the app by hand.
   - `BUILD FAILED` → find the first `error C`/`LNK` line in the output, open the file at that
     line, fix the error, and run this loop again. Repeat until it builds.
   - `NOT RUNNING (crashed on startup?)` → the build linked but the app died on launch. Check
     `%APPDATA%/mikeDAW/CrashLogs` for a fresh minidump/log and inspect the most recent code
     changes for startup-order issues (see the MainComponent member-order gotcha in AGENTS.md).
3. Never leave the loop with a failing build unless blocked on something only the user can decide.

To relaunch without rebuilding: `powershell -ExecutionPolicy Bypass -File run.ps1 -NoBuild`.
Ignore clangd/IDE diagnostics ("JuceHeader.h not found", "undeclared identifier juce") — they
are false positives; MSVC output is the only source of truth.
