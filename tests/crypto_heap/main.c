// SPDX-License-Identifier: Apache-2.0
#include <psa/crypto.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

/* Pinned TF-PSA test_suite_aes.ecb.data, AES-128-ECB NIST KAT #1. */
static const uint8_t aes_key[16];
static const uint8_t aes_plaintext[16] = {
	0xf3, 0x44, 0x81, 0xec, 0x3c, 0xc6, 0x27, 0xba,
	0xcd, 0x5d, 0xc3, 0xfb, 0x08, 0xf2, 0x73, 0xe6,
};
static const uint8_t aes_ciphertext[16] = {
	0x03, 0x36, 0x76, 0x3e, 0x96, 0x6d, 0x92, 0x59,
	0x5a, 0x56, 0x7c, 0xc9, 0xce, 0x53, 0x7f, 0x5e,
};

/* Pinned TF-PSA test_suite_psa_crypto.data, "PSA MAC sign: CMAC-AES-128". */
static const uint8_t cmac_key[16] = {
	0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
	0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c,
};
static const uint8_t cmac_message[40] = {
	0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
	0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
	0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
	0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
	0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11,
};
static const uint8_t cmac_tag[16] = {
	0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30,
	0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27,
};

/* Keep the pointers outside the test stack while exhausting the real system heap. */
static void *held[512];

static size_t exhaust_heap(void)
{
	size_t count = 0;

	while (count < ARRAY_SIZE(held)) {
		void *p = k_malloc(1);

		if (p == NULL) {
			break;
		}
		held[count++] = p;
	}
	return count;
}

static void release_heap(size_t count)
{
	while (count > 0) {
		k_free(held[--count]);
		held[count] = NULL;
	}
}

static size_t heap_capacity(void)
{
	size_t count = exhaust_heap();

	release_heap(count);
	zassert_true(count < ARRAY_SIZE(held),
		     "pointer table filled before the heap was exhausted");
	return count;
}

static psa_key_id_t import_aes_key(const uint8_t *bytes, psa_algorithm_t algorithm,
				  psa_key_usage_t usage)
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key = PSA_KEY_ID_NULL;

	psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
	psa_set_key_algorithm(&attributes, algorithm);
	psa_set_key_usage_flags(&attributes, usage);
	psa_status_t status = psa_import_key(&attributes, bytes, 16, &key);

	psa_reset_key_attributes(&attributes);
	zassert_equal(status, PSA_SUCCESS, "AES key import failed: %d", status);
	return key;
}

static void check_cmac(psa_key_id_t key)
{
	uint8_t tag[sizeof(cmac_tag)];
	size_t length = 0;
	psa_status_t status = psa_mac_compute(key, PSA_ALG_CMAC,
		cmac_message, sizeof(cmac_message), tag, sizeof(tag), &length);

	zassert_equal(status, PSA_SUCCESS, "CMAC failed: %d", status);
	zassert_equal(length, sizeof(cmac_tag));
	zassert_mem_equal(tag, cmac_tag, sizeof(tag));
}

ZTEST(crypto_heap, test_cold_aes_cmac_and_exhaustion_recovery)
{
	psa_status_t status = psa_crypto_init();

	zassert_equal(status, PSA_SUCCESS, "PSA init failed: %d", status);
	psa_key_id_t key = import_aes_key(aes_key, PSA_ALG_ECB_NO_PADDING,
					PSA_KEY_USAGE_ENCRYPT);
	uint8_t ciphertext[sizeof(aes_ciphertext)];
	size_t length = 0;

	status = psa_cipher_encrypt(key, PSA_ALG_ECB_NO_PADDING,
		aes_plaintext, sizeof(aes_plaintext), ciphertext,
		sizeof(ciphertext), &length);
	zassert_equal(status, PSA_SUCCESS, "AES block failed: %d", status);
	zassert_equal(length, sizeof(aes_ciphertext));
	zassert_mem_equal(ciphertext, aes_ciphertext, sizeof(ciphertext));
	zassert_equal(psa_destroy_key(key), PSA_SUCCESS);

	/* Exercise cold allocation above before any diagnostic heap manipulation. */
	size_t initial_capacity = heap_capacity();
	zassert_true(initial_capacity > 0, "system heap unavailable after AES");

	key = import_aes_key(cmac_key, PSA_ALG_CMAC,
			     PSA_KEY_USAGE_SIGN_MESSAGE);
	check_cmac(key);

	/* Leave the imported key alive; only temporary crypto allocations may change. */
	size_t before = exhaust_heap();
	bool exhausted = before < ARRAY_SIZE(held);

	if (!exhausted) {
		release_heap(before);
		zassert_true(false, "pointer table filled before the heap was exhausted");
		return;
	}
	zassert_true(before > 0, "system heap was unavailable before exhaustion");

	uint8_t tag[sizeof(cmac_tag)];
	length = 0;
	status = psa_mac_compute(key, PSA_ALG_CMAC,
		cmac_message, sizeof(cmac_message), tag, sizeof(tag), &length);
	release_heap(before);
	zassert_equal(status, PSA_ERROR_INSUFFICIENT_MEMORY,
		      "exhausted CMAC returned %d", status);

	check_cmac(key);
	size_t after = heap_capacity();
	zassert_equal(after, before, "failed/recovered CMAC leaked heap capacity");
	zassert_equal(psa_destroy_key(key), PSA_SUCCESS);
	zassert_equal(heap_capacity(), initial_capacity,
		      "CMAC key lifecycle leaked heap capacity");
}

ZTEST_SUITE(crypto_heap, NULL, NULL, NULL, NULL, NULL);
