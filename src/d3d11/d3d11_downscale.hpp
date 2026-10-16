#pragma once
/* Madeira M7: d3d11.downscaleMip1 -- store a large single-level texture at half width and
 * height. A single-level texture has no smaller mip for d3d11.mipClampBC to fall back to,
 * and in Frostpunk such textures (UI art, BC3 atlases) were ~1.5 GB of a load that iOS
 * killed at its 6144 MB limit. Each physical texel is the 2x2 box average of the logical
 * image; BC3 is decoded, averaged and re-encoded. */
#include "dxmt_bcn.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstdint>

namespace dxmt {

/* 4-byte, 8-bit-per-channel image -> ceil(w/2) x ceil(h/2), rows of ceil(w/2)*4 bytes.
 * An odd last row/column averages with itself. */
inline void
DownscaleRGBA8Half(const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst) {
  const uint32_t pw = (w + 1) / 2, ph = (h + 1) / 2;
  for (uint32_t y = 0; y < ph; y++) {
    const uint8_t *r0 = src + (size_t)std::min(2 * y, h - 1) * pitch;
    const uint8_t *r1 = src + (size_t)std::min(2 * y + 1, h - 1) * pitch;
    uint8_t *out = dst + (size_t)y * pw * 4;
    for (uint32_t x = 0; x < pw; x++) {
      const uint32_t x0 = std::min(2 * x, w - 1) * 4, x1 = std::min(2 * x + 1, w - 1) * 4;
      for (uint32_t c = 0; c < 4; c++)
        out[x * 4 + c] = (uint8_t)((r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) >> 2);
    }
  }
}

/* 16 RGBA8 texels (row-major 4x4) -> one BC3 block. Colour endpoints are the extremes of
 * the block's colours along their principal axis; every texel takes the nearest palette
 * entry, computed exactly as bcn_bc3_block decodes it. */
inline void
EncodeBC3Block(const uint8_t px[64], uint8_t out[16]) {
  /* alpha: 8-interpolant mode, endpoints max/min */
  uint8_t amax = 0, amin = 255;
  for (int t = 0; t < 16; t++) { amax = std::max(amax, px[t * 4 + 3]); amin = std::min(amin, px[t * 4 + 3]); }
  uint8_t ap[8] = {amax, amin};
  for (int i = 1; i < 7; i++) ap[i + 1] = (uint8_t)(((7 - i) * amax + i * amin + 3) / 7);
  uint64_t abits = 0;
  if (amax != amin) {
    for (int t = 0; t < 16; t++) {
      int best = 0, bd = 1 << 30;
      for (int i = 0; i < 8; i++) {
        const int d = std::abs((int)px[t * 4 + 3] - (int)ap[i]);
        if (d < bd) { bd = d; best = i; }
      }
      abits |= (uint64_t)best << (3 * t);
    }
  }
  out[0] = amax; out[1] = amin;
  for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(abits >> (8 * i));

  /* colour: principal axis by power iteration on the 3x3 covariance */
  float mean[3] = {0, 0, 0};
  for (int t = 0; t < 16; t++) for (int c = 0; c < 3; c++) mean[c] += px[t * 4 + c];
  for (int c = 0; c < 3; c++) mean[c] /= 16.0f;
  float cov[6] = {0, 0, 0, 0, 0, 0};   /* rr rg rb gg gb bb */
  for (int t = 0; t < 16; t++) {
    const float r = px[t * 4] - mean[0], g = px[t * 4 + 1] - mean[1], b = px[t * 4 + 2] - mean[2];
    cov[0] += r * r; cov[1] += r * g; cov[2] += r * b; cov[3] += g * g; cov[4] += g * b; cov[5] += b * b;
  }
  float v[3] = {1, 1, 1};
  for (int it = 0; it < 4; it++) {
    const float x = cov[0] * v[0] + cov[1] * v[1] + cov[2] * v[2];
    const float y = cov[1] * v[0] + cov[3] * v[1] + cov[4] * v[2];
    const float z = cov[2] * v[0] + cov[4] * v[1] + cov[5] * v[2];
    const float m = std::max(std::max(std::abs(x), std::abs(y)), std::abs(z));
    if (m < 1e-6f) break;   /* flat block: any axis works */
    v[0] = x / m; v[1] = y / m; v[2] = z / m;
  }
  int lo = 0, hi = 0;
  float dlo = 1e30f, dhi = -1e30f;
  for (int t = 0; t < 16; t++) {
    const float d = px[t * 4] * v[0] + px[t * 4 + 1] * v[1] + px[t * 4 + 2] * v[2];
    if (d < dlo) { dlo = d; lo = t; }
    if (d > dhi) { dhi = d; hi = t; }
  }
  auto to565 = [](const uint8_t *p) {
    return (uint16_t)((((p[0] * 31 + 127) / 255) << 11) | (((p[1] * 63 + 127) / 255) << 5) | ((p[2] * 31 + 127) / 255));
  };
  uint16_t c0 = to565(&px[hi * 4]), c1 = to565(&px[lo * 4]);
  if (c0 < c1) std::swap(c0, c1);
  uint8_t cp[4][3];
  bcn_rgb565(c0, cp[0]);
  bcn_rgb565(c1, cp[1]);
  for (int c = 0; c < 3; c++) {
    cp[2][c] = (uint8_t)((2 * cp[0][c] + cp[1][c] + 1) / 3);
    cp[3][c] = (uint8_t)((cp[0][c] + 2 * cp[1][c] + 1) / 3);
  }
  uint32_t cbits = 0;
  if (c0 != c1) {
    for (int t = 0; t < 16; t++) {
      int best = 0, bd = 1 << 30;
      for (int i = 0; i < 4; i++) {
        int d = 0;
        for (int c = 0; c < 3; c++) { const int e = (int)px[t * 4 + c] - (int)cp[i][c]; d += e * e; }
        if (d < bd) { bd = d; best = i; }
      }
      cbits |= (uint32_t)best << (2 * t);
    }
  }
  out[8] = (uint8_t)c0; out[9] = (uint8_t)(c0 >> 8);
  out[10] = (uint8_t)c1; out[11] = (uint8_t)(c1 >> 8);
  for (int i = 0; i < 4; i++) out[12 + i] = (uint8_t)(cbits >> (8 * i));
}

/* BC3 image of w x h texels (both multiples of 8), block rows `pitch` bytes apart ->
 * BC3 image of w/2 x h/2, block rows of (w/8)*16 bytes. */
inline void
DownscaleBC3Half(const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst) {
  const uint32_t obw = w / 8, obh = h / 8;
  uint8_t full[8 * 8 * 4], half[64], dec[64];
  for (uint32_t by = 0; by < obh; by++) {
    for (uint32_t bx = 0; bx < obw; bx++) {
      for (uint32_t q = 0; q < 4; q++) {
        const uint32_t sx = 2 * bx + (q & 1), sy = 2 * by + (q >> 1);
        bcn_bc3_block(src + (size_t)sy * pitch + (size_t)sx * 16, dec);
        for (uint32_t t = 0; t < 16; t++)
          std::copy_n(&dec[t * 4], 4, &full[(((q >> 1) * 4 + t / 4) * 8 + (q & 1) * 4 + t % 4) * 4]);
      }
      DownscaleRGBA8Half(full, 8 * 4, 8, 8, half);
      EncodeBC3Block(half, dst + ((size_t)by * obw + bx) * 16);
    }
  }
}

} // namespace dxmt
