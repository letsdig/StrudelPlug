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
std::unique_ptr<juce::ChildProcess> StrudelPlugAudioProcessor::localServerProcess;

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
    setenv ("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 1);
    setenv ("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 1);

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
}

void StrudelPlugAudioProcessor::releaseResources()
{
    bridgeServer.stopServer();
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

    // 0. DAW Transport Synchronization (Play / Stop / BPM Sync)
    if (auto* playHead = getPlayHead())
    {
        if (auto pos = playHead->getPosition())
        {
            const bool isPlaying = pos->getIsPlaying();
            if (isPlaying != wasDawPlaying.load())
            {
                wasDawPlaying.store (isPlaying);
                if (dawSyncEnabled.load())
                {
                    triggerBrowserPlayback (isPlaying);
                    bridgeServer.flushAudioBuffer();
                }
            }

            if (const auto bpmOpt = pos->getBpm())
            {
                const double currentBpm = *bpmOpt;
                if (std::abs (currentBpm - lastDawBpm.load()) > 0.1)
                {
                    lastDawBpm.store (currentBpm);
                    if (dawSyncEnabled.load())
                    {
                        triggerBrowserTempo (currentBpm);
                    }
                }
            }
        }
    }

    // 1. Forward DAW incoming audio to browser (if channel has audio input)
    if (totalNumInputChannels > 0)
    {
        bridgeServer.sendAudioToBrowser (buffer.getArrayOfReadPointers(), totalNumInputChannels, numSamples);
    }

    // 2. Forward DAW incoming MIDI to WebBridge (so DAW notes can reach browser)
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
                     .withUserAgent ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36")
                     .withUserScript (WebBridge::getInjectionScript (bridgeServer.getPort(), getEffectiveSampleRate()))
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
                     });

    browser = std::make_unique<StrudelBrowserComponent> (
        options,
        [this] (const juce::String& loadedUrl)
        {
            setServerUrl (loadedUrl);
            if (auto* ed = activeEditor.load())
                ed->onBrowserUrlChanged (loadedUrl);
        },
        [this] (const juce::String& /*loadedUrl*/)
        {
            if (browser)
            {
                browser->evaluateJavascript (
                    WebBridge::getInjectionScript (bridgeServer.getPort(), getEffectiveSampleRate()));

                if (isDawSyncEnabled() && isDawPlaying())
                {
                    triggerBrowserPlayback (true);
                }
            }
        });

    auto target = getServerUrl();
    if (target.trim().isEmpty())
        target = "https://strudel.cc/";
    browser->goToURL (target);
}

void StrudelPlugAudioProcessor::reattachBrowserToHiddenHost()
{
    juce::MessageManager::callAsync ([this]()
    {
        if (browser == nullptr)
            return;

        if (hiddenHost == nullptr)
        {
            hiddenHost = std::make_unique<HiddenBrowserHost>();
            hiddenHost->setBounds (-10000, -10000, 16, 16); // fully off-screen
            hiddenHost->addToDesktop (0);
        }

        hiddenHost->setVisible (true);
        hiddenHost->addAndMakeVisible (*browser); // reparents; JUCE detaches any previous parent automatically
        browser->setBounds (hiddenHost->getLocalBounds());
    });
}

void StrudelPlugAudioProcessor::triggerBrowserPlayback (bool isPlaying)
{
    juce::MessageManager::callAsync ([this, isPlaying]()
    {
        if (browser)
        {
            if (isPlaying)
            {
                browser->evaluateJavascript (
                    "(function() {"
                    "    if (window.__JUCE_BRIDGE__ && typeof window.__JUCE_BRIDGE__.setTransportPlay === 'function') {"
                    "        window.__JUCE_BRIDGE__.setTransportPlay(true);"
                    "    } else if (window.strudelMirror && typeof window.strudelMirror.evaluate === 'function') {"
                    "        window.strudelMirror.evaluate();"
                    "    }"
                    "})();"
                );
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
                    "            if (window.strudelMirror.repl && window.strudelMirror.repl.scheduler) { window.strudelMirror.repl.scheduler.stop(); stoppedNatively = true; }"
                    "            if (typeof window.strudelMirror.stop === 'function') { window.strudelMirror.stop(); stoppedNatively = true; }"
                    "        }"
                    "        if (typeof window.hush === 'function') window.hush();"
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

bool StrudelPlugAudioProcessor::startLocalServer (const juce::String& localStrudelPath)
{
    if (localServerProcess != nullptr && localServerProcess->isRunning())
    {
        sharedServerUrl = "http://127.0.0.1:54321";
        serverStatus = "Local Strudel server running on :54321";
        return true;
    }

    juce::String command;
    if (localStrudelPath.trim().isNotEmpty())
        command = "node " + localStrudelPath.trim() + " --port 54321";
    else
        command = "npx -y @strudel/repl --port 54321";

    serverStatus = "Launching local Strudel server...";

    localServerProcess = std::make_unique<juce::ChildProcess>();
    if (localServerProcess->start (command) && localServerProcess->isRunning())
    {
        sharedServerStarted = true;
        sharedServerUrl = "http://127.0.0.1:54321";
        sharedSessionId = "local-" + juce::String (juce::Time::getMillisecondCounterHiRes());
        serverStatus = "Local Strudel server running on :54321";
        return true;
    }

    serverStatus = "Cannot start local Strudel server";
    return false;
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
    return sharedServerUrl.isNotEmpty() ? sharedServerUrl : "https://strudel.cc/";
}

void StrudelPlugAudioProcessor::sendMidiRoute (const juce::String& midiRoute)
{
    juce::ignoreUnused (midiRoute);
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
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StrudelPlugAudioProcessor();
}
