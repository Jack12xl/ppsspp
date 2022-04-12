// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "ppsspp_config.h"

#include "ext/xxhash.h"

#include "Common/Common.h"
#include "Common/Data/Convert/ColorConv.h"
#include "Common/CPUDetect.h"
#include "Common/Log.h"

#include "GPU/GPU.h"
#include "GPU/GPUState.h"
#include "GPU/Common/TextureDecoder.h"

#ifdef _M_SSE
#include <emmintrin.h>
#include <smmintrin.h>
#endif

#if PPSSPP_ARCH(ARM_NEON)
#if defined(_MSC_VER) && PPSSPP_ARCH(ARM64)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#endif

#ifdef _M_SSE

u32 QuickTexHashSSE2(const void *checkp, u32 size) {
	u32 check = 0;

	if (((intptr_t)checkp & 0xf) == 0 && (size & 0x3f) == 0) {
		__m128i cursor = _mm_set1_epi32(0);
		__m128i cursor2 = _mm_set_epi16(0x0001U, 0x0083U, 0x4309U, 0x4d9bU, 0xb651U, 0x4b73U, 0x9bd9U, 0xc00bU);
		__m128i update = _mm_set1_epi16(0x2455U);
		const __m128i *p = (const __m128i *)checkp;
		for (u32 i = 0; i < size / 16; i += 4) {
			__m128i chunk = _mm_mullo_epi16(_mm_load_si128(&p[i]), cursor2);
			cursor = _mm_add_epi16(cursor, chunk);
			cursor = _mm_xor_si128(cursor, _mm_load_si128(&p[i + 1]));
			cursor = _mm_add_epi32(cursor, _mm_load_si128(&p[i + 2]));
			chunk = _mm_mullo_epi16(_mm_load_si128(&p[i + 3]), cursor2);
			cursor = _mm_xor_si128(cursor, chunk);
			cursor2 = _mm_add_epi16(cursor2, update);
		}
		cursor = _mm_add_epi32(cursor, cursor2);
		// Add the four parts into the low i32.
		cursor = _mm_add_epi32(cursor, _mm_srli_si128(cursor, 8));
		cursor = _mm_add_epi32(cursor, _mm_srli_si128(cursor, 4));
		check = _mm_cvtsi128_si32(cursor);
	} else {
		const u32 *p = (const u32 *)checkp;
		for (u32 i = 0; i < size / 8; ++i) {
			check += *p++;
			check ^= *p++;
		}
	}

	return check;
}
#endif

#if PPSSPP_ARCH(ARM_NEON)

alignas(16) static const u16 QuickTexHashInitial[8] = { 0xc00bU, 0x9bd9U, 0x4b73U, 0xb651U, 0x4d9bU, 0x4309U, 0x0083U, 0x0001U };

u32 QuickTexHashNEON(const void *checkp, u32 size) {
	u32 check = 0;
	__builtin_prefetch(checkp, 0, 0);

	if (((intptr_t)checkp & 0xf) == 0 && (size & 0x3f) == 0) {
#if PPSSPP_PLATFORM(IOS) || PPSSPP_ARCH(ARM64) || defined(_MSC_VER) || !PPSSPP_ARCH(ARMV7)
		uint32x4_t cursor = vdupq_n_u32(0);
		uint16x8_t cursor2 = vld1q_u16(QuickTexHashInitial);
		uint16x8_t update = vdupq_n_u16(0x2455U);

		const u32 *p = (const u32 *)checkp;
		const u32 *pend = p + size / 4;
		while (p < pend) {
			cursor = vreinterpretq_u32_u16(vmlaq_u16(vreinterpretq_u16_u32(cursor), vreinterpretq_u16_u32(vld1q_u32(&p[4 * 0])), cursor2));
			cursor = veorq_u32(cursor, vld1q_u32(&p[4 * 1]));
			cursor = vaddq_u32(cursor, vld1q_u32(&p[4 * 2]));
			cursor = veorq_u32(cursor, vreinterpretq_u32_u16(vmulq_u16(vreinterpretq_u16_u32(vld1q_u32(&p[4 * 3])), cursor2)));
			cursor2 = vaddq_u16(cursor2, update);

			p += 4 * 4;
		}

		cursor = vaddq_u32(cursor, vreinterpretq_u32_u16(cursor2));
		uint32x2_t mixed = vadd_u32(vget_high_u32(cursor), vget_low_u32(cursor));
		check = vget_lane_u32(mixed, 0) + vget_lane_u32(mixed, 1);
#else
		// TODO: Why does this crash on iOS, but only certain devices?
		// It's faster than the above, but I guess it sucks to be using an iPhone.
		// As of 2020 clang, it's still faster by ~1.4%.

		// d0/d1 (q0) - cursor
		// d2/d3 (q1) - cursor2
		// d4/d5 (q2) - update
		// d16-d23 (q8-q11) - memory transfer
		asm volatile (
			// Initialize cursor.
			"vmov.i32 q0, #0\n"

			// Initialize cursor2.
			"movw r0, 0xc00b\n"
			"movt r0, 0x9bd9\n"
			"movw r1, 0x4b73\n"
			"movt r1, 0xb651\n"
			"vmov d2, r0, r1\n"
			"movw r0, 0x4d9b\n"
			"movt r0, 0x4309\n"
			"movw r1, 0x0083\n"
			"movt r1, 0x0001\n"
			"vmov d3, r0, r1\n"

			// Initialize update.
			"movw r0, 0x2455\n"
			"vdup.i16 q2, r0\n"

			// This is where we end.
			"add r0, %1, %2\n"

			// Okay, do the memory hashing.
			"QuickTexHashNEON_next:\n"
			"pld [%2, #0xc0]\n"
			"vldmia %2!, {d16-d23}\n"
			"vmla.i16 q0, q1, q8\n"
			"vmul.i16 q11, q11, q1\n"
			"veor.i32 q0, q0, q9\n"
			"cmp %2, r0\n"
			"vadd.i32 q0, q0, q10\n"
			"vadd.i16 q1, q1, q2\n"
			"veor.i32 q0, q0, q11\n"
			"blo QuickTexHashNEON_next\n"

			// Now let's get the result.
			"vadd.i32 q0, q0, q1\n"
			"vadd.i32 d0, d0, d1\n"
			"vmov r0, r1, d0\n"
			"add %0, r0, r1\n"

			: "=r"(check)
			: "r"(size), "r"(checkp)
			: "r0", "r1", "d0", "d1", "d2", "d3", "d4", "d5", "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23", "cc"
			);
#endif
	} else {
		const u32 size_u32 = size / 4;
		const u32 *p = (const u32 *)checkp;
		for (u32 i = 0; i < size_u32; i += 4) {
			check += p[i + 0];
			check ^= p[i + 1];
			check += p[i + 2];
			check ^= p[i + 3];
		}
	}

	return check;
}

