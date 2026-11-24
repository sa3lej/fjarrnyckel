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
| Tailscale in the box | In the firmware from 0.3.0 (MicroLink), with sign-in from the setup page. |
| Updates over Wi-Fi | From 0.4.0: the box finds new releases and installs them from its page. |
| Password on the setup page | From 0.5.0: optional, off until you set one. |
| Status light and tones, signed firmware, reset with the key | From 0.6.0. |
| Enclosure (`enclosure/`) | Redrawn for the bare board on its headers, one key socket and an optional headphone socket. Not printed yet; the part sizes in the file must be measured first. `fjarrnyckel-box.jpg` is a generated picture of the design, not a photo of a print. |
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

## Training with practice sites

Set **The key goes to** to **The computer** on the box's page and run a cable from the board's port marked
USB to the computer. The box then shows up as a keyboard and a MIDI device, the way the VBand and Vail
adapters do, and nothing is sent to the radio:

- keyboard: dit is Left Ctrl, dah is Right Ctrl (VBand, Morse Code World, Morseorama, Lids Anonymous, ...);
- MIDI: notes 1 and 2 for the paddles, note 0 for finished keying; a site that speaks Vail's MIDI protocol
  switches the box over by itself.

With **Finished keying** the box's own keyer and sidetone are used and the site sees a straight key. With
**The two paddles** the site does the keying.

## Updates

A new version is on trial for its first minute: if it crashes, hangs or loses power before that, the
box goes back to the version that worked. The box looks for a new version on its own (after it gets Wi-Fi, then twice a day) and shows it under
Software on its page. Nothing installs until you press the button; settings are kept. A firmware file can
also be installed from the same place.

Released firmware is signed, and the box only installs signed firmware. The private key is a secret of
this repository (`FN_SIGNING_KEY`); the public half is in `firmware/src/signing_key.h`. If you build your
own firmware, put your own public key there, or use service mode.

**Service mode and reset, with only the key.** Hold the key down while plugging the box in:

- let go after the first beep (5 seconds): the page's password is removed, and until the next restart the
  box also accepts firmware that is not signed;
- let go after the three beeps (15 seconds): all settings are erased and the box is as new;
- a key that never comes up (stuck, or switched closed) changes nothing.

A release is made by setting the version in `firmware/CMakeLists.txt` and pushing a tag `v<version>`.
GitHub Actions builds the firmware and publishes `fjarrnyckel-update.bin` (for the box's page),
`fjarrnyckel-usb.bin` (a full image for a new board, written at address 0) and `version.txt`.

## Tests

```
python3 -m unittest discover -s tests
```

## Licence

MIT. See [LICENSE](LICENSE). The designs in `enclosure/` and `vfo/` are under the same licence.

## Thanks

The rules of the keying link are modelled on OK1HRA's Paddle2IP. The packet
format and the code are our own.

73 de SA3LEJ
