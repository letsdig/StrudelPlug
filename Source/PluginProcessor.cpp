/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::String StrudelPlugAudioProcessor::sharedServerUrl;
juce::String StrudelPlugAudioProcessor::sharedSessionId;
bool StrudelPlugAudioProcessor::sharedServerStarted = false;

//==============================================================================
StrudelPlugAudioProcessor::StrudelPlugAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
#endif
{
   #if JUCE_LINUX
    setenv ("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 1);
    setenv ("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 1);
   #endif

    bridgeServer.startServer();
}

StrudelPlugAudioProcessor::~StrudelPlugAudioProcessor()
{
    browser.reset();
}

//==============================================================================
const juce::String StrudelPlugAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool StrudelPlugAudioProcessor::acceptsMidi() const
{
    return true;
}

bool StrudelPlugAudioProcessor::producesMidi() const
{
    return true;
}

bool StrudelPlugAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double StrudelPlugAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int StrudelPlugAudioProcessor::getNumPrograms()
{
    return 1;
}

int StrudelPlugAudioProcessor::getCurrentProgram()
{
    return 0;
}

void StrudelPlugAudioProcessor::setCurrentProgram (int index)
{
}

const juce::String StrudelPlugAudioProcessor::getProgramName (int index)
{
    return {};
}

void StrudelPlugAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

//==============================================================================
void StrudelPlugAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    bridgeServer.setDawSampleRate ((int) sampleRate);
    bridgeServer.startServer();
    setLatencySamples (bridgeServer.getJitterCushionSamples());
}

void StrudelPlugAudioProcessor::releaseResources()
{
    // Do not kill the bridge server here: stopping the server on suspend/sample rate changes
    // causes port hopping, dropped WebSocket connections, and multi-second thread blocks.
    // The server is cleanly stopped in ~WebBridgeServer() upon plugin destruction.
    bridgeServer.flushAudioBuffer();
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool StrudelPlugAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    if (! layouts.getMainInputChannelSet().isDisabled()
     && layouts.getMainInputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
   #endif

    return true;
  #endif
}
#endif

void StrudelPlugAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    // 0. DAW Transport Synchronization (Play / Stop / BPM / Beat Sync)
    bool stoppedThisBlock = false;

    if (auto* playHead = getPlayHead())
    {
        if (auto pos = playHead->getPosition())
        {
            const bool isPlaying = pos->getIsPlaying();
            const auto bpmOpt = pos->getBpm();
            const double currentBpm = bpmOpt.hasValue() ? *bpmOpt : 120.0;
            const auto ppqOpt = pos->getPpqPosition();
            const double currentPpq = ppqOpt.hasValue() ? *ppqOpt : 0.0;
            const auto timeSig = pos->getTimeSignature();
            const int sigNum = timeSig.hasValue() ? timeSig->numerator : 4;
            const int sigDen = timeSig.hasValue() ? timeSig->denominator : 4;

            if (isPlaying != wasDawPlaying.load())
            {
                wasDawPlaying.store (isPlaying);
                if (dawSyncEnabled.load())
                {
                    triggerBrowserPlayback (isPlaying, currentBpm, currentPpq, sigNum, sigDen);
                    bridgeServer.flushAudioBuffer();
                }

                if (! isPlaying)
                {
                    stoppedThisBlock = true;
                    bridgeServer.sendAllNotesOff();
                }
            }
            else if (isPlaying && dawSyncEnabled.load())
            {
                // Detect transport seek or loop wrap while playing
                const double expectedPpq = lastPpqPosition.load() + ((double) numSamples / getSampleRate()) * (currentBpm / 60.0);
                if (std::abs (currentPpq - expectedPpq) > 0.25)
                {
                    triggerBrowserSeek (currentPpq, currentBpm, sigNum, sigDen);
                    bridgeServer.flushAudioBuffer();
                }
            }

            lastPpqPosition.store (currentPpq);

            if (std::abs (currentBpm - lastDawBpm.load()) > 0.05)
            {
                lastDawBpm.store (currentBpm);
                if (dawSyncEnabled.load())
                {
                    triggerBrowserTempo (currentBpm);
                }
            }
        }
    }

    // 1. Forward DAW incoming MIDI to WebBridge (so DAW notes can reach browser)
    for (const auto metadata : midiMessages)
    {
        bridgeServer.sendMidiToBrowser (metadata.getMessage());
    }

    // 3. Clear audio outputs before writing captured Web Audio
    for (int i = 0; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, numSamples);

    // 4. Read Web Audio PCM from browser FIFO into DAW output buffer!
    bridgeServer.readAudioFromBrowser (buffer.getArrayOfWritePointers(), totalNumOutputChannels, numSamples);

    // 5. Output any MIDI generated by Strudel into DAW midiMessages!
    midiMessages.clear();
    if (stoppedThisBlock)
    {
        for (int ch = 1; ch <= 16; ++ch)
        {
            midiMessages.addEvent (juce::MidiMessage::allSoundOff (ch), 0);
            midiMessages.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
        }
    }
    bridgeServer.getMidiFromBrowser (midiMessages, 0);
}

void StrudelPlugAudioProcessor::pushBase64AudioFromBrowser (const juce::String& base64, double sourceSampleRate)
{
    juce::MemoryOutputStream mos;
    if (juce::Base64::convertFromBase64 (mos, base64))
    {
        const size_t numBytes = mos.getDataSize();
        const int numFloats = static_cast<int> (numBytes / sizeof (float));
        if (numFloats >= 2)
        {
            const float* f32 = reinterpret_cast<const float*> (mos.getData());
            const int sampleCount = numFloats / 2;
            bridgeServer.writeAudioToFifo (f32, 2, sampleCount, sourceSampleRate);
        }
    }
}

StrudelBrowserComponent* StrudelPlugAudioProcessor::getOrCreateBrowser()
{
    if (browser == nullptr)
    {
        createPersistentBrowser();
    }
    return browser.get();
}

void StrudelPlugAudioProcessor::createPersistentBrowser()
{
    if (browser != nullptr)
        return;

    juce::WebBrowserComponent::Options options;
    options = options.withNativeIntegrationEnabled (true)
                     .withKeepPageLoadedWhenBrowserIsHidden()
                    #if JUCE_LINUX
                     .withUserAgent ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36")
                    #endif
                     .withUserScript (WebBridge::getInjectionScript (bridgeServer.getPort(), getEffectiveSampleRate(), 512, lastDawBpm.load()))
                     .withEventListener ("dawAudioData", [this] (const juce::var& data)
                     {
                         if (auto* obj = data.getDynamicObject())
                         {
                             auto b64 = obj->getProperty ("pcm").toString();
                             double srcRate = (double) obj->getProperty ("sampleRate");
                             if (b64.isNotEmpty())
                                 pushBase64AudioFromBrowser (b64, srcRate);
                         }
                     })
                     .withEventListener ("dawMidiData", [this] (const juce::var& data)
                     {
                         if (auto* obj = data.getDynamicObject())
                         {
                             int status = (int) obj->getProperty ("status");
                             int d1 = (int) obj->getProperty ("d1");
                             int d2 = (int) obj->getProperty ("d2");
                             bridgeServer.injectMidiFromBrowser (status, d1, d2);
                         }
                         else if (data.isArray() && data.size() >= 3)
                         {
                             int status = (int) data[0];
                             int d1 = (int) data[1];
                             int d2 = (int) data[2];
                             bridgeServer.injectMidiFromBrowser (status, d1, d2);
                         }
                     })
                     .withEventListener ("requestCode", [this] (const juce::var&)
                     {
                         restoreBrowserCode (code);
                     })
                     .withEventListener ("saveCode", [this] (const juce::var& data)
                     {
                         if (auto* obj = data.getDynamicObject())
                         {
                             auto c = obj->getProperty ("code").toString();
                             if (c.isNotEmpty())
                                 setCode (c);
                         }
                         else if (data.isString())
                         {
                             setCode (data.toString());
                         }
                     });

    browser = std::make_unique<StrudelBrowserComponent> (
        options,
        [this] (const juce::String& loadedUrl)
        {
            if (auto* ed = activeEditor.load())
                ed->onBrowserUrlChanged (loadedUrl);
        },
        [this] (const juce::String& /*loadedUrl*/)
        {
            if (browser)
            {
                browser->evaluateJavascript (
                    WebBridge::getInjectionScript (bridgeServer.getPort(), getEffectiveSampleRate(), 512, lastDawBpm.load()));

                triggerBrowserTempo (lastDawBpm.load());

                if (code.isNotEmpty())
                {
                    restoreBrowserCode (code);
                }

                if (isDawSyncEnabled() && isDawPlaying())
                {
                    triggerBrowserPlayback (true, lastDawBpm.load(), lastPpqPosition.load());
                }
            }
        });

    browser->goToURL (getLocalStrudelUrl());
}

juce::String StrudelPlugAudioProcessor::getLocalStrudelUrl() const
{
    return "http://127.0.0.1:" + juce::String (bridgeServer.getPort()) + "/strudel/";
}

void StrudelPlugAudioProcessor::restoreBrowserCode (const juce::String& codeToRestore)
{
    if (codeToRestore.isEmpty())
        return;

    juce::MessageManager::callAsync ([this, codeToRestore]()
    {
        if (browser)
        {
            juce::String js = "if (window.__JUCE_BRIDGE__ && typeof window.__JUCE_BRIDGE__.restoreCode === 'function') {"
                              "    window.__JUCE_BRIDGE__.restoreCode(" + juce::JSON::toString (codeToRestore) + ");"
                              "}";
            browser->evaluateJavascript (js);
        }
    });
}

void StrudelPlugAudioProcessor::triggerBrowserPlayback (bool isPlaying, double bpm, double ppq, int sigNum, int sigDen)
{
    juce::MessageManager::callAsync ([this, isPlaying, bpm, ppq, sigNum, sigDen]()
    {
        if (browser)
        {
            if (isPlaying)
            {
                juce::String js = juce::String::formatted (
                    "(function() {"
                    "    if (window.__JUCE_BRIDGE__ && typeof window.__JUCE_BRIDGE__.setTransportPlay === 'function') {"
                    "        window.__JUCE_BRIDGE__.setTransportPlay(true, { bpm: %.2f, ppq: %.4f, sigNum: %d, sigDen: %d });"
                    "    } else if (window.strudelMirror && typeof window.strudelMirror.evaluate === 'function') {"
                    "        window.strudelMirror.evaluate();"
                    "    }"
                    "})();",
                    bpm, ppq, sigNum, sigDen
                );
                browser->evaluateJavascript (js);
            }
            else
            {
                browser->evaluateJavascript (
                    "(function() {"
                    "    if (window.__JUCE_BRIDGE__ && typeof window.__JUCE_BRIDGE__.setTransportPlay === 'function') {"
                    "        window.__JUCE_BRIDGE__.setTransportPlay(false);"
                    "    } else {"
                    "        let stoppedNatively = false;"
                    "        if (window.strudelMirror) {"
                    "            if (typeof window.strudelMirror.stop === 'function') { window.strudelMirror.stop(); stoppedNatively = true; }"
                    "            else if (window.strudelMirror.repl && typeof window.strudelMirror.repl.stop === 'function') { window.strudelMirror.repl.stop(); stoppedNatively = true; }"
                    "        }"
                    "        if (!stoppedNatively) {"
                    "            const sBtn = document.querySelector('button[title=\"stop\"], button[title*=\"stop\" i]');"
                    "            if (sBtn) sBtn.click();"
                    "        }"
                    "    }"
                    "})();"
                );
            }
        }
    });
}

void StrudelPlugAudioProcessor::triggerBrowserSeek (double ppq, double bpm, int sigNum, int sigDen)
{
    juce::MessageManager::callAsync ([this, ppq, bpm, sigNum, sigDen]()
    {
        if (browser)
        {
            juce::String js = juce::String::formatted (
                "if (window.__JUCE_BRIDGE__ && typeof window.__JUCE_BRIDGE__.alignTransport === 'function') {"
                "    window.__JUCE_BRIDGE__.alignTransport(%.4f, %.2f, %d, %d);"
                "}",
                ppq, bpm, sigNum, sigDen
            );
            browser->evaluateJavascript (js);
        }
    });
}

void StrudelPlugAudioProcessor::triggerBrowserTempo (double bpm)
{
    juce::MessageManager::callAsync ([this, bpm]()
    {
        if (browser)
        {
            browser->evaluateJavascript (
                "if (window.__JUCE_BRIDGE__ && typeof window.__JUCE_BRIDGE__.setBpm === 'function') {"
                "    window.__JUCE_BRIDGE__.setBpm(" + juce::String (bpm, 2) + ");"
                "}"
            );
        }
    });
}

//==============================================================================
bool StrudelPlugAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* StrudelPlugAudioProcessor::createEditor()
{
    return new StrudelPlugAudioProcessorEditor (*this);
}

void StrudelPlugAudioProcessor::setCode (const juce::String& newCode)
{
    code = newCode;
}

juce::String StrudelPlugAudioProcessor::getCode() const
{
    return code;
}

void StrudelPlugAudioProcessor::evaluateCode()
{
    if (sharedServerUrl.isEmpty())
        serverStatus = "No Strudel server";
    else
        sendHttpRequest (sharedServerUrl + "/api/evaluate", code, serverStatus);
}

bool StrudelPlugAudioProcessor::connectRemoteServer (const juce::String& newRemoteUrl)
{
    if (newRemoteUrl.trim().isNotEmpty())
    {
        sharedServerUrl = newRemoteUrl.trim();
        sharedSessionId = "remote-" + juce::String (juce::Time::getMillisecondCounterHiRes());
        serverStatus = "Connected to " + sharedServerUrl;
        return true;
    }

    serverStatus = "Remote Strudel URL is empty";
    return false;
}

juce::String StrudelPlugAudioProcessor::joinExistingSession()
{
    if (sharedServerStarted || sharedServerUrl.isNotEmpty())
    {
        sessionId = sharedSessionId;
        serverStatus = "Joined existing shared Strudel session";
        return sessionId;
    }

    sessionId = "session-" + juce::String (juce::Time::getMillisecondCounterHiRes());
    sharedSessionId = sessionId;
    return sessionId;
}

juce::String StrudelPlugAudioProcessor::getServerStatus() const
{
    return serverStatus;
}

void StrudelPlugAudioProcessor::setServerUrl (const juce::String& url)
{
    if (url.trim().isNotEmpty())
        sharedServerUrl = url.trim();
}

juce::String StrudelPlugAudioProcessor::getServerUrl() const
{
    return sharedServerUrl;
}

void StrudelPlugAudioProcessor::sendMidiRoute (const juce::String& midiRoute)
{
    auto route = midiRoute.trim();
    if (route.isEmpty())
    {
        bridgeServer.setMidiOutputDevice({});
    }
    else
    {
        bridgeServer.setMidiOutputDevice(route);
    }
}

bool StrudelPlugAudioProcessor::sendHttpRequest (const juce::String& url, const juce::String& payload, juce::String& response)
{
    juce::URL request (url);
    if (payload.isNotEmpty())
        request = request.withPOSTData (payload);

    int statusCode = 0;
    auto options = juce::URL::InputStreamOptions (payload.isNotEmpty() ? juce::URL::ParameterHandling::inPostData
                                                                        : juce::URL::ParameterHandling::inAddress)
                       .withExtraHeaders ("Content-Type: application/json\r\n")
                       .withConnectionTimeoutMs (5000)
                       .withStatusCode (&statusCode);

    auto stream = request.createInputStream (options);
    if (stream != nullptr)
    {
        response = stream->readEntireStreamAsString();
        if (response.isEmpty() && statusCode > 0)
            response = "HTTP " + juce::String (statusCode);
        return (statusCode >= 200 && statusCode < 400);
    }

    response = "HTTP request failed";
    return false;
}

juce::String StrudelPlugAudioProcessor::shellQuote (const juce::String& value) const
{
    return value.replace ("'", "'\\''");
}

//==============================================================================
void StrudelPlugAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream stream (destData, true);
    stream.writeString (code);
    stream.writeString (sharedServerUrl);
    stream.writeString (sessionId);
    stream.writeBool (dawSyncEnabled.load());
    stream.writeInt (preferredSampleRate.load());
    stream.writeInt (bridgeServer.getJitterCushionSamples());
    stream.writeFloat (bridgeServer.getOutputGain());
}

void StrudelPlugAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::MemoryInputStream stream (data, sizeInBytes, false);
    if (! stream.isExhausted())
        code = stream.readString();
    if (! stream.isExhausted())
        sharedServerUrl = stream.readString();
    if (! stream.isExhausted())
        sessionId = stream.readString();
    if (! stream.isExhausted())
        dawSyncEnabled.store (stream.readBool());
    if (! stream.isExhausted())
        preferredSampleRate.store (stream.readInt());
    if (! stream.isExhausted())
    {
        int cushion = stream.readInt();
        bridgeServer.setJitterCushionSamples (cushion);
        setLatencySamples (cushion);
    }
    if (! stream.isExhausted())
    {
        bridgeServer.setOutputGain (stream.readFloat());
    }

    if (code.isNotEmpty())
    {
        restoreBrowserCode (code);
    }
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StrudelPlugAudioProcessor();
}
