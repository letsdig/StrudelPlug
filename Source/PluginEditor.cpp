/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.
    OpenSynth1 Edition: Sleek dark titanium/slate aesthetic with cyan LCD displays.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
StrudelPlugAudioProcessorEditor::StrudelPlugAudioProcessorEditor (StrudelPlugAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    audioProcessor.setEditor (this);

    // Attach persistent browser owned by AudioProcessor (so it keeps playing even when closed!)
    auto* browser = audioProcessor.getOrCreateBrowser();
    if (browser != nullptr)
    {
        addAndMakeVisible (*browser);
        browser->setVisible (true);
    }

    // =========================================================================
    // Navigation Row Callbacks & Tooltips
    // =========================================================================
    backButton.setTooltip ("Go back");
    forwardButton.setTooltip ("Go forward");
    reloadButton.setTooltip ("Reload page");
    goButton.setTooltip ("Navigate to URL");

    backButton.onClick    = [this] { if (auto* b = audioProcessor.getBrowser()) b->goBack(); };
    forwardButton.onClick = [this] { if (auto* b = audioProcessor.getBrowser()) b->goForward(); };
    reloadButton.onClick  = [this] { if (auto* b = audioProcessor.getBrowser()) b->goToURL (urlEditor.getText().trim()); };
    goButton.onClick      = [this] { navigateTo (urlEditor.getText().trim()); };

    strudelCcBtn.onClick       = [this] { navigateTo ("https://strudel.cc/"); };
    localPort54321Btn.onClick  = [this] { navigateTo ("http://127.0.0.1:54321"); };
    startNodeBtn.onClick       = [this]
    {
        statusLabel.setText ("STARTING NODE...", juce::dontSendNotification);
        statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xffffb703));
        bool ok = audioProcessor.startLocalServer();
        if (ok)
        {
            statusLabel.setText ("NODE :54321 OK", juce::dontSendNotification);
            statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff38ef7d));
            navigateTo ("http://127.0.0.1:54321");
        }
        else
        {
            statusLabel.setText ("START FAILED", juce::dontSendNotification);
            statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xffff5555));
        }
    };

    // =========================================================================
    // Options Strip Setup
    // =========================================================================
    // 1. SYNC DAW Toggle Button
    syncDawBtn.setClickingTogglesState (true);
    syncDawBtn.setToggleState (audioProcessor.isDawSyncEnabled(), juce::dontSendNotification);
    syncDawBtn.setTooltip ("Synchronize Strudel playback with DAW Transport (Play/Stop/BPM)");
    syncDawBtn.onClick = [this]
    {
        bool sync = syncDawBtn.getToggleState();
        audioProcessor.setDawSyncEnabled (sync);
        updateSyncButtonAppearance();

        if (sync && audioProcessor.isDawPlaying())
            audioProcessor.triggerBrowserPlayback (true);
        else if (sync && ! audioProcessor.isDawPlaying())
            audioProcessor.triggerBrowserPlayback (false);
    };
    updateSyncButtonAppearance();
    addAndMakeVisible (syncDawBtn);

    // 2. Sample Rate Selector (Hz)
    srLabel.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
    srLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8c96ab));
    srLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (srLabel);

    srComboBox.addItem ("Auto (DAW)", 1);
    srComboBox.addItem ("44.1 kHz", 2);
    srComboBox.addItem ("48.0 kHz", 3);
    srComboBox.addItem ("88.2 kHz", 4);
    srComboBox.addItem ("96.0 kHz", 5);
    srComboBox.addItem ("192.0 kHz", 6);
    srComboBox.setTooltip ("WebKit AudioContext sample rate");

    int prefRate = audioProcessor.getPreferredSampleRate();
    if (prefRate == 44100)       srComboBox.setSelectedId (2, juce::dontSendNotification);
    else if (prefRate == 48000)  srComboBox.setSelectedId (3, juce::dontSendNotification);
    else if (prefRate == 88200)  srComboBox.setSelectedId (4, juce::dontSendNotification);
    else if (prefRate == 96000)  srComboBox.setSelectedId (5, juce::dontSendNotification);
    else if (prefRate == 192000) srComboBox.setSelectedId (6, juce::dontSendNotification);
    else                         srComboBox.setSelectedId (1, juce::dontSendNotification);

    srComboBox.onChange = [this]
    {
        int id = srComboBox.getSelectedId();
        int rate = 0;
        if (id == 2) rate = 44100;
        else if (id == 3) rate = 48000;
        else if (id == 4) rate = 88200;
        else if (id == 5) rate = 96000;
        else if (id == 6) rate = 192000;

        audioProcessor.setPreferredSampleRate (rate);
        if (auto* b = audioProcessor.getBrowser())
        {
            b->evaluateJavascript (
                "if (window.__JUCE_BRIDGE__) window.__JUCE_BRIDGE__.config.targetSampleRate = " +
                juce::String (audioProcessor.getEffectiveSampleRate()) + ";");
        }
    };
    addAndMakeVisible (srComboBox);

    // 3. Buffer / Cushion Selector
    cushionLabel.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
    cushionLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8c96ab));
    cushionLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (cushionLabel);

    cushionComboBox.addItem ("128 smp (~2.5ms)", 1);
    cushionComboBox.addItem ("256 smp (~5.3ms)", 2);
    cushionComboBox.addItem ("512 smp (~10ms)", 3);
    cushionComboBox.addItem ("720 smp (15ms)", 4);
    cushionComboBox.addItem ("1024 smp (~21ms)", 5);
    cushionComboBox.addItem ("2048 smp (~43ms)", 6);
    cushionComboBox.addItem ("4096 smp (~85ms)", 7);
    cushionComboBox.setTooltip ("FIFO jitter cushion buffer (up to 2048/4096) to eliminate dropouts");

    int currentCushion = audioProcessor.getBridgeServer().getJitterCushionSamples();
    if (currentCushion <= 128)        cushionComboBox.setSelectedId (1, juce::dontSendNotification);
    else if (currentCushion <= 256)   cushionComboBox.setSelectedId (2, juce::dontSendNotification);
    else if (currentCushion <= 512)   cushionComboBox.setSelectedId (3, juce::dontSendNotification);
    else if (currentCushion <= 720)   cushionComboBox.setSelectedId (4, juce::dontSendNotification);
    else if (currentCushion <= 1024)  cushionComboBox.setSelectedId (5, juce::dontSendNotification);
    else if (currentCushion <= 2048)  cushionComboBox.setSelectedId (6, juce::dontSendNotification);
    else                              cushionComboBox.setSelectedId (7, juce::dontSendNotification);

    cushionComboBox.onChange = [this]
    {
        int id = cushionComboBox.getSelectedId();
        int smp = 720;
        if (id == 1) smp = 128;
        else if (id == 2) smp = 256;
        else if (id == 3) smp = 512;
        else if (id == 4) smp = 720;
        else if (id == 5) smp = 1024;
        else if (id == 6) smp = 2048;
        else if (id == 7) smp = 4096;

        audioProcessor.getBridgeServer().setJitterCushionSamples (smp);
    };
    addAndMakeVisible (cushionComboBox);

    // 4. Output Gain Slider
    gainLabel.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
    gainLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8c96ab));
    gainLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (gainLabel);

    gainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 18);
    gainSlider.setRange (-24.0, 6.0, 0.5);
    gainSlider.setValue (0.0);
    gainSlider.setTextValueSuffix (" dB");
    gainSlider.setTooltip ("Plugin output gain trim");
    gainSlider.onValueChange = [this]
    {
        float db = (float) gainSlider.getValue();
        float gain = (db <= -23.5f) ? 0.0f : juce::Decibels::decibelsToGain (db);
        audioProcessor.getBridgeServer().setOutputGain (gain);
    };
    addAndMakeVisible (gainSlider);

    // 5. Volume LED / MIDI Activity LED
    audioLevelLed.setColour (juce::Label::backgroundColourId, juce::Colour (0xff021b1b));
    audioLevelLed.setColour (juce::Label::outlineColourId, juce::Colour (0xff004848));
    audioLevelLed.setColour (juce::Label::textColourId, juce::Colour (0xff00f4f4));
    audioLevelLed.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.5f, juce::Font::bold)));
    audioLevelLed.setJustificationType (juce::Justification::centred);
    audioLevelLed.setText ("VOL -inf", juce::dontSendNotification);
    addAndMakeVisible (audioLevelLed);

    midiLed.setColour (juce::Label::backgroundColourId, juce::Colour (0xff021b1b));
    midiLed.setColour (juce::Label::outlineColourId, juce::Colour (0xff004848));
    midiLed.setColour (juce::Label::textColourId, juce::Colour (0xff7a8296));
    midiLed.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.5f, juce::Font::bold)));
    midiLed.setJustificationType (juce::Justification::centred);
    midiLed.setText ("MIDI", juce::dontSendNotification);
    addAndMakeVisible (midiLed);

    // 6. Reset / Flush Buffer Push Button
    flushBtn.setTooltip ("Flush FIFO ringbuffer and resynchronize Web Audio stream");
    flushBtn.onClick = [this]
    {
        audioProcessor.getBridgeServer().flushAudioBuffer();
        if (auto* b = audioProcessor.getBrowser())
        {
            b->evaluateJavascript ("if (window.__JUCE_BRIDGE__) window.__JUCE_BRIDGE__.resumeAudio();");
        }
    };
    addAndMakeVisible (flushBtn);

    // 7. Real-time Telemetry LCD Display
    telemetryLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff021b1b));
    telemetryLabel.setColour (juce::Label::outlineColourId, juce::Colour (0xff004848));
    telemetryLabel.setColour (juce::Label::textColourId, juce::Colour (0xff00f4f4));
    telemetryLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::bold)));
    telemetryLabel.setJustificationType (juce::Justification::centred);
    telemetryLabel.setText ("DAW: -- | BUF: OK", juce::dontSendNotification);
    addAndMakeVisible (telemetryLabel);

    // =========================================================================
    // OpenSynth1 Dark Titanium / Cyan Styling (Minimal Red)
    // =========================================================================
    const auto darkBtnCol  = juce::Colour (0xff242732);
    const auto textCol     = juce::Colour (0xffd0d5e2);
    const auto cyanGlow    = juce::Colour (0xff00f4f4);
    const auto darkTeal    = juce::Colour (0xff082f36);

    for (auto* b : { &backButton, &forwardButton, &reloadButton, &localPort54321Btn, &strudelCcBtn, &flushBtn })
    {
        b->setColour (juce::TextButton::buttonColourId, darkBtnCol);
        b->setColour (juce::TextButton::textColourOffId, textCol);
        addAndMakeVisible (*b);
    }

    goButton.setColour (juce::TextButton::buttonColourId, darkTeal);
    goButton.setColour (juce::TextButton::textColourOffId, cyanGlow);
    addAndMakeVisible (goButton);

    startNodeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff123e20));
    startNodeBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff6ee7b7));
    addAndMakeVisible (startNodeBtn);

    for (auto* cb : { &srComboBox, &cushionComboBox })
    {
        cb->setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff12141c));
        cb->setColour (juce::ComboBox::textColourId, cyanGlow);
        cb->setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff004448));
        cb->setColour (juce::ComboBox::arrowColourId, cyanGlow);
    }

    gainSlider.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff12141c));
    gainSlider.setColour (juce::Slider::trackColourId, juce::Colour (0xff008899));
    gainSlider.setColour (juce::Slider::thumbColourId, cyanGlow);
    gainSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff021b1b));
    gainSlider.setColour (juce::Slider::textBoxTextColourId, cyanGlow);
    gainSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff004448));

    // Cyan LCD Digital URL Editor
    urlEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff021b1b));
    urlEditor.setColour (juce::TextEditor::textColourId, cyanGlow);
    urlEditor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff004848));
    urlEditor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0xff00ffff));
    urlEditor.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::bold)));
    urlEditor.setTextToShowWhenEmpty ("Enter Strudel URL (e.g. https://strudel.cc/ or http://127.0.0.1:54321)", juce::Colour (0xff008888));
    urlEditor.addListener (this);
    addAndMakeVisible (urlEditor);

    // Cyan LCD Status Display
    statusLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff021b1b));
    statusLabel.setColour (juce::Label::outlineColourId, juce::Colour (0xff004848));
    statusLabel.setColour (juce::Label::textColourId, cyanGlow);
    statusLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.5f, juce::Font::bold)));
    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setText ("BRIDGE READY", juce::dontSendNotification);
    addAndMakeVisible (statusLabel);

    // Window size & limits
    setResizable (true, true);
    setResizeLimits (820, 500, 3840, 2160);
    setSize (1060, 720);

    // Display current target URL
    urlEditor.setText (audioProcessor.getServerUrl(), false);

    // Start 15Hz telemetry update timer
    startTimerHz (15);
}

