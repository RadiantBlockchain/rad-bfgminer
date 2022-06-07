// -ck modified kernel taken from Phoenix taken from poclbm, with aspects of
// phatk and others.
// Modified version copyright 2011-2013 Con Kolivas

// This file is taken and modified from the public-domain poclbm project, and
// we have therefore decided to keep it public-domain in Phoenix.

// kernel-interface: rad SHA512_256d

#ifdef VECTORS4
	typedef ulong4 u;
	typedef uint4 u32;
#elif defined VECTORS2
	typedef ulong2 u;
	typedef uint2 u32;
#else
	typedef ulong u;
	typedef uint u32;
#endif

__constant ulong K[80] =
{
	0x428A2F98D728AE22UL, 0x7137449123EF65CDUL,
	0xB5C0FBCFEC4D3B2FUL, 0xE9B5DBA58189DBBCUL,
	0x3956C25BF348B538UL, 0x59F111F1B605D019UL,
	0x923F82A4AF194F9BUL, 0xAB1C5ED5DA6D8118UL,
	0xD807AA98A3030242UL, 0x12835B0145706FBEUL,
	0x243185BE4EE4B28CUL, 0x550C7DC3D5FFB4E2UL,
	0x72BE5D74F27B896FUL, 0x80DEB1FE3B1696B1UL,
	0x9BDC06A725C71235UL, 0xC19BF174CF692694UL,
	0xE49B69C19EF14AD2UL, 0xEFBE4786384F25E3UL,
	0x0FC19DC68B8CD5B5UL, 0x240CA1CC77AC9C65UL,
	0x2DE92C6F592B0275UL, 0x4A7484AA6EA6E483UL,
	0x5CB0A9DCBD41FBD4UL, 0x76F988DA831153B5UL,
	0x983E5152EE66DFABUL, 0xA831C66D2DB43210UL,
	0xB00327C898FB213FUL, 0xBF597FC7BEEF0EE4UL,
	0xC6E00BF33DA88FC2UL, 0xD5A79147930AA725UL,
	0x06CA6351E003826FUL, 0x142929670A0E6E70UL,
	0x27B70A8546D22FFCUL, 0x2E1B21385C26C926UL,
	0x4D2C6DFC5AC42AEDUL, 0x53380D139D95B3DFUL,
	0x650A73548BAF63DEUL, 0x766A0ABB3C77B2A8UL,
	0x81C2C92E47EDAEE6UL, 0x92722C851482353BUL,
	0xA2BFE8A14CF10364UL, 0xA81A664BBC423001UL,
	0xC24B8B70D0F89791UL, 0xC76C51A30654BE30UL,
	0xD192E819D6EF5218UL, 0xD69906245565A910UL,
	0xF40E35855771202AUL, 0x106AA07032BBD1B8UL,
	0x19A4C116B8D2D0C8UL, 0x1E376C085141AB53UL,
	0x2748774CDF8EEB99UL, 0x34B0BCB5E19B48A8UL,
	0x391C0CB3C5C95A63UL, 0x4ED8AA4AE3418ACBUL,
	0x5B9CCA4F7763E373UL, 0x682E6FF3D6B2B8A3UL,
	0x748F82EE5DEFB2FCUL, 0x78A5636F43172F60UL,
	0x84C87814A1F0AB72UL, 0x8CC702081A6439ECUL,
	0x90BEFFFA23631E28UL, 0xA4506CEBDE82BDE9UL,
	0xBEF9A3F7B2C67915UL, 0xC67178F2E372532BUL,
	0xCA273ECEEA26619CUL, 0xD186B8C721C0C207UL,
	0xEADA7DD6CDE0EB1EUL, 0xF57D4F7FEE6ED178UL,
	0x06F067AA72176FBAUL, 0x0A637DC5A2C898A6UL,
	0x113F9804BEF90DAEUL, 0x1B710B35131C471BUL,
	0x28DB77F523047D84UL, 0x32CAAB7B40C72493UL,
	0x3C9EBE0A15C9BEBCUL, 0x431D67C49C100D4CUL,
	0x4CC5D4BECB3E42B6UL, 0x597F299CFC657E2AUL,
	0x5FCB6FAB3AD6FAECUL, 0x6C44198C4A475817UL
};

__constant ulong H[8] =
{
	0x22312194FC2BF72CUL, 0x9F555FA3C84C64C2UL,
	0x2393B86B6F53B151UL, 0x963877195940EABDUL,
	0x96283EE2A88EFFE3UL, 0xBE5E1E2553863992UL,
	0x2B0199FC2C85B8AAUL, 0x0EB72DDC81C52CA2UL
};

// This part is not from the stock poclbm kernel. It's part of an optimization
// added in the Phoenix Miner.

// Some AMD devices have a BFI_INT opcode, which behaves exactly like the
// SHA-256 ch function, but provides it in exactly one instruction. If
// detected, use it for ch. Otherwise, construct ch out of simpler logical
// primitives.

#ifdef BITALIGN
	#pragma OPENCL EXTENSION cl_amd_media_ops : enable
	#define rotr(x, y) amd_bitalign((u)x, (u)x, (u)y)
#else
	#define rotr(x, y) rotate((u)x, (u)(64 - y))
#endif
#ifdef BFI_INT
	// Well, slight problem... It turns out BFI_INT isn't actually exposed to
	// OpenCL (or CAL IL for that matter) in any way. However, there is 
	// a similar instruction, BYTE_ALIGN_INT, which is exposed to OpenCL via
	// amd_bytealign, takes the same inputs, and provides the same output. 
	// We can use that as a placeholder for BFI_INT and have the application 
	// patch it after compilation.
	
	// This is the BFI_INT function
	#define ch(x, y, z) amd_bytealign(x, y, z)
	
	// Ma can also be implemented in terms of BFI_INT...
	#define Ma(x, y, z) amd_bytealign( (z^x), (y), (x) )

	// AMD's KernelAnalyzer throws errors compiling the kernel if we use
	// amd_bytealign on constants with vectors enabled, so we use this to avoid
	// problems. (this is used 4 times, and likely optimized out by the compiler.)
	#define Ma2(x, y, z) bitselect((u)x, (u)y, (u)z ^ (u)x)
