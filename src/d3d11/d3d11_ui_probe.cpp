#include "d3d11_ui_probe.hpp"
#include "com/com_pointer.hpp"
#include "config/config.hpp"
#include "log/log.hpp"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dxmt::ui_probe {

namespace {

constexpr unsigned kMaxFrames = 5;
constexpr size_t kMaxDraws = 6000;
constexpr size_t kMaxImages = 24;
constexpr const char *kRoot = "C:\\madeira-ui-probe";

std::atomic<bool> g_active{false};
std::mutex g_lock;
std::vector<std::string> g_lines;
std::vector<Com<ID3D11Resource>> g_images;   // read by draws into the back buffer, in first-use order
std::unordered_map<void *, unsigned> g_ids;   // keyed by COM identity (IUnknown)
void *g_backbuffer = nullptr;                // the presenting swap chain's identity, for "BB"
unsigned g_frames = 0;
uint64_t g_presents = 0;

int
every() {
  static int n = -1;
  if (n < 0)
    n = std::max(0, Config::getInstance().getOption<int>("d3d11.uiProbe", 0));
  return n;
}

/* COM identity: the same object through any of its interfaces. */
void *
identity(ID3D11Resource *resource) {
  Com<IUnknown> unknown;
  return resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&unknown))) ? unknown.ptr() : resource;
}

/* C:\madeira-ui-probe\<the program's file name without .exe>, created on first use. */
const std::string &
folder() {
  static std::string path = [] {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string name = exe;
    name = name.substr(name.find_last_of("\\/") + 1);
    if (auto dot = name.rfind('.'); dot != std::string::npos)
      name.resize(dot);
    CreateDirectoryA(kRoot, nullptr);
    std::string dir = std::string(kRoot) + "\\" + (name.empty() ? "game" : name);
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
  }();
  return path;
}

unsigned
id_of(void *p) {
  auto [it, added] = g_ids.try_emplace(p, (unsigned)g_ids.size() + 1);
  return it->second;
}

/* 8-bit layouts a TGA can hold: channels and whether red and blue are swapped already. */
bool
tga_layout(DXGI_FORMAT f, int &channels, bool &bgra) {
  switch (f) {
  case DXGI_FORMAT_A8_UNORM:
  case DXGI_FORMAT_R8_UNORM:
  case DXGI_FORMAT_R8_TYPELESS:
    channels = 1; bgra = false; return true;
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    channels = 4; bgra = false; return true;
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
  case DXGI_FORMAT_B8G8R8X8_UNORM:
  case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    channels = 4; bgra = true; return true;
  default:
    return false;
  }
}

/* Mip 0, slice 0 of a 2D texture to an uncompressed TGA; "" or why it was skipped. */
std::string
save_tga(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Resource *resource, const std::string &path) {
  Com<ID3D11Texture2D> texture;
  if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
    return "not a 2D texture";
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  int channels;
  bool bgra;
  if (!tga_layout(desc.Format, channels, bgra))
    return std::format("format {} not saved", (int)desc.Format);
  if (desc.SampleDesc.Count != 1)
    return "multisampled";
  if (desc.Width > 8192 || desc.Height > 8192)
    return "too large";
  D3D11_TEXTURE2D_DESC staging = desc;
  staging.MipLevels = 1;
  staging.ArraySize = 1;
  staging.Usage = D3D11_USAGE_STAGING;
  staging.BindFlags = 0;
  staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging.MiscFlags = 0;
  Com<ID3D11Texture2D> copy;
  if (FAILED(device->CreateTexture2D(&staging, nullptr, &copy)))
    return "no staging copy";
  context->CopySubresourceRegion(copy.ptr(), 0, 0, 0, 0, texture.ptr(), 0, nullptr);
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context->Map(copy.ptr(), 0, D3D11_MAP_READ, 0, &mapped)))
    return "map failed";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) {
    context->Unmap(copy.ptr(), 0);
    return "cannot write";
  }
  uint8_t header[18] = {};
  header[2] = channels == 1 ? 3 : 2;   // grey or true colour, uncompressed
  header[12] = desc.Width & 0xff; header[13] = desc.Width >> 8;
  header[14] = desc.Height & 0xff; header[15] = desc.Height >> 8;
  header[16] = channels * 8;
  header[17] = 0x20 | (channels == 4 ? 8 : 0);   // top-left origin, alpha bits
  fwrite(header, 1, sizeof(header), f);
  std::vector<uint8_t> row(desc.Width * channels);
  for (UINT y = 0; y < desc.Height; y++) {
    const uint8_t *src = static_cast<const uint8_t *>(mapped.pData) + (size_t)y * mapped.RowPitch;
    memcpy(row.data(), src, row.size());
    if (channels == 4 && !bgra)
      for (UINT x = 0; x < desc.Width; x++) std::swap(row[x * 4], row[x * 4 + 2]);
    fwrite(row.data(), 1, row.size(), f);
  }
  fclose(f);
  context->Unmap(copy.ptr(), 0);
  return "";
}

} // namespace

