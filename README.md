<p align="center"><img src="art/fjarrnyckel-logo.png" width="160" alt="Fjärrnyckel logo: a brass telegraph key with signal arcs"></p>

# Fjärrnyckel

*Swedish for "remote key".* Send Morse code on a radio that is somewhere else,
with your own straight key or paddle, and have it go on the air exactly as you
keyed it.

Fjärrnyckel is for radio amateurs who run a remote station, for example an
Icom radio controlled with [wfview](https://wfview.org/). Voice and digital
modes work well over such a link. Morse does not, because the timing of your
hand is lost on the way. Fjärrnyckel carries the timing.

## How it works

- **The box** sits on your desk. It is an ESP32-S3 with a key jack and a small
  speaker. It reads your key, plays the sidetone at once, and sends every key
  movement with the exact time it happened.
- **The receiver** runs on the computer by the radio. It replays each movement
  a fixed, short moment later and keys the radio over its USB cable. Network
  jitter is absorbed; dots, dashes and your own rhythm are reproduced as you
  made them.
- **The link** between them is signed with a shared key, so only your box can
  key your radio, and the receiver lets go of the key if the link is lost.

## Status

Work in progress. Honest state of each part:

| Part | State |
|---|---|
| Receiver (`receiver/`) | Working. Tested on a Raspberry Pi 4 with an IC-7300 and wfview. |
| Keying protocol (CWL2) | Working. Covered by tests, including loss and jitter. |
| Box firmware (`firmware/`) | Wi-Fi setup, setup page, keyer and sidetone run on hardware. The keying link is written and tested on a computer but not yet in the firmware. |
| Tailscale in the box | Planned. |
| Updates over Wi-Fi | Planned. |
| Enclosure (`enclosure/`) | Draft; will change. |
| VFO knob (`vfo/`) | Draft of a companion tuning knob. |

## Hardware for the box

- ESP32-S3-DevKitC-1 (N16R8) on a screw-terminal expansion board
- MAX98357A I2S amplifier board and a small 4 Ω speaker
- 3.5 mm stereo jack for the key

| Signal | ESP32-S3 pin |
|---|---|
| Key jack tip (straight key, or one paddle) | GPIO4 |
| Key jack ring (the other paddle) | GPIO5 |
| Key jack sleeve | GND |
| Amplifier BCLK | GPIO15 |
| Amplifier LRC | GPIO16 |
| Amplifier DIN | GPIO17 |

On some DevKitC-1 clones the 5V pin gives no USB power until the board's
`IN-OUT` solder pads are bridged. Until then, power the amplifier from 3V3.

## Getting started

- **Receiver:** [Installing the receiver on a Raspberry Pi](docs/receiver-raspberry-pi.md)
- **Box:** build `firmware/` with PlatformIO (`pio run`) and flash it. The box
  then starts its own Wi-Fi network called `Fjarrnyckel-XXXX`; join it and the
  setup page opens.

## Tests

```
python3 -m unittest discover -s tests
```

## Thanks

The rules of the keying link are modelled on OK1HRA's Paddle2IP. The packet
format and the code are our own.

73 de SA3LEJ