StrudelPlugAudioProcessorEditor::~StrudelPlugAudioProcessorEditor()
{
    stopTimer();

    // Don't just detach the browser (that leaves it with no OS-level peer,
    // which caused its Web Audio/JS execution to stall or stop). Reattach it
    // to the processor's always-mapped hidden host so it keeps running.
    audioProcessor.reattachBrowserToHiddenHost();
    audioProcessor.setEditor (nullptr);
}

void StrudelPlugAudioProcessorEditor::onBrowserUrlChanged (const juce::String& url)
{
    urlEditor.setText (url, false);
    statusLabel.setText ("ONLINE: AUDIO+MIDI", juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff00f4f4));
}

void StrudelPlugAudioProcessorEditor::updateSyncButtonAppearance()
{
    if (syncDawBtn.getToggleState())
    {
        syncDawBtn.setButtonText (juce::CharPointer_UTF8 ("\xe2\x97\x8f SYNC DAW")); // ● SYNC DAW
        syncDawBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff0a3d44));
        syncDawBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff0a3d44));
        syncDawBtn.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff00ffff));
        syncDawBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff00ffff));
    }
    else
    {
        syncDawBtn.setButtonText (juce::CharPointer_UTF8 ("\xe2\x97\x8b SYNC DAW")); // ○ SYNC DAW
        syncDawBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff222530));
        syncDawBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff222530));
        syncDawBtn.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff7a8296));
        syncDawBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff7a8296));
    }
}

