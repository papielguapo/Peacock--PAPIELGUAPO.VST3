#include <juce_audio_utils/juce_audio_utils.h>
#include <BinaryData.h>
#include "PeacockDSP.h"
#include <array>
#include <atomic>
#include <cmath>

class Peacock final : public juce::AudioProcessor
{
    using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;
    static Layout layout()
    {
        Layout p;
        for (auto name : {"Harmonic", "Dynamic", "Dither"})
            p.add(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{name, 1}, name,
                juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 0.0f));
        p.add(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"Color", 1}, "Color",
            juce::StringArray{"Silver", "Gold", "Rich", "Fat", "Deep"}, 0));
        p.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"OutputTrim", 1}, "Output Trim",
            juce::NormalisableRange<float>(-6.0f, 6.0f, 0.1f), 0.0f));
        return p;
    }

public:
    juce::AudioProcessorValueTreeState state{*this, nullptr, "PeacockStudy", layout()};
    std::vector<peacock::SPeacock_State> dspState{2};
    std::array<uint32_t, 2> rngSeeds{{22222u, 33335u}};
    peacock::SPeacock_Context dspCtx{};
    double currentSampleRate = 48000.0;

    std::atomic<float> meterIn{0.0f};
    std::atomic<float> meterOut{0.0f};
    std::atomic<float> meterDrive{0.0f};

private:
    const std::atomic<float>* pHarmonic = state.getRawParameterValue("Harmonic");
    const std::atomic<float>* pDynamic  = state.getRawParameterValue("Dynamic");
    const std::atomic<float>* pDither   = state.getRawParameterValue("Dither");
    const std::atomic<float>* pColor    = state.getRawParameterValue("Color");
    const std::atomic<float>* pTrimDb   = state.getRawParameterValue("OutputTrim");

    void updateContextFromParams() noexcept
    {
        const int ditherChoice = (pDither->load() >= 0.5f) ? 1 : 0;
        const int colorChoice  = juce::jlimit(0, 4, static_cast<int>(std::lround(pColor->load())));
        dspCtx = peacock::Peacock_GenerateCoefficients(
            pHarmonic->load(),
            pDynamic->load(),
            pTrimDb->load(),
            ditherChoice,
            colorChoice,
            false);
    }

public:
    Peacock()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
        updateContextFromParams();
        for (size_t ch = 0; ch < dspState.size(); ++ch)
        {
            peacock::Peacock_InitializeState(dspState[ch], static_cast<float>(currentSampleRate));
            rngSeeds[ch] = static_cast<uint32_t>(22222u + ch * 11113u);
        }
    }

    void prepareToPlay(double rate, int) override
    {
        currentSampleRate = (rate > 0.0) ? rate : 48000.0;
        updateContextFromParams();
        for (size_t ch = 0; ch < dspState.size(); ++ch)
        {
            peacock::Peacock_InitializeState(dspState[ch], static_cast<float>(currentSampleRate));
            rngSeeds[ch] = static_cast<uint32_t>(22222u + ch * 11113u);
        }
    }

    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals guard;
        updateContextFromParams();

        const int numSamples  = b.getNumSamples();
        const int numChannels = juce::jmin(b.getNumChannels(), static_cast<int>(dspState.size()));
        if (numSamples <= 0 || numChannels <= 0)
            return;

        float inPeak = 0.0f;
        float outPeak = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* inPtr = b.getReadPointer(ch);
            for (int i = 0; i < numSamples; ++i)
                inPeak = juce::jmax(inPeak, std::abs(inPtr[i]));

            float* outPtr = b.getWritePointer(ch);
            peacock::Peacock_ProcessChannel(
                dspCtx,
                dspState[static_cast<size_t>(ch)],
                outPtr,
                outPtr,
                numSamples,
                rngSeeds[static_cast<size_t>(ch)]);

            for (int i = 0; i < numSamples; ++i)
                outPeak = juce::jmax(outPeak, std::abs(outPtr[i]));
        }

        meterIn.store(inPeak, std::memory_order_relaxed);
        meterOut.store(outPeak, std::memory_order_relaxed);
        meterDrive.store(juce::jmax(dspCtx.harmonic, dspCtx.dynamic), std::memory_order_relaxed);
    }

    bool isBusesLayoutSupported(const BusesLayout& l) const override
    {
        auto out = l.getMainOutputChannelSet();
        return out == l.getMainInputChannelSet()
            && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
    }

    void releaseResources() override {}
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void getStateInformation(juce::MemoryBlock& b) override
    {
        if (auto xml = state.copyState().createXml())
            copyXmlToBinary(*xml, b);
    }

    void setStateInformation(const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary(data, size); xml && xml->hasTagName(state.state.getType()))
            state.replaceState(juce::ValueTree::fromXml(*xml));
    }
};

