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
        return p;
    }
    juce::AudioProcessorValueTreeState state{*this,nullptr,"PeacockStudy",layout()};
    struct Channel { float low{}, body{}, env{}, previous{}, dc{}, noise{}; };
    std::array<Channel,2> channels{};
    std::array<juce::SmoothedValue<float>,4> controls;
    std::array<const std::atomic<float>*,4> params{state.getRawParameterValue("Harmonic"),
        state.getRawParameterValue("Dynamic"),state.getRawParameterValue("Dither"),state.getRawParameterValue("Color")};
    juce::Random random;
    float treble{}, release{}, removeDC{}, noisePole{};
    std::array<float,5> bass{};
public:
    Peacock(): AudioProcessor(BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true)
                                               .withOutput("Output",juce::AudioChannelSet::stereo(),true)) {}
    void prepareToPlay(double rate,int) override
    {
        channels = {};
        auto pole = [rate](double hz){return float(-std::expm1(-juce::MathConstants<double>::twoPi*hz/rate));};
        treble=pole(2600); release=pole(18); removeDC=pole(12); noisePole=pole(1350);
        for(int i=0;i<5;++i) bass[i]=pole(420*std::pow(0.65,i));
        for(size_t i=0;i<4;++i) { controls[i].reset(rate,0.015); controls[i].setCurrentAndTargetValue(params[i]->load()); }
    }
    void processBlock(juce::AudioBuffer<float>& b,juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals guard;
        for(size_t i=0;i<4;++i) controls[i].setTargetValue(params[i]->load());
        for(int n=0;n<b.getNumSamples();++n)
        {
            auto h=controls[0].getNextValue()*0.01f, d=controls[1].getNextValue()*0.01f,
                 noise=controls[2].getNextValue()*0.01f, color=controls[3].getNextValue();
            int idx=int(color);
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
                float shifted=even+mod*(s.previous-even); s.previous=even;
                float y=x+h*0.32f*(a-mid*(1.5f+color*0.4f))
                    +d*(0.08f+0.045f*color)*(shifted-s.dc)
                    -hi*(1-1/(1+h*(3+color)*s.env));
                if(noise>0) {
                    float tpdf=random.nextFloat()-random.nextFloat();
                    s.noise+=noisePole*(tpdf-s.noise);
                    y+=noise*(tpdf+s.noise)*0.000030517578125f;
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
    juce::GenericAudioProcessorEditor controls;
public:
    explicit Panel(Peacock& p):AudioProcessorEditor(p), controls(p) {
        addAndMakeVisible(controls); setSize(440,82+controls.getHeight());
    }
    void resized() override {controls.setBounds(getLocalBounds().withTrimmedTop(82));}
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff123d39)); g.setColour(juce::Colour(0xffa4dec4));
        g.setFont(13.f); g.drawText("PAPIELGUAPO   /   VINYL PROCESSOR",20,9,400,22,juce::Justification::centredLeft);
        g.setColour(juce::Colours::ivory); g.setFont(31.f);
        g.drawText("Peacock",18,29,400,44,juce::Justification::centredLeft);
    }
};
juce::AudioProcessorEditor* Peacock::createEditor(){return new Panel(*this);}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){return new Peacock;}
