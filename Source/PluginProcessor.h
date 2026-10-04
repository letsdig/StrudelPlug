/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <memory>
#include "WebBridge/WebBridgeServer.h"
#include "StrudelBrowserComponent.h"

class StrudelPlugAudioProcessorEditor;

//==============================================================================
/**
*/
class StrudelPlugAudioProcessor  : public juce::AudioProcessor
{
public:
    //==============================================================================
    StrudelPlugAudioProcessor();
    ~StrudelPlugAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Strudel session helpers
    void setServerUrl (const juce::String& url);
    juce::String getServerUrl() const;
    juce::String getLocalStrudelUrl() const;   // the only page the browser ever shows
    void setCode (const juce::String& newCode);
    juce::String getCode() const;
    void evaluateCode();
    bool connectRemoteServer (const juce::String& remoteUrl);
    juce::String joinExistingSession();
    juce::String getServerStatus() const;
    void sendMidiRoute (const juce::String& midiRoute);

    WebBridge::WebBridgeServer& getBridgeServer() noexcept { return bridgeServer; }
    void pushBase64AudioFromBrowser (const juce::String& base64, double sourceSampleRate = 0.0);
    void setEditor (StrudelPlugAudioProcessorEditor* ed) { activeEditor.store (ed); }

    void setDawSyncEnabled (bool enabled) noexcept { dawSyncEnabled.store (enabled); }
    bool isDawSyncEnabled() const noexcept { return dawSyncEnabled.load(); }
    bool isDawPlaying() const noexcept { return wasDawPlaying.load(); }

    void setPreferredSampleRate (int rate) noexcept { preferredSampleRate.store (rate); }
    int getPreferredSampleRate() const noexcept { return preferredSampleRate.load(); }
    int getEffectiveSampleRate() const noexcept
    {
        int p = preferredSampleRate.load();
        return p > 0 ? p : bridgeServer.getDawSampleRate();
    }

    StrudelBrowserComponent* getOrCreateBrowser();
    StrudelBrowserComponent* getBrowser() const noexcept { return browser.get(); }
    void createPersistentBrowser();
    void triggerBrowserPlayback (bool isPlaying, double bpm = 120.0, double ppq = 0.0, int sigNum = 4, int sigDen = 4);
    void triggerBrowserSeek (double ppq, double bpm = 120.0, int sigNum = 4, int sigDen = 4);
    void triggerBrowserTempo (double bpm);
    void restoreBrowserCode (const juce::String& codeToRestore);

private:
    //==============================================================================
    std::atomic<StrudelPlugAudioProcessorEditor*> activeEditor { nullptr };
    WebBridge::WebBridgeServer bridgeServer;
    std::unique_ptr<StrudelBrowserComponent> browser;

    std::atomic<bool> dawSyncEnabled { true };
    std::atomic<bool> wasDawPlaying { false };
    std::atomic<double> lastDawBpm { 120.0 };
    std::atomic<double> lastPpqPosition { 0.0 };
    std::atomic<int> preferredSampleRate { 0 };

    juce::String code;
    juce::String remoteUrl;
    juce::String sessionId;
    juce::String serverStatus;
    static juce::String sharedServerUrl;
    static juce::String sharedSessionId;
    static bool sharedServerStarted;

    bool sendHttpRequest (const juce::String& url, const juce::String& payload, juce::String& response);
    juce::String shellQuote (const juce::String& value) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StrudelPlugAudioProcessor)
};