void StrudelPlugAudioProcessorEditor::paint (juce::Graphics& g)
{
    // OpenSynth1 Dark Titanium / Slate Panel Background
    g.fillAll (juce::Colour (0xff1c1e26));

    // Outer border
    g.setColour (juce::Colour (0xff101116));
    g.drawRect (getLocalBounds(), 1);

    // =========================================================================
    // Header Bar (y = 0..27)
    // =========================================================================
    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (16.0f).withStyle ("Bold")));
    g.drawText ("StrudelPlug", 14, 4, 110, 20, juce::Justification::centredLeft);

    // OpenSynth1 Dark Slate Teal Badge (toned down, no bright red)
    g.setColour (juce::Colour (0xff14222b));
    g.fillRoundedRectangle (128.0f, 5.0f, 136.0f, 18.0f, 3.0f);
    g.setColour (juce::Colour (0xff0096a6));
    g.drawRoundedRectangle (128.0f, 5.0f, 136.0f, 18.0f, 3.0f, 1.0f);
    g.setColour (juce::Colour (0xffe2e8f0));
    g.setFont (juce::Font (juce::FontOptions (10.0f).withStyle ("Bold")));
    g.drawText ("OPENSYNTH1 // DAW", 128, 5, 136, 18, juce::Justification::centred);

    // Hardware corner screws
    auto drawScrew = [&g] (float cx, float cy)
    {
        g.setColour (juce::Colour (0xff101116));
        g.fillEllipse (cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
        g.setColour (juce::Colour (0xff3e4250));
        g.drawEllipse (cx - 4.0f, cy - 4.0f, 8.0f, 8.0f, 1.0f);
        g.drawLine (cx - 2.5f, cy - 1.5f, cx + 2.5f, cy + 1.5f, 1.0f);
    };
    drawScrew (6.0f, 6.0f);
    drawScrew ((float) getWidth() - 6.0f, 6.0f);

    // Separator lines
    g.setColour (juce::Colour (0xff12141a));
    g.drawHorizontalLine (27, 0.0f, (float) getWidth());
    g.drawHorizontalLine (59, 0.0f, (float) getWidth());
    g.setColour (juce::Colour (0xff2d303e));
    g.drawHorizontalLine (60, 0.0f, (float) getWidth());

    // Options section recessed groove background
    auto optRect = juce::Rectangle<float> (0.0f, 61.0f, (float) getWidth(), 36.0f);
    g.setColour (juce::Colour (0xff14161f));
    g.fillRect (optRect);

    // Separator line between Options and Browser
    g.setColour (juce::Colour (0xff12141a));
    g.drawHorizontalLine (97, 0.0f, (float) getWidth());
    g.setColour (juce::Colour (0xff2d303e));
    g.drawHorizontalLine (98, 0.0f, (float) getWidth());

    // Browser recessed bezel frame
    auto browserFrame = getLocalBounds().withTrimmedTop (99).reduced (5, 5).toFloat();
    g.setColour (juce::Colour (0xff101217));
    g.fillRoundedRectangle (browserFrame, 3.0f);
    g.setColour (juce::Colour (0xff323646));
    g.drawRoundedRectangle (browserFrame, 3.0f, 1.5f);
}

void StrudelPlugAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();

    // Row 1 & 2: Top Header & Navigation Row (y = 0..58)
    auto topArea = bounds.removeFromTop (58);
    auto navRow = topArea.withTrimmedTop (28).reduced (8, 1);

    // Navigation Left: Back, Forward, Reload
    backButton.setBounds (navRow.removeFromLeft (26).reduced (1, 0));
    forwardButton.setBounds (navRow.removeFromLeft (26).reduced (1, 0));
    reloadButton.setBounds (navRow.removeFromLeft (26).reduced (1, 0));

    // Navigation Right: Start Node, Local :54321, strudel.cc, Status, Go
    startNodeBtn.setBounds (navRow.removeFromRight (95).reduced (2, 0));
    localPort54321Btn.setBounds (navRow.removeFromRight (85).reduced (2, 0));
    strudelCcBtn.setBounds (navRow.removeFromRight (75).reduced (2, 0));
    statusLabel.setBounds (navRow.removeFromRight (140).reduced (2, 0));
    goButton.setBounds (navRow.removeFromRight (40).reduced (2, 0));

    // Center is URL Editor
    urlEditor.setBounds (navRow.reduced (4, 0));

    // Row 3: Dedicated Options & Transport Strip (y = 61..97, height 36)
    bounds.removeFromTop (3);
    auto optionsRow = bounds.removeFromTop (36).reduced (8, 3);

    syncDawBtn.setBounds (optionsRow.removeFromLeft (115).reduced (2, 0));

    optionsRow.removeFromLeft (6);
    srLabel.setBounds (optionsRow.removeFromLeft (24));
    srComboBox.setBounds (optionsRow.removeFromLeft (105).reduced (1, 1));

    optionsRow.removeFromLeft (6);
    cushionLabel.setBounds (optionsRow.removeFromLeft (28));
    cushionComboBox.setBounds (optionsRow.removeFromLeft (115).reduced (1, 1));

    optionsRow.removeFromLeft (6);
    gainLabel.setBounds (optionsRow.removeFromLeft (32));
    gainSlider.setBounds (optionsRow.removeFromLeft (130).reduced (1, 1));

    optionsRow.removeFromLeft (6);
    audioLevelLed.setBounds (optionsRow.removeFromLeft (92).reduced (2, 0));

    optionsRow.removeFromLeft (6);
    midiLed.setBounds (optionsRow.removeFromLeft (92).reduced (2, 0));

    optionsRow.removeFromLeft (6);
    flushBtn.setBounds (optionsRow.removeFromLeft (70).reduced (1, 0));

    // Remaining right area of options strip is Telemetry LCD
    optionsRow.removeFromLeft (6);
    telemetryLabel.setBounds (optionsRow.reduced (2, 0));

    // Browser Display Area
    bounds.removeFromTop (4);
    auto browserArea = bounds.reduced (6, 6);
    if (auto* b = audioProcessor.getBrowser())
        b->setBounds (browserArea);
}