#endif  // PPSSPP_ARCH(ARM_NEON)

// Masks to downalign bufw to 16 bytes, and wrap at 2048.
static const u32 textureAlignMask16[16] = {
	0x7FF & ~(((8 * 16) / 16) - 1),  //GE_TFMT_5650,
	0x7FF & ~(((8 * 16) / 16) - 1),  //GE_TFMT_5551,
	0x7FF & ~(((8 * 16) / 16) - 1),  //GE_TFMT_4444,
	0x7FF & ~(((8 * 16) / 32) - 1),  //GE_TFMT_8888,
	0x7FF & ~(((8 * 16) / 4) - 1),   //GE_TFMT_CLUT4,
	0x7FF & ~(((8 * 16) / 8) - 1),   //GE_TFMT_CLUT8,
	0x7FF & ~(((8 * 16) / 16) - 1),  //GE_TFMT_CLUT16,
	0x7FF & ~(((8 * 16) / 32) - 1),  //GE_TFMT_CLUT32,
	0x7FF, //GE_TFMT_DXT1,
	0x7FF, //GE_TFMT_DXT3,
	0x7FF, //GE_TFMT_DXT5,
	0,   // INVALID,
	0,   // INVALID,
	0,   // INVALID,
	0,   // INVALID,
	0,   // INVALID,
};

u32 GetTextureBufw(int level, u32 texaddr, GETextureFormat format) {
	// This is a hack to allow for us to draw the huge PPGe texture, which is always in kernel ram.
	if (texaddr >= PSP_GetKernelMemoryBase() && texaddr < PSP_GetKernelMemoryEnd())
		return gstate.texbufwidth[level] & 0x1FFF;

	u32 bufw = gstate.texbufwidth[level] & textureAlignMask16[format];
	if (bufw == 0 && format <= GE_TFMT_DXT5) {
		// If it's less than 16 bytes, use 16 bytes.
		bufw = (8 * 16) / textureBitsPerPixel[format];
	}
	return bufw;
}

// Is this compatible with QuickTexHashNEON/SSE?
u32 QuickTexHashNonSSE(const void *checkp, u32 size) {
	u32 check = 0;

	if (((intptr_t)checkp & 0xf) == 0 && (size & 0x3f) == 0) {
		static const u16 cursor2_initial[8] = {0xc00bU, 0x9bd9U, 0x4b73U, 0xb651U, 0x4d9bU, 0x4309U, 0x0083U, 0x0001U};
		union u32x4_u16x8 {
			u32 x32[4];
			u16 x16[8];
		};
		u32x4_u16x8 cursor{};
		u32x4_u16x8 cursor2;
		static const u16 update[8] = {0x2455U, 0x2455U, 0x2455U, 0x2455U, 0x2455U, 0x2455U, 0x2455U, 0x2455U};

		for (u32 j = 0; j < 8; ++j) {
			cursor2.x16[j] = cursor2_initial[j];
		}

		const u32x4_u16x8 *p = (const u32x4_u16x8 *)checkp;
		for (u32 i = 0; i < size / 16; i += 4) {
			for (u32 j = 0; j < 8; ++j) {
				const u16 temp = p[i + 0].x16[j] * cursor2.x16[j];
				cursor.x16[j] += temp;
			}
			for (u32 j = 0; j < 4; ++j) {
				cursor.x32[j] ^= p[i + 1].x32[j];
				cursor.x32[j] += p[i + 2].x32[j];
			}
			for (u32 j = 0; j < 8; ++j) {
				const u16 temp = p[i + 3].x16[j] * cursor2.x16[j];
				cursor.x16[j] ^= temp;
			}
			for (u32 j = 0; j < 8; ++j) {
				cursor2.x16[j] += update[j];
			}
		}

		for (u32 j = 0; j < 4; ++j) {
			cursor.x32[j] += cursor2.x32[j];
		}
		check = cursor.x32[0] + cursor.x32[1] + cursor.x32[2] + cursor.x32[3];
	} else {
		const u32 *p = (const u32 *)checkp;
		for (u32 i = 0; i < size / 8; ++i) {
			check += *p++;
			check ^= *p++;
		}
	}

	return check;
}

