/*
 * Copyright 2011-2013 Con Kolivas
 * Copyright 2012-2013 Luke Dashjr
 * Copyright 2011 Nils Schneider
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>
#include <string.h>

#include "findnonce.h"
#include "miner.h"

#define rotr(x,y) ((x>>y) | (x<<(sizeof(x)*8-y)))

#ifdef USE_SHA256D
const uint32_t SHA256_K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define rotate(x,y) ((x<<y) | (x>>(sizeof(x)*8-y)))

#define R(a, b, c, d, e, f, g, h, w, k) \
	h = h + (rotate(e, 26) ^ rotate(e, 21) ^ rotate(e, 7)) + (g ^ (e & (f ^ g))) + k + w; \
	d = d + h; \
	h = h + (rotate(a, 30) ^ rotate(a, 19) ^ rotate(a, 10)) + ((a & b) | (c & (a | b)))

void precalc_hash(struct opencl_work_data *blk, uint32_t *state, uint32_t *data)
{
	cl_uint A, B, C, D, E, F, G, H;

	A = state[0];
	B = state[1];
	C = state[2];
	D = state[3];
	E = state[4];
	F = state[5];
	G = state[6];
	H = state[7];

	R(A, B, C, D, E, F, G, H, data[0], SHA256_K[0]);
	R(H, A, B, C, D, E, F, G, data[1], SHA256_K[1]);
	R(G, H, A, B, C, D, E, F, data[2], SHA256_K[2]);

	blk->cty_a = A;
	blk->cty_b = B;
	blk->cty_c = C;
	blk->cty_d = D;

	blk->D1A = D + 0xb956c25b;

	blk->cty_e = E;
	blk->cty_f = F;
	blk->cty_g = G;
	blk->cty_h = H;

	blk->ctx_a = state[0];
	blk->ctx_b = state[1];
	blk->ctx_c = state[2];
	blk->ctx_d = state[3];
	blk->ctx_e = state[4];
	blk->ctx_f = state[5];
	blk->ctx_g = state[6];
	blk->ctx_h = state[7];

	blk->merkle = data[0];
	blk->ntime = data[1];
	blk->nbits = data[2];

	blk->W16 = blk->fW0 = data[0] + (rotr(data[1], 7) ^ rotr(data[1], 18) ^ (data[1] >> 3));
	blk->W17 = blk->fW1 = data[1] + (rotr(data[2], 7) ^ rotr(data[2], 18) ^ (data[2] >> 3)) + 0x2d00001; // rotr(0x00000280, 17) ^ rotr(0x00000280, 19) ^ 0x00000280 >> 10
	blk->PreVal4 = blk->fcty_e = blk->ctx_e + (rotr(B, 6) ^ rotr(B, 11) ^ rotr(B, 25)) + (D ^ (B & (C ^ D))) + 0xe9b5dba5; // K[4]
	blk->T1 = blk->fcty_e2 = (rotr(F, 2) ^ rotr(F, 13) ^ rotr(F, 22)) + ((F & G) | (H & (F | G)));
	blk->PreVal4_2 = blk->PreVal4 + blk->T1;
	blk->PreVal0 = blk->PreVal4 + blk->ctx_a;
	blk->PreW31 = 0x00000480 + (rotr(blk->W16,  7) ^ rotr(blk->W16, 18) ^ (blk->W16 >> 3));
	blk->PreW32 = blk->W16 + (rotr(blk->W17, 7) ^ rotr(blk->W17, 18) ^ (blk->W17 >> 3));
	blk->PreW18 = data[2] + (rotr(blk->W16, 17) ^ rotr(blk->W16, 19) ^ (blk->W16 >> 10));
	blk->PreW19 = 0x11002000 + (rotr(blk->W17, 17) ^ rotr(blk->W17, 19) ^ (blk->W17 >> 10));


	blk->W2 = data[2];

	blk->W2A = blk->W2 + (rotr(blk->W16, 19) ^ rotr(blk->W16, 17) ^ (blk->W16 >> 10));
	blk->W17_2 = 0x11002000 + (rotr(blk->W17, 19) ^ rotr(blk->W17, 17) ^ (blk->W17 >> 10));

	blk->fW2 = data[2] + (rotr(blk->fW0, 17) ^ rotr(blk->fW0, 19) ^ (blk->fW0 >> 10));
	blk->fW3 = 0x11002000 + (rotr(blk->fW1, 17) ^ rotr(blk->fW1, 19) ^ (blk->fW1 >> 10));
	blk->fW15 = 0x00000480 + (rotr(blk->fW0, 7) ^ rotr(blk->fW0, 18) ^ (blk->fW0 >> 3));
	blk->fW01r = blk->fW0 + (rotr(blk->fW1, 7) ^ rotr(blk->fW1, 18) ^ (blk->fW1 >> 3));


	blk->PreVal4addT1 = blk->PreVal4 + blk->T1;
	blk->T1substate0 = blk->ctx_a - blk->T1;

	blk->C1addK5 = blk->cty_c + SHA256_K[5];
	blk->B1addK6 = blk->cty_b + SHA256_K[6];
	blk->PreVal0addK7 = blk->PreVal0 + SHA256_K[7];
	blk->W16addK16 = blk->W16 + SHA256_K[16];
	blk->W17addK17 = blk->W17 + SHA256_K[17];

	blk->zeroA = blk->ctx_a + 0x98c7e2a2;
	blk->zeroB = blk->ctx_a + 0xfc08884d;
	blk->oneA = blk->ctx_b + 0x90bb1e3c;
	blk->twoA = blk->ctx_c + 0x50c6645b;
	blk->threeA = blk->ctx_d + 0x3ac42e24;
	blk->fourA = blk->ctx_e + SHA256_K[4];
	blk->fiveA = blk->ctx_f + SHA256_K[5];
	blk->sixA = blk->ctx_g + SHA256_K[6];
	blk->sevenA = blk->ctx_h + SHA256_K[7];
}
#endif

#define SHA512_256_R(a, b, c, d, e, f, g, h, w, k) \
	h = h + (rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41)) + (g ^ (e & (f ^ g))) + k + w; \
	d = d + h; \
	h = h + (rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39)) + ((a & b) | (c & (a | b)))

#define sha512_S0(x) (rotr(x, 28) ^ rotr(x, 34) ^ rotr(x, 39))
#define sha512_S1(x) (rotr(x, 14) ^ rotr(x, 18) ^ rotr(x, 41))
#define sha512_s0(x) (rotr(x,  1) ^ rotr(x,  8) ^ (x >> 7))
#define sha512_s1(x) (rotr(x, 19) ^ rotr(x, 61) ^ (x >>  6))
#define CH(x, y, z)  ((x & y) ^ (~x & z))
#define MAJ(x, y, z) ((x & y) ^ (x & z) ^ (y & z))

const uint64_t SHA512_256_K[80] = {
	0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
	0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
	0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
	0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
	0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
	0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
	0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
	0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
	0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
	0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
	0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
	0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
	0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
	0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
	0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
	0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
	0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
	0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
	0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
	0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
	0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
	0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
	0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
	0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
	0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
	0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
	0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
	0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
	0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
	0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
	0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
	0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
	0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
	0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

uint64_t SHA512_256_H[8] = {
	0x22312194FC2BF72CULL, 0x9F555FA3C84C64C2ULL,
	0x2393B86B6F53B151ULL, 0x963877195940EABDULL,
	0x96283EE2A88EFFE3ULL, 0xBE5E1E2553863992ULL,
	0x2B0199FC2C85B8AAULL, 0x0EB72DDC81C52CA2ULL
};

void precalc_sha512_256(struct rad_work_data *blk, uint64_t *data)
{
	blk->w0 = data[0] << 32 | data[0] >> 32;
	blk->w1 = data[1] << 32 | data[1] >> 32;
	blk->w2 = data[2] << 32 | data[2] >> 32;
	blk->w3 = data[3] << 32 | data[3] >> 32;
	blk->w4 = data[4] << 32 | data[4] >> 32;
	blk->w5 = data[5] << 32 | data[5] >> 32;
	blk->w6 = data[6] << 32 | data[6] >> 32;
	blk->w7 = data[7] << 32 | data[7] >> 32;
	blk->w8 = data[8] << 32 | data[8] >> 32;
	blk->w9 = data[9] << 32 | data[9] >> 32;

	cl_ulong A, B, C, D, E, F, G, H;

	A = SHA512_256_H[0];
	B = SHA512_256_H[1];
	C = SHA512_256_H[2];
	D = SHA512_256_H[3];
	E = SHA512_256_H[4];
	F = SHA512_256_H[5];
	G = SHA512_256_H[6];
	H = SHA512_256_H[7];

	SHA512_256_R(A, B, C, D, E, F, G, H, blk->w0, SHA512_256_K[0]);
	SHA512_256_R(H, A, B, C, D, E, F, G, blk->w1, SHA512_256_K[1]);
	SHA512_256_R(G, H, A, B, C, D, E, F, blk->w2, SHA512_256_K[2]);
	SHA512_256_R(F, G, H, A, B, C, D, E, blk->w3, SHA512_256_K[3]);
	SHA512_256_R(E, F, G, H, A, B, C, D, blk->w4, SHA512_256_K[4]);
	SHA512_256_R(D, E, F, G, H, A, B, C, blk->w5, SHA512_256_K[5]);
	SHA512_256_R(C, D, E, F, G, H, A, B, blk->w6, SHA512_256_K[6]);
	SHA512_256_R(B, C, D, E, F, G, H, A, blk->w7, SHA512_256_K[7]);
	SHA512_256_R(A, B, C, D, E, F, G, H, blk->w8, SHA512_256_K[8]);

	G += sha512_S1(D) + SHA512_256_K[9] + CH(D, E, F);
	C += G;
	G += sha512_S0(H) + MAJ(B, H, A);
	F += 0x8000000000000000ULL + SHA512_256_K[10];

	blk->ctx_a = A;
	blk->ctx_b = B;
	blk->ctx_c = C;
	blk->ctx_d = D;
	blk->ctx_e = E;
	blk->ctx_f = F;
	blk->ctx_g = G;
	blk->ctx_h = H;
}

struct pc_data {
	struct thr_info *thr;
	struct work work;
	uint32_t res[OPENCL_MAX_BUFFERSIZE];
	pthread_t pth;
	int found;
	enum cl_kernels kinterface;
};

static void *postcalc_hash(void *userdata)
{
	struct pc_data *pcd = (struct pc_data *)userdata;
	struct thr_info *thr = pcd->thr;
	unsigned int entry = 0;
	int found = FOUND;
#ifdef USE_SCRYPT
	if (pcd->kinterface == KL_SCRYPT)
		found = SCRYPT_FOUND;
#endif

	pthread_detach(pthread_self());
	RenameThread("postcalchsh");

	/* To prevent corrupt values in FOUND from trying to read beyond the
	 * end of the res[] array */
	if (unlikely(pcd->res[found] & ~found)) {
		applog(LOG_WARNING, "%"PRIpreprv": invalid nonce count - HW error",
				thr->cgpu->proc_repr);
		inc_hw_errors_only(thr);
		pcd->res[found] &= found;
	}

	for (entry = 0; entry < pcd->res[found]; entry++) {
		uint32_t nonce = pcd->res[entry];
#ifdef USE_OPENCL_FULLHEADER
		if (pcd->kinterface == KL_FULLHEADER)
			nonce = swab32(nonce);
#endif

		applog(LOG_DEBUG, "OCL NONCE %u found in slot %d", nonce, entry);
		submit_nonce(thr, &pcd->work, nonce);
	}

	clean_work(&pcd->work);
	free(pcd);

	return NULL;
}

void postcalc_hash_async(struct thr_info * const thr, struct work * const work, uint32_t * const res, const enum cl_kernels kinterface)
{
	struct pc_data *pcd = malloc(sizeof(struct pc_data));
	int buffersize;

	if (unlikely(!pcd)) {
		applog(LOG_ERR, "Failed to malloc pc_data in postcalc_hash_async");
		return;
	}

	*pcd = (struct pc_data){
		.thr = thr,
		.kinterface = kinterface,
	};
	__copy_work(&pcd->work, work);
#ifdef USE_SCRYPT
	if (kinterface == KL_SCRYPT)
		buffersize = SCRYPT_BUFFERSIZE;
	else
#endif
		buffersize = BUFFERSIZE;
	memcpy(&pcd->res, res, buffersize);

	if (pthread_create(&pcd->pth, NULL, postcalc_hash, (void *)pcd)) {
		applog(LOG_ERR, "Failed to create postcalc_hash thread");
		return;
	}
}