void StrudelPlugAudioProcessorEditor::timerCallback()
{
    auto& server = audioProcessor.getBridgeServer();
    const int dawRate = server.getDawSampleRate();
    const float peak = server.getPeakAudioOutLevel();
    const float peakDb = peak > 0.00001f ? juce::Decibels::gainToDecibels (peak) : -60.0f;
    const bool midiIn = server.checkAndResetMidiInActivity();
    const bool midiOut = server.checkAndResetMidiOutActivity();
    const bool midiActive = midiIn || midiOut;
    const int cushionMs = server.getJitterCushionMs();
    const int cushionSmp = server.getJitterCushionSamples();
    const bool isPlaying = audioProcessor.isDawPlaying();
    const bool syncOn = audioProcessor.isDawSyncEnabled();

    juce::String status = "DAW: " + juce::String (dawRate / 1000.0, 1) + "k | ";
    status += (syncOn ? (isPlaying ? "RUN [DAW]" : "STOP") : "FREE") + juce::String (" | ");
    status += "OUT: " + (peakDb <= -59.0f ? "-inf" : juce::String (peakDb, 1)) + " dB | ";
    status += "BUF: " + juce::String (cushionSmp) + " (" + juce::String (cushionMs) + "ms)";

    teleportLabel:
    telemetryLabel.setText (status, juce::dontSendNotification);

    audioLevelLed.setText (peakDb <= -59.0f ? "VOL -inf" : "VOL " + juce::String (peakDb, 1) + "dB",
                            juce::dontSendNotification);
    audioLevelLed.setColour (juce::Label::textColourId,
                             peakDb > -28.0f ? juce::Colour (0xff00f4f4)
                                              : juce::Colour (0xff7a8296));

    midiLed.setColour (juce::Label::textColourId,
                       midiActive ? juce::Colour (0xff38ef7d)
                                   : juce::Colour (0xff7a8296));
}

