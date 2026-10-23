#pragma once
// Sensors, light and sound. SIM fakes them over serial and the user LED;
// real drivers replace hw.cpp's SIM half in phase 5.
#include <Arduino.h>

enum Pix : uint8_t { PIX_OFF, PIX_IDLE, PIX_HIDE, PIX_DARK, PIX_COVERED, PIX_CAPTURED, PIX_REVEAL,
                     PIX_LIVE, PIX_LOCKED };
enum Snd : uint8_t { SND_BEEP, SND_WHISTLE, SND_TAUNT, SND_FANFARE, SND_BUZZ, SND_CELEBRATE, SND_FIRE };

void hw_begin();
void hw_update();              // call every loop: animations, serial commands
void hw_pixel(Pix p);
void hw_sound(Snd s);
uint16_t hw_ldr();             // body: 0..4095
int hw_ir_hit();               // body: shooter id of a decoded shot, or -1
uint32_t hw_beam_ms();         // body: ms spent in the seeker's scan beam since the last call
bool hw_trigger();             // blaster: true once per press
void hw_fire();                // blaster: send an IR shot frame
bool hw_cmd(String& out);      // next serial line not eaten by hw, for game commands
