#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <cmath>

class Peacock final : public juce::AudioProcessor
{
    using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;
    static Layout layout()
    {
        Layout p;
        for (auto name : {"Harmonic", "Dynamic", "Dither"})
            p.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{name,1}, name,
                juce::NormalisableRange<float>(0, 100, 0.1f), 0));
        p.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{"Color",1}, "Color",
            juce::StringArray{"Silver","Gold","Rich","Fat","Deep"}, 0));
        p.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{"OutputTrim",1},"Output Trim",
            juce::NormalisableRange<float>(-6,6,0.1f),0.f));
        return p;
    }
    juce::AudioProcessorValueTreeState state{*this,nullptr,"PeacockStudy",layout()};
    struct Channel { float low{}, body{}, env{}, previous{}, dc{}, noise{}; };
    std::array<Channel,2> channels{};
    std::array<juce::SmoothedValue<float>,4> controls;
    juce::SmoothedValue<float,juce::ValueSmoothingTypes::Multiplicative> trim;
    const std::atomic<float>* trimDb=state.getRawParameterValue("OutputTrim");
    std::array<const std::atomic<float>*,4> params{state.getRawParameterValue("Harmonic"),
        state.getRawParameterValue("Dynamic"),state.getRawParameterValue("Dither"),state.getRawParameterValue("Color")};
    juce::Random random;
    float treble{}, release{}, removeDC{}, noisePole{}, tracingPole{};
    std::array<float,5> bass{};
public:
    Peacock(): AudioProcessor(BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true)
                                               .withOutput("Output",juce::AudioChannelSet::stereo(),true)) {}
    void prepareToPlay(double rate,int) override
    {
        channels = {};
        auto pole = [rate](double hz){return float(-std::expm1(-juce::MathConstants<double>::twoPi*hz/rate));};
        treble=pole(2600); release=pole(18); removeDC=pole(12); noisePole=pole(1350);
        tracingPole=pole(7600);
        trim.reset(rate,0.015);trim.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(trimDb->load()));
        for(int i=0;i<5;++i) bass[i]=pole(420*std::pow(0.65,i));
        for(size_t i=0;i<4;++i) { controls[i].reset(rate,0.015); controls[i].setCurrentAndTargetValue(params[i]->load()); }
    }
    void processBlock(juce::AudioBuffer<float>& b,juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals guard;
        trim.setTargetValue(juce::Decibels::decibelsToGain(trimDb->load()));
        for(size_t i=0;i<4;++i) controls[i].setTargetValue(params[i]->load());
        for(int n=0;n<b.getNumSamples();++n)
        {
            auto h=controls[0].getNextValue()*0.01f, d=controls[1].getNextValue()*0.01f,
                 noise=controls[2].getNextValue()*0.01f, color=controls[3].getNextValue();
            int idx=int(color);
            float gain=trim.getNextValue();
            float cut=bass[idx]+(color-idx)*(bass[juce::jmin(4,idx+1)]-bass[idx]);
            for(int c=0;c<b.getNumChannels();++c)
            {
                auto& s=channels[c]; float x=b.getSample(c,n);
                s.low+=treble*(x-s.low); s.body+=cut*(x-s.body);
                float hi=x-s.low, mid=s.low-s.body;
                s.env=juce::jmax(std::abs(hi),s.env*(1-release));
                float a=std::tanh(mid*(1.5f+color*0.4f));
                float even=a*a;
                s.dc+=removeDC*(even-s.dc);
                float mod=d*(1-0.55f*d)*s.env/(0.12f+s.env);
                // Filter only the distortion residual; its time scale must not follow the host's sample period.
                s.previous+=tracingPole*(even-s.previous);
                float shifted=even+mod*(s.previous-even);
                float y=x+h*0.32f*(a-mid*(1.5f+color*0.4f))
                    +d*(0.08f+0.045f*color)*(shifted-s.dc)
                    -hi*(1-1/(1+h*(3+color)*s.env));
                y*=gain;
                if(noise>0) {
                    float tpdf=random.nextFloat()-random.nextFloat();
                    s.noise+=noisePole*(tpdf-s.noise);
                    y+=noise*(tpdf+s.noise)*0.000030517578125f;
                    if(noise>=1.f) y=std::round(y*32768.f)/32768.f;
                }
                b.setSample(c,n,y);
            }
        }
    }
    bool isBusesLayoutSupported(const BusesLayout& l) const override {
        auto out=l.getMainOutputChannelSet(); return out==l.getMainInputChannelSet()
            && (out==juce::AudioChannelSet::mono() || out==juce::AudioChannelSet::stereo());
    }
    void releaseResources() override {}
    const juce::String getName() const override {return JucePlugin_Name;}
    bool acceptsMidi() const override {return false;}
    bool producesMidi() const override {return false;}
    bool isMidiEffect() const override {return false;}
    double getTailLengthSeconds() const override {return 0;}
    int getNumPrograms() override {return 1;}
    int getCurrentProgram() override {return 0;}
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override {return {};}
    void changeProgramName(int,const juce::String&) override {}
    bool hasEditor() const override {return true;}
    juce::AudioProcessorEditor* createEditor() override;
    void getStateInformation(juce::MemoryBlock& b) override {if(auto xml=state.copyState().createXml()) copyXmlToBinary(*xml,b);}
    void setStateInformation(const void* data,int size) override {
        if(auto xml=getXmlFromBinary(data,size); xml && xml->hasTagName(state.state.getType())) state.replaceState(juce::ValueTree::fromXml(*xml));
    }
};

