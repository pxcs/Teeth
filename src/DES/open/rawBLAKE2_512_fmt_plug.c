/*
 * This file is part of John the Ripper password cracker,
 * Copyright (c) 2012 by Solar Designer
 * based on rawMD4_fmt.c code, with trivial changes by groszek.
 *
 * Re-used for BLAKE2 by Dhiru Kholia (dhiru at openwall.com).
 */

#if FMT_EXTERNS_H
extern struct fmt_main fmt_rawBLAKE2;
#elif FMT_REGISTERS_H
john_register_one(&fmt_rawBLAKE2);
#else

#include <string.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "arch.h"
#include "blake2.h"
#include "params.h"
#include "common.h"
#include "formats.h"

#define FORMAT_LABEL            "Raw-Blake2"
#define FORMAT_NAME             ""
#if !defined(JOHN_NO_SIMD) && defined(__AVX__)
#define ALGORITHM_NAME          "128/128 AVX"
#elif !defined(JOHN_NO_SIMD) && defined(__XOP__)
#define ALGORITHM_NAME          "128/128 XOP"
#elif !defined(JOHN_NO_SIMD) && defined(__SSE4_1__)
#define ALGORITHM_NAME          "128/128 SSE4.1"
#elif !defined(JOHN_NO_SIMD) && defined(__SSSE3__)
#define ALGORITHM_NAME          "128/128 SSSE3"
#elif !defined(JOHN_NO_SIMD) && defined(__SSE2__)
#define ALGORITHM_NAME          "128/128 SSE2"
#else
#define ALGORITHM_NAME          "32/" ARCH_BITS_STR
#endif
#define FORMAT_TAG              "$BLAKE2$"
#define FORMAT_TAG_LEN          (sizeof(FORMAT_TAG)-1)
#define BENCHMARK_COMMENT       ""
#define BENCHMARK_LENGTH        0x107
#define PLAINTEXT_LENGTH        125
#define CIPHERTEXT_LENGTH       128
#define BINARY_SIZE             64
#define SALT_SIZE               0
#define BINARY_ALIGN            4
#define SALT_ALIGN              1
#define MIN_KEYS_PER_CRYPT      1
#define MAX_KEYS_PER_CRYPT      64

#ifndef OMP_SCALE
#define OMP_SCALE               512
#endif

static struct fmt_tests tests[] = {
	{"4245af08b46fbb290222ab8a68613621d92ce78577152d712467742417ebc1153668f1c9e1ec1e152a32a9c242dc686d175e087906377f0c483c5be2cb68953e", "blake2"},
	{"$BLAKE2$021ced8799296ceca557832ab941a50b4a11f83478cf141f51f933f653ab9fbcc05a037cddbed06e309bf334942c4e58cdf1a46e237911ccd7fcf9787cbc7fd0", "hello world"},
	/* hash generated by multiple versions (in C and Go) of b2sum program */
	{"$BLAKE2$1f7d9b7c9a90f7bfc66e52b69f3b6c3befbd6aee11aac860e99347a495526f30c9e51f6b0db01c24825092a09dd1a15740f0ade8def87e60c15da487571bcef7", "verystrongandlongpassword"},
	/* test vectors from Wikipedia */
	{"$BLAKE2$a8add4bdddfd93e4877d2746e62817b116364a1fa7bc148d95090bc7333b3673f82401cf7aa2e4cb1ecd90296e3f14cb5413f8ed77be73045b13914cdcd6a918", "The quick brown fox jumps over the lazy dog"},
	{"$BLAKE2$786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce", ""},
	{"$BLAKE2$da40d8f48e9e7560c56e2b92205aed6342a276994ca0287ea4f8c1423ef07d519ecb4bf8668c118379a36be8aa6c077bbc6213fa81fbb332fad9d8a19a7756e6", "UPPERCASE"},
	{"$BLAKE2$f5ab8bafa6f2f72b431188ac38ae2de7bb618fb3d38b6cbf639defcdd5e10a86b22fccff571da37e42b23b80b657ee4d936478f582280a87d6dbb1da73f5c47d", "123456789"},
	{NULL}
};

static int (*saved_len);
static char (*saved_key)[PLAINTEXT_LENGTH + 1];
static uint32_t (*crypt_out)
    [(BINARY_SIZE + sizeof(uint32_t) - 1) / sizeof(uint32_t)];

static void init(struct fmt_main *self)
{
	omp_autotune(self, OMP_SCALE);

	saved_len = mem_calloc(self->params.max_keys_per_crypt,
	                       sizeof(*saved_len));
	saved_key = mem_calloc(self->params.max_keys_per_crypt,
	                       sizeof(*saved_key));
	crypt_out = mem_calloc(self->params.max_keys_per_crypt,
	                       sizeof(*crypt_out));
}

