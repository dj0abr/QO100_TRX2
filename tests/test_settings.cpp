#include "qo100/settings.hpp"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
template<class F> void rejects(F f) {bool failed=false;try {f();}catch(const std::exception&){failed=true;}require(failed,"Invalid settings accepted");}
int main() {
    const auto directory=std::filesystem::temp_directory_path()/("qo100-settings-"+std::to_string(getpid()));
    try {
        std::filesystem::create_directory(directory);
        qo100::Config base;base.derive();base.validate();
        require(base.rx5_frequency_hz==10489993000ULL,"Wrong RX5 default");
        auto monitors=qo100::patch_settings(base,{{"rx3_frequency_hz","10489540321"},{"rx4_frequency_hz","10489748821"},{"rx5_frequency_hz","10489993001"}});
        require(monitors.rx3_frequency_hz==10489540321ULL && monitors.rx4_frequency_hz==10489748821ULL && monitors.rx5_frequency_hz==10489993001ULL,"Monitoring RF lost precision");
        require(!qo100::hardware_settings_changed(base,monitors),"Monitoring frequency must not restart hardware");
        for(const auto& invalid:{"0","-1","10489000000","10489540321.5","18446744073709551615"})
            for(const auto& key:{"rx3_frequency_hz","rx4_frequency_hz","rx5_frequency_hz"})
                rejects([&]{qo100::patch_settings(base,{{key,invalid}});});
        auto next=qo100::patch_settings(base,{{"rx_correction_hz","123"},{"tx_correction_hz","-456"},{"lnb_reference_hz","24999999"}});
        require(next.pluto_rx_hz==739750513 && next.pluto_tx_hz==1290249544,"Incorrect LNB/calibration calculation");
        next=qo100::patch_settings(next,{{"pluto_connection","usb"},{"gui_audio_gain","5.5"},{"initial_rx_hz","10489897327"}});
        auto digital=qo100::patch_settings(next,{{"initial_tx2_hz","2400103123"}});
        require(!qo100::hardware_settings_changed(next,digital) && digital.initial_tx_hz==next.initial_tx_hz,"TX2 tuning changed hardware/TX1");
        next=digital;
        for(const auto& invalid:{"0","2400600000","-1","2400103123.5"})
            rejects([&]{qo100::patch_settings(base,{{"initial_tx2_hz",invalid}});});
        require(next.pluto_uri=="usb:","USB selection failed");
        for(const auto& patch:{qo100::SettingsMap{{"tx_attenuation_db","41"}},{{"tx_attenuation_db","1.1"}},
            {{"pluto_ip","999.0.0.1"}},{{"rx_correction_hz","-9223372036854775808"}},{{"lnb_reference_hz","30000000"}},
            {{"ptt","1"}},{{"pluto_rx_hz","1"}},{{"gui_audio_gain","nan"}}}) rejects([&]{qo100::patch_settings(base,patch);});
        rejects([&]{qo100::parse_settings("gui_audio_gain=4\ngui_audio_gain=5\n");});
        const auto migrated=qo100::patch_settings(base,{{"rx1_filter_bandwidth_hz","501"},{"rx1_filter_center_hz","700"}});
        require(migrated.rx1_filter.low_hz()==449.5 && migrated.rx1_filter.high_hz()==950.5,"Legacy filter changed");
        next=qo100::patch_settings(next,{{"rx1_filter_low_hz","301"},{"rx1_filter_high_hz","2700"}});
        require(next.rx1_filter.center_hz==1500.5 && next.rx1_filter.bandwidth_hz==2399,"Cutoffs lost half-Hz center");
        auto upper=qo100::patch_settings(next,{{"rx1_filter_high_hz","2300"}});
        require(upper.rx1_filter.low_hz()==301 && upper.rx1_filter.high_hz()==2300,"Upper cutoff moved lower cutoff");
        auto lower=qo100::patch_settings(upper,{{"rx1_filter_low_hz","401"}});
        require(lower.rx1_filter.high_hz()==2300 && lower.rx1_filter.low_hz()==401,"Lower cutoff moved upper cutoff");
        for(const auto& patch:{qo100::SettingsMap{{"rx1_filter_low_hz","99"}},{{"rx1_filter_high_hz","4001"}},
            {{"rx1_filter_low_hz","2300"},{"rx1_filter_high_hz","2700"}},{{"rx1_filter_low_hz","nan"}},
            {{"rx1_filter_low_hz","300"},{"rx1_filter_bandwidth_hz","500"}}}) rejects([&]{qo100::patch_settings(base,patch);});
        const auto path=directory/"backend.settings";
        {std::ofstream saved(path);saved<<"rx1_filter_bandwidth_hz=501\nrx1_filter_center_hz=700\n";}
        {qo100::SettingsStore legacy(base,path);require(legacy.config().rx1_filter==migrated.rx1_filter,"Legacy saved settings not migrated");legacy.changed();legacy.flush();}
        {std::ifstream saved(path);std::string text((std::istreambuf_iterator<char>(saved)),{});
            require(text.find("rx1_filter_low_hz=449.5")!=std::string::npos && text.find("rx1_filter_bandwidth_hz")==std::string::npos,"Saved filter was not migrated to cutoff keys");}

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