u32 QuickTexHashBasic(const void *checkp, u32 size) {
	u32 check = 0;
	const u32 size_u32 = size / 4;
	const u32 *p = (const u32 *)checkp;
	for (u32 i = 0; i < size_u32; i += 4) {
		check += p[i + 0];
		check ^= p[i + 1];
		check += p[i + 2];
		check ^= p[i + 3];
	}

	return check;
}

void DoSwizzleTex16(const u32 *ysrcp, u8 *texptr, int bxc, int byc, u32 pitch) {
	// ysrcp is in 32-bits, so this is convenient.
	const u32 pitchBy32 = pitch >> 2;
#ifdef _M_SSE
	if (((uintptr_t)ysrcp & 0xF) == 0 && (pitch & 0xF) == 0) {
		__m128i *dest = (__m128i *)texptr;
		// The pitch parameter is in bytes, so shift down for 128-bit.
		// Note: it's always aligned to 16 bytes, so this is safe.
		const u32 pitchBy128 = pitch >> 4;
		for (int by = 0; by < byc; by++) {
			const __m128i *xsrc = (const __m128i *)ysrcp;
			for (int bx = 0; bx < bxc; bx++) {
				const __m128i *src = xsrc;
				for (int n = 0; n < 2; n++) {
					// Textures are always 16-byte aligned so this is fine.
					__m128i temp1 = _mm_load_si128(src);
					src += pitchBy128;
					__m128i temp2 = _mm_load_si128(src);
					src += pitchBy128;
					__m128i temp3 = _mm_load_si128(src);
					src += pitchBy128;
					__m128i temp4 = _mm_load_si128(src);
					src += pitchBy128;

					_mm_store_si128(dest, temp1);
					_mm_store_si128(dest + 1, temp2);
					_mm_store_si128(dest + 2, temp3);
					_mm_store_si128(dest + 3, temp4);
					dest += 4;
				}
				xsrc++;
			}
			ysrcp += pitchBy32 * 8;
		}
	} else
#endif
	{
		u32 *dest = (u32 *)texptr;
		for (int by = 0; by < byc; by++) {
			const u32 *xsrc = ysrcp;
			for (int bx = 0; bx < bxc; bx++) {
				const u32 *src = xsrc;
				for (int n = 0; n < 8; n++) {
					memcpy(dest, src, 16);
					src += pitchBy32;
					dest += 4;
				}
				xsrc += 4;
			}
			ysrcp += pitchBy32 * 8;
		}
	}
}

