# Bench results

Every hardware and radio test, with real numbers.

| Date | Test | Setup | Result | Notes |
|---|---|---|---|---|
| 2026-10-24 | Build all envs | PlatformIO 6.1.19, espressif32 6.11.0 | body, blaster, body_sim, blaster_sim build | First clean build of the env takes about 2 min |
| 2026-10-24 | ESP-NOW + SoftAP, ch 1 | 1 host (SoftAP "CamoBots") + 2 bodies, 10 Hz unicast pings each, all on desk within 1 m, phone joined to AP | 0 lost of 600 per body over 60 s; 0 lost of about 1,400 over an earlier 70 s run; MAC ACK fails only during host reboots | Link test page served in 3 ms or less with the phone polling every 1 s |
| 2026-10-24 | Flashing over native USB | 3 boards on 3 laptop ports | Uploads sometimes drop mid-write ("device does not recognize the command"). One board failing alone was swapped. | A failed upload leaves a half-written app that boot-loops; recover with BOOT held while plugging in. Opening the serial port with DTR/RTS asserted resets the board. |
| 2026-10-24 | ESP-NOW range | Body on power bank, host on laptop USB, indoors. Walked to about 10 m line of sight, then 2 walls away (next apartment rooms) | 0 lost, 0 MAC ACK fails over about 5,500 pings (about 9 min) | Newest ping never older than 51 ms. Indoor play area is well inside range. |
