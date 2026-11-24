#!/bin/sh
# Builds and runs the backup test. TFPSA = a built TF-PSA-Crypto checkout (github.com/Mbed-TLS/TF-PSA-Crypto).
set -e
: "${TFPSA:?set TFPSA to a built TF-PSA-Crypto checkout}"
cd "$(dirname "$0")/../.."
T=$(mktemp -d)
cc -Wall -O2 -Ifirmware/src -I"$TFPSA/include" -I"$TFPSA/build/include" -I"$TFPSA/drivers/builtin/include" \
   -o "$T/backup_test" tests/host/backup_test.c firmware/src/backup.c "$TFPSA/build/core/libtfpsacrypto.a"
"$T/backup_test" self
# The computer tool and the firmware code must read each other's files.
"$T/backup_test" seal "fönster" "$T/c.fnb"
FN_BACKUP_PASS="fönster" python3 -c "import sys; sys.path.insert(0, 'tools'); import backup; print(backup.open_('fönster', open('$T/c.fnb','rb').read()).rstrip(b'\0').decode())" | grep -q "backup test text" && echo "ok    the computer tool opens a file sealed by the firmware code"
python3 -c "import sys; sys.path.insert(0, 'tools'); import backup; open('$T/p.fnb','wb').write(backup.seal('fönster', b'from python'))"
"$T/backup_test" open "fönster" "$T/p.fnb" | grep -q "from python" && echo "ok    the firmware code opens a file sealed by the computer tool"
rm -rf "$T"
# tools/backup.py must know the settings layout as the firmware has it (append-only; x86 and Xtensa align alike).
awk '/^typedef struct \{/{f=1} f{print} /\} settings_t;/{exit}' firmware/src/fn.h > "$T.h"
printf '#include <stdio.h>\n#include <stdint.h>\n#include <stddef.h>\n#include "%s"\n#define F(x) printf("%%s %%zu\\n", #x, offsetof(settings_t, x));\nint main(void){F(ssid)F(pass)F(key_type)F(swap)F(wpm)F(tone_hz)F(volume)F(reserved)F(link_host)F(link_port)F(link_key)F(iambic_b)F(ts_on)F(ts_key)F(page_lock)F(page_salt)F(page_hash)F(led_off)F(tones_off)F(key_target)F(usb_raw)F(hp_volume)F(ts_region)F(ts_region_auto)F(radio_volume)printf("size %%zu\\n", sizeof(settings_t));}\n' "$T.h" > "$T.c"
cc -o "$T.layout" "$T.c" && "$T.layout" > "$T.c.txt"
python3 -c "
import sys, struct; sys.path.insert(0, 'tools'); import backup
at, mine = 0, []
for n, f, _ in backup.FIELDS:
    if n: mine.append(f'{n} {at}')
    at += struct.calcsize('<' + f)
mine.append(f'size {at}')
theirs = open('$T.c.txt').read().split(chr(10))[:-1]
assert mine == theirs, (mine, theirs)
print('ok    tools/backup.py reads the settings as the firmware lays them out')"
rm -f "$T.h" "$T.c" "$T.layout" "$T.c.txt"
