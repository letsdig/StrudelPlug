#pragma once

#include <juce_core/juce_core.h>

namespace WebBridge
{

inline juce::String getInjectionScript(int bridgePort = 8788, int targetSampleRate = 48000, int bufferSize = 256, double initialBpm = 120.0)
{
    return juce::String(R"JS(
(function() {
    // =========================================================================
    // WEBKIT-GTK WEBAUDIO COMPATIBILITY FIXES FOR STRUDEL / SUPERDOUGH
    // Fixes "Channel count cannot be 0" bug on Linux WebKitGTK where destination.maxChannelCount is 0
    // =========================================================================
    if (typeof AudioDestinationNode !== 'undefined') {
        try {
            Object.defineProperty(AudioDestinationNode.prototype, 'maxChannelCount', {
                get: function() { return 2; },
                set: function() {},
                configurable: true
            });
            const origCc = Object.getOwnPropertyDescriptor(AudioNode.prototype, 'channelCount');
            Object.defineProperty(AudioDestinationNode.prototype, 'channelCount', {
                get: function() { return 2; },
                set: function(val) {
                    if (val < 1) val = 2;
                    if (origCc && origCc.set) {
                        try { origCc.set.call(this, val); } catch(e) {}
                    }
                },
                configurable: true
            });
        } catch(e) {
            console.warn("[JUCE-WebBridge] AudioDestinationNode patch warning:", e);
        }
    }

    if (typeof ChannelMergerNode !== 'undefined') {
        try {
            const OrigMerger = window.ChannelMergerNode;
            window.ChannelMergerNode = function(ctx, opts) {
                let options = opts;
                if (options && typeof options.numberOfInputs === 'number' && options.numberOfInputs < 1) {
                    options = Object.assign({}, options, { numberOfInputs: 2 });
                }
                return new OrigMerger(ctx, options);
            };
            window.ChannelMergerNode.prototype = OrigMerger.prototype;
        } catch(e) {}
    }

    try {
        const desc = Object.getOwnPropertyDescriptor(AudioNode.prototype, 'channelCount');
        if (desc && desc.set) {
            const origSet = desc.set;
            Object.defineProperty(AudioNode.prototype, 'channelCount', {
                get: desc.get,
                set: function(val) {
                    if (typeof val === 'number' && val < 1) val = 2;
                    return origSet.call(this, val);
                },
                configurable: true
            });
        }
    } catch(e) {}

    if (window.__JUCE_BRIDGE_LOADED__) {
        console.log("[JUCE-WebBridge] Re-arming context scan...");
        if (typeof scanAndHook === "function") scanAndHook();
        return;
    }
    window.__JUCE_BRIDGE_LOADED__ = true;

    console.log("[JUCE-WebBridge] Initializing DAW Web Audio & MIDI Bridge for Bitwig Studio...");

    // Polyfill MIDIMessageEvent if not natively exposed
    if (typeof window.MIDIMessageEvent === "undefined") {
        window.MIDIMessageEvent = class MIDIMessageEvent extends Event {
            constructor(type, eventInitDict) {
                super(type, eventInitDict);
                this.data = (eventInitDict && eventInitDict.data) ? eventInitDict.data : new Uint8Array();
                this.receivedTime = performance.now();
            }
        };
    }

    const PORTS = [%PORT%, 8788, 8789, 8790, 8791, 8766];
    let portIndex = 0;
    let ws = null;
    let wsConnected = false;
    let reconnectTimer = null;
    const dawInputRingBuffer = [];
    const allHookedContexts = new Set();

    // Master configuration
    const CONFIG = {
        muteSystemAudio: true,
        bufferSize: %BUFFER_SIZE%,
        targetSampleRate: %SAMPLE_RATE%,
        transportPlaying: false,
        dawBpm: %INITIAL_BPM%
    };

    if (typeof window.setcpm === "undefined") {
        window.setcpm = function(cpm) {
            if (window.strudelMirror && window.strudelMirror.repl) {
                window.strudelMirror.repl.setCps(cpm / 60.0);
            }
        };
    }
    if (typeof window.setcps === "undefined") {
        window.setcps = function(cps) {
            if (window.strudelMirror && window.strudelMirror.repl) {
                window.strudelMirror.repl.setCps(cps);
            }
        };
    }

    // =========================================================================
    // 0. DIRECT NATIVE JUCE IN-MEMORY IPC (100% immune to CORS / Mixed Content)
    // =========================================================================
    function sendNativeJuceEvent(eventId, payload) {
        try {
            const jsonStr = JSON.stringify({ eventId: eventId, payload: payload });
            if (window.__JUCE__ && window.__JUCE__.backend && typeof window.__JUCE__.backend.emitEvent === "function") {
                window.__JUCE__.backend.emitEvent(eventId, payload);
                return true;
            }
            if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.__JUCE__) {
                window.webkit.messageHandlers.__JUCE__.postMessage(jsonStr);
                return true;
            }
            if (window.__JUCE__ && typeof window.__JUCE__.postMessage === "function") {
                window.__JUCE__.postMessage(jsonStr);
                return true;
            }
        } catch(e) {
            console.warn("[JUCE-WebBridge] Error sending native event:", e);
        }
        return false;
    }

    // Signal native bridge readiness immediately
    sendNativeJuceEvent("bridgeStatus", { status: "connected", mode: "native" });

    // =========================================================================
    // 1. WEBSOCKET CONNECTION (For External Chrome & Localhost)
    // =========================================================================
    function tryConnect() {
        if (wsConnected) return;
        if (typeof window !== "undefined" && window.location && window.location.protocol === "https:") {
            return;
        }

        const currentPort = PORTS[portIndex % PORTS.length];
        try {
            ws = new WebSocket("ws://127.0.0.1:" + currentPort);
            ws.binaryType = "arraybuffer";

            const connectionTimeout = setTimeout(() => {
                if (!wsConnected && ws && ws.readyState !== WebSocket.OPEN) {
                    try { ws.close(); } catch(e) {}
                    portIndex++;
                    tryConnect();
                }
            }, 1200);

            ws.onopen = function() {
                clearTimeout(connectionTimeout);
                wsConnected = true;
                console.log("[JUCE-WebBridge] WebSocket CONNECTED on port " + currentPort + "!");
                sendNativeJuceEvent("bridgeStatus", { status: "connected", port: currentPort });
                resumeAllContexts();
            };

            ws.onmessage = function(event) {
                if (!(event.data instanceof ArrayBuffer)) return;
                const data = new Uint8Array(event.data);
                if (data.length === 0) return;

                const packetType = data[0];

                if (packetType === 0x02 && data.length >= 4) {
                    // MIDI from Bitwig: [0x02, status, data1, data2]
                    dispatchMidiFromDaw(data[1], data[2], data[3]);
                } else if (packetType === 0x01 && data.length >= 4) {
                    // Audio from Bitwig (for sampling e.g. in YPC2000)
                    const numChannels = data[1];
                    const sampleCount = data[2] | (data[3] << 8);
                    const f32 = new Float32Array(event.data, 4, sampleCount * numChannels);
                    for (let i = 0; i < f32.length; i++) {
                        dawInputRingBuffer.push(f32[i]);
                    }
                    if (dawInputRingBuffer.length > 48000 * 2) {
                        dawInputRingBuffer.splice(0, dawInputRingBuffer.length - 48000);
                    }
                }
            };

            ws.onclose = function() {
                if (wsConnected) {
                    wsConnected = false;
                }
                clearTimeout(connectionTimeout);
                clearTimeout(reconnectTimer);
                reconnectTimer = setTimeout(tryConnect, 1500);
            };

            ws.onerror = function() {
                try { ws.close(); } catch(e) {}
            };
        } catch (err) {
            portIndex++;
            clearTimeout(reconnectTimer);
            reconnectTimer = setTimeout(tryConnect, 1500);
        }
    }

    function resumeAllContexts() {
        if (window.__juceAudioCtx && window.__juceAudioCtx.state === "suspended") {
            window.__juceAudioCtx.resume().catch(() => {});
        }
        for (const ctx of allHookedContexts) {
            if (ctx && ctx.state === "suspended") {
                ctx.resume().catch(() => {});
            }
        }
    }

    if (typeof window !== "undefined") {
        window.addEventListener("pointerdown", resumeAllContexts, { passive: true });
        window.addEventListener("keydown", resumeAllContexts, { passive: true });
    }

    tryConnect();

    // =========================================================================
    // 2. WEB MIDI SHIM (Bitwig Track <-> Web Instrument)
    // =========================================================================
    class VirtualMIDIPort extends EventTarget {
        constructor(id, name, type) {
            super();
            this.id = id;
            this.name = name;
            this.manufacturer = "AudioWebLab";
            this.version = "1.0";
            this.type = type;
            this.state = "connected";
            this.connection = "open";
            this.onstatechange = null;
        }

        async open() {
            this.connection = "open";
            return this;
        }

        async close() {
            this.connection = "closed";
            return this;
        }
    }

    class VirtualMIDIInput extends VirtualMIDIPort {
        constructor() {
            super("juce-daw-midi-in", "DAW MIDI Input (Bitwig Track)", "input");
            this.onmidimessage = null;
        }
    }

    class VirtualMIDIOutput extends VirtualMIDIPort {
        constructor() {
            super("juce-daw-midi-out", "DAW MIDI Output (Bitwig / IAC Driver / Track)", "output");
            this.scheduledTimers = new Set();
            this.activeNotes = new Map();
        }

        send(data, timestamp) {
            const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
            if (!bytes || bytes.length === 0) return;

            const dispatchBytes = () => {
                // Parse chunks so individual MIDI messages are safely forwarded
                for (let i = 0; i < bytes.length;) {
                    const status = bytes[i];
                    if (status >= 0xF8) {
                        // System Real-Time (1 byte)
                        sendNativeJuceEvent("dawMidiData", { status: status, d1: 0, d2: 0 });
                        i += 1;
                    } else if ((status & 0xF0) === 0xC0 || (status & 0xF0) === 0xD0 || status === 0xF1 || status === 0xF3) {
                        // 2-byte messages (Program Change, Channel Pressure, MTC, Song Select)
                        sendNativeJuceEvent("dawMidiData", { status: status, d1: bytes[i + 1] || 0, d2: 0 });
                        i += 2;
                    } else if (status >= 0x80) {
                        // 3-byte messages (NoteOn, NoteOff, CC, PitchBend, Aftertouch, Song Position)
                        const d1 = bytes[i + 1] || 0;
                        const d2 = bytes[i + 2] || 0;
                        const ch = status & 0x0F;
                        const type = status & 0xF0;

                        if (type === 0x90 && d2 > 0) {
                            this.activeNotes.set((ch << 8) | d1, true);
                        } else if (type === 0x80 || (type === 0x90 && d2 === 0)) {
                            this.activeNotes.delete((ch << 8) | d1);
                        }

                        sendNativeJuceEvent("dawMidiData", { status: status, d1: d1, d2: d2 });
                        i += 3;
                    } else {
                        // Fallback: send 3 bytes or advance 1
                        sendNativeJuceEvent("dawMidiData", { status: bytes[i] || 0, d1: bytes[i + 1] || 0, d2: bytes[i + 2] || 0 });
                        i += 3;
                    }
                }

                // 2. Also forward via WebSocket if open (for external browser / Chrome)
                if (wsConnected && ws && ws.readyState === WebSocket.OPEN) {
                    const packet = new Uint8Array(1 + bytes.length);
                    packet[0] = 0x02; // MIDI opcode
                    packet.set(bytes, 1);
                    try {
                        ws.send(packet.buffer);
                    } catch (e) {}
                }
            };

            // High-resolution timestamp scheduling for WebMidi lookahead
            const now = performance.now();
            if (typeof timestamp === 'number' && timestamp > now + 1) {
                const delay = Math.max(0, timestamp - now);
                const timerId = setTimeout(() => {
                    this.scheduledTimers.delete(timerId);
                    dispatchBytes();
                }, delay);
                this.scheduledTimers.add(timerId);
            } else {
                dispatchBytes();
            }
        }

        clear() {
            for (const t of this.scheduledTimers) {
                clearTimeout(t);
            }
            this.scheduledTimers.clear();
        }

        allNotesOff() {
            this.clear();

            // 1. Send explicit Note-Off for each active note
            for (const [key] of this.activeNotes.entries()) {
                const ch = (key >> 8) & 0x0F;
                const note = key & 0xFF;
                sendNativeJuceEvent("dawMidiData", { status: 0x80 | ch, d1: note, d2: 0 });
                if (wsConnected && ws && ws.readyState === WebSocket.OPEN) {
                    const p = new Uint8Array([0x02, 0x80 | ch, note, 0]);
                    try { ws.send(p.buffer); } catch(e) {}
                }
            }
            this.activeNotes.clear();

            // 2. Send All Sound Off (CC 120) & All Notes Off (CC 123) across all 16 MIDI channels
            for (let ch = 0; ch < 16; ch++) {
                sendNativeJuceEvent("dawMidiData", { status: 0xB0 | ch, d1: 120, d2: 0 });
                sendNativeJuceEvent("dawMidiData", { status: 0xB0 | ch, d1: 123, d2: 0 });
                if (wsConnected && ws && ws.readyState === WebSocket.OPEN) {
                    const p1 = new Uint8Array([0x02, 0xB0 | ch, 120, 0]);
                    const p2 = new Uint8Array([0x02, 0xB0 | ch, 123, 0]);
                    try { ws.send(p1.buffer); ws.send(p2.buffer); } catch(e) {}
                }
            }
        }
    }

    const virtualMidiInput = new VirtualMIDIInput();
    const virtualMidiOutput = new VirtualMIDIOutput();

    function createMidiEvent(bytes) {
        let ev;
        try {
            ev = new MIDIMessageEvent("midimessage", { data: bytes });
        } catch(e) {
            try {
                ev = new Event("midimessage");
            } catch(e2) {
                ev = { type: "midimessage" };
            }
        }
        ev.data = bytes;
        ev.receivedTime = performance.now();
        return ev;
    }

    const PAD_KEY_MAP = {
        36: '1', 37: '2', 38: '3', 39: '4',
        40: 'q', 41: 'w', 42: 'e', 43: 'r',
        44: 'a', 45: 's', 46: 'd', 47: 'f',
        48: 'z', 49: 'x', 50: 'c', 51: 'v'
    };

    // Computer keyboard note mapping for browser synths that listen to typing
    const NOTE_KEY_MAP = {
        48: 'z', 49: 's', 50: 'x', 51: 'd', 52: 'c', 53: 'v', 54: 'g',
        55: 'b', 56: 'h', 57: 'n', 58: 'j', 59: 'm',
        60: 'a', 61: 'w', 62: 's', 63: 'e', 64: 'd', 65: 'f', 66: 't',
        67: 'g', 68: 'y', 69: 'h', 70: 'u', 71: 'j', 72: 'k', 73: 'o',
        74: 'l', 75: 'p'
    };

    function dispatchMidiFromDaw(status, d1, d2) {
        resumeAllContexts();

        const bytes = new Uint8Array([status, d1, d2]);
        const event = createMidiEvent(bytes);

        // 1. Direct callback invocation if set
        if (typeof virtualMidiInput.onmidimessage === "function") {
            try { virtualMidiInput.onmidimessage(event); } catch (e) { console.error(e); }
        }
        // 2. Dispatch via standard EventTarget
        try {
            virtualMidiInput.dispatchEvent(event);
        } catch (e) { console.error(e); }

        // 3. Computer KeyboardEvent simulation (for MPC samplers and keyboard synths)
        const isNoteOn = (status & 0xF0) === 0x90 && d2 > 0;
        const isNoteOff = (status & 0xF0) === 0x80 || ((status & 0xF0) === 0x90 && d2 === 0);

        if (isNoteOn || isNoteOff) {
            const evName = isNoteOn ? "keydown" : "keyup";
            const padChar = PAD_KEY_MAP[d1];
            if (padChar) {
                const codeName = (padChar >= '0' && padChar <= '9') ? ("Digit" + padChar) : ("Key" + padChar.toUpperCase());
                const keyEv = new KeyboardEvent(evName, {
                    key: padChar,
                    code: codeName,
                    bubbles: true,
                    cancelable: true
                });
                window.dispatchEvent(keyEv);
                document.dispatchEvent(keyEv);
            }

            const noteChar = NOTE_KEY_MAP[d1];
            if (noteChar && noteChar !== padChar) {
                const codeName = "Key" + noteChar.toUpperCase();
                const keyEv = new KeyboardEvent(evName, {
                    key: noteChar,
                    code: codeName,
                    bubbles: true,
                    cancelable: true
                });
                window.dispatchEvent(keyEv);
                document.dispatchEvent(keyEv);
            }
        }
    }

    class VirtualMIDIAccess extends EventTarget {
        constructor() {
            super();
            this.inputs = new Map([
                [virtualMidiInput.id, virtualMidiInput]
            ]);
            this.outputs = new Map([[virtualMidiOutput.id, virtualMidiOutput]]);
            this.sysexEnabled = true;
            this.onstatechange = null;
        }
    }

    const virtualMidiAccessInstance = new VirtualMIDIAccess();

    if (typeof navigator !== "undefined") {
        navigator.requestMIDIAccess = async function() {
            console.log("[JUCE-WebBridge] requestMIDIAccess() intercepted, returning Bitwig MIDI ports!");
            return virtualMidiAccessInstance;
        };

        if (navigator.permissions && navigator.permissions.query) {
            const origQuery = navigator.permissions.query.bind(navigator.permissions);
            navigator.permissions.query = async function(desc) {
                if (desc && (desc.name === "midi" || desc.name === "midi-sysex")) {
                    return { state: "granted", onchange: null };
                }
                return origQuery(desc);
            };
        }
    }

    // =========================================================================
    // 3. BULLETPROOF WEB AUDIO CAPTURE (Direct Native IPC + WebSocket)
    //
    // Uses an AudioWorkletNode (runs on the dedicated audio-rendering thread)
    // instead of the legacy, main-thread ScriptProcessorNode. This removes the
    // #1 cause of glitches/dropouts (main-thread jank stalling audio callbacks)
    // and keeps audio flowing even while the WebView is hidden/backgrounded
    // (e.g. when the plugin editor window is closed), since the audio thread
    // is not subject to the same visibility throttling as the main thread.
    // Falls back to ScriptProcessorNode only if AudioWorklet is unavailable.
    // =========================================================================
    const OrigAudioContext = window.AudioContext || window.webkitAudioContext;
    const origConnect = (typeof AudioNode !== "undefined" && AudioNode.prototype) ? AudioNode.prototype.connect : null;

    const CAPTURE_WORKLET_SRC = [
        "class JuceCaptureProcessor extends AudioWorkletProcessor {",
        "  process(inputs, outputs) {",
        "    const input = inputs[0];",
        "    const output = outputs[0];",
        "    const inL = (input && input[0]) ? input[0] : new Float32Array(128);",
        "    const inR = (input && input.length > 1 && input[1]) ? input[1] : inL;",
        "    const len = inL.length;",
        "    if (output && output[0]) output[0].set(inL);",
        "    if (output && output.length > 1 && output[1]) output[1].set(inR);",
        "    const packet = new Float32Array(len * 2);",
        "    for (let i = 0; i < len; i++) {",
        "      packet[i * 2] = inL[i];",
        "      packet[i * 2 + 1] = inR[i];",
        "    }",
        "    this.port.postMessage({ sr: sampleRate, len: len, pcm: packet.buffer }, [packet.buffer]);",
        "    return true;",
        "  }",
        "}",
        "registerProcessor('juce-capture-processor', JuceCaptureProcessor);"
    ].join("\n");
    let captureWorkletUrl = null;

    // Shared by both the AudioWorklet path and the ScriptProcessor fallback.
    function handleCapturedBlock(currentRate, len, pcmBuffer) {
        // 1. FAST PATH: Direct Binary WebSocket Transmission (Zero GC, zero base64 overhead)
        if (wsConnected && ws && ws.readyState === WebSocket.OPEN) {
            const headerSize = 8;
            const packet = new Uint8Array(headerSize + len * 2 * 4);
            packet[0] = 0x01; // Audio Output
            packet[1] = 0x02; // 2 channels
            packet[2] = len & 0xFF;
            packet[3] = (len >> 8) & 0xFF;

            const rateInt = Math.round(currentRate);
            packet[4] = rateInt & 0xFF;
            packet[5] = (rateInt >> 8) & 0xFF;
            packet[6] = (rateInt >> 16) & 0xFF;
            packet[7] = (rateInt >> 24) & 0xFF;

            new Float32Array(packet.buffer, headerSize, len * 2).set(new Float32Array(pcmBuffer));

            try {
                ws.send(packet.buffer);
            } catch (err) {}
        }
        else {
            // 2. FALLBACK ONLY: Direct Native IPC (Used ONLY when WebSocket is not yet connected)
            const u8 = new Uint8Array(pcmBuffer);
            let bin = "";
            const chunkSz = 1024;
            for (let i = 0; i < u8.length; i += chunkSz) {
                bin += String.fromCharCode.apply(null, u8.subarray(i, i + chunkSz));
            }
            const b64 = btoa(bin);

            sendNativeJuceEvent("dawAudioData", { pcm: b64, channels: 2, samples: len, sampleRate: currentRate });
        }
    }

    async function hookContext(ctx) {
        if (!ctx || ctx.__juceHooked) return;
        ctx.__juceHooked = true;
        allHookedContexts.add(ctx);
        window.__juceAudioCtx = ctx;

        try {
            if (ctx.destination) {
                try { ctx.destination.channelCount = 2; } catch(e) {}
            }

            const masterTap = ctx.createGain();
            masterTap.gain.value = 1.0;
            try { masterTap.channelCount = 2; } catch(e) {}
            ctx.__juceMasterTap = masterTap;

            const silentSink = ctx.createGain();
            // Imperceptible non-zero gain when muted so browser never treats as dead branch
            silentSink.gain.value = CONFIG.muteSystemAudio ? 0.00001 : 1.0;
            try { silentSink.channelCount = 2; } catch(e) {}
            ctx.__juceSilentSink = silentSink;

            let node = null;

            // Preferred path: AudioWorkletNode
            if (ctx.audioWorklet && typeof ctx.audioWorklet.addModule === "function") {
                try {
                    if (!captureWorkletUrl) {
                        const blob = new Blob([CAPTURE_WORKLET_SRC], { type: "application/javascript" });
                        captureWorkletUrl = URL.createObjectURL(blob);
                    }
                    await ctx.audioWorklet.addModule(captureWorkletUrl);
                    node = new AudioWorkletNode(ctx, "juce-capture-processor", {
                        numberOfInputs: 1,
                        numberOfOutputs: 1,
                        channelCount: 2,
                        channelCountMode: "explicit",
                        channelInterpretation: "discrete",
                        outputChannelCount: [2]
                    });
                    node.port.onmessage = (e) => {
                        handleCapturedBlock(e.data.sr, e.data.len, e.data.pcm);
                    };
                } catch (err) {
                    console.warn("[JUCE-WebBridge] AudioWorklet unavailable, falling back to ScriptProcessor:", err);
                    node = null;
                }
            }

            // Fallback: legacy ScriptProcessorNode (main-thread; only used on
            // engines without AudioWorklet support)
            if (!node) {
                const proc = ctx.createScriptProcessor(CONFIG.bufferSize, 2, 2);
                try { proc.channelCount = 2; } catch(e) {}
                proc.onaudioprocess = function(e) {
                    const inL = e.inputBuffer.getChannelData(0);
                    const inR = e.inputBuffer.numberOfChannels > 1 ? e.inputBuffer.getChannelData(1) : inL;
                    const len = inL.length;
                    const currentRate = (this.context && this.context.sampleRate) ? this.context.sampleRate : (CONFIG.targetSampleRate || 48000);

                    if (!CONFIG.muteSystemAudio) {
                        const outL = e.outputBuffer.getChannelData(0);
                        const outR = e.outputBuffer.numberOfChannels > 1 ? e.outputBuffer.getChannelData(1) : outL;
                        outL.set(inL);
                        if (outR !== outL) outR.set(inR);
                    }

                    const f32 = new Float32Array(len * 2);
                    for (let i = 0; i < len; i++) {
                        f32[i * 2 + 0] = inL[i];
                        f32[i * 2 + 1] = inR[i];
                    }
                    handleCapturedBlock(currentRate, len, f32.buffer);
                };
                node = proc;
            }

            ctx.__juceProc = node;

            // Direct native connection to destination (bypassing custom connect hook)
            if (origConnect) {
                origConnect.call(masterTap, node);
                origConnect.call(node, silentSink);
                try {
                    origConnect.call(silentSink, ctx.destination);
                } catch(e) {}
            }

            console.log("[JUCE-WebBridge] AudioContext hooked successfully (" + (node.port ? "AudioWorklet" : "ScriptProcessor") + ") for Bitwig VST3!");
        } catch (err) {
            console.error("[JUCE-WebBridge] Error hooking AudioContext:", err);
        }
    }

    function updateMuteState() {
        for (const ctx of allHookedContexts) {
            if (ctx && ctx.__juceSilentSink) {
                ctx.__juceSilentSink.gain.value = CONFIG.muteSystemAudio ? 0.00001 : 1.0;
            }
        }
    }

    if (OrigAudioContext) {
        window.AudioContext = class extends OrigAudioContext {
            constructor(...args) {
                let opts = args[0] || {};
                if (typeof opts !== 'object') opts = {};
                if (CONFIG.targetSampleRate > 0 && !opts.sampleRate) {
                    opts = Object.assign({}, opts, { sampleRate: CONFIG.targetSampleRate });
                    args[0] = opts;
                }
                super(...args);
                hookContext(this);
            }
        };

        if (window.webkitAudioContext) {
            window.webkitAudioContext = window.AudioContext;
        }

        if (origConnect) {
            AudioNode.prototype.connect = function(destination, outputIndex, inputIndex) {
                try {
                    if (this.context) {
                        // CRITICAL: if this is one of our internal bridge nodes, connect natively
                        if (this === this.context.__juceMasterTap ||
                            this === this.context.__juceProc ||
                            this === this.context.__juceSilentSink) {
                            return origConnect.apply(this, arguments);
                        }

                        hookContext(this.context);
                        const tap = this.context.__juceMasterTap;

                        // If connecting to native destination: redirect to master tap!
                        if (tap && (destination === this.context.destination || 
                                    (typeof AudioDestinationNode !== "undefined" && destination instanceof AudioDestinationNode))) {
                            origConnect.call(this, tap, outputIndex || 0, inputIndex || 0);

                            if (!CONFIG.muteSystemAudio) {
                                return origConnect.call(this, destination, outputIndex || 0, inputIndex || 0);
                            }
                            return destination;
                        }
                    }
                } catch (e) {
                    console.warn("[JUCE-WebBridge] Error in connect tap:", e);
                }
                return origConnect.apply(this, arguments);
            };
        }
    }

    // =========================================================================
    // 4. HTML5 MEDIA ELEMENT CAPTURE (<audio>, <video>, YouTube in YPC2000)
    // =========================================================================
    if (typeof HTMLMediaElement !== "undefined") {
        const origPlay = HTMLMediaElement.prototype.play;
        HTMLMediaElement.prototype.play = function() {
            try {
                if (!this.__juceMediaTapped && window.__juceAudioCtx && window.__juceAudioCtx.__juceMasterTap) {
                    this.__juceMediaTapped = true;
                    try {
                        const source = window.__juceAudioCtx.createMediaElementSource(this);
                        source.connect(window.__juceAudioCtx.__juceMasterTap);
                        if (CONFIG.muteSystemAudio) {
                            this.muted = false; // keep element playing internally
                        }
                    } catch (err) {}
                }
            } catch(e) {}
            return origPlay.apply(this, arguments);
        };
    }

    // =========================================================================
    // 5. GETUSERMEDIA SHIM (Sample directly from Bitwig Track Audio)
    // =========================================================================
    if (typeof navigator !== "undefined" && navigator.mediaDevices) {
        const origGetUserMedia = navigator.mediaDevices.getUserMedia ? 
            navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices) : null;

        navigator.mediaDevices.getUserMedia = async function(constraints) {
            if (constraints && (constraints.audio || constraints.audio === true)) {
                console.log("[JUCE-WebBridge] getUserMedia({ audio }) intercepted! Streaming Bitwig track audio into sampler...");
                const ctx = window.__juceAudioCtx || new OrigAudioContext();
                hookContext(ctx);

                const streamDest = ctx.createMediaStreamDestination();
                const feedProc = ctx.createScriptProcessor(CONFIG.bufferSize, 0, 2);

                feedProc.onaudioprocess = function(e) {
                    const outL = e.outputBuffer.getChannelData(0);
                    const outR = e.outputBuffer.getChannelData(1);
                    const len = outL.length;

                    if (dawInputRingBuffer.length >= len * 2) {
                        for (let i = 0; i < len; i++) {
                            outL[i] = dawInputRingBuffer.shift();
                            outR[i] = dawInputRingBuffer.shift();
                        }
                    } else {
                        outL.fill(0);
                        outR.fill(0);
                    }
                };

                feedProc.connect(streamDest);
                return streamDest.stream;
            }

            if (origGetUserMedia) return origGetUserMedia(constraints);
            throw new Error("getUserMedia not supported");
        };
    }

    function scanAndHook() {
        if (typeof window !== "undefined") {
            for (const k of Object.getOwnPropertyNames(window)) {
                try {
                    const v = window[k];
                    if (v && (v instanceof OrigAudioContext || (v.destination && typeof v.createGain === "function"))) {
                        hookContext(v);
                    }
                } catch(e) {}
            }
        }
    }

    scanAndHook();
    if (typeof window !== "undefined") {
        window.addEventListener("DOMContentLoaded", scanAndHook);
        window.addEventListener("load", scanAndHook);
    }

    // Global controller
    window.__JUCE_BRIDGE__ = {
        config: CONFIG,
        getConnected: () => true,
        setMuteSystemAudio: (mute) => {
            CONFIG.muteSystemAudio = !!mute;
            updateMuteState();
        },
        dispatchMidiFromDaw: dispatchMidiFromDaw,
        dispatchMidiFromOsc: function(address, args) {
            try {
                let status = 0;
                let d1 = 0;
                let d2 = 0;

                // Support a generic OSC payload shape that carries MIDI-style fields
                // as either an array [status, d1, d2] or an object with keys.
                if (Array.isArray(args) && args.length >= 3) {
                    status = Number(args[0]) || 0;
                    d1 = Number(args[1]) || 0;
                    d2 = Number(args[2]) || 0;
                }
                else if (args && typeof args === 'object') {
                    status = Number(args.status || args.type || args.event || 0) || 0;
                    d1 = Number(args.d1 || args.note || args.data1 || args.channel || 0) || 0;
                    d2 = Number(args.d2 || args.velocity || args.data2 || args.value || 0) || 0;
                }
                else if (typeof args === 'string') {
                    const m = String(args).trim().match(/^(\d+)[,\s]+(\d+)[,\s]+(\d+)$/);
                    if (m) {
                        status = Number(m[1]);
                        d1 = Number(m[2]);
                        d2 = Number(m[3]);
                    }
                }

                if (address && typeof address === 'string' && address.length > 0) {
                    // Keep this permissive: only MIDI-style packets are forwarded.
                    if (status >= 0x80 && status <= 0xEF && d1 >= 0 && d1 <= 127 && d2 >= 0 && d2 <= 127) {
                        sendNativeJuceEvent("dawMidiData", {
                            status: status & 0xFF,
                            d1: d1 & 0xFF,
                            d2: d2 & 0xFF
                        });
                    }
                }
            } catch (e) {
                console.warn("[JUCE-WebBridge] dispatchMidiFromOsc error:", e);
            }
        },
        setBpm: function(bpm) {
            try {
                if (typeof bpm !== 'number' || isNaN(bpm) || bpm <= 0) return;
                CONFIG.dawBpm = bpm;
                const cps = bpm / 240.0;
                const cpm = bpm / 4.0;

                if (window.strudelMirror && window.strudelMirror.repl) {
                    if (typeof window.strudelMirror.repl.setCps === 'function') {
                        window.strudelMirror.repl.setCps(cps);
                    }
                    if (window.strudelMirror.repl.scheduler && typeof window.strudelMirror.repl.scheduler.setCps === 'function') {
                        window.strudelMirror.repl.scheduler.setCps(cps);
                    }
                }
                if (typeof window.setcpm === 'function') {
                    try { window.setcpm(cpm); } catch(e) {}
                }
                if (typeof window.setcps === 'function') {
                    try { window.setcps(cps); } catch(e) {}
                }
            } catch(e) {
                console.warn("[JUCE-WebBridge] setBpm error:", e);
            }
        },
        alignTransport: function(ppq, bpm, sigNum, sigDen) {
            try {
                const bpc = (typeof sigNum === 'number' && sigNum > 0) ? sigNum : 4;
                const targetBpm = (typeof bpm === 'number' && bpm > 0) ? bpm : (CONFIG.dawBpm || 120.0);
                const targetCps = targetBpm / (bpc * 60.0);
                const cycle = (typeof ppq === 'number' ? ppq : 0.0) / bpc;

                if (window.strudelMirror && window.strudelMirror.repl && window.strudelMirror.repl.scheduler) {
                    const sched = window.strudelMirror.repl.scheduler;
                    sched.lastEnd = cycle;
                    sched.lastBegin = cycle;
                    sched.num_cycles_at_cps_change = cycle;
                    sched.num_ticks_since_cps_change = 0;
                    sched.setCps(targetCps);
                }
            } catch(e) {
                console.warn("[JUCE-WebBridge] alignTransport error:", e);
            }
        },
        setTransportPlay: function(play, transportInfo) {
            try {
                if (play) {
                    CONFIG.transportPlaying = true;
                    resumeAllContexts();

                    // Restore master tap volume
                    for (const ctx of allHookedContexts) {
                        if (ctx && ctx.__juceMasterTap) {
                            try {
                                ctx.__juceMasterTap.gain.cancelScheduledValues(ctx.currentTime);
                                ctx.__juceMasterTap.gain.setValueAtTime(1.0, ctx.currentTime);
                            } catch(e) {}
                        }
                    }

                    const info = transportInfo || {};
                    const targetBpm = (typeof info.bpm === 'number' && info.bpm > 0) ? info.bpm : (CONFIG.dawBpm || 120.0);
                    const bpc = (typeof info.sigNum === 'number' && info.sigNum > 0) ? info.sigNum : 4;
                    const targetCps = targetBpm / (bpc * 60.0);
                    CONFIG.dawBpm = targetBpm;

                    if (window.strudelMirror) {
                        try {
                            const repl = window.strudelMirror.repl;
                            if (repl && typeof repl.setCps === 'function') {
                                repl.setCps(targetCps);
                            }
                            if (repl && repl.scheduler && typeof repl.scheduler.setCps === 'function') {
                                repl.scheduler.setCps(targetCps);
                            }

                            const align = () => {
                                if (CONFIG.transportPlaying && window.__JUCE_BRIDGE__ && typeof window.__JUCE_BRIDGE__.alignTransport === 'function') {
                                    window.__JUCE_BRIDGE__.alignTransport(info.ppq, targetBpm, bpc);
                                }
                            };

                            if (typeof window.strudelMirror.evaluate === 'function') {
                                const evalRes = window.strudelMirror.evaluate();
                                if (evalRes && typeof evalRes.then === 'function') {
                                    evalRes.then(align).catch(e => console.warn("[JUCE-WebBridge] evaluate error:", e));
                                } else {
                                    align();
                                }
                                return;
                            }
                            if (repl && typeof repl.evaluate === 'function') {
                                const evalRes = repl.evaluate(window.strudelMirror.code || "");
                                if (evalRes && typeof evalRes.then === 'function') {
                                    evalRes.then(align).catch(e => console.warn("[JUCE-WebBridge] repl evaluate error:", e));
                                } else {
                                    align();
                                }
                                return;
                            }
                        } catch(e) {
                            console.warn("[JUCE-WebBridge] strudelMirror start error:", e);
                        }
                    }

                    const playBtns = document.querySelectorAll('button[title="play"], button[title*="play" i], button[aria-label*="play" i]');
                    playBtns.forEach(btn => btn.click());
                } else {
                    CONFIG.transportPlaying = false;
                    try {
                        virtualMidiOutput.allNotesOff();
                        window.postMessage('strudel-stop', '*');
                    } catch(e) {}

                    // Mute master tap instantly to cut off any ringing Web Audio voices
                    for (const ctx of allHookedContexts) {
                        if (ctx && ctx.__juceMasterTap) {
                            try {
                                ctx.__juceMasterTap.gain.cancelScheduledValues(ctx.currentTime);
                                ctx.__juceMasterTap.gain.setValueAtTime(0.0, ctx.currentTime);
                            } catch(e) {}
                        }
                    }

                    let stoppedNatively = false;
                    if (window.strudelMirror) {
                        try {
                            if (typeof window.strudelMirror.stop === 'function') {
                                window.strudelMirror.stop();
                                stoppedNatively = true;
                            } else if (window.strudelMirror.repl && typeof window.strudelMirror.repl.stop === 'function') {
                                window.strudelMirror.repl.stop();
                                stoppedNatively = true;
                            }
                        } catch(e) {}
                        try {
                            const ed = window.strudelMirror.editor;
                            document.dispatchEvent(new CustomEvent('repl-stop', { detail: { view: ed } }));
                            window.dispatchEvent(new CustomEvent('repl-stop', { detail: { view: ed } }));
                        } catch(e) {}
                    }

                    if (!stoppedNatively) {
                        try {
                            const stopBtns = document.querySelectorAll('button[title="stop"], button[title*="stop" i], button[aria-label*="stop" i]');
                            stopBtns.forEach(btn => btn.click());
                            document.dispatchEvent(new CustomEvent('stop-repl'));
                        } catch(e) {}
                    }
                }
            } catch(e) {
                console.warn("[JUCE-WebBridge] setTransportPlay error:", e);
            }
        },
        sendTestMidi: (note = 60, vel = 100) => {
            dispatchMidiFromDaw(0x90, note, vel);
            setTimeout(() => dispatchMidiFromDaw(0x80, note, 0), 250);
        },
        resumeAudio: resumeAllContexts
    };

    console.log("[JUCE-WebBridge] All hooks active: Bitwig Track Audio & MIDI Ready!");
})();
)JS")
        .replace("%PORT%", juce::String(bridgePort))
        .replace("%SAMPLE_RATE%", juce::String(targetSampleRate))
        .replace("%BUFFER_SIZE%", juce::String(bufferSize))
        .replace("%INITIAL_BPM%", juce::String(initialBpm, 2));
}

} // namespace WebBridge
