# CamoBots

A real-life paint-and-hide shooter game. Hiders paint small white robot bodies to blend into a room and hide them. The seeker hunts them down with an infrared blaster before time runs out.

Inspired by the game Meccha Chameleon. Not affiliated with it, and it uses none of its audio, art or logos.

## How it plays

1. **Hide:** a timed countdown. Hiders place their painted bodies. The blaster is locked.
2. **Hunt:** the blaster goes live. Bodies whistle every so often, giving away their position.
3. **Shoot:** an IR hit captures a body. A miss locks the trigger for a few seconds.
4. **Reveal:** surviving bodies whistle and flash a rainbow. Points are tallied and roles rotate.

Burying a body is not allowed. Each body has a light sensor, and a covered body whistles nonstop and loses points.

## Hardware

All boards are Seeed Studio XIAO ESP32-S3.

| Unit | Parts |
|---|---|
| Body (x2) | TSOP38238 IR receiver, LDR, speaker on an NPN driver, NeoPixel, 400-500 mAh LiPo, slide switch |
| Blaster (x1) | 940 nm IR LED on a transistor, red aim LED, trigger button, speaker, optional 0.96" OLED, 2200 mAh Li-ion |

The blaster is also the game host. It runs a Wi-Fi hotspot called `CamoBots`, talks to the bodies over ESP-NOW on channel 1, and serves the game page at `http://192.168.4.1`.

### Pin map

| Pin | GPIO | Body | Blaster |
|---|---|---|---|
| D0 | 1 | TSOP IR in | Trigger button |
| D1 | 2 | LDR | IR LED (38 kHz) |
| D2 | 3 | Speaker | Aim LED |
| D3 | 4 | NeoPixel | Speaker |
| D4 / D5 | 5 / 6 | free | OLED SDA / SCL |

## Building

Uses [PlatformIO](https://platformio.org/). One firmware, built for each role:

```
pio run -e body    -t upload --upload-port COMx
pio run -e blaster -t upload --upload-port COMy
```

`body_sim` and `blaster_sim` build the same firmware with simulated sensors, so the game runs on bare boards with no parts wired.

In the SIM builds, type these into the serial monitor:

| Board | Commands |
|---|---|
| Blaster | `start`, `stop`, `fire`, `whistle N`, `set hide/hunt/whistle/reveal/lock/hp/strikes N`, `status` |
| Body | `hit [seeker]`, `cover`, `uncover`, `dark`, `ldr N`, `status` |

The BOOT button fires on the blaster and counts as a hit on a body.

If an upload fails and the board keeps restarting, hold **BOOT** while plugging in the USB cable, then upload again.

## Status

- [x] ESP-NOW link with the hotspot on one channel (0 lost packets, 10 m and 2 walls)
- [x] Full game logic with simulated sensors (hide, hunt, cover, strikes, misses, seeker HP, reveal)
- [ ] Scoring and badges
- [ ] Web page
- [ ] IR, light sensor, speaker and NeoPixel drivers

Bench numbers are in [docs/bench-results.md](docs/bench-results.md).

## License

MIT
