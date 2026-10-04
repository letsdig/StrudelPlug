# StrudelPlug VST3 — Technical Documentation & User Guide

**StrudelPlug** is a native Linux VST3 audio plugin built with the JUCE framework. It embeds the **[Strudel](https://strudel.cc/)** live-coding music environment directly into your DAW (e.g. Bitwig Studio, Reaper, Ardour), streaming multi-channel Web Audio and bidirectional MIDI directly through your track's channel strip.

---

## 1. Architecture & Real-Time Safety

### 1.1 In-Track Web Audio Loopback
- Strudel's sound synthesis engine (Superdough, Web Audio API, and audio worklets) is captured in-process by an injected bridge script.
- Captured 32-bit float PCM audio frames are delivered to an internal low-latency ring buffer.
- The DAW's `processBlock()` audio thread reads from this lock-free FIFO ring buffer directly into the channel output pins with zero external cabling or JACK patchbays.

### 1.2 Real-Time & Realtime Thread Safety
- **Non-Blocking WebSocket Pipeline**: Network transmissions between the browser engine and C++ bridge utilize non-blocking socket writes (`MSG_DONTWAIT | MSG_NOSIGNAL`) and non-blocking mutex locks (`std::try_to_lock`).
- **Zero DAW Stalls**: Even during heavy JavaScript compilation, WASM module loads, or network latency on WebKit, the host DAW's audio engine is never blocked or frozen.
- **Persistent Background Playback**: The embedded WebKit browser instance is retained continuously by the `AudioProcessor`. Closing the editor window never silences or interrupts your live patterns.

### 1.3 Bidirectional Web MIDI Integration
- **DAW to Strudel**: Incoming notes from MIDI clips or hardware controllers are forwarded directly into Strudel's Web MIDI input handlers.
- **Strudel to DAW (`.midi()`)**:
  - Implements a W3C-compliant `VirtualMIDIPort` interface (`open()`, `close()`, `onstatechange`, `EventTarget`).
  - High-resolution performance timer scheduling handles future timestamps from Strudel lookahead, eliminating note jitter and premature playback.
  - Generates VST3 MIDI output that drives subsequent instruments in the track chain or can be picked up via Bitwig's **Note Receiver**.

---

## 2. Live-Coding with Strudel

### 2.1 Basic Pattern Syntax
Open the plugin editor and write standard Strudel pattern code:

```javascript
// Basic 4-on-the-floor beat
s("bd [~ cp] bd [~ cp]").bank("RolandTR909")

// Melodic synth line with built-in synths
note("c3 eb3 g3 bb3").s("sawtooth").cutoff(1200).resonance(8)
```

Press **`Ctrl + Enter`** to evaluate and run the pattern.

### 2.2 Routing MIDI Out to DAW Instruments
To play native DAW instruments, virtual synthesizers, or hardware MIDI synths, append `.midi()` to any Strudel pattern:

```javascript
// Default MIDI output:
$: note("c3 eb3 g3 bb3").midi()

// Specifying MIDI channel (1-16):
$: note("c3 eb3 g3 bb3").midichan(1).midi()

// Complex chords and voicings:
$: chord("<Cm7 Fm7 Gm7 Cm7>").voicing().midi()
```

### 2.3 DAW Transport, Tempo & Phase Synchronization (v1.0.7+)
- When **SYNC DAW** is enabled in the bottom telemetry bar:
  - **Tempo & BPM Tracking**: Host DAW BPM is automatically synchronized to Strudel's pattern scheduler (`cps = DAW_BPM / 240.0`, `cpm = DAW_BPM / 4.0`).
  - **Beat & Phase Locking**: On transport start, loop wrap, or timeline seek, Strudel's cycle phase is locked to the host playhead (`startCycle = ppqPosition / 4.0`), keeping kick drums, downbeats, and polyrhythms sample-aligned with DAW audio and MIDI clips.
  - **Plugin Delay Compensation (PDC)**: Reports exact jitter cushion buffer latency to the host via JUCE `setLatencySamples()`, ensuring the DAW aligns Strudel's audio output with other tracks.
  - **Instant Clean Stop & Anti-Hang**: Stopping DAW transport instantly silences audio, flushes FIFO ring buffers, and clears all active notes with All Sound Off (CC 120) and All Notes Off (CC 123).
- You can also toggle SYNC off to let Strudel run free independently of host transport.

### 2.4 DAW State Persistence & Project Auto-Save (v1.0.9+)
- **Two-Way Pattern Synchronization**: Live edits inside Strudel's CodeMirror editor are continuously debounced and synchronized to the C++ audio processor in real time.
- **DAW Project Save / Reload**: When saving your session in Bitwig, Reaper, or any VST3 host, the plugin state automatically serializes:
  - The active Strudel pattern code.
  - Transport synchronization toggle state (`SYNC DAW`).
  - Web Audio sample rate selection.
  - Jitter cushion buffer size (samples & ms).
  - Master output gain trim (dB).
- **Zero-Loss Reopen**: Reopening the project restores your pattern code directly into the editor and re-evaluates it if DAW transport is active.

### 2.5 Ultra-Low CPU Architecture & Idle Silence Suppression (v1.0.9+)
- **512-Sample Worklet Batching**: Web Audio blocks are accumulated into 512-sample frames (~10.6ms @ 48kHz), slashing WebKit IPC and Base64 conversion frequency by 75% compared to raw 128-frame processing.
- **Automatic 500ms Silence Hangover**: When transport is stopped and synthesis is silent, the AudioWorklet streaming enters a low-power IDLE state after a 500ms ringout window. During DAW pauses, CPU consumption drops to **~0%**, waking instantly upon the first audible transient.

---

## 3. User Interface & Controls

### 3.1 Header Bar
The browser always shows the Strudel REPL served by the plugin's own bridge server (`http://127.0.0.1:<bridge port>/strudel/`); there is no address bar and no remote option.
- **Status LCD** (next to the title): `CONNECTING...`, `ONLINE: AUDIO+MIDI`, `FETCHING STRUDEL...`, `STRUDEL READY`, `FETCH FAILED: ...`.
- **SYNC DAW Button**: Toggles automatic transport synchronization with the DAW.
- **VOL Meter**: Real-time peak level readout in decibels (`dB`).
- **MIDI LED**: Real-time green activity indicator flashing when Strudel emits MIDI note events.
- **Gain Slider**: Adjusts master output volume (-24 dB to +6 dB).
- **`⚙` Settings** (far right): shows/hides the settings row below. Collapsed by default.

### 3.2 Settings Row (behind `⚙`)
- **Sample Rate Selector (Hz)**: WebKit AudioContext rate, `Auto (DAW)` or fixed.
- **Buffer Cushion Selector**: Choose from `128 smp (~2.6ms)` to `16384 smp (~340ms)` to match session CPU load and project requirements.
- **Telemetry LCD**: DAW rate, transport state, output dB, buffer size.
- **`⟳` Reload**: reloads the local page.
- **`↓` Download / Update**: fetches the latest `@strudel/repl` package tarball from the npm registry, unpacks its `dist/` folder into the user app-data directory (`StrudelPlug/strudel`) and reloads the page. Use it for the first install and for later updates.
- **Not installed yet**: until a build is on disk the page shows a warning pointing at `⚙` → `↓` instead of a blank editor.
- **Default pattern**: a fresh instance opens with `silence`; the code saved in the DAW project replaces it once the editor is ready.
- **Offline**: synth sounds work without internet; sample packs still need a connection.
- **MIDI input works**: the page is plain `http`, so the bridge script can open the MIDI-in WebSocket that an `https` page would block as mixed content.
- *Licensing*: Strudel is AGPL and is not bundled with the plugin; it is only downloaded at runtime. No Node.js required.

---

## 4. Installation & Requirements

### System Compatibility: Ubuntu 24.04+ (x86_64)
- **OS**: Ubuntu 24.04 LTS (Noble Numbat) or newer Linux distributions with modern WebKitGTK.
- **Dependencies**: `libwebkit2gtk-4.1-0`, `libgtk-3-0`, `libasound2t64`, `libcurl4`.

### Installation Steps
```bash
# 1. Download release archive
tar -xzf StrudelPlug-v1.0.9-linux-x86_64.tar.gz

# 2. Copy to VST3 folder
mkdir -p ~/.vst3
cp -r StrudelPlug-v1.0.9-linux-x86_64/StrudelPlug.vst3 ~/.vst3/
```

After installation, perform a plugin rescan in your DAW.

---

## 5. Troubleshooting & FAQ

* **Notes playing ahead of time or bunching up?**
  Ensure you are running version **1.0.1** or newer, which features high-resolution lookahead compensation.
* **Audio crackles or glitching under high CPU load?**
  Switch the buffer cushion in the bottom options bar to `1024 smp` or `2048 smp`.
* **Blank screen or WebKit errors?**
  Ensure `libwebkit2gtk-4.1-0` is installed (`sudo apt install libwebkit2gtk-4.1-0`).
