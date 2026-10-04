/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.
    OpenSynth1 Edition: Sleek dark titanium/slate aesthetic with cyan LCD displays.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "StrudelFetch.h"

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
    // Header Row Callbacks & Tooltips
    // =========================================================================
    // Strudel is served by the plugin's own bridge server: no internet, and
    // being plain http the page can open the MIDI-in WebSocket (https pages can't).
    reloadButton.setTooltip ("Reload Strudel");
    reloadButton.onClick = [this] { reloadPage(); };

    fetchBtn.setTooltip ("Download / update Strudel from npm");
    fetchBtn.onClick = [this] { fetchStrudel(); };

    settingsBtn.setTooltip ("Settings");
    settingsBtn.setClickingTogglesState (true);
    settingsBtn.onClick = [this] { setSettingsOpen (settingsBtn.getToggleState()); };

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
    cushionComboBox.addItem ("6000 smp (125ms)", 8);
    cushionComboBox.addItem ("8192 smp (~170ms)", 9);
    cushionComboBox.addItem ("12000 smp (250ms)", 10);
    cushionComboBox.addItem ("16384 smp (~340ms)", 11);
    cushionComboBox.setTooltip ("FIFO jitter cushion buffer (up to 16384) to eliminate dropouts");

    int currentCushion = audioProcessor.getBridgeServer().getJitterCushionSamples();
    if (currentCushion <= 128)        cushionComboBox.setSelectedId (1, juce::dontSendNotification);
    else if (currentCushion <= 256)   cushionComboBox.setSelectedId (2, juce::dontSendNotification);
    else if (currentCushion <= 512)   cushionComboBox.setSelectedId (3, juce::dontSendNotification);
    else if (currentCushion <= 720)   cushionComboBox.setSelectedId (4, juce::dontSendNotification);
    else if (currentCushion <= 1024)  cushionComboBox.setSelectedId (5, juce::dontSendNotification);
    else if (currentCushion <= 2048)  cushionComboBox.setSelectedId (6, juce::dontSendNotification);
    else if (currentCushion <= 4096)  cushionComboBox.setSelectedId (7, juce::dontSendNotification);
    else if (currentCushion <= 6000)  cushionComboBox.setSelectedId (8, juce::dontSendNotification);
    else if (currentCushion <= 8192)  cushionComboBox.setSelectedId (9, juce::dontSendNotification);
    else if (currentCushion <= 12000) cushionComboBox.setSelectedId (10, juce::dontSendNotification);
    else                              cushionComboBox.setSelectedId (11, juce::dontSendNotification);

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
        else if (id == 8) smp = 6000;
        else if (id == 9) smp = 8192;
        else if (id == 10) smp = 12000;
        else if (id == 11) smp = 16384;

        audioProcessor.getBridgeServer().setJitterCushionSamples (smp);
        audioProcessor.setLatencySamples (smp);
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
    const float currentGain = audioProcessor.getBridgeServer().getOutputGain();
    const double currentDb = currentGain > 0.0001f ? (double) juce::Decibels::gainToDecibels (currentGain) : -24.0;
    gainSlider.setValue (currentDb, juce::dontSendNotification);
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

    reloadButton.setColour (juce::TextButton::buttonColourId, darkBtnCol);
    reloadButton.setColour (juce::TextButton::textColourOffId, textCol);
    addAndMakeVisible (reloadButton);

    fetchBtn.setColour (juce::TextButton::buttonColourId, darkBtnCol);
    fetchBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff6ee7b7));
    addAndMakeVisible (fetchBtn);

    settingsBtn.setColour (juce::TextButton::buttonColourId, darkBtnCol);
    settingsBtn.setColour (juce::TextButton::buttonOnColourId, darkTeal);
    settingsBtn.setColour (juce::TextButton::textColourOffId, textCol);
    settingsBtn.setColour (juce::TextButton::textColourOnId, cyanGlow);
    addAndMakeVisible (settingsBtn);

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
    setSettingsOpen (false);

    // Start 15Hz telemetry update timer
    startTimerHz (15);
}

StrudelPlugAudioProcessorEditor::~StrudelPlugAudioProcessorEditor()
{
    stopTimer();

    // Clear active editor first
    audioProcessor.setEditor (nullptr);

    // Cleanly detach the persistent browser from this editor component
    if (auto* b = audioProcessor.getBrowser())
        removeChildComponent (b);
}

void StrudelPlugAudioProcessorEditor::onBrowserUrlChanged (const juce::String&)
{
    statusLabel.setText ("ONLINE: AUDIO+MIDI", juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff00f4f4));
}

void StrudelPlugAudioProcessorEditor::reloadPage()
{
    statusLabel.setText ("CONNECTING...", juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xffffb703));
    if (auto* b = audioProcessor.getBrowser())
        b->goToURL (audioProcessor.getLocalStrudelUrl());
}

