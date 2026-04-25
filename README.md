# vst3_debugger

A macOS command-line tool that loads a VST3 plug-in bundle step-by-step and logs every API call before invoking it. A signal handler catches crashes (`SIGSEGV`, `SIGBUS`, `SIGILL`) and reports which VST3 call caused the crash.

## Requirements

- macOS (arm64 or x86_64)
- Xcode Command Line Tools (`clang++`)

## Build

```sh
make
```

Or manually:

```sh
clang++ -std=c++17 -g \
    -framework CoreFoundation -framework AppKit \
    -o vst3_debugger vst3_debugger.cpp host_window.mm
```

## Usage

```sh
./vst3_debugger "/Library/Audio/Plug-Ins/VST3/MyPlugin.vst3"
```

### Options

| Flag | Description |
|---|---|
| `--class-index N` | Select which plug-in class to instantiate (default: 0) |
| `--skip-editor` | Skip `IEditController` / `IPlugView` initialization |
| `--skip-dlclose` | Do not call `dlclose` on the bundle after testing |
| `--dlclose-delay N` | Wait N seconds before calling `dlclose` |

## How it works

1. Opens the `.vst3` bundle with `dlopen` and resolves `GetPluginFactory`.
2. Enumerates all exported classes and selects the target `IAudioProcessor`.
3. Calls each VST3 lifecycle method in order (`initialize`, `setupProcessing`, `setActive`, `process`, etc.), logging the call and its return value.
4. Optionally opens a minimal `NSWindow` host via `host_window.mm` and attaches the plug-in's `IPlugView`.
5. On any signal (`SIGSEGV`, `SIGBUS`, `SIGILL`), prints which VST3 call was in progress and a backtrace, then exits.

## Clean

```sh
make clean
```
