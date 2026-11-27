/*
 * png.c - PNG pictures of the screen for the MCP tools.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "mcp.h"

static void be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static void png_chunk(sbuf *out, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t hdr[8];
	be32(hdr, len);
	memcpy(hdr + 4, type, 4);
	sb_add(out, hdr, 8);
	if (len)
		sb_add(out, data, len);
	uLong crc = crc32(0, (const Bytef *)type, 4);
	if (len)
		crc = crc32(crc, data, len);
	uint8_t c[4];
	be32(c, (uint32_t)crc);
	sb_add(out, c, 4);
}

bool png_encode_rgb(sbuf *out, int w, int h, const uint32_t *px, int stride)
{
	size_t rowlen = (size_t)w * 3 + 1;
	size_t rawlen = rowlen * h;
	uint8_t *raw = malloc(rawlen);
	if (!raw)
		return false;
	for (int y = 0; y < h; y++) {
		uint8_t *r = raw + y * rowlen;
		const uint32_t *s = px + (size_t)y * stride;
		*r++ = 0;	// filter: none
		for (int x = 0; x < w; x++) {
			uint32_t v = s[x];
			*r++ = v >> 16;
			*r++ = v >> 8;
			*r++ = v;
		}
	}
	uLongf zlen = compressBound(rawlen);
	uint8_t *z = malloc(zlen);
	if (!z || compress2(z, &zlen, raw, rawlen, 6) != Z_OK) {
		free(raw);
		free(z);
		return false;
	}
	free(raw);
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	uint8_t ihdr[13];
	be32(ihdr, w);
	be32(ihdr + 4, h);
	ihdr[8] = 8;	// bit depth
	ihdr[9] = 2;	// RGB
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	sb_add(out, sig, 8);
	png_chunk(out, "IHDR", ihdr, 13);
	png_chunk(out, "IDAT", z, zlen);
	png_chunk(out, "IEND", NULL, 0);
	free(z);
	return true;
}