void DoUnswizzleTex16(const u8 *texptr, u32 *ydestp, int bxc, int byc, u32 pitch) {
	// ydestp is in 32-bits, so this is convenient.
	const u32 pitchBy32 = pitch >> 2;

#ifdef _M_SSE
	// This check is pretty much a given, right?
	if (((uintptr_t)ydestp & 0xF) == 0 && (pitch & 0xF) == 0) {
		const __m128i *src = (const __m128i *)texptr;
		// The pitch parameter is in bytes, so shift down for 128-bit.
		// Note: it's always aligned to 16 bytes, so this is safe.
		const u32 pitchBy128 = pitch >> 4;
		for (int by = 0; by < byc; by++) {
			__m128i *xdest = (__m128i *)ydestp;
			for (int bx = 0; bx < bxc; bx++) {
				__m128i *dest = xdest;
				for (int n = 0; n < 2; n++) {
					// Textures are always 16-byte aligned so this is fine.
					__m128i temp1 = _mm_load_si128(src);
					__m128i temp2 = _mm_load_si128(src + 1);
					__m128i temp3 = _mm_load_si128(src + 2);
					__m128i temp4 = _mm_load_si128(src + 3);
					_mm_store_si128(dest, temp1);
					dest += pitchBy128;
					_mm_store_si128(dest, temp2);
					dest += pitchBy128;
					_mm_store_si128(dest, temp3);
					dest += pitchBy128;
					_mm_store_si128(dest, temp4);
					dest += pitchBy128;
					src += 4;
				}
				xdest++;
			}
			ydestp += pitchBy32 * 8;
		}
	} else
#elif PPSSPP_ARCH(ARM_NEON)
	if (((uintptr_t)ydestp & 0xF) == 0 && (pitch & 0xF) == 0) {
		// TODO: Does this really do anything meaningful? 
		__builtin_prefetch(texptr, 0, 0);

		const u32 *src = (const u32 *)texptr;
		for (int by = 0; by < byc; by++) {
			u32 *xdest = ydestp;
			for (int bx = 0; bx < bxc; bx++) {
				u32 *dest = xdest;
				for (int n = 0; n < 2; n++) {
					// Textures are always 16-byte aligned so this is fine.
					uint32x4_t temp1 = vld1q_u32(src);
					uint32x4_t temp2 = vld1q_u32(src + 4);
					uint32x4_t temp3 = vld1q_u32(src + 8);
					uint32x4_t temp4 = vld1q_u32(src + 12);
					vst1q_u32(dest, temp1);
					dest += pitchBy32;
					vst1q_u32(dest, temp2);
					dest += pitchBy32;
					vst1q_u32(dest, temp3);
					dest += pitchBy32;
					vst1q_u32(dest, temp4);
					dest += pitchBy32;
					src += 16;
				}
				xdest += 4;
			}
			ydestp += pitchBy32 * 8;
		}
	} else
#endif
	{
		const u32 *src = (const u32 *)texptr;
		for (int by = 0; by < byc; by++) {
			u32 *xdest = ydestp;
			for (int bx = 0; bx < bxc; bx++) {
				u32 *dest = xdest;
				for (int n = 0; n < 8; n++) {
					memcpy(dest, src, 16);
					dest += pitchBy32;
					src += 4;
				}
				xdest += 4;
			}
			ydestp += pitchBy32 * 8;
		}
	}
}

// S3TC / DXT Decoder
class DXTDecoder {
public:
	inline void DecodeColors(const DXT1Block *src, bool ignore1bitAlpha);
	inline void DecodeAlphaDXT5(const DXT5Block *src);
	inline void WriteColorsDXT1(u32 *dst, const DXT1Block *src, int pitch, int height);
	inline void WriteColorsDXT3(u32 *dst, const DXT3Block *src, int pitch, int height);
	inline void WriteColorsDXT5(u32 *dst, const DXT5Block *src, int pitch, int height);

protected:
	u32 colors_[4];
	u8 alpha_[8];
};

static inline u32 makecol(int r, int g, int b, int a) {
	return (a << 24) | (b << 16) | (g << 8) | r;
}

static inline int mix_2_3(int c1, int c2) {
	return (c1 + c1 + c2) / 3;
}

// This could probably be done faster by decoding two or four blocks at a time with SSE/NEON.
void DXTDecoder::DecodeColors(const DXT1Block *src, bool ignore1bitAlpha) {
	u16 c1 = src->color1;
	u16 c2 = src->color2;
	int blue1 = (c1 << 3) & 0xF8;
	int blue2 = (c2 << 3) & 0xF8;
	int green1 = (c1 >> 3) & 0xFC;
	int green2 = (c2 >> 3) & 0xFC;
	int red1 = (c1 >> 8) & 0xF8;
	int red2 = (c2 >> 8) & 0xF8;

	// Keep alpha zero for non-DXT1 to skip masking the colors.
	int alpha = ignore1bitAlpha ? 0 : 255;

	colors_[0] = makecol(red1, green1, blue1, alpha);
	colors_[1] = makecol(red2, green2, blue2, alpha);
	if (c1 > c2) {
		colors_[2] = makecol(mix_2_3(red1, red2), mix_2_3(green1, green2), mix_2_3(blue1, blue2), alpha);
		colors_[3] = makecol(mix_2_3(red2, red1), mix_2_3(green2, green1), mix_2_3(blue2, blue1), alpha);
	} else {
		// Average - these are always left shifted, so no need to worry about ties.
		int red3 = (red1 + red2) / 2;
		int green3 = (green1 + green2) / 2;
		int blue3 = (blue1 + blue2) / 2;
		colors_[2] = makecol(red3, green3, blue3, alpha);
		colors_[3] = makecol(0, 0, 0, 0);
	}
}

static inline u8 lerp8(const DXT5Block *src, int n) {
	// These weights multiple alpha1/alpha2 to fixed 8.8 point.
	int alpha1 = (src->alpha1 * ((7 - n) << 8)) / 7;
	int alpha2 = (src->alpha2 * (n << 8)) / 7;
	return (u8)((alpha1 + alpha2 + 31) >> 8);
}

static inline u8 lerp6(const DXT5Block *src, int n) {
	int alpha1 = (src->alpha1 * ((5 - n) << 8)) / 5;
	int alpha2 = (src->alpha2 * (n << 8)) / 5;
	return (u8)((alpha1 + alpha2 + 31) >> 8);
}

