// Offline test fixture only: never opens a sound card or sends network data.
#include "hsmodem.h"
#include <iostream>
#include <string>
#include "fec/schifra_galois_field.hpp"
#include "fec/schifra_galois_field_polynomial.hpp"
#include "fec/schifra_sequential_root_generator_polynomial_creator.hpp"
#include "fec/schifra_reed_solomon_encoder.hpp"
#include "fec/schifra_reed_solomon_block.hpp"
int main(int argc, char** argv) {
    double offset = argc > 1 ? atof(argv[1]) : 0;
    std::string mode = argc > 2 ? argv[2] : "clean";
    bool corrupt = mode == "corrupt";
    int frame_count = argc > 3 ? atoi(argv[3]) : 45;
    const schifra::galois::field field(8, schifra::galois::primitive_polynomial_size06,
                                      schifra::galois::primitive_polynomial06);
    schifra::galois::field_polynomial generator(field);
    schifra::make_sequential_root_generator_polynomial(field,120,32,generator);
    schifra::reed_solomon::encoder<255,32,223> encoder(field,generator);
    auto mod=modem_create(LIQUID_MODEM_APSK8);
    float h[601];
    liquid_firdes_prototype(LIQUID_FIRFILT_RRC,20,15,0.2f,-0.2f,h);
    auto interp=firinterp_crcf_create(20,h,601);
    double phase=0, pilot_phase=0;
    uint64_t audio_samples = 0;
    for(int frame=0;frame<frame_count;++frame) {
        uint8_t raw[223]={}; raw[0]=frame; raw[1]=8|0x30;
        raw[2]=2;
        const char* msg="TEST CW-SKIMMER RX VALIDATION";
        memcpy(raw+3,msg,strlen(msg));
        uint16_t crc=Crc16_messagecalc(0,raw,221);
        raw[221]=crc>>8; raw[222]=crc;
        if(corrupt) raw[222]^=1; // Valid RS, deliberately invalid frame CRC.
        schifra::reed_solomon::block<255,32> block;
        for(int i=0;i<223;++i)block[i]=raw[i];
        if(!encoder.encode(block))return 2;
        uint8_t air[258]={0x53,0xe1,0xa6};
        for(int i=0;i<255;++i)air[3+i]=block[i];
        uint8_t* scrambled=RX_Scramble(air+3,255); // XOR is symmetric.
        memcpy(air+3,scrambled,255);
        unsigned int acc=0; int bits=0;
        for(auto byte:air) {
            acc=(acc<<8)|byte;bits+=8;
            while(bits>=3) {
                bits-=3;unsigned int sym=(acc>>bits)&7;
                liquid_float_complex x,y[20];modem_modulate(mod,sym,&x);
                firinterp_crcf_execute(interp,x,y);
                for(auto c:y) {
                    double t = audio_samples++/48000.0;
                    double frequency_offset = offset;
                    if(mode == "drift") frequency_offset += t;
                    if(mode == "jump" && t >= 8) frequency_offset += 70;
                    if(mode == "dropout" && t >= 11) frequency_offset += 40;
                    phase+=2*M_PI*(1500+frequency_offset)/48000;
                    pilot_phase+=2*M_PI*(100+frequency_offset)/48000;
                    // Matches the original SSB audio synthesis (real + imag).
                    float re=c.real*cos(phase)-c.imag*sin(phase);
                    float im=c.real*sin(phase)+c.imag*cos(phase);
                    float audio=0.1f*(re+im)+0.006f*cos(pilot_phase);
                    if(mode == "nopilot") audio-=0.006f*cos(pilot_phase);
                    if(mode == "interferer") audio+=0.04f*cos(2*M_PI*310*t);
                    if(mode == "dropout" && t>=8 && t<11) audio=0;
                    fwrite(&audio,sizeof(audio),1,stdout);
                }
            }
        }
    }
    firinterp_crcf_destroy(interp);modem_destroy(mod);
}
