// SPDX-License-Identifier: Apache-2.0
// ----------------------------------------------------------------------------
// Copyright 2011-2020 Arm Limited
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy
// of the License at:
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations
// under the License.
// ----------------------------------------------------------------------------

/**
 * @brief Functions for creating in-memory ASTC image structures.
 */

#include <cassert>
#include <cstring>

#include "astcenc_internal.h"

// conversion functions between the LNS representation and the FP16 representation.
static float float_to_lns(float p)
{
	if (astc::isnan(p) || p <= 1.0f / 67108864.0f)
	{
		// underflow or NaN value, return 0.
		// We count underflow if the input value is smaller than 2^-26.
		return 0.0f;
	}

	if (fabsf(p) >= 65536.0f)
	{
		// overflow, return a +INF value
		return 65535.0f;
	}

	int expo;
	float normfrac = frexpf(p, &expo);
	float p1;
	if (expo < -13)
	{
		// input number is smaller than 2^-14. In this case, multiply by 2^25.
		p1 = p * 33554432.0f;
		expo = 0;
	}
	else
	{
		expo += 14;
		p1 = (normfrac - 0.5f) * 4096.0f;
	}

	if (p1 < 384.0f)
		p1 *= 4.0f / 3.0f;
	else if (p1 <= 1408.0f)
		p1 += 128.0f;
	else
		p1 = (p1 + 512.0f) * (4.0f / 5.0f);

	p1 += ((float)expo) * 2048.0f;
	return p1 + 1.0f;
}

static uint16_t lns_to_sf16(uint16_t p)
{
	uint16_t mc = p & 0x7FF;
	uint16_t ec = p >> 11;
	uint16_t mt;
	if (mc < 512)
		mt = 3 * mc;
	else if (mc < 1536)
		mt = 4 * mc - 512;
	else
		mt = 5 * mc - 2048;

	uint16_t res = (ec << 10) | (mt >> 3);
	if (res >= 0x7BFF)
		res = 0x7BFF;
	return res;
}

// conversion function from 16-bit LDR value to FP16.
// note: for LDR interpolation, it is impossible to get a denormal result;
// this simplifies the conversion.
// FALSE; we can receive a very small UNORM16 through the constant-block.
uint16_t unorm16_to_sf16(uint16_t p)
{
	if (p == 0xFFFF)
		return 0x3C00;			// value of 1.0 .
	if (p < 4)
		return p << 8;

	int lz = clz32(p) - 16;
	p <<= (lz + 1);
	p >>= 6;
	p |= (14 - lz) << 10;
	return p;
}

void imageblock_initialize_deriv(
	const imageblock* pb,
	int pixelcount,
	float4* dptr
) {
	for (int i = 0; i < pixelcount; i++)
	{
		// compute derivatives for RGB first
		if (pb->rgb_lns[i])
		{
			float3 fdata = float3(pb->data_r[i], pb->data_g[i], pb->data_b[i]);
			fdata.r = sf16_to_float(lns_to_sf16((uint16_t)fdata.r));
			fdata.g = sf16_to_float(lns_to_sf16((uint16_t)fdata.g));
			fdata.b = sf16_to_float(lns_to_sf16((uint16_t)fdata.b));

			float r = MAX(fdata.r, 6e-5f);
			float g = MAX(fdata.g, 6e-5f);
			float b = MAX(fdata.b, 6e-5f);

			float rderiv = (float_to_lns(r * 1.05f) - float_to_lns(r)) / (r * 0.05f);
			float gderiv = (float_to_lns(g * 1.05f) - float_to_lns(g)) / (g * 0.05f);
			float bderiv = (float_to_lns(b * 1.05f) - float_to_lns(b)) / (b * 0.05f);

			// the derivative may not actually take values smaller than 1/32 or larger than 2^25;
			// if it does, we clamp it.
			if (rderiv < (1.0f / 32.0f))
			{
				rderiv = (1.0f / 32.0f);
			}
			else if (rderiv > 33554432.0f)
			{
				rderiv = 33554432.0f;
			}

			if (gderiv < (1.0f / 32.0f))
			{
				gderiv = (1.0f / 32.0f);
			}
			else if (gderiv > 33554432.0f)
			{
				gderiv = 33554432.0f;
			}

			if (bderiv < (1.0f / 32.0f))
			{
				bderiv = (1.0f / 32.0f);
			}
			else if (bderiv > 33554432.0f)
			{
				bderiv = 33554432.0f;
			}

			dptr->r = rderiv;
			dptr->g = gderiv;
			dptr->b = bderiv;
		}
		else
		{
			dptr->r = 65535.0f;
			dptr->g = 65535.0f;
			dptr->b = 65535.0f;
		}

		// then compute derivatives for Alpha
		if (pb->alpha_lns[i])
		{
			float fdata = pb->data_a[i];
			fdata = sf16_to_float(lns_to_sf16((uint16_t)fdata));

			float a = MAX(fdata, 6e-5f);
			float aderiv = (float_to_lns(a * 1.05f) - float_to_lns(a)) / (a * 0.05f);
			// the derivative may not actually take values smaller than 1/32 or larger than 2^25;
			// if it does, we clamp it.
			if (aderiv < (1.0f / 32.0f))
			{
				aderiv = (1.0f / 32.0f);
			}
			else if (aderiv > 33554432.0f)
			{
				aderiv = 33554432.0f;
			}

			dptr->a = aderiv;
		}
		else
		{
			dptr->a = 65535.0f;
		}

		dptr++;
	}
}