void DXTDecoder::DecodeAlphaDXT5(const DXT5Block *src) {
	alpha_[0] = src->alpha1;
	alpha_[1] = src->alpha2;
	if (alpha_[0] > alpha_[1]) {
		alpha_[2] = lerp8(src, 1);
		alpha_[3] = lerp8(src, 2);
		alpha_[4] = lerp8(src, 3);
		alpha_[5] = lerp8(src, 4);
		alpha_[6] = lerp8(src, 5);
		alpha_[7] = lerp8(src, 6);
	} else {
		alpha_[2] = lerp6(src, 1);
		alpha_[3] = lerp6(src, 2);
		alpha_[4] = lerp6(src, 3);
		alpha_[5] = lerp6(src, 4);
		alpha_[6] = 0;
		alpha_[7] = 255;
	}
}

void DXTDecoder::WriteColorsDXT1(u32 *dst, const DXT1Block *src, int pitch, int height) {
	for (int y = 0; y < height; y++) {
		int colordata = src->lines[y];
		for (int x = 0; x < 4; x++) {
			dst[x] = colors_[colordata & 3];
			colordata >>= 2;
		}
		dst += pitch;
	}
}

void DXTDecoder::WriteColorsDXT3(u32 *dst, const DXT3Block *src, int pitch, int height) {
	for (int y = 0; y < height; y++) {
		int colordata = src->color.lines[y];
		u32 alphadata = src->alphaLines[y];
		for (int x = 0; x < 4; x++) {
			dst[x] = colors_[colordata & 3] | (alphadata << 28);
			colordata >>= 2;
			alphadata >>= 4;
		}
		dst += pitch;
	}
}

void DXTDecoder::WriteColorsDXT5(u32 *dst, const DXT5Block *src, int pitch, int height) {
	// 48 bits, 3 bit index per pixel, 12 bits per line.
	u64 alphadata = ((u64)(u16)src->alphadata1 << 32) | (u32)src->alphadata2;

	for (int y = 0; y < height; y++) {
		int colordata = src->color.lines[y];
		for (int x = 0; x < 4; x++) {
			dst[x] = colors_[colordata & 3] | (alpha_[alphadata & 7] << 24);
			colordata >>= 2;
			alphadata >>= 3;
		}
		dst += pitch;
	}
}

uint32_t GetDXTTexelColor(const DXT1Block *src, int x, int y, int alpha) {
	_dbg_assert_(x >= 0 && x < 4);
	_dbg_assert_(y >= 0 && y < 4);

	uint16_t c1 = src->color1;
	uint16_t c2 = src->color2;
	int blue1 = (c1 << 3) & 0xF8;
	int blue2 = (c2 << 3) & 0xF8;
	int green1 = (c1 >> 3) & 0xFC;
	int green2 = (c2 >> 3) & 0xFC;
	int red1 = (c1 >> 8) & 0xF8;
	int red2 = (c2 >> 8) & 0xF8;

	int colorIndex = (src->lines[y] >> (x * 2)) & 3;
	if (colorIndex == 0) {
		return makecol(red1, green1, blue1, alpha);
	} else if (colorIndex == 1) {
		return makecol(red2, green2, blue2, alpha);
	} else if (c1 > c2) {
		if (colorIndex == 2) {
			return makecol(mix_2_3(red1, red2), mix_2_3(green1, green2), mix_2_3(blue1, blue2), alpha);
		}
		return makecol(mix_2_3(red2, red1), mix_2_3(green2, green1), mix_2_3(blue2, blue1), alpha);
	} else if (colorIndex == 3) {
		return makecol(0, 0, 0, 0);
	}

	// Average - these are always left shifted, so no need to worry about ties.
	int red3 = (red1 + red2) / 2;
	int green3 = (green1 + green2) / 2;
	int blue3 = (blue1 + blue2) / 2;
	return makecol(red3, green3, blue3, alpha);
}

uint32_t GetDXT1Texel(const DXT1Block *src, int x, int y) {
	return GetDXTTexelColor(src, x, y, 255);
}

uint32_t GetDXT3Texel(const DXT3Block *src, int x, int y) {
	uint32_t color = GetDXTTexelColor(&src->color, x, y, 0);
	u32 alpha = (src->alphaLines[y] >> (x * 4)) & 0xF;
	return color | (alpha << 28);
}

uint32_t GetDXT5Texel(const DXT5Block *src, int x, int y) {
	uint32_t color = GetDXTTexelColor(&src->color, x, y, 0);
	uint64_t alphadata = ((uint64_t)(uint16_t)src->alphadata1 << 32) | (uint32_t)src->alphadata2;
	int alphaIndex = (alphadata >> (y * 12 + x * 3)) & 7;

	if (alphaIndex == 0) {
		return color | (src->alpha1 << 24);
	} else if (alphaIndex == 1) {
		return color | (src->alpha2 << 24);
	} else if (src->alpha1 > src->alpha2) {
		return color | (lerp8(src, alphaIndex - 1) << 24);
	} else if (alphaIndex == 6) {
		return color;
	} else if (alphaIndex == 7) {
		return color | 0xFF000000;
	}
	return color | (lerp6(src, alphaIndex - 1) << 24);
}