class Panel final : public juce::AudioProcessorEditor
{
    struct DialStyle : juce::LookAndFeel_V4 {
        bool deck=false;
        juce::Colour accent;
        void drawRotarySlider(juce::Graphics& g,int x,int y,int w,int h,float value,float start,float end,juce::Slider&) override {
            auto r=float(juce::jmin(w,h))/2-6; auto cx=x+w*.5f, cy=y+h*.5f;
            auto polar=[&](float angle,float radius){return juce::Point<float>(cx+radius*std::sin(angle),cy-radius*std::cos(angle));};
            if(deck){
                auto box=juce::Rectangle<float>(2*r,2*r).withCentre({cx,cy});
                g.setColour(juce::Colour(0xff090e10));g.fillEllipse(box);
                juce::Path arc;arc.addCentredArc(cx,cy,r-2,r-2,0,start,end,true);
                g.setColour(juce::Colour(0xff455053));g.strokePath(arc,juce::PathStrokeType(3));
                juce::Path active;active.addCentredArc(cx,cy,r-2,r-2,0,start,start+value*(end-start),true);
                g.setColour(accent);g.strokePath(active,juce::PathStrokeType(3));
                g.drawLine({polar(start+value*(end-start),r*.45f),polar(start+value*(end-start),r-5)},2.5f);return;
            }
            auto ring=juce::Rectangle<float>(2*r-4,2*r-4).withCentre({cx,cy});
            g.setGradientFill(juce::ColourGradient(juce::Colour(0xfff1e9cd),ring.getX(),ring.getY(),juce::Colour(0xff47504a),ring.getRight(),ring.getBottom(),false));
            g.fillEllipse(ring);auto face=ring.reduced(4);
            g.setGradientFill(juce::ColourGradient(juce::Colour(0xff39988c),cx-8,cy-9,juce::Colour(0xff07443e),cx+r,cy+r,true));
            g.fillEllipse(face);g.setColour(juce::Colour(0xff163e39));g.drawEllipse(face,1);
            auto angle=start+value*(end-start);
            g.setColour(juce::Colour(0xffffdf99));g.drawLine({polar(angle,r*.42f),polar(angle,r-11)},3);
        }
    } style;
    using State=juce::AudioProcessorValueTreeState;
    std::array<juce::Slider,5> knobs;
    std::array<std::unique_ptr<State::SliderAttachment>,5> links;
    juce::StringArray colors{"Silver","Gold","Rich","Fat","Deep"};
    State& ui;
    juce::TextButton flip;
    std::array<juce::TextButton,7> tabs;
    int palette=0;
    void refresh() {
        constexpr juce::uint32 accents[]={0xff55d5ad,0xffffad54,0xffb4a4ff,0xffeabe62,0xff75dbf4,0xffe0ddaa,0xff7ef684};
        style.accent=juce::Colour(accents[palette]);
        flip.setButtonText(style.deck?"A":"B");flip.setTooltip(style.deck?"Cara A":"Cara B");
        for(int i=0;i<7;++i){tabs[i].setVisible(style.deck);tabs[i].setColour(juce::TextButton::textColourOffId,i==palette?style.accent:juce::Colours::white);}
        resized();repaint();
    }
public:
    Panel(Peacock& p,State& state):AudioProcessorEditor(p),ui(state) {
        setLookAndFeel(&style);
        const char* ids[]={"Harmonic","Dynamic","OutputTrim","Dither","Color"};
        for(int i=0;i<5;++i){
            auto& k=knobs[i]; k.setName(ids[i]); k.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            k.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);
            k.setColour(juce::Slider::textBoxOutlineColourId,juce::Colours::transparentBlack);
            addAndMakeVisible(k);links[i]=std::make_unique<State::SliderAttachment>(state,ids[i],k);
            k.onValueChange=[this]{repaint();};
        }
        knobs[2].setName("Output Trim");
        knobs[3].setRange(0,100,100);
        flip.setComponentID("flip");flip.setName("Cambiar cara");
        flip.onClick=[this]{style.deck=!style.deck;ui.state.setProperty("deck",style.deck,nullptr);refresh();};
        addAndMakeVisible(flip);
        const char* codes[]={"CS","FL","PT","AB","LP","98","AT"};
        for(int i=0;i<7;++i){auto& t=tabs[i];t.setButtonText(codes[i]);t.onClick=[this,i]{palette=i;ui.state.setProperty("palette",i,nullptr);refresh();};addChildComponent(t);}
        style.deck=bool(ui.state.getProperty("deck",false));palette=juce::jlimit(0,6,int(ui.state.getProperty("palette",0)));
        setSize(545,173);refresh();
    }
    ~Panel() override {setLookAndFeel(nullptr);}
    void resized() override {
        for(int i=0;i<5;++i) knobs[i].setBounds(style.deck?8+i*106:13+i*95,style.deck?32:32,style.deck?96:79,style.deck?65:81);
        flip.setBounds(517,4,23,20);
        for(int i=0;i<7;++i) tabs[i].setBounds(280+32*i,4,30,20);
    }
    void mouseDoubleClick(const juce::MouseEvent& e) override {
        if(e.y<29 || (style.deck && e.y>136) || (!style.deck && e.x>=471 && e.y>136)) flip.onClick();
    }
    void paint(juce::Graphics& g) override {
        if(style.deck){
            g.fillAll(juce::Colour(0xff1c2529));g.setColour(style.accent);g.setFont(16.f);
            g.drawText("PEACOCK  /  DECK",12,3,240,22,juce::Justification::centredLeft);
            g.drawHorizontalLine(27,9,536);
            for(int i=0;i<5;++i){auto x=8+i*106;g.setColour(juce::Colours::white);g.setFont(13.f);g.drawText(knobs[i].getName(),x,97,96,19,juce::Justification::centred);
                auto v=knobs[i].getValue();g.setColour(style.accent);g.drawText(i==4?colors[int(v)]:i==3?(v>0?juce::String("16 Bit"):juce::String("Off")):juce::String(v,1),x,115,96,18,juce::Justification::centred);}
            g.setColour(juce::Colour(0xff0d1517));g.fillRect(8,138,529,28);g.setColour(style.accent);g.setFont(12.f);
            g.drawText("COLOR  /  "+colors[int(knobs[4].getValue())].toUpperCase(),18,140,180,24,juce::Justification::centredLeft);
            g.drawText("OUTPUT  /  "+juce::String(knobs[2].getValue(),1)+" dB",180,140,165,24,juce::Justification::centred);
            g.drawText("PAPIELGUAPO",350,140,174,24,juce::Justification::centredRight);return;
        }
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xffbecbc0),0,0,juce::Colour(0xff8fa7a2),545,173,false));g.fillAll();
        juce::Random grain(541);g.setColour(juce::Colours::black.withAlpha(.055f));
        for(int i=0;i<1800;++i) g.fillRect(grain.nextInt(600),grain.nextInt(192),2,1);
        auto ink=juce::Colour(0xff173832);g.setColour(ink);g.setFont(26.f);
        g.drawText("Peacock",18,4,160,34,juce::Justification::centredLeft);
        g.setFont(13.f);g.drawText("VINYL EMULATION",132,5,174,26,juce::Justification::centredLeft);
        g.setFont(10.f);g.drawText("PAPIELGUAPO",461,145,80,18,juce::Justification::centredRight);
        g.setFont(11.f);for(int i=0;i<5;++i)g.drawText(colors[i].toUpperCase(),472,39+14*i,69,15,juce::Justification::centredLeft);
        const char* ranges[]={"0  -  100","0  -  100","-6  /  +6 dB","OFF  /  ON","SILVER  -  DEEP"};
        for(int i=0;i<5;++i){
            int x=5+i*95;g.setColour(ink);g.setFont(10.f);if(i<4)g.drawText(ranges[i],x,107,95,13,juce::Justification::centred);
            g.setFont(13.f);g.drawText(knobs[i].getName().toUpperCase(),x,120,95,18,juce::Justification::centred);
            g.setColour(juce::Colour(0xff102222));g.fillRect(x+17,140,61,20);
            auto v=knobs[i].getValue();auto text=i==4?colors[int(v)]:i==3?(v>0?juce::String("16 Bit"):juce::String("Off")):juce::String(v,1);
            g.setColour(juce::Colour(0xffedeacb));g.drawText(text,x,140,95,20,juce::Justification::centred);
        }
    }
};
juce::AudioProcessorEditor* Peacock::createEditor(){return new Panel(*this,state);}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){return new Peacock;}
