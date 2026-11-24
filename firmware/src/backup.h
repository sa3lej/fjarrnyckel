// Backup files: a passphrase-sealed copy of everything the box knows. Kept free of ESP-IDF so it can be
// tested on a computer (tests/host/backup_test.c) and read by tools/backup.py.
//
// File, format 1:  "FNBACKUP" | format (1) | 3 zero bytes | salt (16) | nonce (12) | AES-256-GCM ciphertext | tag (16)
// The 40-byte header is authenticated as associated data. The AES key is PBKDF2-HMAC-SHA256(passphrase, salt,
// 100000 rounds); format 1 fixes the round count, so a damaged header cannot make the box count for ever.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define BACKUP_HEAD 40
#define BACKUP_TAG 16
#define BACKUP_OVERHEAD (BACKUP_HEAD + BACKUP_TAG)
#define BACKUP_ROUNDS 100000

// Seals plain into out. Returns the file length, or -1.
int backup_seal(const char *pass, const uint8_t *plain, size_t len, uint8_t *out, size_t size);
// Opens a file into plain. Returns the plain length, -1 when it is not a backup this firmware knows,
// -2 when the passphrase is wrong or the file was changed.
int backup_open(const char *pass, const uint8_t *file, size_t len, uint8_t *plain, size_t size);
// Called now and then during the key derivation, so other tasks get to run. The box supplies it.
void backup_yield(void);