// This could probably be done faster by decoding two or four blocks at a time with SSE/NEON.
void DecodeDXT1Block(u32 *dst, const DXT1Block *src, int pitch, int height, bool ignore1bitAlpha) {
	DXTDecoder dxt;
	dxt.DecodeColors(src, ignore1bitAlpha);
	dxt.WriteColorsDXT1(dst, src, pitch, height);
}

void DecodeDXT3Block(u32 *dst, const DXT3Block *src, int pitch, int height) {
	DXTDecoder dxt;
	dxt.DecodeColors(&src->color, true);
	dxt.WriteColorsDXT3(dst, src, pitch, height);
}

void DecodeDXT5Block(u32 *dst, const DXT5Block *src, int pitch, int height) {
	DXTDecoder dxt;
	dxt.DecodeColors(&src->color, true);
	dxt.DecodeAlphaDXT5(src);
	dxt.WriteColorsDXT5(dst, src, pitch, height);
}

#ifdef _M_SSE
static inline u32 CombineSSEBitsToDWORD(const __m128i &v) {
	__m128i temp;
	temp = _mm_or_si128(v, _mm_srli_si128(v, 8));
	temp = _mm_or_si128(temp, _mm_srli_si128(temp, 4));
	return _mm_cvtsi128_si32(temp);
}

CheckAlphaResult CheckAlphaRGBA8888SSE2(const u32 *pixelData, int stride, int w, int h) {
	const __m128i mask = _mm_set1_epi32(0xFF000000);

	const __m128i *p = (const __m128i *)pixelData;
	const int w4 = w / 4;
	const int stride4 = stride / 4;

	__m128i bits = mask;
	for (int y = 0; y < h; ++y) {
		for (int i = 0; i < w4; ++i) {
			const __m128i a = _mm_load_si128(&p[i]);
			bits = _mm_and_si128(bits, a);
		}

		__m128i result = _mm_xor_si128(bits, mask);
		if (CombineSSEBitsToDWORD(result) != 0) {
			return CHECKALPHA_ANY;
		}

		p += stride4;
	}

	return CHECKALPHA_FULL;
}

CheckAlphaResult CheckAlphaABGR4444SSE2(const u32 *pixelData, int stride, int w, int h) {
	const __m128i mask = _mm_set1_epi16((short)0x000F);

	const __m128i *p = (const __m128i *)pixelData;
	const int w8 = w / 8;
	const int stride8 = stride / 8;

	__m128i bits = mask;
	for (int y = 0; y < h; ++y) {
		for (int i = 0; i < w8; ++i) {
			const __m128i a = _mm_load_si128(&p[i]);
			bits = _mm_and_si128(bits, a);
		}

		__m128i result = _mm_xor_si128(bits, mask);
		if (CombineSSEBitsToDWORD(result) != 0) {
			return CHECKALPHA_ANY;
		}

		p += stride8;
	}

	return CHECKALPHA_FULL;
}

CheckAlphaResult CheckAlphaABGR1555SSE2(const u32 *pixelData, int stride, int w, int h) {
	const __m128i mask = _mm_set1_epi16((short)0x0001);

	const __m128i *p = (const __m128i *)pixelData;
	const int w8 = w / 8;
	const int stride8 = stride / 8;

	__m128i bits = mask;
	for (int y = 0; y < h; ++y) {
		for (int i = 0; i < w8; ++i) {
			const __m128i a = _mm_load_si128(&p[i]);
			bits = _mm_and_si128(bits, a);
		}

		__m128i result = _mm_xor_si128(bits, mask);
		if (CombineSSEBitsToDWORD(result) != 0) {
			return CHECKALPHA_ANY;
		}

		p += stride8;
	}

	return CHECKALPHA_FULL;
}

CheckAlphaResult CheckAlphaRGBA4444SSE2(const u32 *pixelData, int stride, int w, int h) {
	const __m128i mask = _mm_set1_epi16((short)0xF000);

	const __m128i *p = (const __m128i *)pixelData;
	const int w8 = w / 8;
	const int stride8 = stride / 8;

	__m128i bits = mask;
	for (int y = 0; y < h; ++y) {
		for (int i = 0; i < w8; ++i) {
			const __m128i a = _mm_load_si128(&p[i]);
			bits = _mm_and_si128(bits, a);
		}

		__m128i result = _mm_xor_si128(bits, mask);
		if (CombineSSEBitsToDWORD(result) != 0) {
			return CHECKALPHA_ANY;
		}

		p += stride8;
	}

	return CHECKALPHA_FULL;
}

CheckAlphaResult CheckAlphaRGBA5551SSE2(const u32 *pixelData, int stride, int w, int h) {
	const __m128i mask = _mm_set1_epi16((short)0x8000);

	const __m128i *p = (const __m128i *)pixelData;
	const int w8 = w / 8;
	const int stride8 = stride / 8;

	__m128i bits = mask;
	for (int y = 0; y < h; ++y) {
		for (int i = 0; i < w8; ++i) {
			const __m128i a = _mm_load_si128(&p[i]);
			bits = _mm_and_si128(bits, a);
		}

		__m128i result = _mm_xor_si128(bits, mask);
		if (CombineSSEBitsToDWORD(result) != 0) {
			return CHECKALPHA_ANY;
		}

		p += stride8;
	}

	return CHECKALPHA_FULL;
}

