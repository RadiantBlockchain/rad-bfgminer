// This file is taken and modified from the public-domain poclbm project, and
// I have therefore decided to keep it public-domain.
// Modified version copyright 2011-2012 Con Kolivas

// kernel-interface: rad sha512_256

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


#define Ch(x, y, z) (z ^ (x & (y ^ z)))
#define Ma(x, y, z) ((x & z) | (y & (x | z)))
#define rot(x, y) rotate((x), 64UL - (y))



//Various intermediate calculations for each SHA round
#define s0(n) (S0(Vals[(0 + 160 - (n)) % 8]))
#define S0(n) (rot(n, 28u)^rot(n, 34u)^rot(n, 39u))

#define s1(n) (S1(Vals[(4 + 160 - (n)) % 8]))
#define S1(n) (rot(n, 14u)^rot(n, 18u)^rot(n, 41u))

#define ch(n) Ch(Vals[(4 + 160 - (n)) % 8],Vals[(5 + 160 - (n)) % 8],Vals[(6 + 160 - (n)) % 8])
#define maj(n) Ma(Vals[(1 + 160 - (n)) % 8],Vals[(2 + 160 - (n)) % 8],Vals[(0 + 160 - (n)) % 8])

//t1 calc when W is already calculated
#define t1(n) K[(n) % 80] + Vals[(7 + 160 - (n)) % 8] +  W[(n)] + s1(n) + ch(n)

//t1 calc which calculates W
#define t1W(n) K[(n) % 80] + Vals[(7 + 160 - (n)) % 8] +  W(n) + s1(n) + ch(n)

//t2 Calc
#define t2(n)  maj(n) + s0(n)

//W calculation used for SHA round
#define W(n) (W[n] = P4(n) + P3(n) + P2(n) + P1(n))



//Partial W calculations (used for the begining where only some values are nonzero)
#define P1(n) ((rot(W[(n)-2],19u)^rot(W[(n)-2],61u)^((W[(n)-2])>>6U)))
#define P2(n) ((rot(W[(n)-15],1u)^rot(W[(n)-15],8u)^((W[(n)-15])>>7U)))


#define p1(x) ((rot(x,19u)^rot(x,61u)^((x)>>6U)))
#define p2(x) ((rot(x,1u)^rot(x,8u)^((x)>>7U)))


#define P3(n)  W[n-7]
#define P4(n)  W[n-16]


//SHA round with built in W calc
#define sharoundW(n) Vals[(3 + 160 - (n)) % 8] += t1W(n); Vals[(7 + 160 - (n)) % 8] = t1W(n) + t2(n);

//SHA round without W calc
#define sharound(n)  Vals[(3 + 160 - (n)) % 8] += t1(n); Vals[(7 + 160 - (n)) % 8] = t1(n) + t2(n);

//#define WORKSIZE 256
#define MAXBUFFERS (4095)

__kernel 
 __attribute__((reqd_work_group_size(WORKSIZE, 1, 1)))