// helper function to initialize the work-data from the orig-data
void imageblock_initialize_work_from_orig(
	imageblock* pb,
	int pixelcount
) {
	pb->origin_texel = float4(pb->data_r[0], pb->data_g[0],
	                          pb->data_b[0], pb->data_a[0]);

	for (int i = 0; i < pixelcount; i++)
	{
		float4 inc = float4(pb->data_r[i], pb->data_g[i],
		                    pb->data_b[i], pb->data_a[i]);

		if (pb->rgb_lns[i])
		{
			pb->data_r[i] = float_to_lns(inc.r);
			pb->data_g[i] = float_to_lns(inc.g);
			pb->data_b[i] = float_to_lns(inc.b);
		}
		else
		{
			pb->data_r[i] = inc.r * 65535.0f;
			pb->data_g[i] = inc.g * 65535.0f;
			pb->data_b[i] = inc.b * 65535.0f;
		}

		if (pb->alpha_lns[i])
		{
			pb->data_a[i] = float_to_lns(inc.a);
		}
		else
		{
			pb->data_a[i] = inc.a * 65535.0f;
		}
	}
}

// helper function to initialize the orig-data from the work-data
void imageblock_initialize_orig_from_work(
	imageblock* pb,
	int pixelcount
) {
	for (int i = 0; i < pixelcount; i++)
	{
		float4 inc = float4(pb->data_r[i], pb->data_g[i],
		                    pb->data_b[i], pb->data_a[i]);

		if (pb->rgb_lns[i])
		{
			pb->data_r[i] = sf16_to_float(lns_to_sf16((uint16_t)inc.r));
			pb->data_g[i] = sf16_to_float(lns_to_sf16((uint16_t)inc.g));
			pb->data_b[i] = sf16_to_float(lns_to_sf16((uint16_t)inc.b));
		}
		else
		{
			pb->data_r[i] = sf16_to_float(unorm16_to_sf16((uint16_t)inc.r));
			pb->data_g[i] = sf16_to_float(unorm16_to_sf16((uint16_t)inc.g));
			pb->data_b[i] = sf16_to_float(unorm16_to_sf16((uint16_t)inc.b));
		}

		if (pb->alpha_lns[i])
		{
			pb->data_a[i] = sf16_to_float(lns_to_sf16((uint16_t)inc.a));
		}
		else
		{
			pb->data_a[i] = sf16_to_float(unorm16_to_sf16((uint16_t)inc.a));
		}
	}
}

