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
| Enclosure (`enclosure/`) | Sized for the parts in use: the wide ESP32-S3 clone on its headers (YD-ESP32-S3 drawing), two MAX98357A boards standing in grooved posts with Dupont plugs, the CQRobot 70 × 31 × 16 mm speaker held by lid pins in its screw holes, PJ-392 key and headphone sockets, ruthex M3 inserts. Reviewed by Codex; fixes applied. Not printed yet: print `stl/fjarrnyckel-coupon.stl` first and try the real parts in it. Put 1 mm foam tape on the lid's four speaker pads. `fjarrnyckel-box.jpg` is a generated picture of an earlier version, not a photo of a print. |
| VFO knob (`vfo/`) | Draft of a companion tuning knob. |

## Hardware for the box

**[Build guide](docs/build-guide.md):** parts with links, pin map, wiring chart, printing and assembly,
step by step.

- ESP32-S3 N16R8 DevKitC-1, wide clone (YD-ESP32-S3 layout: pin rows 25.4 mm apart, IN-OUT pads)
- two MAX98357A I2S amplifier boards, a CQRobot 3 W 4 Ω speaker, two PJ-392 3.5 mm sockets
- the printed box in `enclosure/`

| Signal | ESP32-S3 pin |
|---|---|
| Key jack tip (straight key, or the dit paddle) | GPIO4 (J1-4) |
| Key jack ring (the dah paddle) | GPIO5 (J1-5) |
| Key jack sleeve | GND |
| Speaker amplifier BCLK / LRC / DIN | GPIO15 / GPIO16 / GPIO17 |
| Headphone amplifier BCLK / LRC / DIN | GPIO12 / GPIO13 / GPIO14 |
| Both amplifiers Vin | 5Vin (J1-21), with the IN-OUT pads bridged |

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

## Wiring as built

| Part | Connection |
|---|---|
| Key socket (3.5 mm stereo) | tip GPIO 4, ring GPIO 5, sleeve GND |
| Speaker amplifier (MAX98357A) | BCLK GPIO 15, LRC GPIO 16, DIN GPIO 17, Vin 5V, GND |
| Headphone amplifier (MAX98357A) | BCLK GPIO 12, LRC GPIO 13, DIN GPIO 14, Vin 5V, GND, GAIN left open |
| Headphone socket | amplifier + through 220 Ω to tip and ring joined; amplifier − to the sleeve, which must not touch GND |

Both amplifiers run from the board's 5V pin. On 3.3 V the headphone amplifier distorted at higher volume.
On some clone boards the 5V pin only carries USB power once the IN-OUT pads are bridged.

**Tailscale relay region.** The box's Tailscale client keeps one relay connection, to its own home region.
It has to be the home region of the computer by the radio; with different regions the box cannot reach the
receiver until the receiver happens to contact the box. Left on *Automatic*, the box reads the receiver's
region from the tailnet and moves there, restarting its Tailscale connection once. The region can also be
chosen by hand under Connection → Tailscale → Relay. `tailscale netcheck` on the receiver's computer shows
its region. Choosing the region needed a change to MicroLink, so the firmware carries its own copy in
`firmware/components/microlink` (see `FJARRNYCKEL.md` there).

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

## Backups

Box → Backup downloads one file (`.fnb`) with everything the box knows: Wi-Fi, the receiver and its key,
the Tailscale identity, the page password and every setting. Restoring it, also from the first-time setup
page after a reset, brings the box back as it was without adding it to Tailscale again. (If the box's
Tailscale key has expired in the meantime, it asks for a sign-in as usual.) Use one backup in one box at a
time: two boxes with the same Tailscale identity push each other off the tailnet.

The file is sealed with a passphrase you choose (PBKDF2-HMAC-SHA256, 100 000 rounds, then AES-256-GCM; see
`firmware/src/backup.h`), so it can be kept anywhere, but a weak passphrase is the weak point. Without a page
password anyone on the box's network can make a backup with their own passphrase, so set one if others
share that network. `tools/backup.py show <file>` opens a backup on a computer and lists what is in it.

A release is made by setting the version in `firmware/CMakeLists.txt` and pushing a tag `v<version>`.
GitHub Actions builds the firmware and publishes `fjarrnyckel-update.bin` (for the box's page),
`fjarrnyckel-usb.bin` (a full image for a new board, written at address 0) and `version.txt`.

## Tests

```
python3 -m unittest discover -s tests
```

Keyer logic and backup files are also tested in C on the computer:

```
gcc -Wall -Ifirmware/src -o /tmp/iambic_test tests/host/iambic_test.c firmware/src/iambic.c && /tmp/iambic_test
TFPSA=<built TF-PSA-Crypto checkout> tests/host/run_backup_test.sh
```

## Licence

MIT. See [LICENSE](LICENSE). The designs in `enclosure/` and `vfo/` are under the same licence.

## Thanks

The rules of the keying link are modelled on OK1HRA's Paddle2IP. The packet
format and the code are our own.

73 de SA3LEJ
