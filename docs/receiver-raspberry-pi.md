# Installing the receiver on a Raspberry Pi

The receiver is the half of Fjärrnyckel that sits by the radio. It listens for
your key movements from the Fjärrnyckel box and keys the radio over its USB
cable. This guide is for the common remote setup: an Icom radio on a
Raspberry Pi where **wfview already holds the radio's serial port**.

Two programs cannot normally share a serial port, and opening the port a second
time would key the transmitter. The receiver therefore does not open the port.
It borrows the connection wfview already has and only moves the keying line on
it. For that to be allowed, **the receiver must run as the same user as
wfview** (not as root).

## What you need

- A Raspberry Pi with Raspberry Pi OS or Debian. Developed on a Pi 4 with
  Debian 13 and Python 3.13. Linux 5.6 or later and Python 3.9 or later are
  required.
- An Icom radio on USB that wfview controls. Tested with the IC-7300; see
  [Radios](#radios) for the others.
- wfview running on the Pi, connected to the radio, with **Enable RigCtld**
  switched on (Settings → External Control, port `4533`).
- A way for the box to reach the Pi: the same network, Tailscale, or a public
  IPv6 address. The receiver listens on UDP port `7373`.

## 1. Get the files

Log in as the user that runs wfview.

```
sudo apt install python3-serial git
git clone https://github.com/sa3lej/fjarrnyckel.git ~/fjarrnyckel
```

## 2. Make the shared key

The box and the receiver sign every packet with the same secret key. Make one:

```
mkdir -p ~/.config/fjarrnyckel
head -c 32 /dev/urandom | base64 > ~/.config/fjarrnyckel/cw.key
chmod 600 ~/.config/fjarrnyckel/cw.key
cat ~/.config/fjarrnyckel/cw.key
```

Keep the line it prints. You enter the same text in the box's setup page.
Anyone with this key can key your transmitter, so treat it like a password.

## 3. Find the radio's serial port

```
ls -l /dev/serial/by-id/
```

Use the full `/dev/serial/by-id/...` name of the radio. Unlike `/dev/ttyUSB0`
it stays the same when other USB devices are plugged in. It must be the same
port wfview uses.

## 4. Try it by hand

With wfview running:

```
python3 ~/fjarrnyckel/receiver/cwrecv.py \
    --key ~/.config/fjarrnyckel/cw.key \
    --serial /dev/serial/by-id/YOUR-RADIO \
    --wfview
```

It prints a line with `"event": "listening"` and
`"output": "wfview-dtr:..."`. Stop it with Ctrl-C.

## 5. Run it as a service

```
mkdir -p ~/.config/systemd/user
cp ~/fjarrnyckel/deploy/fjarrnyckel-receiver.service ~/.config/systemd/user/
nano ~/.config/systemd/user/fjarrnyckel-receiver.service    # set the serial port
systemctl --user daemon-reload
systemctl --user enable --now fjarrnyckel-receiver
sudo loginctl enable-linger "$USER"      # start at boot, without logging in
```

Check it with `systemctl --user status fjarrnyckel-receiver` and read its log
with `journalctl --user -u fjarrnyckel-receiver -f`.

## How keying is switched on and off

**CW mode is the switch.** While the radio is in CW (or CW-R), the receiver sets
the radio's *USB Keying (CW)* menu to DTR and accepts keying. When you leave CW,
or the receiver stops, it sets the menu back to OFF. In any other mode the
receiver refuses to key.

You select CW in wfview, or from the box: its page has buttons for the radio's
mode (CW, USB, LSB), frequency and band, and one that starts the antenna tuner.
The receiver passes mode and frequency to wfview and changes nothing else; the
filter stays as the radio had it. The tuner is started with a CI-V command on
the radio port, and the radio transmits while it tunes. Start the receiver with
`--no-radio-control` if it should do keying only, with wfview as the only way
to change the radio.

## Radios

**So far only the IC-7300 has been tested.** The other radios below are expected to
work from Icom's documentation, but nobody has tried them yet.

The receiver asks wfview which radio it is connected to, and its CI-V address.
It keys by switching DTR on the radio's USB serial port, the one wfview has open.

| Radio | *USB Keying (CW)* | Tested |
|---|---|---|
| IC-7300 | switched by the receiver (menu `1A 05 00 79`) | yes |
| IC-7300MK2 | switched by the receiver (`00 97`) | no |
| IC-705 | switched by the receiver (`01 26`) | no |
| IC-9700 | switched by the receiver (`01 21`) | no |
| IC-7610 | switched by the receiver (`00 96`) | no |
| IC-905 | switched by the receiver (`01 37`) | no |
| other Icoms with a USB port | set it to **DTR** on the radio yourself | no |

The menu numbers are from Icom's CI-V reference guide for each radio. The radios
from the IC-7300MK2 down have two virtual serial ports on their USB connection,
(A) and (B); the receiver picks *USB (A) DTR* or *USB (B) DTR* to match the port
wfview uses. If it cannot tell which port that is, it leaves the menu alone and
says so in its log; start it with `--usb-port A` or `--usb-port B`. If the radio's
CI-V address is not its default, add `--civ-address` with the address in hex.

On a radio whose menu the receiver does not switch, it still refuses to key
outside CW mode; set *USB Keying (CW)* to DTR once and leave it there.

Radios without a USB port (IC-706, IC-718, IC-746, IC-756) and the Kenwood and
Yaesu radios wfview supports are not covered: they key differently. If you try a
radio on this list, please tell us how it went.

The receiver also lets go of the key by itself when:

- packets from the box stop while the key is down,
- one key-down lasts longer than five seconds,
- wfview exits or restarts (keying resumes when wfview has the port again).

## What the log tells

`journalctl --user -u fjarrnyckel-receiver` shows one line when the receiver starts (`"event": "listening"`,
with its version), one whenever it lets go of the key by itself (`"released"`, with the reason), and one a
minute after each stretch of keying (`"keying"`): how many key movements were played, how far the length of
each mark was from the original (`mark_length_error_ms`) and how far from its planned moment each movement was
played (`actuation_error_ms`), as median, 99th percentile and largest.

## Test without transmitting

Put the radio in CW with **break-in (BK-IN) off**. The radio then only sounds
its sidetone when keyed and does not transmit. Send a test from another
computer that has the same key file:

```
python3 receiver/cwsend.py --key cw.key --host ADDRESS-OF-THE-PI --text "PARIS PARIS"
```

You should hear `PARIS PARIS` in the radio's sidetone, and the sender prints a
summary where `late` and `lost` are `0`.

With break-in on, the same keying transmits. Use a dummy load or a clear
frequency, and follow your licence.

## If it does not work

- **`"output": "none"`**: `--serial` was left out. Nothing is keyed.
- **Nothing is keyed in CW**: check that wfview's RigCtld is enabled on port
  4533, and that the receiver and wfview run as the same user.
- **Permission error when borrowing the port**: some systems restrict one
  program from reaching into another, even for the same user. Check with
  `cat /proc/sys/kernel/yama/ptrace_scope`. If the file does not exist, or it
  prints `0`, there is no restriction (this is the case on Raspberry Pi OS).
  Ubuntu sets it to `1`, which blocks the receiver.
- **wfview runs under another program name**: the receiver looks for a process
  called `wfview`. The server-only `wfserver` is not supported yet.
- **No answer from the receiver**: check that UDP port 7373 is open between the
  box and the Pi.