#endif  // _M_SSE

#if PPSSPP_ARCH(ARM_NEON)

static inline bool VectorIsNonZeroNEON(const uint32x4_t &v) {
	u64 low = vgetq_lane_u64(vreinterpretq_u64_u32(v), 0);
	u64 high = vgetq_lane_u64(vreinterpretq_u64_u32(v), 1);

	return (low | high) != 0;
}

#ifndef _MSC_VER
// MSVC consider this function the same as the one above! uint16x8_t is typedef'd to the same type as uint32x4_t.
static inline bool VectorIsNonZeroNEON(const uint16x8_t &v) {
	u64 low = vgetq_lane_u64(vreinterpretq_u64_u16(v), 0);
	u64 high = vgetq_lane_u64(vreinterpretq_u64_u16(v), 1);

	return (low | high) != 0;
}
#endif

CheckAlphaResult CheckAlphaRGBA8888NEON(const u32 *pixelData, int stride, int w, int h) {
	const u32 *p = (const u32 *)pixelData;

	const uint32x4_t mask = vdupq_n_u32(0xFF000000);
	uint32x4_t bits = mask;
	for (int y = 0; y < h; ++y) {
		for (int i = 0; i < w; i += 4) {
			const uint32x4_t a = vld1q_u32(&p[i]);
			bits = vandq_u32(bits, a);
		}

		uint32x4_t result = veorq_u32(bits, mask);
		if (VectorIsNonZeroNEON(result)) {
			return CHECKALPHA_ANY;
		}

		p += stride;
	}

	return CHECKALPHA_FULL;
}

CheckAlphaResult CheckAlphaABGR4444NEON(const u32 *pixelData, int stride, int w, int h) {
	const u16 *p = (const u16 *)pixelData;

	const uint16x8_t mask = vdupq_n_u16((u16)0x000F);
	uint16x8_t bits = mask;
	for (int y = 0; y < h; ++y) {
		for (int i = 0; i < w; i += 8) {
			const uint16x8_t a = vld1q_u16(&p[i]);
			bits = vandq_u16(bits, a);
		}

		uint16x8_t result = veorq_u16(bits, mask);
		if (VectorIsNonZeroNEON(result)) {
			return CHECKALPHA_ANY;
		}

		p += stride;
	}

	return CHECKALPHA_FULL;
}

CheckAlphaResult CheckAlphaABGR1555NEON(const u32 *pixelData, int stride, int w, int h) {
	const u16 *p = (const u16 *)pixelData;

	const uint16x8_t mask = vdupq_n_u16((u16)0x0001);
	uint16x8_t bits = mask;
	for (int y = 0; y < h; ++y) {
		for (int i = 0; i < w; i += 8) {
			const uint16x8_t a = vld1q_u16(&p[i]);
			bits = vandq_u16(bits, a);
		}

		uint16x8_t result = veorq_u16(bits, mask);
		if (VectorIsNonZeroNEON(result)) {
			return CHECKALPHA_ANY;
		}

		p += stride;
	}

	return CHECKALPHA_FULL;
}

CheckAlphaResult CheckAlphaRGBA4444NEON(const u32 *pixelData, int stride, int w, int h) {
	const u16 *p = (const u16 *)pixelData;

	const uint16x8_t mask = vdupq_n_u16((u16)0xF000);
	uint16x8_t bits = mask;
	for (int y = 0; y < h; ++y) {
		for (int i = 0; i < w; i += 8) {
			const uint16x8_t a = vld1q_u16(&p[i]);
			bits = vandq_u16(bits, a);
		}

		uint16x8_t result = veorq_u16(bits, mask);
		if (VectorIsNonZeroNEON(result)) {
			return CHECKALPHA_ANY;
		}

		p += stride;
	}

	return CHECKALPHA_FULL;
}

CheckAlphaResult CheckAlphaRGBA5551NEON(const u32 *pixelData, int stride, int w, int h) {
	const u16 *p = (const u16 *)pixelData;

	const uint16x8_t mask = vdupq_n_u16((u16)0x8000);
	uint16x8_t bits = mask;
	for (int y = 0; y < h; ++y) {
		for (int i = 0; i < w; i += 8) {
			const uint16x8_t a = vld1q_u16(&p[i]);
			bits = vandq_u16(bits, a);
		}

		uint16x8_t result = veorq_u16(bits, mask);
		if (VectorIsNonZeroNEON(result)) {
			return CHECKALPHA_ANY;
		}

		p += stride;
	}

	return CHECKALPHA_FULL;
}

#endif

