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
    auto save=[&](const char* path){
    auto image=editor->createComponentSnapshot(editor->getLocalBounds(),true,2);
    auto file=juce::File::getCurrentWorkingDirectory().getChildFile(path);
    juce::FileOutputStream out(file);
    if(!out.openedOk() || !out.setPosition(0) || out.truncate().failed()) return 1;
    return juce::PNGImageFormat().writeImageToStream(image,out)?0:1;};
    if(save("docs/peacock.png")) return 1;
    auto* flip=dynamic_cast<juce::Button*>(editor->findChildWithID("flip"));
    if(!flip || flip->getButtonText()!="B") return 1;
    std::vector<float> before;
    for(auto* p:processor->getParameters()) before.push_back(p->getValue());
    flip->onClick();
    if(flip->getButtonText()!="A" || save("docs/peacock-deck.png")) return 1;
    flip->onClick();
    for(auto* p:processor->getParameters()) if(p->getValue()!=before.at(p->getParameterIndex())) return 1;
    return flip->getButtonText()=="B"?0:1;
}