// fetch an imageblock from the input file.
void fetch_imageblock(
	astcenc_profile decode_mode,
	const astcenc_image& img,
	imageblock* pb,	// picture-block to initialize with image data
	const block_size_descriptor* bsd,
	// position in texture.
	int xpos,
	int ypos,
	int zpos,
	astcenc_swizzle swz
) {
	int xsize = img.dim_x;
	int ysize = img.dim_y;
	int zsize = img.dim_z;

	pb->xpos = xpos;
	pb->ypos = ypos;
	pb->zpos = zpos;

	// True if any non-identity swizzle
	bool needs_swz = (swz.r != ASTCENC_SWZ_R) || (swz.g != ASTCENC_SWZ_G) ||
	                 (swz.b != ASTCENC_SWZ_B) || (swz.a != ASTCENC_SWZ_A);

	int idx = 0;
	if (img.data_type == ASTCENC_TYPE_U8)
	{
		uint8_t data[6];
		data[ASTCENC_SWZ_0] = 0x00;
		data[ASTCENC_SWZ_1] = 0xFF;

		for (int z = 0; z < bsd->zdim; z++)
		{
			int zi = astc::clamp(zpos + z, 0, zsize - 1);
			uint8_t* data8 = static_cast<uint8_t*>(img.data[zi]);

			for (int y = 0; y < bsd->ydim; y++)
			{
				int yi = astc::clamp(ypos + y, 0, ysize - 1);

				for (int x = 0; x < bsd->xdim; x++)
				{
					int xi = astc::clamp(xpos + x, 0, xsize - 1);

					int r = data8[(4 * xsize * yi) + (4 * xi    )];
					int g = data8[(4 * xsize * yi) + (4 * xi + 1)];
					int b = data8[(4 * xsize * yi) + (4 * xi + 2)];
					int a = data8[(4 * xsize * yi) + (4 * xi + 3)];

					if (needs_swz)
					{
						data[ASTCENC_SWZ_R] = r;
						data[ASTCENC_SWZ_G] = g;
						data[ASTCENC_SWZ_B] = b;
						data[ASTCENC_SWZ_A] = a;

						r = data[swz.r];
						g = data[swz.g];
						b = data[swz.b];
						a = data[swz.a];
					}

					pb->data_r[idx] = r / 255.0f;
					pb->data_g[idx] = g / 255.0f;
					pb->data_b[idx] = b / 255.0f;
					pb->data_a[idx] = a / 255.0f;
					idx++;
				}
			}
		}
	}
	else if (img.data_type == ASTCENC_TYPE_F16)
	{
		uint16_t data[6];
		data[ASTCENC_SWZ_0] = 0x0000;
		data[ASTCENC_SWZ_1] = 0x3C00;

		for (int z = 0; z < bsd->zdim; z++)
		{
			int zi = astc::clamp(zpos + z, 0, zsize - 1);
			uint16_t* data16 = static_cast<uint16_t*>(img.data[zi]);

			for (int y = 0; y < bsd->ydim; y++)
			{
				int yi = astc::clamp(ypos + y, 0, ysize - 1);

				for (int x = 0; x < bsd->xdim; x++)
				{
					int xi = astc::clamp(xpos + x, 0, xsize - 1);

					int r = data16[(4 * xsize * yi) + (4 * xi    )];
					int g = data16[(4 * xsize * yi) + (4 * xi + 1)];
					int b = data16[(4 * xsize * yi) + (4 * xi + 2)];
					int a = data16[(4 * xsize * yi) + (4 * xi + 3)];

					if (needs_swz)
					{
						data[ASTCENC_SWZ_R] = r;
						data[ASTCENC_SWZ_G] = g;
						data[ASTCENC_SWZ_B] = b;
						data[ASTCENC_SWZ_A] = a;

						r = data[swz.r];
						g = data[swz.g];
						b = data[swz.b];
						a = data[swz.a];
					}

					pb->data_r[idx] = MAX(sf16_to_float(r), 1e-8f);
					pb->data_g[idx] = MAX(sf16_to_float(g), 1e-8f);
					pb->data_b[idx] = MAX(sf16_to_float(b), 1e-8f);
					pb->data_a[idx] = MAX(sf16_to_float(a), 1e-8f);
					idx++;
				}
			}
		}
	}
	else // if (img.data_type == ASTCENC_TYPE_F32)
	{
		assert(img.data_type == ASTCENC_TYPE_F32);

		float data[6];
		data[ASTCENC_SWZ_0] = 0.0f;
		data[ASTCENC_SWZ_1] = 1.0f;

		for (int z = 0; z < bsd->zdim; z++)
		{
			int zi = astc::clamp(zpos + z, 0, zsize - 1);
			float* data32 = static_cast<float*>(img.data[zi]);

			for (int y = 0; y < bsd->ydim; y++)
			{
				int yi = astc::clamp(ypos + y, 0, ysize - 1);

				for (int x = 0; x < bsd->xdim; x++)
				{
					int xi = astc::clamp(xpos + x, 0, xsize - 1);

					float r = data32[(4 * xsize * yi) + (4 * xi    )];
					float g = data32[(4 * xsize * yi) + (4 * xi + 1)];
					float b = data32[(4 * xsize * yi) + (4 * xi + 2)];
					float a = data32[(4 * xsize * yi) + (4 * xi + 3)];

					if (needs_swz)
					{
						data[ASTCENC_SWZ_R] = r;
						data[ASTCENC_SWZ_G] = g;
						data[ASTCENC_SWZ_B] = b;
						data[ASTCENC_SWZ_A] = a;

						r = data[swz.r];
						g = data[swz.g];
						b = data[swz.b];
						a = data[swz.a];
					}

					pb->data_r[idx] = MAX(r, 1e-8f);
					pb->data_g[idx] = MAX(g, 1e-8f);
					pb->data_b[idx] = MAX(b, 1e-8f);
					pb->data_a[idx] = MAX(a, 1e-8f);
					idx++;
				}
			}
		}
	}

	int rgb_lns = (decode_mode == ASTCENC_PRF_HDR) || (decode_mode == ASTCENC_PRF_HDR_RGB_LDR_A);
	int alpha_lns = decode_mode == ASTCENC_PRF_HDR;

	// impose the choice on every pixel when encoding.
	for (int i = 0; i < bsd->texel_count; i++)
	{
		pb->rgb_lns[i] = rgb_lns;
		pb->alpha_lns[i] = alpha_lns;
		pb->nan_texel[i] = 0;
	}

	imageblock_initialize_work_from_orig(pb, bsd->texel_count);
	update_imageblock_flags(pb, bsd->xdim, bsd->ydim, bsd->zdim);
}