void StrudelPlugAudioProcessorEditor::sendMidiToBrowser (const juce::MidiMessage& msg)
{
    auto* b = audioProcessor.getBrowser();
    if (b == nullptr || msg.getRawDataSize() < 1)
        return;

    const auto* raw = msg.getRawData();
    int status = raw[0];
    int d1 = msg.getRawDataSize() > 1 ? raw[1] : 0;
    int d2 = msg.getRawDataSize() > 2 ? raw[2] : 0;

    juce::MessageManager::callAsync ([b, status, d1, d2]()
    {
        b->evaluateJavascript (
            "if (window.__JUCE_BRIDGE__) window.__JUCE_BRIDGE__.dispatchMidiFromDaw(" +
            juce::String (status) + "," + juce::String (d1) + "," + juce::String (d2) + ");");
    });
}

void StrudelPlugAudioProcessorEditor::navigateTo (const juce::String& url)
{
    auto target = url.trim();
    if (target.isEmpty())
        return;

    if (! target.startsWithIgnoreCase ("http://") && ! target.startsWithIgnoreCase ("https://") && ! target.startsWithIgnoreCase ("file://"))
    {
        if (target.startsWith ("localhost") || target.startsWith ("127.0.0.1"))
            target = "http://" + target;
        else
            target = "https://" + target;
    }

    urlEditor.setText (target, false);
    audioProcessor.setServerUrl (target);
    statusLabel.setText ("CONNECTING...", juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xffffb703));

    if (auto* b = audioProcessor.getBrowser())
        b->goToURL (target);
}

void StrudelPlugAudioProcessorEditor::textEditorReturnKeyPressed (juce::TextEditor& editor)
{
    if (&editor == &urlEditor)
        navigateTo (urlEditor.getText().trim());
}
