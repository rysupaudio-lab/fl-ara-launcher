// Offline interface-contract test only. No real factory/DC/plug-in/app is loaded.
#include "VST3ARAEntry.h"
#include <iostream>
#include <stdexcept>

using namespace rysup::ara_lab;
namespace {
struct Provider final : ARA::IMainFactory {
    ARA::ARAFactory factory{};
    unsigned refs=1;
    Provider(){factory.factoryID="offline-contract-only";}
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID,void** out)override{if(out)*out=nullptr;return Steinberg::kNoInterface;}
    Steinberg::uint32 PLUGIN_API addRef()override{return ++refs;}
    Steinberg::uint32 PLUGIN_API release()override{return --refs;}
    const ARA::ARAFactory* PLUGIN_API getFactory()override{return &factory;}
};
struct Component final : ARA::IPlugInEntryPoint,ARA::IPlugInEntryPoint2 {
    Provider& provider;unsigned refs=1,binds=0;
    ARA::ARAPlugInExtensionInstance extension{};
    explicit Component(Provider& p):provider(p){}
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,void** out)override{
        if(!out)return Steinberg::kInvalidArgument;*out=nullptr;
        if(Steinberg::FUnknownPrivate::iidEqual(iid,INLINE_UID_OF(ARA::IPlugInEntryPoint)))*out=static_cast<ARA::IPlugInEntryPoint*>(this);
        else if(Steinberg::FUnknownPrivate::iidEqual(iid,INLINE_UID_OF(ARA::IPlugInEntryPoint2)))*out=static_cast<ARA::IPlugInEntryPoint2*>(this);
        else return Steinberg::kNoInterface;addRef();return Steinberg::kResultOk;
    }
    Steinberg::uint32 PLUGIN_API addRef()override{return ++refs;}
    Steinberg::uint32 PLUGIN_API release()override{return --refs;}
    const ARA::ARAFactory* PLUGIN_API getFactory()override{return provider.getFactory();}
    const ARA::ARAPlugInExtensionInstance* PLUGIN_API bindToDocumentController(ARA::ARADocumentControllerRef)override{++binds;return &extension;}
    const ARA::ARAPlugInExtensionInstance* PLUGIN_API bindToDocumentControllerWithRoles(ARA::ARADocumentControllerRef,ARA::ARAPlugInInstanceRoleFlags,ARA::ARAPlugInInstanceRoleFlags)override{++binds;return &extension;}
};
VST3ARAEntry::NativeLifecycle lifecycle(bool& actualFresh,unsigned& observations){
    VST3ARAEntry::NativeLifecycle native;
    native.freshBeforeFirstBinding=[] {return true;};native.inactive=[] {return true;};
    native.saveState=[] {return Bytes{};};native.restoreState=[](const Bytes&){};
    native.verifyBorrowedNativeFreshnessImmediatelyBeforeBind=[&]{++observations;return actualFresh;};return native;
}
}
int main(){
    Provider provider;unsigned checks=0;
    const auto require=[&](bool good){++checks;if(!good)throw std::runtime_error("last actual native freshness contract failed");};
    const auto dc=reinterpret_cast<ARA::ARADocumentControllerRef>(std::uintptr_t{1});
    bool actualFresh=true;unsigned observations=0;Component changed(provider);
    {
        VST3ARAEntry entry(static_cast<ARA::IPlugInEntryPoint&>(changed),provider,std::make_shared<int>(0),lifecycle(actualFresh,observations));
        actualFresh=false;bool refused=false;
        try{entry.bind(dc,ARA::kARAEditorRendererRole|ARA::kARAEditorViewRole);}catch(const std::runtime_error&){refused=true;}
        require(refused&&changed.binds==0&&observations==1);
    }
    require(changed.refs==1&&provider.refs==1);
    actualFresh=true;observations=0;Component fresh(provider);
    {
        VST3ARAEntry entry(static_cast<ARA::IPlugInEntryPoint&>(fresh),provider,std::make_shared<int>(0),lifecycle(actualFresh,observations));
        require(entry.bind(dc,ARA::kARAEditorRendererRole|ARA::kARAEditorViewRole)==&fresh.extension&&fresh.binds==1&&observations==1);
        bool refused=false;try{entry.bind(dc,ARA::kARAEditorRendererRole);}catch(const std::runtime_error&){refused=true;}
        require(refused&&fresh.binds==1&&observations==1);
    }
    require(fresh.refs==1&&provider.refs==1);
    std::cout<<"{\"pass\":true,\"checks\":"<<checks<<",\"nativePluginLoaded\":false}\n";
}