void write_imageblock(
	astcenc_image& img,
	const imageblock* pb,	// picture-block to initialize with image data. We assume that orig_data is valid.
	const block_size_descriptor* bsd,
	// position to write the block to
	int xpos,
	int ypos,
	int zpos,
	astcenc_swizzle swz
) {
	const uint8_t *nptr = pb->nan_texel;
	int xsize = img.dim_x;
	int ysize = img.dim_y;
	int zsize = img.dim_z;

	float data[7];
	data[ASTCENC_SWZ_0] = 0.0f;
	data[ASTCENC_SWZ_1] = 1.0f;

	// True if any non-identity swizzle
	bool needs_swz = (swz.r != ASTCENC_SWZ_R) || (swz.g != ASTCENC_SWZ_G) ||
	                 (swz.b != ASTCENC_SWZ_B) || (swz.a != ASTCENC_SWZ_A);

	// True if any swizzle uses Z reconstruct
	bool needs_z = (swz.r == ASTCENC_SWZ_Z) || (swz.g == ASTCENC_SWZ_Z) ||
	               (swz.b == ASTCENC_SWZ_Z) || (swz.a == ASTCENC_SWZ_Z);

	int idx = 0;
	if (img.data_type == ASTCENC_TYPE_U8)
	{
		for (int z = 0; z < bsd->zdim; z++)
		{
			int zc = astc::clamp(zpos + z, 0, zsize - 1);
			uint8_t* data8 = static_cast<uint8_t*>(img.data[zc]);

			for (int y = 0; y < bsd->ydim; y++)
			{
				for (int x = 0; x < bsd->xdim; x++)
				{
					int xi = xpos + x;
					int yi = ypos + y;
					int zi = zpos + z;

					if (xi >= 0 && yi >= 0 && zi >= 0 && xi < xsize && yi < ysize && zi < zsize)
					{
						int ri, gi, bi, ai;

						if (*nptr)
						{
							// NaN-pixel, but we can't display it. Display purple instead.
							ri = 0xFF;
							gi = 0x00;
							bi = 0xFF;
							ai = 0xFF;
						}
						else if (needs_swz)
						{
							data[ASTCENC_SWZ_R] = pb->data_r[idx];
							data[ASTCENC_SWZ_G] = pb->data_g[idx];
							data[ASTCENC_SWZ_B] = pb->data_b[idx];
							data[ASTCENC_SWZ_A] = pb->data_a[idx];

							if (needs_z)
							{
								float xcoord = (data[0] * 2.0f) - 1.0f;
								float ycoord = (data[3] * 2.0f) - 1.0f;
								float zcoord = 1.0f - xcoord * xcoord - ycoord * ycoord;
								if (zcoord < 0.0f)
								{
									zcoord = 0.0f;
								}
								data[ASTCENC_SWZ_Z] = (astc::sqrt(zcoord) * 0.5f) + 0.5f;
							}

							ri = astc::flt2int_rtn(MIN(data[swz.r], 1.0f) * 255.0f);
							gi = astc::flt2int_rtn(MIN(data[swz.g], 1.0f) * 255.0f);
							bi = astc::flt2int_rtn(MIN(data[swz.b], 1.0f) * 255.0f);
							ai = astc::flt2int_rtn(MIN(data[swz.a], 1.0f) * 255.0f);
						}
						else
						{
							ri = astc::flt2int_rtn(MIN(pb->data_r[idx], 1.0f) * 255.0f);
							gi = astc::flt2int_rtn(MIN(pb->data_g[idx], 1.0f) * 255.0f);
							bi = astc::flt2int_rtn(MIN(pb->data_b[idx], 1.0f) * 255.0f);
							ai = astc::flt2int_rtn(MIN(pb->data_a[idx], 1.0f) * 255.0f);
						}

						data8[(4 * xsize * yi) + (4 * xi    )] = ri;
						data8[(4 * xsize * yi) + (4 * xi + 1)] = gi;
						data8[(4 * xsize * yi) + (4 * xi + 2)] = bi;
						data8[(4 * xsize * yi) + (4 * xi + 3)] = ai;
					}
					idx++;
					nptr++;
				}
			}
		}
	}
	else if (img.data_type == ASTCENC_TYPE_F16)
	{
		for (int z = 0; z < bsd->zdim; z++)
		{
			int zc = astc::clamp(zpos + z, 0, zsize - 1);
			uint16_t* data16 = static_cast<uint16_t*>(img.data[zc]);

			for (int y = 0; y < bsd->ydim; y++)
			{
				for (int x = 0; x < bsd->xdim; x++)
				{
					int xi = xpos + x;
					int yi = ypos + y;
					int zi = zpos + z;

					if (xi >= 0 && yi >= 0 && zi >= 0 && xi < xsize && yi < ysize && zi < zsize)
					{
						int ri, gi, bi, ai;

						if (*nptr)
						{
							ri = 0xFFFF;
							gi = 0xFFFF;
							bi = 0xFFFF;
							ai = 0xFFFF;
						}
						else if (needs_swz)
						{
							data[ASTCENC_SWZ_R] = pb->data_r[idx];
							data[ASTCENC_SWZ_G] = pb->data_g[idx];
							data[ASTCENC_SWZ_B] = pb->data_b[idx];
							data[ASTCENC_SWZ_A] = pb->data_a[idx];

							if (needs_z)
							{
								float xN = (data[0] * 2.0f) - 1.0f;
								float yN = (data[3] * 2.0f) - 1.0f;
								float zN = 1.0f - xN * xN - yN * yN;
								if (zN < 0.0f)
								{
									zN = 0.0f;
								}
								data[ASTCENC_SWZ_Z] = (astc::sqrt(zN) * 0.5f) + 0.5f;
							}

							ri = float_to_sf16(data[swz.r], SF_NEARESTEVEN);
							gi = float_to_sf16(data[swz.g], SF_NEARESTEVEN);
							bi = float_to_sf16(data[swz.b], SF_NEARESTEVEN);
							ai = float_to_sf16(data[swz.a], SF_NEARESTEVEN);
						}
						else
						{
							ri = float_to_sf16(pb->data_r[idx], SF_NEARESTEVEN);
							gi = float_to_sf16(pb->data_g[idx], SF_NEARESTEVEN);
							bi = float_to_sf16(pb->data_b[idx], SF_NEARESTEVEN);
							ai = float_to_sf16(pb->data_a[idx], SF_NEARESTEVEN);
						}

						data16[(4 * xsize * yi) + (4 * xi    )] = ri;
						data16[(4 * xsize * yi) + (4 * xi + 1)] = gi;
						data16[(4 * xsize * yi) + (4 * xi + 2)] = bi;
						data16[(4 * xsize * yi) + (4 * xi + 3)] = ai;
					}
					idx++;
					nptr++;
				}
			}
		}
	}
	else // if (img.data_type == ASTCENC_TYPE_F32)
	{
		assert(img.data_type == ASTCENC_TYPE_F32);

		for (int z = 0; z < bsd->zdim; z++)
		{
			int zc = astc::clamp(zpos + z, 0, zsize - 1);
			float* data32 = static_cast<float*>(img.data[zc]);

			for (int y = 0; y < bsd->ydim; y++)
			{
				for (int x = 0; x < bsd->xdim; x++)
				{
					int xi = xpos + x;
					int yi = ypos + y;
					int zi = zpos + z;

					if (xi >= 0 && yi >= 0 && zi >= 0 && xi < xsize && yi < ysize && zi < zsize)
					{
						float rf, gf, bf, af;

						if (*nptr)
						{
							rf = std::numeric_limits<float>::quiet_NaN();
							gf = std::numeric_limits<float>::quiet_NaN();
							bf = std::numeric_limits<float>::quiet_NaN();
							af = std::numeric_limits<float>::quiet_NaN();
						}
						else if (needs_swz)
						{
							data[ASTCENC_SWZ_R] = pb->data_r[idx];
							data[ASTCENC_SWZ_G] = pb->data_g[idx];
							data[ASTCENC_SWZ_B] = pb->data_b[idx];
							data[ASTCENC_SWZ_A] = pb->data_a[idx];

							if (needs_z)
							{
								float xN = (data[0] * 2.0f) - 1.0f;
								float yN = (data[3] * 2.0f) - 1.0f;
								float zN = 1.0f - xN * xN - yN * yN;
								if (zN < 0.0f)
								{
									zN = 0.0f;
								}
								data[ASTCENC_SWZ_Z] = (astc::sqrt(zN) * 0.5f) + 0.5f;
							}

							rf = data[swz.r];
							gf = data[swz.g];
							bf = data[swz.b];
							af = data[swz.a];
						}
						else
						{
							rf = pb->data_r[idx];
							gf = pb->data_g[idx];
							bf = pb->data_b[idx];
							af = pb->data_a[idx];
						}

						data32[(4 * xsize * yi) + (4 * xi    )] = rf;
						data32[(4 * xsize * yi) + (4 * xi + 1)] = gf;
						data32[(4 * xsize * yi) + (4 * xi + 2)] = bf;
						data32[(4 * xsize * yi) + (4 * xi + 3)] = af;
					}
					idx++;
					nptr++;
				}
			}
		}
	}
}

