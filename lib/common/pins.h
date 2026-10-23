#pragma once
// Same physical pins on every board; meaning depends on role.
// XIAO ESP32-S3: D0..D5 = GPIO1..6 (all ADC1, safe with Wi-Fi on).

#define PIN_USER_LED   21   // active low
#define PIN_BOOT_BTN   0    // active low, SIM fake trigger

// Body
#define PIN_TSOP       1    // D0, TSOP38238 out, active low
#define PIN_LDR        2    // D1, ADC1
#define PIN_BODY_SPK   3    // D2, strapping pin; move to 7 (D8) if boot misbehaves
#define PIN_PIXEL      4    // D3

// Blaster
#define PIN_TRIGGER    1    // D0, to GND, internal pull-up
#define PIN_IR_LED     2    // D1, 38 kHz LEDC into transistor base
#define PIN_AIM_LED    3    // D2
#define PIN_BLASTER_SPK 4   // D3
#define PIN_OLED_SDA   5    // D4
#define PIN_OLED_SCL   6    // D5
// D6/D7 (GPIO43/44) left free for UART.