class Panel final : public juce::AudioProcessorEditor, private juce::Timer
{
    using State = juce::AudioProcessorValueTreeState;

    struct FilmstripLookAndFeel : juce::LookAndFeel_V4
    {
        bool deck = false;
        juce::Colour accent{0xff55d5ad};
        juce::Image knobStrip;   // 60 x 8100 (135 frames of 60x60)
        juce::Image ditherStrip; // 60 x 120  (2 frames of 60x60)
        juce::Image colorStrip;  // 60 x 300  (5 frames of 60x60)

        void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                              float sliderPosProportional, float startAngle, float endAngle,
                              juce::Slider& s) override
        {
            const juce::String compId = s.getComponentID();

            if (!deck)
            {
                if (compId == "Dither" && ditherStrip.isValid())
                {
                    const int frame = (s.getValue() >= 0.5) ? 1 : 0;
                    g.drawImage(ditherStrip, x, y, w, h, 0, frame * 60, 60, 60);
                    return;
                }
                if (compId == "Color" && colorStrip.isValid())
                {
                    const int frame = juce::jlimit(0, 4, static_cast<int>(std::lround(s.getValue())));
                    g.drawImage(colorStrip, x, y, w, h, 0, frame * 60, 60, 60);
                    return;
                }
                if (knobStrip.isValid())
                {
                    const int frame = juce::jlimit(0, 134, static_cast<int>(std::lround(sliderPosProportional * 134.0f)));
                    g.drawImage(knobStrip, x, y, w, h, 0, frame * 60, 60, 60);
                    return;
                }
            }

            // Cara B (Phoenix II adapted deck rotary dials)
            const float r  = static_cast<float>(juce::jmin(w, h)) * 0.5f - 4.0f;
            const float cx = static_cast<float>(x) + static_cast<float>(w) * 0.5f;
            const float cy = static_cast<float>(y) + static_cast<float>(h) * 0.5f;
            auto polar = [&](float angle, float radius)
            {
                return juce::Point<float>(cx + radius * std::sin(angle), cy - radius * std::cos(angle));
            };

            auto box = juce::Rectangle<float>(2.0f * r, 2.0f * r).withCentre({cx, cy});
            g.setColour(juce::Colour(0xff0b1013));
            g.fillEllipse(box);
            g.setColour(juce::Colour(0xff232d32));
            g.drawEllipse(box, 1.2f);

            juce::Path arc;
            arc.addCentredArc(cx, cy, r - 2.5f, r - 2.5f, 0.0f, startAngle, endAngle, true);
            g.setColour(juce::Colour(0xff2c383e));
            g.strokePath(arc, juce::PathStrokeType(2.6f));

            const float angle = startAngle + sliderPosProportional * (endAngle - startAngle);
            juce::Path active;
            active.addCentredArc(cx, cy, r - 2.5f, r - 2.5f, 0.0f, startAngle, angle, true);
            g.setColour(accent);
            g.strokePath(active, juce::PathStrokeType(2.8f));

            g.drawLine({polar(angle, r * 0.35f), polar(angle, r - 4.5f)}, 2.3f);
        }
    } style;

    struct SkinPalette
    {
        const char* code;
        const char* name;
        juce::uint32 bgTop;
        juce::uint32 bgBot;
        juce::uint32 visorBg;
        juce::uint32 accent;
        juce::uint32 text;
    };

    static constexpr std::array<SkinPalette, 7> kSkins{{
        {"CS", "Crane Song",  0xff1a2426, 0xff0f1719, 0xff080d0e, 0xff55d5ad, 0xffe7f4ef},
        {"FL", "FL Studio",   0xff282c34, 0xff1b1e24, 0xff121519, 0xffff9f43, 0xfff3f4f6},
        {"PT", "Pro Tools",   0xff23252c, 0xff15171c, 0xff0c0d11, 0xff70a1ff, 0xffeef2ff},
        {"AB", "Ableton",     0xff2b2b2b, 0xff1c1c1c, 0xff111111, 0xffffbe2e, 0xfff5f5f5},
        {"LP", "Logic Pro",   0xff1e242b, 0xff13171c, 0xff0a0d10, 0xff48dbfb, 0xffeaf6fa},
        {"98", "Win98 Retro", 0xff323639, 0xff1f2224, 0xff09140d, 0xff4cd137, 0xffe8f5e9},
        {"AT", "Atari ST",    0xff2a2421, 0xff1a1614, 0xff0f0c0a, 0xffff6b6b, 0xffffecec}
    }};

    Peacock& proc;
    State& ui;

    juce::Image bgCaraA;
    std::array<juce::Image, 2> ditherLabels; // Off (31026), 16Bit (31027)
    std::array<juce::Image, 5> colorLabels;  // Silver..Deep (31021..31025)

    // Exact VSTGUI CRect coordinates extracted from Peacock.aaxplugin (0x18040CB20..0x18040D500)
    struct RectI { int x, y, w, h; };
    static constexpr std::array<RectI, 2> kDitherLabelRects{{
        {311, 23, 22, 27}, // Off   (res_PNG_31026_22x54.png)
        {334, 33, 22, 22}  // 16Bit (res_PNG_31027_22x44.png)
    }};
    static constexpr std::array<RectI, 5> kColorLabelRects{{
        {421, 39, 54, 9},  // Silver (res_PNG_31021_54x18.png)
        {430, 52, 48, 9},  // Gold   (res_PNG_31022_48x18.png)
        {433, 66, 41, 9},  // Rich   (res_PNG_31023_41x18.png)
        {431, 80, 36, 9},  // Fat    (res_PNG_31024_36x18.png)
        {421, 94, 53, 9}   // Deep   (res_PNG_31025_53x18.png)
    }};

    std::array<juce::Slider, 5> knobs;
    std::array<std::unique_ptr<State::SliderAttachment>, 5> links;
    juce::StringArray colors{"Silver", "Gold", "Rich", "Fat", "Deep"};
    juce::TextButton flip;
    std::array<juce::TextButton, 7> tabs;
    int palette = 0;

    void setParameterNormalized(const char* paramID, float normValue)
    {
        if (auto* param = ui.getParameter(paramID))
            param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, normValue));
    }

    void refresh()
    {
        const auto& sk = kSkins[static_cast<size_t>(palette)];
        style.accent = juce::Colour(sk.accent);
        flip.setButtonText(style.deck ? "A" : "B");
        flip.setTooltip(style.deck ? "Volver a Cara A (Hardware 1:1)" : "Cambiar a Cara B (Phoenix II Deck)");
        flip.setColour(juce::TextButton::buttonColourId,
                       style.deck ? juce::Colour(0xff141c1f) : juce::Colour(0xaa102222));
        flip.setColour(juce::TextButton::textColourOffId,
                       style.deck ? style.accent : juce::Colour(0xffd7e8e2));

        for (int i = 0; i < 7; ++i)
        {
            tabs[static_cast<size_t>(i)].setVisible(style.deck);
            tabs[static_cast<size_t>(i)].setColour(juce::TextButton::buttonColourId,
                                                   i == palette ? style.accent.withAlpha(0.22f) : juce::Colour(0xff101619));
            tabs[static_cast<size_t>(i)].setColour(juce::TextButton::textColourOffId,
                                                   i == palette ? style.accent : juce::Colour(0xff9fb1b7));
        }

        resized();
        repaint();
    }

    void timerCallback() override
    {
        if (style.deck)
            repaint(245, 30, 262, 95);
    }

