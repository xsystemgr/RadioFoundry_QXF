// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104). What the fleet admin service signs and checks its messages with
// (docs/spec/otap.md): every command carries an HMAC over the fleet's shared secret, so a radio obeys only its own
// fleet. Small and portable, no tables beyond the round constants
#ifndef RFOUNDRY_SHA256_H
#define RFOUNDRY_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_LEN	32
#define SHA256_BLOCK	64

typedef struct {
	uint32_t h[8];
	uint64_t bytes;
	uint8_t buf[SHA256_BLOCK];
	uint8_t fill;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t len);
void sha256_final(sha256_t *s, uint8_t out[SHA256_LEN]);
void sha256(const void *data, size_t len, uint8_t out[SHA256_LEN]);

typedef struct {
	sha256_t inner;
	uint8_t okey[SHA256_BLOCK];
} hmac_sha256_t;

void hmac_sha256_init(hmac_sha256_t *h, const uint8_t *key, size_t key_len);
void hmac_sha256_update(hmac_sha256_t *h, const void *data, size_t len);
void hmac_sha256_final(hmac_sha256_t *h, uint8_t out[SHA256_LEN]);
void hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t len, uint8_t out[SHA256_LEN]);

// Compare in time that doesn't depend on where they differ
int crypto_equal(const uint8_t *a, const uint8_t *b, size_t len);
// Overwrite with zeros in a way the compiler keeps
void crypto_wipe(void *p, size_t len);

#endif
