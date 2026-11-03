#pragma once

/* Madeira UI probe, d3d11.uiProbe = N (off when 0, the default).
 *
 * At every Nth present (five times at most) the next frame is probed: each draw is
 * listed in C:\madeira-ui-probe\frame-<k>.txt with what it writes (render target, the
 * swap chain's back buffer marked BB, depth test and write, blending), its shaders
 * (first bytes of their SHA-1) and the textures its pixel shader reads; the frame as
 * presented and the textures read by draws into the back buffer are saved as TGA
 * (mip 0 of uncompressed 8-bit RGBA, BGRA, R8 and A8 formats). It answers whether a
 * game's interface can be told from its 3D and how much detail its glyph textures
 * hold. A probed frame stalls once for the read-backs. */

#include "d3d11_1.h"
#include <string>

namespace dxmt::ui_probe {

/* d3d11.uiProbe is set (read once). */
bool enabled();

/* A probed frame is being recorded (cheap: one relaxed load). */
bool active();

/* One draw of the probed frame: `state` is its description, `rt0` its first render
 * target's resource (may be null), `srvs` the resources its pixel shader reads. */
void record(const std::string &state, ID3D11Resource *rt0, ID3D11Resource *const *srvs, unsigned count);

/* The resource's probe id ("T<n>", stable for the run) and, for a 2D texture, its
 * format and size. */
std::string describe(ID3D11Resource *resource);

/* Called at the start of every Present: ends a probed frame (writes its report and
 * images, reading textures back through `context`) and starts the next one when due. */
void present(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Resource *backbuffer);

} // namespace dxmt::ui_probe