/*
   For an imageblock, update its flags.
   The updating is done based on data, not orig_data.
*/
void update_imageblock_flags(
	imageblock* pb,
	int xdim,
	int ydim,
	int zdim
) {
	float red_min = 1e38f, red_max = -1e38f;
	float green_min = 1e38f, green_max = -1e38f;
	float blue_min = 1e38f, blue_max = -1e38f;
	float alpha_min = 1e38f, alpha_max = -1e38f;

	int texels_per_block = xdim * ydim * zdim;

	int grayscale = 1;

	for (int i = 0; i < texels_per_block; i++)
	{
		float red = pb->data_r[i];
		float green = pb->data_g[i];
		float blue = pb->data_b[i];
		float alpha = pb->data_a[i];
		if (red < red_min)
			red_min = red;
		if (red > red_max)
			red_max = red;
		if (green < green_min)
			green_min = green;
		if (green > green_max)
			green_max = green;
		if (blue < blue_min)
			blue_min = blue;
		if (blue > blue_max)
			blue_max = blue;
		if (alpha < alpha_min)
			alpha_min = alpha;
		if (alpha > alpha_max)
			alpha_max = alpha;

		if (grayscale == 1 && (red != green || red != blue))
		{
			grayscale = 0;
		}
	}

	pb->red_min = red_min;
	pb->red_max = red_max;
	pb->green_min = green_min;
	pb->green_max = green_max;
	pb->blue_min = blue_min;
	pb->blue_max = blue_max;
	pb->alpha_min = alpha_min;
	pb->alpha_max = alpha_max;
	pb->grayscale = grayscale;
}