bool
enabled() {
  return every() > 0;
}

bool
active() {
  return g_active.load(std::memory_order_relaxed);
}

std::string
describe(ID3D11Resource *resource) {
  if (!resource)
    return "-";
  void *key = identity(resource);
  std::string out;
  {
    std::lock_guard lock(g_lock);
    out = std::format("T{}", id_of(key));
  }
  Com<ID3D11Texture2D> texture;
  if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
    D3D11_TEXTURE2D_DESC d;
    texture->GetDesc(&d);
    out += std::format("({} {}x{}{}{})", (int)d.Format, d.Width, d.Height, d.MipLevels > 1 ? std::format(" m{}", d.MipLevels) : "",
                       d.ArraySize > 1 ? std::format(" a{}", d.ArraySize) : "");
  } else {
    D3D11_RESOURCE_DIMENSION dim;
    resource->GetType(&dim);
    out += dim == D3D11_RESOURCE_DIMENSION_BUFFER ? "(buffer)" : std::format("(dim {})", (int)dim);
  }
  if (key == g_backbuffer)
    out += "BB";
  return out;
}

void
record(const std::string &state, ID3D11Resource *rt0, ID3D11Resource *const *srvs, unsigned count) {
  void *target = identity(rt0);
  std::lock_guard lock(g_lock);
  if (!g_active.load(std::memory_order_relaxed) || g_lines.size() >= kMaxDraws)
    return;
  g_lines.push_back(std::format("#{} {}", g_lines.size(), state));
  if (!target || target != g_backbuffer)
    return;
  for (unsigned i = 0; i < count; i++) {
    if (!srvs[i] || g_images.size() >= kMaxImages)
      continue;
    bool seen = false;
    for (auto &image : g_images) seen |= image.ptr() == srvs[i];
    if (!seen)
      g_images.push_back(srvs[i]);
  }
}

void
present(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Resource *backbuffer) {
  const int n = every();
  if (!n)
    return;
  g_presents++;
  if (g_active.load(std::memory_order_relaxed)) {
    std::vector<std::string> lines;
    std::vector<Com<ID3D11Resource>> images;
    {
      std::lock_guard lock(g_lock);
      g_active.store(false, std::memory_order_relaxed);
      lines.swap(g_lines);
      images.swap(g_images);
    }
    const unsigned k = g_frames;
    std::string report = std::format("Madeira UI probe frame {} (present #{}), {} draws; BB = the swap chain's back buffer\n"
                                     "draw: render target(format WxH) dsv depth=test/write blend=enable(src,dst) "
                                     "vs ps topology viewport | pixel-shader textures\n",
                                     k, g_presents, lines.size());
    for (auto &line : lines) report += line + "\n";
    report += "\nimages:\n";
    std::string frame_path = std::format("{}\\frame-{}-presented.tga", folder(), k);
    std::string why = backbuffer ? save_tga(device, context, backbuffer, frame_path) : "no back buffer";
    report += std::format("frame-{}-presented.tga {}\n", k, why.empty() ? "saved" : why);
    for (auto &image : images) {
      std::string name = describe(image.ptr());
      std::string id = name.substr(0, name.find('('));
      std::string path = std::format("{}\\frame-{}-{}.tga", folder(), k, id);
      why = save_tga(device, context, image.ptr(), path);
      report += std::format("frame-{}-{}.tga {} {}\n", k, id, name, why.empty() ? "saved" : why);
    }
    if (FILE *f = fopen(std::format("{}\\frame-{}.txt", folder(), k).c_str(), "wb")) {
      fwrite(report.data(), 1, report.size(), f);
      fclose(f);
    }
    Logger::info(std::format("[ui-probe] frame {} written: {} draws, {} textures", k, lines.size(), images.size()));
    g_frames++;
  }
  if (g_frames < kMaxFrames && g_presents % n == 0) {
    std::lock_guard lock(g_lock);
    g_backbuffer = identity(backbuffer);
    g_active.store(true, std::memory_order_relaxed);
    Logger::info(std::format("[ui-probe] probing frame {} at present #{}", g_frames, g_presents));
  }
}

} // namespace dxmt::ui_probe
