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
                                         private juce::TextEditor::Listener,
                                         private juce::Timer
{
public:
    explicit StrudelPlugAudioProcessorEditor (StrudelPlugAudioProcessor&);
    ~StrudelPlugAudioProcessorEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

    void navigateTo (const juce::String& url);
    void sendMidiToBrowser (const juce::MidiMessage& msg);
    void updateSyncButtonAppearance();
    void onBrowserUrlChanged (const juce::String& url);

private:
    StrudelPlugAudioProcessor& audioProcessor;

    // --- Navigation Header Row ---
    juce::TextButton backButton { juce::CharPointer_UTF8 ("\xe2\x97\x80") };    // ◀
    juce::TextButton forwardButton { juce::CharPointer_UTF8 ("\xe2\x96\xb6") }; // ▶
    juce::TextButton reloadButton { juce::CharPointer_UTF8 ("\xe2\x86\xbb") };  // ⟳
    juce::TextEditor urlEditor;
    juce::TextButton goButton { "GO" };

    // Preset quick buttons
    juce::TextButton strudelCcBtn { "strudel.cc" };
    juce::TextButton localPort54321Btn { "Local :54321" };
    juce::TextButton startNodeBtn { "▶ Start Node" };

    juce::Label statusLabel;

    // --- Options Strip Controls ---
    juce::TextButton syncDawBtn { "● SYNC DAW" };
    
    juce::Label srLabel { {}, "Hz:" };
    juce::ComboBox srComboBox;

    juce::Label cushionLabel { {}, "Buf:" };
    juce::ComboBox cushionComboBox;

    juce::Label gainLabel { {}, "Gain:" };
    juce::Slider gainSlider;

    juce::TextButton flushBtn { juce::CharPointer_UTF8 ("\xe2\x9f\xb2 Flush") }; // ⟲ Flush

    juce::Label audioLevelLed { {}, "OUT" };
    juce::Label midiLed { {}, "MIDI" };
    juce::Label telemetryLabel;

    void textEditorReturnKeyPressed (juce::TextEditor&) override;
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StrudelPlugAudioProcessorEditor)
};
