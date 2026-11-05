#include "qo100/settings.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
template<class F> void rejects(F f) {bool failed=false;try {f();}catch(const std::exception&){failed=true;}require(failed,"Invalid settings accepted");}
int main() {
    const auto directory=std::filesystem::temp_directory_path()/("qo100-settings-"+std::to_string(getpid()));
    try {
        std::filesystem::create_directory(directory);
        qo100::Config base;base.derive();base.validate();
        auto next=qo100::patch_settings(base,{{"rx_correction_hz","123"},{"tx_correction_hz","-456"},{"lnb_reference_hz","24999999"}});
        require(next.pluto_rx_hz==739750513 && next.pluto_tx_hz==1290249544,"Incorrect LNB/calibration calculation");
        next=qo100::patch_settings(next,{{"pluto_connection","usb"},{"gui_audio_gain","5.5"},{"initial_rx_hz","10489897327"}});
        require(next.pluto_uri=="usb:","USB selection failed");
        for(const auto& patch:{qo100::SettingsMap{{"tx_attenuation_db","41"}},{{"tx_attenuation_db","1.1"}},
            {{"pluto_ip","999.0.0.1"}},{{"rx_correction_hz","-9223372036854775808"}},{{"lnb_reference_hz","30000000"}},
            {{"ptt","1"}},{{"pluto_rx_hz","1"}},{{"gui_audio_gain","nan"}}}) rejects([&]{qo100::patch_settings(base,patch);});
        rejects([&]{qo100::parse_settings("gui_audio_gain=4\ngui_audio_gain=5\n");});
        const auto path=directory/"backend.settings";
        qo100::SettingsStore store(base,path);const auto initial_revision=store.revision();
        store.commit(next);
        require(store.revision()==initial_revision+1 && !store.dirty(),"Commit not durable");
        qo100::SettingsStore reloaded(base,path);
        require(qo100::settings_values(reloaded.config())==qo100::settings_values(next),"Stored settings did not survive restart");
        require(reloaded.config().pluto_rx_hz==739750513,"Derived RX LO not restored");
        store.config().gui_display_floor=-120;store.changed();require(store.dirty(),"Runtime settings not marked dirty");store.flush();
        qo100::SettingsStore again(base,path);require(again.config().gui_display_floor==-120,"Runtime flush lost value");
        const auto revision=store.revision();store.commit(store.config());require(store.revision()==revision,"No-op commit changed revision");
        qo100::SettingsStore unwritable(base,directory/"missing/backend.settings");
        rejects([&]{unwritable.commit(next);});require(unwritable.config().gui_audio_gain==base.gui_audio_gain,"Failed write changed live config");
        for(const auto& entry:std::filesystem::directory_iterator(directory)) require(entry.path().filename()=="backend.settings","Temporary settings file leaked");
        std::filesystem::remove_all(directory);
        std::cout<<"Settings: LNB/offsets, USB, validation, atomic save/reload, runtime flush, write failure and no-op revision passed\n";return 0;
    } catch(const std::exception& e) {std::filesystem::remove_all(directory);std::cerr<<e.what()<<'\n';return 1;}
}