// Always downloads the latest @strudel/repl, then reloads the page so it picks
// up the new build (or leaves the "not installed" warning if it was showing).
void StrudelPlugAudioProcessorEditor::fetchStrudel()
{
    statusLabel.setText ("FETCHING STRUDEL...", juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xffffb703));
    fetchBtn.setEnabled (false);

    juce::Component::SafePointer<StrudelPlugAudioProcessorEditor> self (this);
    StrudelFetch::downloadAsync ([self] (bool ok, juce::String error)
    {
        if (self == nullptr)
            return;
        self->fetchBtn.setEnabled (true);
        if (ok)
        {
            self->statusLabel.setText ("STRUDEL READY", juce::dontSendNotification);
            self->statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff38ef7d));
            self->reloadPage();
        }
        else
        {
            self->statusLabel.setText ("FETCH FAILED: " + error.toUpperCase(), juce::dontSendNotification);
            self->statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xffff5555));
        }
    });
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
    // Header Bar: title, status LCD, sync, levels, gain, settings toggle
    // =========================================================================
    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (16.0f).withStyle ("Bold")));
    g.drawText ("StrudelPlug", 14, 0, 110, headerHeight, juce::Justification::centredLeft);

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

    // Separator under header
    int y = headerHeight;
    g.setColour (juce::Colour (0xff12141a));
    g.drawHorizontalLine (y - 1, 0.0f, (float) getWidth());
    g.setColour (juce::Colour (0xff2d303e));
    g.drawHorizontalLine (y, 0.0f, (float) getWidth());
    y += 1;

    if (settingsOpen)
    {
        // Settings strip recessed groove background + separator to browser
        g.setColour (juce::Colour (0xff14161f));
        g.fillRect (juce::Rectangle<float> (0.0f, (float) y, (float) getWidth(), (float) settingsHeight));
        y += settingsHeight;
        g.setColour (juce::Colour (0xff12141a));
        g.drawHorizontalLine (y, 0.0f, (float) getWidth());
        g.setColour (juce::Colour (0xff2d303e));
        g.drawHorizontalLine (y + 1, 0.0f, (float) getWidth());
        y += 2;
    }

    // Browser recessed bezel frame
    auto browserFrame = getLocalBounds().withTrimmedTop (y).reduced (5, 5).toFloat();
    g.setColour (juce::Colour (0xff101217));
    g.fillRoundedRectangle (browserFrame, 3.0f);
    g.setColour (juce::Colour (0xff323646));
    g.drawRoundedRectangle (browserFrame, 3.0f, 1.5f);
}

void StrudelPlugAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();

    // Row 1: Header: title (painted), status LCD, sync, levels, gain; settings toggle at far right
    auto header = bounds.removeFromTop (headerHeight).reduced (16, 5);
    settingsBtn.setBounds (header.removeFromRight (24).reduced (1, 0));
    header.removeFromLeft (112);   // painted title
    statusLabel.setBounds (header.removeFromLeft (150));

    header.removeFromLeft (8);
    syncDawBtn.setBounds (header.removeFromLeft (105));

    header.removeFromLeft (8);
    audioLevelLed.setBounds (header.removeFromLeft (84));
    header.removeFromLeft (4);
    midiLed.setBounds (header.removeFromLeft (56));

    header.removeFromLeft (8);
    gainLabel.setBounds (header.removeFromLeft (36));
    gainSlider.setBounds (header.removeFromLeft (juce::jmin (160, header.getWidth())));

    // Row 2 (optional): Settings strip
    bounds.removeFromTop (1);
    if (settingsOpen)
    {
        auto row = bounds.removeFromTop (settingsHeight).reduced (8, 3);

        fetchBtn.setBounds (row.removeFromRight (30).reduced (2, 0));
        reloadButton.setBounds (row.removeFromRight (30).reduced (2, 0));

        srLabel.setBounds (row.removeFromLeft (24));
        srComboBox.setBounds (row.removeFromLeft (105).reduced (1, 1));

        row.removeFromLeft (6);
        cushionLabel.setBounds (row.removeFromLeft (28));
        cushionComboBox.setBounds (row.removeFromLeft (130).reduced (1, 1));

        // Remaining middle area is Telemetry LCD
        row.removeFromLeft (6);
        telemetryLabel.setBounds (row.reduced (2, 0));
        bounds.removeFromTop (2);
    }

    // Browser Display Area
    bounds.removeFromTop (4);
    auto browserArea = bounds.reduced (6, 6);
    if (auto* b = audioProcessor.getBrowser())
        b->setBounds (browserArea);
}

void StrudelPlugAudioProcessorEditor::setSettingsOpen (bool open)
{
    settingsOpen = open;
    settingsBtn.setToggleState (open, juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &srLabel, &srComboBox, &cushionLabel, &cushionComboBox,
                                                            &telemetryLabel, &reloadButton, &fetchBtn })
        c->setVisible (open);
    resized();
    repaint();
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