public:
    Panel(Peacock& p, State& state) : AudioProcessorEditor(p), proc(p), ui(state)
    {
        bgCaraA = juce::ImageCache::getFromMemory(PeacockData::res_PNG_31011_515x161_png,
                                                  PeacockData::res_PNG_31011_515x161_pngSize);
        style.knobStrip   = juce::ImageCache::getFromMemory(PeacockData::res_PNG_31010_60x8100_png,
                                                            PeacockData::res_PNG_31010_60x8100_pngSize);
        style.ditherStrip = juce::ImageCache::getFromMemory(PeacockData::res_PNG_31013_60x120_png,
                                                            PeacockData::res_PNG_31013_60x120_pngSize);
        style.colorStrip  = juce::ImageCache::getFromMemory(PeacockData::res_PNG_31015_60x300_png,
                                                            PeacockData::res_PNG_31015_60x300_pngSize);

        ditherLabels[0] = juce::ImageCache::getFromMemory(PeacockData::res_PNG_31026_22x54_png,
                                                          PeacockData::res_PNG_31026_22x54_pngSize);
        ditherLabels[1] = juce::ImageCache::getFromMemory(PeacockData::res_PNG_31027_22x44_png,
                                                          PeacockData::res_PNG_31027_22x44_pngSize);

        colorLabels[0] = juce::ImageCache::getFromMemory(PeacockData::res_PNG_31021_54x18_png,
                                                         PeacockData::res_PNG_31021_54x18_pngSize);
        colorLabels[1] = juce::ImageCache::getFromMemory(PeacockData::res_PNG_31022_48x18_png,
                                                         PeacockData::res_PNG_31022_48x18_pngSize);
        colorLabels[2] = juce::ImageCache::getFromMemory(PeacockData::res_PNG_31023_41x18_png,
                                                         PeacockData::res_PNG_31023_41x18_pngSize);
        colorLabels[3] = juce::ImageCache::getFromMemory(PeacockData::res_PNG_31024_36x18_png,
                                                         PeacockData::res_PNG_31024_36x18_pngSize);
        colorLabels[4] = juce::ImageCache::getFromMemory(PeacockData::res_PNG_31025_53x18_png,
                                                         PeacockData::res_PNG_31025_53x18_pngSize);

        setLookAndFeel(&style);
        const char* ids[] = {"Harmonic", "Dynamic", "OutputTrim", "Dither", "Color"};
        const char* names[] = {"Harmonic", "Dynamic", "Output Trim", "Dither", "Color"};
        for (int i = 0; i < 5; ++i)
        {
            auto& k = knobs[static_cast<size_t>(i)];
            k.setComponentID(ids[i]);
            k.setName(names[i]);
            k.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            k.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            addAndMakeVisible(k);
            links[static_cast<size_t>(i)] = std::make_unique<State::SliderAttachment>(state, ids[i], k);
            k.onValueChange = [this] { repaint(); };
        }

        flip.setComponentID("flip");
        flip.setName("Cambiar cara");
        flip.onClick = [this]
        {
            style.deck = !style.deck;
            ui.state.setProperty("deck", style.deck, nullptr);
            refresh();
        };
        addAndMakeVisible(flip);

        for (int i = 0; i < 7; ++i)
        {
            auto& t = tabs[static_cast<size_t>(i)];
            t.setButtonText(kSkins[static_cast<size_t>(i)].code);
            t.onClick = [this, i]
            {
                palette = i;
                ui.state.setProperty("palette", i, nullptr);
                refresh();
            };
            addChildComponent(t);
        }

        style.deck = static_cast<bool>(ui.state.getProperty("deck", false));
        palette = juce::jlimit(0, 6, static_cast<int>(ui.state.getProperty("palette", 0)));

        // Exact 515x161 dimensions from Peacock.aaxplugin
        setSize(515, 161);
        refresh();
        startTimerHz(24);
    }

    ~Panel() override
    {
        stopTimer();
        setLookAndFeel(nullptr);
    }

    void resized() override
    {
        if (!style.deck)
        {
            // Exact VSTGUI CRect positions from Peacock.aaxplugin (0x18040CC10..0x18040CCC4)
            constexpr int knobX[5] = {19, 109, 199, 289, 379};
            for (int i = 0; i < 5; ++i)
            {
                knobs[static_cast<size_t>(i)].setVisible(true);
                knobs[static_cast<size_t>(i)].setBounds(knobX[i], 41, 60, 60);
            }
            flip.setBounds(376, 8, 22, 15);
        }
        else
        {
            // Cara B (Phoenix II adapted 515x161 layout): 3 primary dials on left, visor + pills on right
            for (int i = 0; i < 3; ++i)
            {
                knobs[static_cast<size_t>(i)].setVisible(true);
                knobs[static_cast<size_t>(i)].setBounds(12 + i * 76, 32, 68, 60);
            }
            knobs[3].setVisible(false);
            knobs[4].setVisible(false);

            for (int i = 0; i < 7; ++i)
                tabs[static_cast<size_t>(i)].setBounds(268 + i * 31, 4, 29, 18);
            flip.setBounds(488, 4, 22, 18);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const auto pt = e.getPosition();

        if (!style.deck)
        {
            auto toRect = [](const RectI& r) { return juce::Rectangle<int>(r.x, r.y, r.w, r.h); };
            // Click on Dither switch or Dither Off / 16Bit label overlays
            if (toRect(kDitherLabelRects[0]).expanded(3).contains(pt))
            {
                setParameterNormalized("Dither", 0.0f);
                return;
            }
            if (toRect(kDitherLabelRects[1]).expanded(3).contains(pt))
            {
                setParameterNormalized("Dither", 1.0f);
                return;
            }
            // Click on Color Silver / Gold / Rich / Fat / Deep label overlays
            for (int i = 0; i < 5; ++i)
            {
                if (toRect(kColorLabelRects[static_cast<size_t>(i)]).expanded(3, 2).contains(pt))
                {
                    setParameterNormalized("Color", static_cast<float>(i) / 4.0f);
                    return;
                }
            }
        }
        else
        {
            // Cara B interactive pills for Dither and Color
            juce::Rectangle<int> ditherRect(246, 102, 58, 22);
            if (ditherRect.contains(pt))
            {
                const bool on = knobs[3].getValue() >= 0.5;
                setParameterNormalized("Dither", on ? 0.0f : 1.0f);
                return;
            }
            for (int i = 0; i < 5; ++i)
            {
                juce::Rectangle<int> colRect(310 + i * 39, 102, 37, 22);
                if (colRect.contains(pt))
                {
                    setParameterNormalized("Color", static_cast<float>(i) / 4.0f);
                    return;
                }
            }
        }
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (e.y < 24 || (style.deck && e.y > 132) || (!style.deck && e.x >= 445 && e.y > 130))
            flip.onClick();
    }

    void paint(juce::Graphics& g) override
    {
        if (!style.deck)
        {
            // 1. Original 515x161 background bitmap from Peacock.aaxplugin (res_PNG_31011_515x161.png)
            // Note: In Peacock.aaxplugin (0x180067547 & 0x180067A9D), the 7 green debug frames
            // (31021..31027) and 5 CTextEdit readouts are gated by debug flag [0x180538474] == 0
            // (false in release builds). The release hardware face consists solely of the 515x161
            // background and the 5 sprite-sheet knobs/switches.
            if (bgCaraA.isValid())
                g.drawImageAt(bgCaraA, 0, 0);
            else
                g.fillAll(juce::Colour(0xff8ca6a8));

            return;
        }

        // ====================================================================
        // CARA B: Phoenix II Adapted 7-Skin Deck (515 x 161)
        // ====================================================================
        const auto& sk = kSkins[static_cast<size_t>(palette)];
        g.setGradientFill(juce::ColourGradient(
            juce::Colour(sk.bgTop), 0.0f, 0.0f,
            juce::Colour(sk.bgBot), 0.0f, 161.0f, false));
        g.fillAll();

        // Top header bar
        g.setColour(juce::Colour(0xff0a0f12));
        g.fillRect(0, 0, 515, 25);
        g.setColour(style.accent);
        g.setFont(juce::FontOptions(12.2f, juce::Font::bold));
        g.drawText("PEACOCK // CARA B [" + juce::String(sk.name).toUpperCase() + "]",
                   10, 2, 255, 21, juce::Justification::centredLeft);
        g.setColour(style.accent.withAlpha(0.35f));
        g.drawHorizontalLine(25, 0.0f, 515.0f);

        // Left 3 continuous rotary controls (Harmonic, Dynamic, Output Trim)
        for (int i = 0; i < 3; ++i)
        {
            const int x = 12 + i * 76;
            g.setColour(juce::Colour(sk.text));
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            g.drawText(knobs[static_cast<size_t>(i)].getName().toUpperCase(),
                       x - 3, 92, 74, 15, juce::Justification::centred);

            const double v = knobs[static_cast<size_t>(i)].getValue();
            juce::String valStr = (i == 2)
                ? ((v >= 0.0 ? "+" : "") + juce::String(v, 1) + " dB")
                : (juce::String(v, 1) + "%");
            g.setColour(style.accent);
            g.setFont(juce::FontOptions(11.5f));
            g.drawText(valStr, x - 3, 108, 74, 16, juce::Justification::centred);
        }

        // Right Visor: Real-time 1:1 Wavetable Transfer Curve & Groove Delay Monitor
        juce::Rectangle<float> visor(246.0f, 32.0f, 258.0f, 64.0f);
        g.setColour(juce::Colour(sk.visorBg));
        g.fillRoundedRectangle(visor, 4.0f);
        g.setColour(style.accent.withAlpha(0.4f));
        g.drawRoundedRectangle(visor, 4.0f, 1.0f);

        // Grid lines inside visor
        g.setColour(juce::Colours::white.withAlpha(0.07f));
        g.drawHorizontalLine(static_cast<int>(visor.getCentreY()), visor.getX() + 4.0f, visor.getRight() - 4.0f);
        g.drawVerticalLine(static_cast<int>(visor.getX() + 120.0f), visor.getY() + 4.0f, visor.getBottom() - 4.0f);

        // Plot live Peacock_LookupWavetable transfer curve (Table B + Harmonic/Dynamic shaping)
        juce::Path curve;
        const float harm = static_cast<float>(knobs[0].getValue()) * 0.01f;
        const float dyn  = static_cast<float>(knobs[1].getValue()) * 0.01f;
        const int colIdx = juce::jlimit(0, 4, static_cast<int>(std::lround(knobs[4].getValue())));
        const float colBoost = 1.0f + 0.18f * static_cast<float>(colIdx);

        for (int px = 0; px <= 110; ++px)
        {
            const float normX = (static_cast<float>(px) / 110.0f) * 2.0f - 1.0f;
            const float wt = peacock::Peacock_LookupWavetable(juce::jlimit(-1.0f, 1.0f, normX * colBoost), 0x2000u);
            const float nonlin = wt - normX;
            const float shaped = juce::jlimit(-1.0f, 1.0f,
                normX + (0.35f + 1.8f * harm) * nonlin - 0.35f * dyn * normX * normX);
            const float sx = visor.getX() + 8.0f + static_cast<float>(px);
            const float sy = visor.getCentreY() - shaped * (visor.getHeight() * 0.40f);
            if (px == 0)
                curve.startNewSubPath(sx, sy);
            else
                curve.lineTo(sx, sy);
        }
        g.setColour(style.accent);
        g.strokePath(curve, juce::PathStrokeType(1.8f));

        // Signal meters & DSP status inside right side of visor
        const float inLvl  = juce::jlimit(0.0f, 1.0f, proc.meterIn.load(std::memory_order_relaxed));
        const float outLvl = juce::jlimit(0.0f, 1.0f, proc.meterOut.load(std::memory_order_relaxed));
        const float drvLvl = juce::jlimit(0.0f, 1.0f, proc.meterDrive.load(std::memory_order_relaxed));

        auto drawBar = [&](const char* lbl, float val, float yPos)
        {
            g.setColour(juce::Colour(sk.text).withAlpha(0.75f));
            g.setFont(juce::FontOptions(9.5f, juce::Font::bold));
            g.drawText(lbl, static_cast<int>(visor.getX()) + 128, static_cast<int>(yPos) - 2, 34, 12,
                       juce::Justification::centredLeft);
            juce::Rectangle<float> barBg(visor.getX() + 164.0f, yPos, 82.0f, 7.0f);
            g.setColour(juce::Colours::white.withAlpha(0.10f));
            g.fillRoundedRectangle(barBg, 2.0f);
            g.setColour(style.accent);
            g.fillRoundedRectangle(barBg.withWidth(barBg.getWidth() * val), 2.0f);
        };
        drawBar("IN",   inLvl,  visor.getY() + 12.0f);
        drawBar("OUT",  outLvl, visor.getY() + 28.0f);
        drawBar("HARM", drvLvl, visor.getY() + 44.0f);

        // Interactive Dither & Color pills below visor (y = 102)
        const bool ditherOn = knobs[3].getValue() >= 0.5;
        juce::Rectangle<float> ditherPill(246.0f, 102.0f, 58.0f, 22.0f);
        g.setColour(ditherOn ? style.accent.withAlpha(0.25f) : juce::Colour(0xff12191c));
        g.fillRoundedRectangle(ditherPill, 3.5f);
        g.setColour(ditherOn ? style.accent : juce::Colour(0xff38464c));
        g.drawRoundedRectangle(ditherPill, 3.5f, 1.0f);
        g.setColour(ditherOn ? style.accent : juce::Colour(0xff9fb1b7));
        g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        g.drawText(ditherOn ? "16BIT ON" : "DITHER", ditherPill.toNearestInt(), juce::Justification::centred);

        for (int i = 0; i < 5; ++i)
        {
            const bool active = (colIdx == i);
            juce::Rectangle<float> colPill(310.0f + i * 39.0f, 102.0f, 37.0f, 22.0f);
            g.setColour(active ? style.accent.withAlpha(0.25f) : juce::Colour(0xff12191c));
            g.fillRoundedRectangle(colPill, 3.5f);
            g.setColour(active ? style.accent : juce::Colour(0xff38464c));
            g.drawRoundedRectangle(colPill, 3.5f, 1.0f);
            g.setColour(active ? style.accent : juce::Colour(0xff9fb1b7));
            g.setFont(juce::FontOptions(9.5f, juce::Font::bold));
            g.drawText(colors[i].toUpperCase(), colPill.toNearestInt(), juce::Justification::centred);
        }

        // Bottom status strip
        g.setColour(juce::Colour(0xff090e10));
        g.fillRect(0, 134, 515, 27);
        g.setColour(style.accent.withAlpha(0.3f));
        g.drawHorizontalLine(134, 0.0f, 515.0f);

        g.setColour(style.accent);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText("COLOR: " + colors[colIdx].toUpperCase()
                   + "   |   DITHER: " + (ditherOn ? "16-BIT" : "OFF")
                   + "   |   SR TIER: " + juce::String(proc.dspState[0].srIndex),
                   12, 136, 355, 23, juce::Justification::centredLeft);
        g.drawText("PAPIELGUAPO  1:1 DSP", 365, 136, 140, 23, juce::Justification::centredRight);
    }
};

juce::AudioProcessorEditor* Peacock::createEditor() { return new Panel(*this, state); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new Peacock; }