void search(const u w0, const u w1, const u w2, const u w3, const u w4, const u w5, const u w6, const u w7, const u w8, const u w9, const u32 base, volatile __global uint * output)
{
	u W[160];
	u Vals[8];
	u32 nonce;

//Dummy Variable to prevent compiler from reordering between rounds
	u t1;

	W[0] = w0;
	W[1] = w1;
	W[2] = w2;
	W[3] = w3;
	W[4] = w4;
	W[5] = w5;
	W[6] = w6;
	W[7] = w7;
	W[8] = w8;

#ifdef VECTORS4
	nonce = base + (uint)(get_local_id(0)) * 4u + (uint)(get_group_id(0)) * (WORKSIZE * 4u);
	W[9] = w9 + nonce;
#elif defined VECTORS2
	nonce = base + (uint)(get_local_id(0)) * 2u + (uint)(get_group_id(0)) * (WORKSIZE * 2u);
	W[9] = w9 + nonce;
#else
	nonce = base + get_local_id(0) + get_group_id(0) * (WORKSIZE);
	W[9] = w9 + nonce;
#endif

	W[10] = 0x8000000000000000UL;
	W[11] = 0x0000000000000000UL;
	W[12] = 0x0000000000000000UL;
	W[13] = 0x0000000000000000UL;
	W[14] = 0x0000000000000000UL;
	W[15] = 0x0000000000000280UL;

	Vals[0]=H[0];
	Vals[1]=H[1];
	Vals[2]=H[2];
	Vals[3]=H[3];
	Vals[4]=H[4];
	Vals[5]=H[5];
	Vals[6]=H[6];
	Vals[7]=H[7];


	sharound(0);
	sharound(1);
	sharound(2);
	sharound(3);
	sharound(4);
	sharound(5);
	sharound(6);
	sharound(7);
	sharound(8);
	sharound(9);
	sharound(10);
	sharound(11);
	sharound(12);
	sharound(13);
	sharound(14);
	sharound(15);
	sharoundW(16);
	sharoundW(17);
	sharoundW(18);
	sharoundW(19);
	sharoundW(20);
	sharoundW(21);
	sharoundW(22);
	sharoundW(23);
	sharoundW(24);
	sharoundW(25);
	sharoundW(26);
	sharoundW(27);
	sharoundW(28);
	sharoundW(29);
	sharoundW(30);
	sharoundW(31);
	sharoundW(32);
	sharoundW(33);
	sharoundW(34);
	sharoundW(35);
	sharoundW(36);
	sharoundW(37);
	sharoundW(38);
	sharoundW(39);
	sharoundW(40);
	sharoundW(41);
	sharoundW(42);
	sharoundW(43);
	sharoundW(44);
	sharoundW(45);
	sharoundW(46);
	sharoundW(47);
	sharoundW(48);
	sharoundW(49);
	sharoundW(50);
	sharoundW(51);
	sharoundW(52);
	sharoundW(53);
	sharoundW(54);
	sharoundW(55);
	sharoundW(56);
	sharoundW(57);
	sharoundW(58);
	sharoundW(59);
	sharoundW(60);
	sharoundW(61);
	sharoundW(62);
	sharoundW(63);
	sharoundW(64);
	sharoundW(65);
	sharoundW(66);
	sharoundW(67);
	sharoundW(68);
	sharoundW(69);
	sharoundW(70);
	sharoundW(71);
	sharoundW(72);
	sharoundW(73);
	sharoundW(74);
	sharoundW(75);
	sharoundW(76);
	sharoundW(77);
	sharoundW(78);
	sharoundW(79);

	W[80]=H[0]+Vals[0];
	W[81]=H[1]+Vals[1];
	W[82]=H[2]+Vals[2];
	W[83]=H[3]+Vals[3];
	W[84] = 0x8000000000000000UL;
	W[85] = 0x0000000000000000UL;
	W[86] = 0x0000000000000000UL;
	W[87] = 0x0000000000000000UL;
	W[88] = 0x0000000000000000UL;
	W[89] = 0x0000000000000000UL;
	W[90] = 0x0000000000000000UL;
	W[91] = 0x0000000000000000UL;
	W[92] = 0x0000000000000000UL;
	W[93] = 0x0000000000000000UL;
	W[94] = 0x0000000000000000UL;
	W[95] = 0x0000000000000100UL;

	Vals[0]=H[0];
	Vals[1]=H[1];
	Vals[2]=H[2];
	Vals[3]=H[3];
	Vals[4]=H[4];
	Vals[5]=H[5];
	Vals[6]=H[6];
	Vals[7]=H[7];

	sharound(80 + 0);
	sharound(80 + 1);
	sharound(80 + 2);
	sharound(80 + 3);
	sharound(80 + 4);
	sharound(80 + 5);
	sharound(80 + 6);
	sharound(80 + 7);
	sharound(80 + 8);
	sharound(80 + 9);
	sharound(80 + 10);
	sharound(80 + 11);
	sharound(80 + 12);
	sharound(80 + 13);
	sharound(80 + 14);
	sharound(80 + 15);
	sharoundW(80 + 16);
	sharoundW(80 + 17);
	sharoundW(80 + 18);
	sharoundW(80 + 19);
	sharoundW(80 + 20);
	sharoundW(80 + 21);
	sharoundW(80 + 22);
	sharoundW(80 + 23);
	sharoundW(80 + 24);
	sharoundW(80 + 25);
	sharoundW(80 + 26);
	sharoundW(80 + 27);
	sharoundW(80 + 28);
	sharoundW(80 + 29);
	sharoundW(80 + 30);
	sharoundW(80 + 31);
	sharoundW(80 + 32);
	sharoundW(80 + 33);
	sharoundW(80 + 34);
	sharoundW(80 + 35);
	sharoundW(80 + 36);
	sharoundW(80 + 37);
	sharoundW(80 + 38);
	sharoundW(80 + 39);
	sharoundW(80 + 40);
	sharoundW(80 + 41);
	sharoundW(80 + 42);
	sharoundW(80 + 43);
	sharoundW(80 + 44);
	sharoundW(80 + 45);
	sharoundW(80 + 46);
	sharoundW(80 + 47);
	sharoundW(80 + 48);
	sharoundW(80 + 49);
	sharoundW(80 + 50);
	sharoundW(80 + 51);
	sharoundW(80 + 52);
	sharoundW(80 + 53);
	sharoundW(80 + 54);
	sharoundW(80 + 55);
	sharoundW(80 + 56);
	sharoundW(80 + 57);
	sharoundW(80 + 58);
	sharoundW(80 + 59);
	sharoundW(80 + 60);
	sharoundW(80 + 61);
	sharoundW(80 + 62);
	sharoundW(80 + 63);
	sharoundW(80 + 64);
	sharoundW(80 + 65);
	sharoundW(80 + 66);
	sharoundW(80 + 67);
	sharoundW(80 + 68);
	sharoundW(80 + 69);
	sharoundW(80 + 70);
	sharoundW(80 + 71);
	sharoundW(80 + 72);
	sharoundW(80 + 73);
	sharoundW(80 + 74);
	sharoundW(80 + 75);
	sharoundW(80 + 76);

	u result = (Vals[3] + H[3]) & 0x00000000fffffffful;

#define FOUND (0x0F)
#define SETFOUND(Xnonce) output[output[FOUND]++] = Xnonce

#ifdef VECTORS4
	bool result = result.x & result.y & result.z & result.w;
	if (!result) {
		if (!result.x)
			SETFOUND(nonce.x);
		if (!result.y)
			SETFOUND(nonce.y);
		if (!result.z)
			SETFOUND(nonce.z);
		if (!result.w)
			SETFOUND(nonce.w);
	}
#elif defined VECTORS2
	bool result = result.x & result.y;
	if (!result) {
		if (!result.x)
			SETFOUND(nonce.x);
		if (!result.y)
			SETFOUND(nonce.y);
	}
#else
	if (!result)
		SETFOUND(nonce);
#endif
}