static void done(void)
{
	MEM_FREE(crypt_out);
	MEM_FREE(saved_key);
	MEM_FREE(saved_len);
}

static int valid(char *ciphertext, struct fmt_main *self)
{
	char *p, *q;

	p = ciphertext;
	if (!strncmp(p, FORMAT_TAG, FORMAT_TAG_LEN))
		p += FORMAT_TAG_LEN;

	q = p;
	while (atoi16[ARCH_INDEX(*q)] != 0x7F)
		q++;
	return !*q && q - p == CIPHERTEXT_LENGTH;
}

static char *split(char *ciphertext, int index, struct fmt_main *pFmt)
{
	static char out[FORMAT_TAG_LEN + CIPHERTEXT_LENGTH + 1];

	if (!strncmp(ciphertext, FORMAT_TAG, FORMAT_TAG_LEN))
		ciphertext += FORMAT_TAG_LEN;

	memcpy(out, FORMAT_TAG, FORMAT_TAG_LEN);
	memcpylwr(out + FORMAT_TAG_LEN, ciphertext, CIPHERTEXT_LENGTH + 1);
	return out;
}

static void *get_binary(char *ciphertext)
{
	static unsigned char *out;
	char *p;
	int i;

	if (!out) out = mem_alloc_tiny(BINARY_SIZE, MEM_ALIGN_WORD);

	p = ciphertext + FORMAT_TAG_LEN;
	for (i = 0; i < BINARY_SIZE; i++) {
		out[i] =
		    (atoi16[ARCH_INDEX(*p)] << 4) |
		    atoi16[ARCH_INDEX(p[1])];
		p += 2;
	}

	return out;
}

#define COMMON_GET_HASH_VAR crypt_out
#include "common-get-hash.h"

static void set_key(char *key, int index)
{
	saved_len[index] = strnzcpyn(saved_key[index], key, sizeof(*saved_key));
}

static char *get_key(int index)
{
	saved_key[index][saved_len[index]] = 0;
	return saved_key[index];
}

static int crypt_all(int *pcount, struct db_salt *salt)
{
	const int count = *pcount;
	int index;

#ifdef _OPENMP
#pragma omp parallel for
#endif
	for (index = 0; index < count; index++) {
		(void)blake2b((uint8_t *)crypt_out[index], saved_key[index], NULL, 64, saved_len[index], 0);
	}

	return count;
}

static int cmp_all(void *binary, int count)
{
	int index;

	for (index = 0; index < count; index++)
		if (!memcmp(binary, crypt_out[index], ARCH_SIZE))
			return 1;
	return 0;
}

static int cmp_one(void *binary, int index)
{
	return !memcmp(binary, crypt_out[index], BINARY_SIZE);
}

static int cmp_exact(char *source, int index)
{
	return 1;
}

struct fmt_main fmt_rawBLAKE2 = {
	{
		FORMAT_LABEL,
		FORMAT_NAME,
		"BLAKE2b 512 " ALGORITHM_NAME,
		BENCHMARK_COMMENT,
		BENCHMARK_LENGTH,
		0,
		PLAINTEXT_LENGTH,
		BINARY_SIZE,
		BINARY_ALIGN,
		SALT_SIZE,
		SALT_ALIGN,
		MIN_KEYS_PER_CRYPT,
		MAX_KEYS_PER_CRYPT,
		FMT_OMP | FMT_CASE | FMT_8_BIT | FMT_SPLIT_UNIFIES_CASE,
		{ NULL },
		{ FORMAT_TAG },
		tests
	}, {
		init,
		done,
		fmt_default_reset,
		fmt_default_prepare,
		valid,
		split,
		get_binary,
		fmt_default_salt,
		{ NULL },
		fmt_default_source,
		{
			fmt_default_binary_hash_0,
			fmt_default_binary_hash_1,
			fmt_default_binary_hash_2,
			fmt_default_binary_hash_3,
			fmt_default_binary_hash_4,
			fmt_default_binary_hash_5,
			fmt_default_binary_hash_6
		},
		fmt_default_salt_hash,
		NULL,
		fmt_default_set_salt,
		set_key,
		get_key,
		fmt_default_clear_keys,
		crypt_all,
		{
#define COMMON_GET_HASH_LINK
#include "common-get-hash.h"
		},
		cmp_all,
		cmp_one,
		cmp_exact
	}
};

#endif /* plugin stanza */
