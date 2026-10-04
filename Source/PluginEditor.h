/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "StrudelBrowserComponent.h"

//==============================================================================
/**
*/
class StrudelPlugAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                         private juce::Timer
{
public:
    explicit StrudelPlugAudioProcessorEditor (StrudelPlugAudioProcessor&);
    ~StrudelPlugAudioProcessorEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

    void sendMidiToBrowser (const juce::MidiMessage& msg);
    void updateSyncButtonAppearance();
    void onBrowserUrlChanged (const juce::String& url);

private:
    StrudelPlugAudioProcessor& audioProcessor;

    static constexpr int headerHeight   = 32;
    static constexpr int settingsHeight = 36;

    // --- Header Row: status, transport, levels, gain; settings toggle at far right ---
    juce::Label statusLabel;
    juce::TextButton syncDawBtn { "● SYNC DAW" };
    juce::Label audioLevelLed { {}, "OUT" };
    juce::Label midiLed { {}, "MIDI" };
    juce::Label gainLabel { {}, "Gain:" };
    juce::Slider gainSlider;
    juce::TextButton settingsBtn { juce::CharPointer_UTF8 ("\xe2\x9a\x99") };  // ⚙ shows/hides the settings row

    // --- Settings Row (collapsed by default) ---
    juce::Label srLabel { {}, "Hz:" };
    juce::ComboBox srComboBox;
    juce::Label cushionLabel { {}, "Buf:" };
    juce::ComboBox cushionComboBox;
    juce::Label telemetryLabel;
    juce::TextButton reloadButton { juce::CharPointer_UTF8 ("\xe2\x86\xbb") };  // ⟳
    juce::TextButton fetchBtn { juce::CharPointer_UTF8 ("\xe2\x86\x93") };     // ↓ download/update @strudel/repl
    bool settingsOpen = false;

    void setSettingsOpen (bool open);
    void reloadPage();
    void fetchStrudel();

    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StrudelPlugAudioProcessorEditor)
};