#else // BFI_INT
	//GCN actually fails if manually patched with BFI_INT

	#define ch(x, y, z) bitselect((u)z, (u)y, (u)x)
	#define Ma(x, y, z) bitselect((u)x, (u)y, (u)z ^ (u)x)
	#define Ma2(x, y, z) Ma(x, y, z)
#endif


__kernel
__attribute__((vec_type_hint(u)))
__attribute__((reqd_work_group_size(WORKSIZE, 1, 1)))
void search(const u w0, const u w1, const u w2, const u w3, const u w4, const u w5, const u w6, const u w7, const u w8, const u w9,
	const u ctx_a, const u ctx_b, const u ctx_c, const u ctx_d, const u ctx_e, const u ctx_f, const u ctx_g, const u ctx_h,
#ifndef GOFFSET
	const u32 base,
#endif
	volatile __global uint * output)
{
	u Vals[24];
	u *W = &Vals[8];

#ifdef GOFFSET
	const u nonce = (uint)(get_global_id(0));
#else
	const u nonce = base + (uint)(get_global_id(0));
#endif

W[0] = w0;
W[1] = w1;
W[2] = w2;
W[3] = w3;
W[4] = w4;
W[5] = w5;
W[6] = w6;
W[7] = w7;
W[8] = w8;
W[9] = w9 + nonce;
W[10] = 0x8000000000000000UL;
W[11] = 0x0000000000000000UL;
W[12] = 0x0000000000000000UL;
W[13] = 0x0000000000000000UL;
W[14] = 0x0000000000000000UL;
W[15] = 0x0000000000000280UL;

Vals[0] = ctx_d;
Vals[1] = ctx_e;
Vals[2] = ctx_h;
Vals[3] = ctx_g;
Vals[4] = ctx_f;
Vals[5] = ctx_a;
Vals[6] = ctx_c;
Vals[7] = ctx_b;

Vals[3]+=W[9];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[9];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

Vals[4]+=W[10];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[10];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[11];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[12];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[13];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[14];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

Vals[5]+=W[15];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[15];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[0]+=(rotr(W[1],1)^rotr(W[1],8)^(W[1]>>7U));
W[0]+=W[9];
//W[0]+=(rotr(W[14],19)^rotr(W[14],61)^(W[14]>>6U));
Vals[2]+=W[0];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[16];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[1]+=(rotr(W[2],1)^rotr(W[2],8)^(W[2]>>7U));
W[1]+=W[10];
// W[1]+=(rotr(W[15],19)^rotr(W[15],61)^(W[15]>>6U));
W[1] += 0X5000000000140AUL;
Vals[3]+=W[1];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[17];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

W[2]+=(rotr(W[3],1)^rotr(W[3],8)^(W[3]>>7U));
//W[2]+=W[11];
W[2]+=(rotr(W[0],19)^rotr(W[0],61)^(W[0]>>6U));
Vals[4]+=W[2];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[18];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

W[3]+=(rotr(W[4],1)^rotr(W[4],8)^(W[4]>>7U));
//W[3]+=W[12];
W[3]+=(rotr(W[1],19)^rotr(W[1],61)^(W[1]>>6U));
Vals[1]+=W[3];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[19];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

W[4]+=(rotr(W[5],1)^rotr(W[5],8)^(W[5]>>7U));
//W[4]+=W[13];
W[4]+=(rotr(W[2],19)^rotr(W[2],61)^(W[2]>>6U));
Vals[0]+=W[4];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[20];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

W[5]+=(rotr(W[6],1)^rotr(W[6],8)^(W[6]>>7U));
//W[5]+=W[14];
W[5]+=(rotr(W[3],19)^rotr(W[3],61)^(W[3]>>6U));
Vals[6]+=W[5];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[21];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

W[6]+=(rotr(W[7],1)^rotr(W[7],8)^(W[7]>>7U));
W[6]+=W[15];
W[6]+=(rotr(W[4],19)^rotr(W[4],61)^(W[4]>>6U));
Vals[7]+=W[6];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[22];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

W[7]+=(rotr(W[8],1)^rotr(W[8],8)^(W[8]>>7U));
W[7]+=W[0];
W[7]+=(rotr(W[5],19)^rotr(W[5],61)^(W[5]>>6U));
Vals[5]+=W[7];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[23];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[8]+=(rotr(W[9],1)^rotr(W[9],8)^(W[9]>>7U));
W[8]+=W[1];
W[8]+=(rotr(W[6],19)^rotr(W[6],61)^(W[6]>>6U));
Vals[2]+=W[8];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[24];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[9]+=(rotr(W[10],1)^rotr(W[10],8)^(W[10]>>7U));
W[9]+=W[2];
W[9]+=(rotr(W[7],19)^rotr(W[7],61)^(W[7]>>6U));
Vals[3]+=W[9];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[25];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

//W[10]+=(rotr(W[11],1)^rotr(W[11],8)^(W[11]>>7U));
W[10]+=W[3];
W[10]+=(rotr(W[8],19)^rotr(W[8],61)^(W[8]>>6U));
Vals[4]+=W[10];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[26];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

//W[11]+=(rotr(W[12],1)^rotr(W[12],8)^(W[12]>>7U));
W[11]+=W[4];
W[11]+=(rotr(W[9],19)^rotr(W[9],61)^(W[9]>>6U));
Vals[1]+=W[11];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[27];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

//W[12]+=(rotr(W[13],1)^rotr(W[13],8)^(W[13]>>7U));
W[12]+=W[5];
W[12]+=(rotr(W[10],19)^rotr(W[10],61)^(W[10]>>6U));
Vals[0]+=W[12];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[28];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

//W[13]+=(rotr(W[14],1)^rotr(W[14],8)^(W[14]>>7U));
W[13]+=W[6];
W[13]+=(rotr(W[11],19)^rotr(W[11],61)^(W[11]>>6U));
Vals[6]+=W[13];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[29];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

//W[14]+=(rotr(W[15],1)^rotr(W[15],8)^(W[15]>>7U));
W[14]+=0x8000000000000147UL;
W[14]+=W[7];
W[14]+=(rotr(W[12],19)^rotr(W[12],61)^(W[12]>>6U));
Vals[7]+=W[14];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[30];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

W[15]+=(rotr(W[0],1)^rotr(W[0],8)^(W[0]>>7U));
W[15]+=W[8];
W[15]+=(rotr(W[13],19)^rotr(W[13],61)^(W[13]>>6U));
Vals[5]+=W[15];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[31];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[0]+=(rotr(W[1],1)^rotr(W[1],8)^(W[1]>>7U));
W[0]+=W[9];
W[0]+=(rotr(W[14],19)^rotr(W[14],61)^(W[14]>>6U));
Vals[2]+=W[0];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[32];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[1]+=(rotr(W[2],1)^rotr(W[2],8)^(W[2]>>7U));
W[1]+=W[10];
W[1]+=(rotr(W[15],19)^rotr(W[15],61)^(W[15]>>6U));
Vals[3]+=W[1];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[33];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

W[2]+=(rotr(W[3],1)^rotr(W[3],8)^(W[3]>>7U));
W[2]+=W[11];
W[2]+=(rotr(W[0],19)^rotr(W[0],61)^(W[0]>>6U));
Vals[4]+=W[2];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[34];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

W[3]+=(rotr(W[4],1)^rotr(W[4],8)^(W[4]>>7U));
W[3]+=W[12];
W[3]+=(rotr(W[1],19)^rotr(W[1],61)^(W[1]>>6U));
Vals[1]+=W[3];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[35];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

W[4]+=(rotr(W[5],1)^rotr(W[5],8)^(W[5]>>7U));
W[4]+=W[13];
W[4]+=(rotr(W[2],19)^rotr(W[2],61)^(W[2]>>6U));
Vals[0]+=W[4];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[36];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

W[5]+=(rotr(W[6],1)^rotr(W[6],8)^(W[6]>>7U));
W[5]+=W[14];
W[5]+=(rotr(W[3],19)^rotr(W[3],61)^(W[3]>>6U));
Vals[6]+=W[5];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[37];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

W[6]+=(rotr(W[7],1)^rotr(W[7],8)^(W[7]>>7U));
W[6]+=W[15];
W[6]+=(rotr(W[4],19)^rotr(W[4],61)^(W[4]>>6U));
Vals[7]+=W[6];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[38];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

W[7]+=(rotr(W[8],1)^rotr(W[8],8)^(W[8]>>7U));
W[7]+=W[0];
W[7]+=(rotr(W[5],19)^rotr(W[5],61)^(W[5]>>6U));
Vals[5]+=W[7];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[39];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[8]+=(rotr(W[9],1)^rotr(W[9],8)^(W[9]>>7U));
W[8]+=W[1];
W[8]+=(rotr(W[6],19)^rotr(W[6],61)^(W[6]>>6U));
Vals[2]+=W[8];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[40];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[9]+=(rotr(W[10],1)^rotr(W[10],8)^(W[10]>>7U));
W[9]+=W[2];
W[9]+=(rotr(W[7],19)^rotr(W[7],61)^(W[7]>>6U));
Vals[3]+=W[9];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[41];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

W[10]+=(rotr(W[11],1)^rotr(W[11],8)^(W[11]>>7U));
W[10]+=W[3];
W[10]+=(rotr(W[8],19)^rotr(W[8],61)^(W[8]>>6U));
Vals[4]+=W[10];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[42];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

W[11]+=(rotr(W[12],1)^rotr(W[12],8)^(W[12]>>7U));
W[11]+=W[4];
W[11]+=(rotr(W[9],19)^rotr(W[9],61)^(W[9]>>6U));
Vals[1]+=W[11];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[43];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

W[12]+=(rotr(W[13],1)^rotr(W[13],8)^(W[13]>>7U));
W[12]+=W[5];
W[12]+=(rotr(W[10],19)^rotr(W[10],61)^(W[10]>>6U));
Vals[0]+=W[12];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[44];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

W[13]+=(rotr(W[14],1)^rotr(W[14],8)^(W[14]>>7U));
W[13]+=W[6];
W[13]+=(rotr(W[11],19)^rotr(W[11],61)^(W[11]>>6U));
Vals[6]+=W[13];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[45];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

W[14]+=(rotr(W[15],1)^rotr(W[15],8)^(W[15]>>7U));
W[14]+=W[7];
W[14]+=(rotr(W[12],19)^rotr(W[12],61)^(W[12]>>6U));
Vals[7]+=W[14];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[46];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

W[15]+=(rotr(W[0],1)^rotr(W[0],8)^(W[0]>>7U));
W[15]+=W[8];
W[15]+=(rotr(W[13],19)^rotr(W[13],61)^(W[13]>>6U));
Vals[5]+=W[15];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[47];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[0]+=(rotr(W[1],1)^rotr(W[1],8)^(W[1]>>7U));
W[0]+=W[9];
W[0]+=(rotr(W[14],19)^rotr(W[14],61)^(W[14]>>6U));
Vals[2]+=W[0];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[48];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[1]+=(rotr(W[2],1)^rotr(W[2],8)^(W[2]>>7U));
W[1]+=W[10];
W[1]+=(rotr(W[15],19)^rotr(W[15],61)^(W[15]>>6U));
Vals[3]+=W[1];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[49];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

W[2]+=(rotr(W[3],1)^rotr(W[3],8)^(W[3]>>7U));
W[2]+=W[11];
W[2]+=(rotr(W[0],19)^rotr(W[0],61)^(W[0]>>6U));
Vals[4]+=W[2];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[50];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

W[3]+=(rotr(W[4],1)^rotr(W[4],8)^(W[4]>>7U));
W[3]+=W[12];
W[3]+=(rotr(W[1],19)^rotr(W[1],61)^(W[1]>>6U));
Vals[1]+=W[3];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[51];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

W[4]+=(rotr(W[5],1)^rotr(W[5],8)^(W[5]>>7U));
W[4]+=W[13];
W[4]+=(rotr(W[2],19)^rotr(W[2],61)^(W[2]>>6U));
Vals[0]+=W[4];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[52];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

W[5]+=(rotr(W[6],1)^rotr(W[6],8)^(W[6]>>7U));
W[5]+=W[14];
W[5]+=(rotr(W[3],19)^rotr(W[3],61)^(W[3]>>6U));
Vals[6]+=W[5];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[53];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

W[6]+=(rotr(W[7],1)^rotr(W[7],8)^(W[7]>>7U));
W[6]+=W[15];
W[6]+=(rotr(W[4],19)^rotr(W[4],61)^(W[4]>>6U));
Vals[7]+=W[6];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[54];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

W[7]+=(rotr(W[8],1)^rotr(W[8],8)^(W[8]>>7U));
W[7]+=W[0];
W[7]+=(rotr(W[5],19)^rotr(W[5],61)^(W[5]>>6U));
Vals[5]+=W[7];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[55];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[8]+=(rotr(W[9],1)^rotr(W[9],8)^(W[9]>>7U));
W[8]+=W[1];
W[8]+=(rotr(W[6],19)^rotr(W[6],61)^(W[6]>>6U));
Vals[2]+=W[8];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[56];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[9]+=(rotr(W[10],1)^rotr(W[10],8)^(W[10]>>7U));
W[9]+=W[2];
W[9]+=(rotr(W[7],19)^rotr(W[7],61)^(W[7]>>6U));
Vals[3]+=W[9];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[57];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

W[10]+=(rotr(W[11],1)^rotr(W[11],8)^(W[11]>>7U));
W[10]+=W[3];
W[10]+=(rotr(W[8],19)^rotr(W[8],61)^(W[8]>>6U));
Vals[4]+=W[10];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[58];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

W[11]+=(rotr(W[12],1)^rotr(W[12],8)^(W[12]>>7U));
W[11]+=W[4];
W[11]+=(rotr(W[9],19)^rotr(W[9],61)^(W[9]>>6U));
Vals[1]+=W[11];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[59];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

W[12]+=(rotr(W[13],1)^rotr(W[13],8)^(W[13]>>7U));
W[12]+=W[5];
W[12]+=(rotr(W[10],19)^rotr(W[10],61)^(W[10]>>6U));
Vals[0]+=W[12];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[60];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

W[13]+=(rotr(W[14],1)^rotr(W[14],8)^(W[14]>>7U));
W[13]+=W[6];
W[13]+=(rotr(W[11],19)^rotr(W[11],61)^(W[11]>>6U));
Vals[6]+=W[13];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[61];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

W[14]+=(rotr(W[15],1)^rotr(W[15],8)^(W[15]>>7U));
W[14]+=W[7];
W[14]+=(rotr(W[12],19)^rotr(W[12],61)^(W[12]>>6U));
Vals[7]+=W[14];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[62];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

W[15]+=(rotr(W[0],1)^rotr(W[0],8)^(W[0]>>7U));
W[15]+=W[8];
W[15]+=(rotr(W[13],19)^rotr(W[13],61)^(W[13]>>6U));
Vals[5]+=W[15];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[63];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[0]+=(rotr(W[1],1)^rotr(W[1],8)^(W[1]>>7U));
W[0]+=W[9];
W[0]+=(rotr(W[14],19)^rotr(W[14],61)^(W[14]>>6U));
Vals[2]+=W[0];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[64];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[1]+=(rotr(W[2],1)^rotr(W[2],8)^(W[2]>>7U));
W[1]+=W[10];
W[1]+=(rotr(W[15],19)^rotr(W[15],61)^(W[15]>>6U));
Vals[3]+=W[1];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[65];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

W[2]+=(rotr(W[3],1)^rotr(W[3],8)^(W[3]>>7U));
W[2]+=W[11];
W[2]+=(rotr(W[0],19)^rotr(W[0],61)^(W[0]>>6U));
Vals[4]+=W[2];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[66];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

W[3]+=(rotr(W[4],1)^rotr(W[4],8)^(W[4]>>7U));
W[3]+=W[12];
W[3]+=(rotr(W[1],19)^rotr(W[1],61)^(W[1]>>6U));
Vals[1]+=W[3];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[67];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

W[4]+=(rotr(W[5],1)^rotr(W[5],8)^(W[5]>>7U));
W[4]+=W[13];
W[4]+=(rotr(W[2],19)^rotr(W[2],61)^(W[2]>>6U));
Vals[0]+=W[4];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[68];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

W[5]+=(rotr(W[6],1)^rotr(W[6],8)^(W[6]>>7U));
W[5]+=W[14];
W[5]+=(rotr(W[3],19)^rotr(W[3],61)^(W[3]>>6U));
Vals[6]+=W[5];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[69];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

W[6]+=(rotr(W[7],1)^rotr(W[7],8)^(W[7]>>7U));
W[6]+=W[15];
W[6]+=(rotr(W[4],19)^rotr(W[4],61)^(W[4]>>6U));
Vals[7]+=W[6];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[70];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

W[7]+=(rotr(W[8],1)^rotr(W[8],8)^(W[8]>>7U));
W[7]+=W[0];
W[7]+=(rotr(W[5],19)^rotr(W[5],61)^(W[5]>>6U));
Vals[5]+=W[7];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[71];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[8]+=(rotr(W[9],1)^rotr(W[9],8)^(W[9]>>7U));
W[8]+=W[1];
W[8]+=(rotr(W[6],19)^rotr(W[6],61)^(W[6]>>6U));
Vals[2]+=W[8];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[72];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[9]+=(rotr(W[10],1)^rotr(W[10],8)^(W[10]>>7U));
W[9]+=W[2];
W[9]+=(rotr(W[7],19)^rotr(W[7],61)^(W[7]>>6U));
Vals[3]+=W[9];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[73];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

W[10]+=(rotr(W[11],1)^rotr(W[11],8)^(W[11]>>7U));
W[10]+=W[3];
W[10]+=(rotr(W[8],19)^rotr(W[8],61)^(W[8]>>6U));
Vals[4]+=W[10];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[74];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

W[11]+=(rotr(W[12],1)^rotr(W[12],8)^(W[12]>>7U));
W[11]+=W[4];
W[11]+=(rotr(W[9],19)^rotr(W[9],61)^(W[9]>>6U));
Vals[1]+=W[11];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[75];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

W[12]+=(rotr(W[13],1)^rotr(W[13],8)^(W[13]>>7U));
W[12]+=W[5];
W[12]+=(rotr(W[10],19)^rotr(W[10],61)^(W[10]>>6U));
Vals[0]+=W[12];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[76];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

W[13]+=(rotr(W[14],1)^rotr(W[14],8)^(W[14]>>7U));
W[13]+=W[6];
W[13]+=(rotr(W[11],19)^rotr(W[11],61)^(W[11]>>6U));
Vals[6]+=W[13];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[77];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

W[14]+=(rotr(W[15],1)^rotr(W[15],8)^(W[15]>>7U));
W[14]+=W[7];
W[14]+=(rotr(W[12],19)^rotr(W[12],61)^(W[12]>>6U));
Vals[7]+=W[14];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[78];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

W[15]+=(rotr(W[0],1)^rotr(W[0],8)^(W[0]>>7U));
W[15]+=W[8];
W[15]+=(rotr(W[13],19)^rotr(W[13],61)^(W[13]>>6U));
Vals[5]+=W[15];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[79];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[0]=H[0]+Vals[5];
W[1]=H[1]+Vals[7];
W[2]=H[2]+Vals[6];
W[3]=H[3]+Vals[0];
W[4] = 0x8000000000000000UL;
W[5] = 0x0000000000000000UL;
W[6] = 0x0000000000000000UL;
W[7] = 0x0000000000000000UL;
W[8] = 0x0000000000000000UL;
W[9] = 0x0000000000000000UL;
W[10] = 0x0000000000000000UL;
W[11] = 0x0000000000000000UL;
W[12] = 0x0000000000000000UL;
W[13] = 0x0000000000000000UL;
W[14] = 0x0000000000000000UL;
W[15] = 0x0000000000000100UL;

Vals[5]=H[0];
Vals[7]=H[1];
Vals[6]=H[2];
Vals[0]=H[3];
Vals[1]=H[4];
Vals[4]=H[5];
Vals[3]=H[6];
Vals[2]=H[7];

Vals[2]+=W[0];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[0];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

Vals[3]+=W[1];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[1];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

Vals[4]+=W[2];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[2];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

Vals[1]+=W[3];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[3];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

Vals[0]+=W[4];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[4];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[5];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[6];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[7];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[8];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[9];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[10];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[11];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[12];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[13];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[14];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

Vals[5]+=W[15];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[15];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[0]+=(rotr(W[1],1)^rotr(W[1],8)^(W[1]>>7U));
//W[0]+=W[9];
//W[0]+=(rotr(W[14],19)^rotr(W[14],61)^(W[14]>>6U));
Vals[2]+=W[0];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[16];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[1]+=(rotr(W[2],1)^rotr(W[2],8)^(W[2]>>7U));
//W[1]+=W[10];
// W[1]+=(rotr(W[15],19)^rotr(W[15],61)^(W[15]>>6U));
W[1]+=0x20000000000804UL;
Vals[3]+=W[1];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[17];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

W[2]+=(rotr(W[3],1)^rotr(W[3],8)^(W[3]>>7U));
//W[2]+=W[11];
W[2]+=(rotr(W[0],19)^rotr(W[0],61)^(W[0]>>6U));
Vals[4]+=W[2];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[18];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

//W[3]+=(rotr(W[4],1)^rotr(W[4],8)^(W[4]>>7U));
W[3]+=0x4180000000000000UL;
//W[3]+=W[12];
W[3]+=(rotr(W[1],19)^rotr(W[1],61)^(W[1]>>6U));
Vals[1]+=W[3];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[19];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

//W[4]+=(rotr(W[5],1)^rotr(W[5],8)^(W[5]>>7U));
//W[4]+=W[13];
W[4]+=(rotr(W[2],19)^rotr(W[2],61)^(W[2]>>6U));
Vals[0]+=W[4];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[20];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

//W[5]+=(rotr(W[6],1)^rotr(W[6],8)^(W[6]>>7U));
W[5]+=W[14];
W[5]+=(rotr(W[3],19)^rotr(W[3],61)^(W[3]>>6U));
Vals[6]+=W[5];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[21];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

//W[6]+=(rotr(W[7],1)^rotr(W[7],8)^(W[7]>>7U));
W[6]+=W[15];
W[6]+=(rotr(W[4],19)^rotr(W[4],61)^(W[4]>>6U));
Vals[7]+=W[6];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[22];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

//W[7]+=(rotr(W[8],1)^rotr(W[8],8)^(W[8]>>7U));
W[7]+=W[0];
W[7]+=(rotr(W[5],19)^rotr(W[5],61)^(W[5]>>6U));
Vals[5]+=W[7];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[23];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

//W[8]+=(rotr(W[9],1)^rotr(W[9],8)^(W[9]>>7U));
W[8]+=W[1];
W[8]+=(rotr(W[6],19)^rotr(W[6],61)^(W[6]>>6U));
Vals[2]+=W[8];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[24];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

//W[9]+=(rotr(W[10],1)^rotr(W[10],8)^(W[10]>>7U));
W[9]+=W[2];
W[9]+=(rotr(W[7],19)^rotr(W[7],61)^(W[7]>>6U));
Vals[3]+=W[9];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[25];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

//W[10]+=(rotr(W[11],1)^rotr(W[11],8)^(W[11]>>7U));
W[10]+=W[3];
W[10]+=(rotr(W[8],19)^rotr(W[8],61)^(W[8]>>6U));
Vals[4]+=W[10];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[26];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

//W[11]+=(rotr(W[12],1)^rotr(W[12],8)^(W[12]>>7U));
W[11]+=W[4];
W[11]+=(rotr(W[9],19)^rotr(W[9],61)^(W[9]>>6U));
Vals[1]+=W[11];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[27];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

//W[12]+=(rotr(W[13],1)^rotr(W[13],8)^(W[13]>>7U));
W[12]+=W[5];
W[12]+=(rotr(W[10],19)^rotr(W[10],61)^(W[10]>>6U));
Vals[0]+=W[12];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[28];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

//W[13]+=(rotr(W[14],1)^rotr(W[14],8)^(W[14]>>7U));
W[13]+=W[6];
W[13]+=(rotr(W[11],19)^rotr(W[11],61)^(W[11]>>6U));
Vals[6]+=W[13];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[29];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

//W[14]+=(rotr(W[15],1)^rotr(W[15],8)^(W[15]>>7U));
W[14]+=0x0000000000000083UL;
W[14]+=W[7];
W[14]+=(rotr(W[12],19)^rotr(W[12],61)^(W[12]>>6U));
Vals[7]+=W[14];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[30];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

W[15]+=(rotr(W[0],1)^rotr(W[0],8)^(W[0]>>7U));
W[15]+=W[8];
W[15]+=(rotr(W[13],19)^rotr(W[13],61)^(W[13]>>6U));
Vals[5]+=W[15];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[31];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[0]+=(rotr(W[1],1)^rotr(W[1],8)^(W[1]>>7U));
W[0]+=W[9];
W[0]+=(rotr(W[14],19)^rotr(W[14],61)^(W[14]>>6U));
Vals[2]+=W[0];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[32];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[1]+=(rotr(W[2],1)^rotr(W[2],8)^(W[2]>>7U));
W[1]+=W[10];
W[1]+=(rotr(W[15],19)^rotr(W[15],61)^(W[15]>>6U));
Vals[3]+=W[1];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[33];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

W[2]+=(rotr(W[3],1)^rotr(W[3],8)^(W[3]>>7U));
W[2]+=W[11];
W[2]+=(rotr(W[0],19)^rotr(W[0],61)^(W[0]>>6U));
Vals[4]+=W[2];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[34];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

W[3]+=(rotr(W[4],1)^rotr(W[4],8)^(W[4]>>7U));
W[3]+=W[12];
W[3]+=(rotr(W[1],19)^rotr(W[1],61)^(W[1]>>6U));
Vals[1]+=W[3];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[35];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

W[4]+=(rotr(W[5],1)^rotr(W[5],8)^(W[5]>>7U));
W[4]+=W[13];
W[4]+=(rotr(W[2],19)^rotr(W[2],61)^(W[2]>>6U));
Vals[0]+=W[4];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[36];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

W[5]+=(rotr(W[6],1)^rotr(W[6],8)^(W[6]>>7U));
W[5]+=W[14];
W[5]+=(rotr(W[3],19)^rotr(W[3],61)^(W[3]>>6U));
Vals[6]+=W[5];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[37];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

W[6]+=(rotr(W[7],1)^rotr(W[7],8)^(W[7]>>7U));
W[6]+=W[15];
W[6]+=(rotr(W[4],19)^rotr(W[4],61)^(W[4]>>6U));
Vals[7]+=W[6];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[38];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

W[7]+=(rotr(W[8],1)^rotr(W[8],8)^(W[8]>>7U));
W[7]+=W[0];
W[7]+=(rotr(W[5],19)^rotr(W[5],61)^(W[5]>>6U));
Vals[5]+=W[7];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[39];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[8]+=(rotr(W[9],1)^rotr(W[9],8)^(W[9]>>7U));
W[8]+=W[1];
W[8]+=(rotr(W[6],19)^rotr(W[6],61)^(W[6]>>6U));
Vals[2]+=W[8];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[40];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[9]+=(rotr(W[10],1)^rotr(W[10],8)^(W[10]>>7U));
W[9]+=W[2];
W[9]+=(rotr(W[7],19)^rotr(W[7],61)^(W[7]>>6U));
Vals[3]+=W[9];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[41];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

W[10]+=(rotr(W[11],1)^rotr(W[11],8)^(W[11]>>7U));
W[10]+=W[3];
W[10]+=(rotr(W[8],19)^rotr(W[8],61)^(W[8]>>6U));
Vals[4]+=W[10];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[42];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

W[11]+=(rotr(W[12],1)^rotr(W[12],8)^(W[12]>>7U));
W[11]+=W[4];
W[11]+=(rotr(W[9],19)^rotr(W[9],61)^(W[9]>>6U));
Vals[1]+=W[11];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[43];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

W[12]+=(rotr(W[13],1)^rotr(W[13],8)^(W[13]>>7U));
W[12]+=W[5];
W[12]+=(rotr(W[10],19)^rotr(W[10],61)^(W[10]>>6U));
Vals[0]+=W[12];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[44];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

W[13]+=(rotr(W[14],1)^rotr(W[14],8)^(W[14]>>7U));
W[13]+=W[6];
W[13]+=(rotr(W[11],19)^rotr(W[11],61)^(W[11]>>6U));
Vals[6]+=W[13];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[45];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

W[14]+=(rotr(W[15],1)^rotr(W[15],8)^(W[15]>>7U));
W[14]+=W[7];
W[14]+=(rotr(W[12],19)^rotr(W[12],61)^(W[12]>>6U));
Vals[7]+=W[14];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[46];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

W[15]+=(rotr(W[0],1)^rotr(W[0],8)^(W[0]>>7U));
W[15]+=W[8];
W[15]+=(rotr(W[13],19)^rotr(W[13],61)^(W[13]>>6U));
Vals[5]+=W[15];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[47];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[0]+=(rotr(W[1],1)^rotr(W[1],8)^(W[1]>>7U));
W[0]+=W[9];
W[0]+=(rotr(W[14],19)^rotr(W[14],61)^(W[14]>>6U));
Vals[2]+=W[0];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[48];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[1]+=(rotr(W[2],1)^rotr(W[2],8)^(W[2]>>7U));
W[1]+=W[10];
W[1]+=(rotr(W[15],19)^rotr(W[15],61)^(W[15]>>6U));
Vals[3]+=W[1];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[49];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

W[2]+=(rotr(W[3],1)^rotr(W[3],8)^(W[3]>>7U));
W[2]+=W[11];
W[2]+=(rotr(W[0],19)^rotr(W[0],61)^(W[0]>>6U));
Vals[4]+=W[2];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[50];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

W[3]+=(rotr(W[4],1)^rotr(W[4],8)^(W[4]>>7U));
W[3]+=W[12];
W[3]+=(rotr(W[1],19)^rotr(W[1],61)^(W[1]>>6U));
Vals[1]+=W[3];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[51];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

W[4]+=(rotr(W[5],1)^rotr(W[5],8)^(W[5]>>7U));
W[4]+=W[13];
W[4]+=(rotr(W[2],19)^rotr(W[2],61)^(W[2]>>6U));
Vals[0]+=W[4];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[52];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

W[5]+=(rotr(W[6],1)^rotr(W[6],8)^(W[6]>>7U));
W[5]+=W[14];
W[5]+=(rotr(W[3],19)^rotr(W[3],61)^(W[3]>>6U));
Vals[6]+=W[5];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[53];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

W[6]+=(rotr(W[7],1)^rotr(W[7],8)^(W[7]>>7U));
W[6]+=W[15];
W[6]+=(rotr(W[4],19)^rotr(W[4],61)^(W[4]>>6U));
Vals[7]+=W[6];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[54];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

W[7]+=(rotr(W[8],1)^rotr(W[8],8)^(W[8]>>7U));
W[7]+=W[0];
W[7]+=(rotr(W[5],19)^rotr(W[5],61)^(W[5]>>6U));
Vals[5]+=W[7];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[55];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[8]+=(rotr(W[9],1)^rotr(W[9],8)^(W[9]>>7U));
W[8]+=W[1];
W[8]+=(rotr(W[6],19)^rotr(W[6],61)^(W[6]>>6U));
Vals[2]+=W[8];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[56];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[9]+=(rotr(W[10],1)^rotr(W[10],8)^(W[10]>>7U));
W[9]+=W[2];
W[9]+=(rotr(W[7],19)^rotr(W[7],61)^(W[7]>>6U));
Vals[3]+=W[9];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[57];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

W[10]+=(rotr(W[11],1)^rotr(W[11],8)^(W[11]>>7U));
W[10]+=W[3];
W[10]+=(rotr(W[8],19)^rotr(W[8],61)^(W[8]>>6U));
Vals[4]+=W[10];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[58];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

W[11]+=(rotr(W[12],1)^rotr(W[12],8)^(W[12]>>7U));
W[11]+=W[4];
W[11]+=(rotr(W[9],19)^rotr(W[9],61)^(W[9]>>6U));
Vals[1]+=W[11];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[59];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

W[12]+=(rotr(W[13],1)^rotr(W[13],8)^(W[13]>>7U));
W[12]+=W[5];
W[12]+=(rotr(W[10],19)^rotr(W[10],61)^(W[10]>>6U));
Vals[0]+=W[12];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[60];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

W[13]+=(rotr(W[14],1)^rotr(W[14],8)^(W[14]>>7U));
W[13]+=W[6];
W[13]+=(rotr(W[11],19)^rotr(W[11],61)^(W[11]>>6U));
Vals[6]+=W[13];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[61];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

W[14]+=(rotr(W[15],1)^rotr(W[15],8)^(W[15]>>7U));
W[14]+=W[7];
W[14]+=(rotr(W[12],19)^rotr(W[12],61)^(W[12]>>6U));
Vals[7]+=W[14];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[62];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

W[15]+=(rotr(W[0],1)^rotr(W[0],8)^(W[0]>>7U));
W[15]+=W[8];
W[15]+=(rotr(W[13],19)^rotr(W[13],61)^(W[13]>>6U));
Vals[5]+=W[15];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[63];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[0]+=(rotr(W[1],1)^rotr(W[1],8)^(W[1]>>7U));
W[0]+=W[9];
W[0]+=(rotr(W[14],19)^rotr(W[14],61)^(W[14]>>6U));
Vals[2]+=W[0];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[64];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[1]+=(rotr(W[2],1)^rotr(W[2],8)^(W[2]>>7U));
W[1]+=W[10];
W[1]+=(rotr(W[15],19)^rotr(W[15],61)^(W[15]>>6U));
Vals[3]+=W[1];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[65];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

W[2]+=(rotr(W[3],1)^rotr(W[3],8)^(W[3]>>7U));
W[2]+=W[11];
W[2]+=(rotr(W[0],19)^rotr(W[0],61)^(W[0]>>6U));
Vals[4]+=W[2];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[66];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

W[3]+=(rotr(W[4],1)^rotr(W[4],8)^(W[4]>>7U));
W[3]+=W[12];
W[3]+=(rotr(W[1],19)^rotr(W[1],61)^(W[1]>>6U));
Vals[1]+=W[3];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[67];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

W[4]+=(rotr(W[5],1)^rotr(W[5],8)^(W[5]>>7U));
W[4]+=W[13];
W[4]+=(rotr(W[2],19)^rotr(W[2],61)^(W[2]>>6U));
Vals[0]+=W[4];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=K[68];
Vals[2]+=Vals[0];
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

W[5]+=(rotr(W[6],1)^rotr(W[6],8)^(W[6]>>7U));
W[5]+=W[14];
W[5]+=(rotr(W[3],19)^rotr(W[3],61)^(W[3]>>6U));
Vals[6]+=W[5];
Vals[6]+=(rotr(Vals[2],14)^rotr(Vals[2],18)^rotr(Vals[2],41));
Vals[6]+=ch(Vals[2],Vals[5],Vals[7]);
Vals[6]+=K[69];
Vals[3]+=Vals[6];
Vals[6]+=(rotr(Vals[0],28)^rotr(Vals[0],34)^rotr(Vals[0],39));
Vals[6]+=Ma(Vals[4],Vals[0],Vals[1]);

W[6]+=(rotr(W[7],1)^rotr(W[7],8)^(W[7]>>7U));
W[6]+=W[15];
W[6]+=(rotr(W[4],19)^rotr(W[4],61)^(W[4]>>6U));
Vals[7]+=W[6];
Vals[7]+=(rotr(Vals[3],14)^rotr(Vals[3],18)^rotr(Vals[3],41));
Vals[7]+=ch(Vals[3],Vals[2],Vals[5]);
Vals[7]+=K[70];
Vals[4]+=Vals[7];
Vals[7]+=(rotr(Vals[6],28)^rotr(Vals[6],34)^rotr(Vals[6],39));
Vals[7]+=Ma(Vals[1],Vals[6],Vals[0]);

W[7]+=(rotr(W[8],1)^rotr(W[8],8)^(W[8]>>7U));
W[7]+=W[0];
W[7]+=(rotr(W[5],19)^rotr(W[5],61)^(W[5]>>6U));
Vals[5]+=W[7];
Vals[5]+=(rotr(Vals[4],14)^rotr(Vals[4],18)^rotr(Vals[4],41));
Vals[5]+=ch(Vals[4],Vals[3],Vals[2]);
Vals[5]+=K[71];
Vals[1]+=Vals[5];
Vals[5]+=(rotr(Vals[7],28)^rotr(Vals[7],34)^rotr(Vals[7],39));
Vals[5]+=Ma(Vals[0],Vals[7],Vals[6]);

W[8]+=(rotr(W[9],1)^rotr(W[9],8)^(W[9]>>7U));
W[8]+=W[1];
W[8]+=(rotr(W[6],19)^rotr(W[6],61)^(W[6]>>6U));
Vals[2]+=W[8];
Vals[2]+=(rotr(Vals[1],14)^rotr(Vals[1],18)^rotr(Vals[1],41));
Vals[2]+=ch(Vals[1],Vals[4],Vals[3]);
Vals[2]+=K[72];
Vals[0]+=Vals[2];
Vals[2]+=(rotr(Vals[5],28)^rotr(Vals[5],34)^rotr(Vals[5],39));
Vals[2]+=Ma(Vals[6],Vals[5],Vals[7]);

W[9]+=(rotr(W[10],1)^rotr(W[10],8)^(W[10]>>7U));
W[9]+=W[2];
W[9]+=(rotr(W[7],19)^rotr(W[7],61)^(W[7]>>6U));
Vals[3]+=W[9];
Vals[3]+=(rotr(Vals[0],14)^rotr(Vals[0],18)^rotr(Vals[0],41));
Vals[3]+=ch(Vals[0],Vals[1],Vals[4]);
Vals[3]+=K[73];
Vals[6]+=Vals[3];
Vals[3]+=(rotr(Vals[2],28)^rotr(Vals[2],34)^rotr(Vals[2],39));
Vals[3]+=Ma(Vals[7],Vals[2],Vals[5]);

W[10]+=(rotr(W[11],1)^rotr(W[11],8)^(W[11]>>7U));
W[10]+=W[3];
W[10]+=(rotr(W[8],19)^rotr(W[8],61)^(W[8]>>6U));
Vals[4]+=W[10];
Vals[4]+=(rotr(Vals[6],14)^rotr(Vals[6],18)^rotr(Vals[6],41));
Vals[4]+=ch(Vals[6],Vals[0],Vals[1]);
Vals[4]+=K[74];
Vals[7]+=Vals[4];
Vals[4]+=(rotr(Vals[3],28)^rotr(Vals[3],34)^rotr(Vals[3],39));
Vals[4]+=Ma(Vals[5],Vals[3],Vals[2]);

W[11]+=(rotr(W[12],1)^rotr(W[12],8)^(W[12]>>7U));
W[11]+=W[4];
W[11]+=(rotr(W[9],19)^rotr(W[9],61)^(W[9]>>6U));
Vals[1]+=W[11];
Vals[1]+=(rotr(Vals[7],14)^rotr(Vals[7],18)^rotr(Vals[7],41));
Vals[1]+=ch(Vals[7],Vals[6],Vals[0]);
Vals[1]+=K[75];
Vals[5]+=Vals[1];
Vals[1]+=(rotr(Vals[4],28)^rotr(Vals[4],34)^rotr(Vals[4],39));
Vals[1]+=Ma(Vals[2],Vals[4],Vals[3]);

W[12]+=(rotr(W[13],1)^rotr(W[13],8)^(W[13]>>7U));
W[12]+=W[5];
W[12]+=(rotr(W[10],19)^rotr(W[10],61)^(W[10]>>6U));
Vals[0]+=W[12];
Vals[0]+=(rotr(Vals[5],14)^rotr(Vals[5],18)^rotr(Vals[5],41));
Vals[0]+=ch(Vals[5],Vals[7],Vals[6]);
Vals[0]+=(rotr(Vals[1],28)^rotr(Vals[1],34)^rotr(Vals[1],39));
Vals[0]+=Ma(Vals[3],Vals[1],Vals[4]);

u32 result = Vals[0];

#define FOUND (0x0F)
#define SETFOUND(Xnonce) output[output[FOUND]++] = Xnonce

#if defined(VECTORS2) || defined(VECTORS4)
	if (any(result == 0xDB80D28DUL)) {
		if (result.x == 0xDB80D28DUL)
			SETFOUND(nonce.x);
		if (result.y == 0xDB80D28DUL)
			SETFOUND(nonce.y);
#if defined(VECTORS4)
		if (result.z == 0xDB80D28DUL)
			SETFOUND(nonce.z);
		if (result.w == 0xDB80D28DUL)
			SETFOUND(nonce.w);
#endif
	}
#else
	if (result == 0xDB80D28DUL)
		SETFOUND(nonce);
#endif
}
