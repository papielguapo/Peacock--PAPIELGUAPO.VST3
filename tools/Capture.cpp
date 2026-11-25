#include <juce_audio_processors/juce_audio_processors.h>
int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    std::unique_ptr<juce::AudioProcessor> processor(createPluginFilter());
    for(auto* p:processor->getParameters()){
        auto name=p->getName(32);
        if(name=="Harmonic") p->setValueNotifyingHost(.6f);
        if(name=="Dynamic") p->setValueNotifyingHost(.67f);
        if(name=="Color") p->setValueNotifyingHost(.75f);
    }
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditor());
    auto image=editor->createComponentSnapshot(editor->getLocalBounds(),true,2);
    auto file=juce::File::getCurrentWorkingDirectory().getChildFile("docs/peacock.png");
    juce::FileOutputStream out(file);
    if(!out.openedOk() || !out.setPosition(0) || out.truncate().failed()) return 1;
    return juce::PNGImageFormat().writeImageToStream(image,out)?0:1;
}
