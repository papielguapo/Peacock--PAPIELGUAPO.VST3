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
        void drawRotarySlider(juce::Graphics& g,int x,int y,int w,int h,float value,float start,float end,juce::Slider&) override {
            auto r=float(juce::jmin(w,h))/2-6; auto cx=x+w*.5f, cy=y+h*.5f;
            auto polar=[&](float angle,float radius){return juce::Point<float>(cx+radius*std::sin(angle),cy-radius*std::cos(angle));};
            for(int i=0;i<=20;++i){
                auto angle=start+(end-start)*i/20;
                g.setColour(juce::Colour(i<=value*20?0xffeed29b:0xff63716d));
                g.drawLine({polar(angle,r-2),polar(angle,r+(i%5==0?5:1))},1.6f);
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
public:
    Panel(Peacock& p,State& state):AudioProcessorEditor(p) {
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
        setSize(600,192);
    }
    ~Panel() override {setLookAndFeel(nullptr);}
    void resized() override {
        for(int i=0;i<5;++i) knobs[i].setBounds(14+i*116,42,108,83);
    }
    void paint(juce::Graphics& g) override {
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xffbecbc0),0,0,juce::Colour(0xff8fa7a2),600,192,false));g.fillAll();
        juce::Random grain(541);g.setColour(juce::Colours::black.withAlpha(.055f));
        for(int i=0;i<1800;++i) g.fillRect(grain.nextInt(600),grain.nextInt(192),2,1);
        auto ink=juce::Colour(0xff173832);g.setColour(ink);g.setFont(26.f);
        g.drawText("Peacock",18,4,160,34,juce::Justification::centredLeft);
        g.setFont(13.f);g.drawText("VINYL EMULATION",188,8,224,26,juce::Justification::centred);
        g.drawText("PAPIELGUAPO",425,8,154,26,juce::Justification::centredRight);
        g.drawHorizontalLine(39,16,584);
        const char* ranges[]={"0  -  100","0  -  100","-6  /  +6 dB","OFF  /  ON","SILVER  -  DEEP"};
        for(int i=0;i<5;++i){
            int x=14+i*116;g.setColour(ink);g.setFont(11.f);g.drawText(ranges[i],x,121,108,15,juce::Justification::centred);
            g.setFont(14.f);g.drawText(knobs[i].getName().toUpperCase(),x,136,108,19,juce::Justification::centred);
            g.setColour(juce::Colour(0xff102222));g.fillRoundedRectangle(float(x+15),158,78,23,3);
            auto v=knobs[i].getValue();auto text=i==4?colors[int(v)]:i==3?(v>0?juce::String("16 Bit"):juce::String("Off")):juce::String(v,1);
            g.setColour(juce::Colour(0xffedeacb));g.drawText(text,x,158,108,23,juce::Justification::centred);
        }
    }
};
juce::AudioProcessorEditor* Peacock::createEditor(){return new Panel(*this,state);}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){return new Peacock;}
