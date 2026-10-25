# Bench results

Every hardware and radio test, with real numbers.

| Date | Test | Setup | Result | Notes |
|---|---|---|---|---|
| 2026-10-24 | Build all envs | PlatformIO 6.1.19, espressif32 6.11.0 | body, blaster, body_sim, blaster_sim build | First clean build of the env takes about 2 min |
| 2026-10-24 | ESP-NOW + SoftAP, ch 1 | 1 host (SoftAP "CamoBots") + 2 bodies, 10 Hz unicast pings each, all on desk within 1 m, phone joined to AP | 0 lost of 600 per body over 60 s; 0 lost of about 1,400 over an earlier 70 s run; MAC ACK fails only during host reboots | Link test page served in 3 ms or less with the phone polling every 1 s |
| 2026-10-24 | Flashing over native USB | 3 boards on 3 laptop ports | Uploads sometimes drop mid-write ("device does not recognize the command"). One board failing alone was swapped. | A failed upload leaves a half-written app that boot-loops; recover with BOOT held while plugging in. Opening the serial port with DTR/RTS asserted resets the board. |
| 2026-10-24 | ESP-NOW range | Body on power bank, host on laptop USB, indoors. Walked to about 10 m line of sight, then 2 walls away (next apartment rooms) | 0 lost, 0 MAC ACK fails over about 5,500 pings (about 9 min) | Newest ping never older than 51 ms. Indoor play area is well inside range. |
| 2026-10-24 | SIM game, 2 rounds | 1 host + 2 bodies, sensors faked over serial, scripted commands | Round 1: miss and 3 s lockout, trigger locked in hide, cover, clear, strike-out on 2nd cover, capture, seeker wins. Round 2: dark body (cover check off), manual whistle, panic whistle, seeker out of HP, hiders win with reveal. 0 ACK fails. | Captured bodies hold red until the reveal ends. |
| 2026-10-24 | Bad antenna on one body | Body B after being moved around | Body could send but barely receive: 1 frame received in 60 s, every unicast unACKed, joins took 10-40 s | Reseating did not last. Swapping the antenna fixed it at once. The U.FL antenna is the first thing to check when one board has a bad link. |
| 2026-10-25 | IR shot range | Blaster IR LED (RMT 38 kHz) aimed straight at body TSOP38238, indoors, `burst 20` (0.5 s apart) | 1 m: 20/20, 2 m: 20/20, 3 m: 20/20, 0 bad frames | An earlier 11/20 run was the bench playing a 0.8 s blocking clip on each hit, so frames overwrote each other. The bench now beeps instead. |
| 2026-10-25 | Blaster OLED | 0.96" SSD1306 at 0x3C on D4/D5 | Answered on I2C but stayed blank with VCC on 3V3. Works with VCC on 5V. | I2C dropped to 100 kHz for long jumpers while debugging. |
