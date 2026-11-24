// Host test for backup files: tests/host/run_backup_test.sh builds it against TF-PSA-Crypto.
//   backup_test seal PASS OUT      seals a known text into OUT
//   backup_test open PASS IN       prints the text, or exits 2 (wrong passphrase / changed) or 1 (not a backup)
//   backup_test self               round trip, wrong passphrase, changed byte, changed header, unknown format
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "backup.h"

void backup_yield(void) {}
static const char TEXT[] = "Fjarrnyckel backup test text";
static int fails;
static void check(int ok, const char *what) { printf("%s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

int main(int argc, char **argv) {
  uint8_t file[512], plain[512];
  if (argc == 4 && !strcmp(argv[1], "seal")) {
    int n = backup_seal(argv[2], (const uint8_t *)TEXT, sizeof TEXT, file, sizeof file);
    FILE *f = fopen(argv[3], "wb"); fwrite(file, 1, n, f); fclose(f); return n > 0 ? 0 : 3;
  }
  if (argc == 4 && !strcmp(argv[1], "open")) {
    FILE *f = fopen(argv[3], "rb"); size_t len = fread(file, 1, sizeof file, f); fclose(f);
    int n = backup_open(argv[2], file, len, plain, sizeof plain);
    if (n < 0) return -n;
    fwrite(plain, 1, n, stdout); return 0;
  }
  int n = backup_seal("korv med bröd", (const uint8_t *)TEXT, sizeof TEXT, file, sizeof file);
  check(n == (int)(sizeof TEXT + BACKUP_OVERHEAD), "seal gives header + text + tag");
  check(backup_open("korv med bröd", file, n, plain, sizeof plain) == (int)sizeof TEXT && !memcmp(plain, TEXT, sizeof TEXT), "opens with the passphrase");
  check(backup_open("korv med brod", file, n, plain, sizeof plain) == -2, "wrong passphrase is refused");
  check(backup_open("", file, n, plain, sizeof plain) == -2, "empty passphrase is refused");
  file[n - 20] ^= 1; check(backup_open("korv med bröd", file, n, plain, sizeof plain) == -2, "a changed byte is noticed"); file[n - 20] ^= 1;
  file[10] = 7; check(backup_open("korv med bröd", file, n, plain, sizeof plain) == -2, "a changed header is noticed"); file[10] = 0;
  file[8] = 2; check(backup_open("korv med bröd", file, n, plain, sizeof plain) == -1, "an unknown format is not a backup"); file[8] = 1;
  check(backup_open("korv med bröd", file, 30, plain, sizeof plain) == -1, "a short file is not a backup");
  check(backup_seal("", (const uint8_t *)TEXT, sizeof TEXT, file, sizeof file) == -1, "sealing needs a passphrase");
  uint8_t a[512], b[512];
  int na = backup_seal("x", (const uint8_t *)TEXT, sizeof TEXT, a, sizeof a), nb = backup_seal("x", (const uint8_t *)TEXT, sizeof TEXT, b, sizeof b);
  check(na == nb && memcmp(a, b, na), "two backups of the same thing differ (fresh salt and nonce)");
  return fails ? 1 : 0;
}