CheckAlphaResult CheckAlphaRGBA8888Basic(const u32 *pixelData, int stride, int w, int h) {
	// Use SIMD if aligned to 16 bytes / 4 pixels (almost always the case.)
	if ((w & 3) == 0 && (stride & 3) == 0) {
#ifdef _M_SSE
		return CheckAlphaRGBA8888SSE2(pixelData, stride, w, h);
#elif PPSSPP_ARCH(ARM_NEON)
		if (cpu_info.bNEON) {
			return CheckAlphaRGBA8888NEON(pixelData, stride, w, h);
		}
#endif
	}

	const u32 *p = pixelData;
	for (int y = 0; y < h; ++y) {
		u32 bits = 0xFF000000;
		for (int i = 0; i < w; ++i) {
			bits &= p[i];
		}

		if (bits != 0xFF000000) {
			// We're done, we hit non-full alpha.
			return CHECKALPHA_ANY;
		}

		p += stride;
	}

	return CHECKALPHA_FULL;
}

CheckAlphaResult CheckAlphaABGR4444Basic(const u32 *pixelData, int stride, int w, int h) {
	// Use SIMD if aligned to 16 bytes / 8 pixels (usually the case.)
	if ((w & 7) == 0 && (stride & 7) == 0) {
#ifdef _M_SSE
		return CheckAlphaABGR4444SSE2(pixelData, stride, w, h);
#elif PPSSPP_ARCH(ARM_NEON)
		if (cpu_info.bNEON) {
			return CheckAlphaABGR4444NEON(pixelData, stride, w, h);
		}
#endif
	}

	const u32 *p = pixelData;
	const int w2 = (w + 1) / 2;
	const int stride2 = (stride + 1) / 2;

	for (int y = 0; y < h; ++y) {
		u32 bits = 0x000F000F;
		for (int i = 0; i < w2; ++i) {
			bits &= p[i];
		}

		if (bits != 0x000F000F) {
			// We're done, we hit non-full alpha.
			return CHECKALPHA_ANY;
		}

		p += stride2;
	}

	return CHECKALPHA_FULL;
}

CheckAlphaResult CheckAlphaABGR1555Basic(const u32 *pixelData, int stride, int w, int h) {
	// Use SIMD if aligned to 16 bytes / 8 pixels (usually the case.)
	if ((w & 7) == 0 && (stride & 7) == 0) {
#ifdef _M_SSE
		return CheckAlphaABGR1555SSE2(pixelData, stride, w, h);
#elif PPSSPP_ARCH(ARM_NEON)
		if (cpu_info.bNEON) {
			return CheckAlphaABGR1555NEON(pixelData, stride, w, h);
		}
#endif
	}

	const u32 *p = pixelData;
	const int w2 = (w + 1) / 2;
	const int stride2 = (stride + 1) / 2;

	for (int y = 0; y < h; ++y) {
		u32 bits = 0x00010001;
		for (int i = 0; i < w2; ++i) {
			bits &= p[i];
		}

		if (bits != 0x00010001) {
			return CHECKALPHA_ANY;
		}

		p += stride2;
	}

	return CHECKALPHA_FULL;
}

CheckAlphaResult CheckAlphaRGBA4444Basic(const u32 *pixelData, int stride, int w, int h) {
	// Use SSE if aligned to 16 bytes / 8 pixels (usually the case.)
	if ((w & 7) == 0 && (stride & 7) == 0) {
#ifdef _M_SSE
		return CheckAlphaRGBA4444SSE2(pixelData, stride, w, h);
#elif PPSSPP_ARCH(ARM_NEON)
		if (cpu_info.bNEON) {
			return CheckAlphaRGBA4444NEON(pixelData, stride, w, h);
		}
#endif
	}

	const u32 *p = pixelData;
	const int w2 = (w + 1) / 2;
	const int stride2 = (stride + 1) / 2;

	for (int y = 0; y < h; ++y) {
		u32 bits = 0xF000F000;
		for (int i = 0; i < w2; ++i) {
			bits &= p[i];
		}

		if (bits != 0xF000F000) {
			// We're done, we hit non-full alpha.
			return CHECKALPHA_ANY;
		}

		p += stride2;
	}

	return CHECKALPHA_FULL;
}

CheckAlphaResult CheckAlphaRGBA5551Basic(const u32 *pixelData, int stride, int w, int h) {
	// Use SSE if aligned to 16 bytes / 8 pixels (usually the case.)
	if ((w & 7) == 0 && (stride & 7) == 0) {
#ifdef _M_SSE
		return CheckAlphaRGBA5551SSE2(pixelData, stride, w, h);
#elif PPSSPP_ARCH(ARM_NEON)
		if (cpu_info.bNEON) {
			return CheckAlphaRGBA5551NEON(pixelData, stride, w, h);
		}
#endif
	}

	const u32 *p = pixelData;
	const int w2 = (w + 1) / 2;
	const int stride2 = (stride + 1) / 2;

	for (int y = 0; y < h; ++y) {
		u32 bits = 0x80008000;
		for (int i = 0; i < w2; ++i) {
			bits &= p[i];
		}

		if (bits != 0x80008000) {
			return CHECKALPHA_ANY;
		}

		p += stride2;
	}

	return CHECKALPHA_FULL;
}
