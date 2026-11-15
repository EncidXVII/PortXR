#include "gpu.h"
#include <MinHook.h>
#include <wrl/client.h>
#include <cstdio>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

const char kBlitShader[] = R"(
cbuffer Params : register(b0) {
    float flip; float decode; float encode; float vignette; float2 scale; float2 offset;
    float4 beam; float beamWidth; float aspect; float dstLinear; float key;
    float4 watch;
    float4 keep;
    float4 aim;
    float4 hole;
    float4 watchLook;
};
Texture2D src : register(t0);
SamplerState smp : register(s0);

void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 toLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 toSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 at = uv;
    if (flip > 0.5) uv.y = 1 - uv.y;
    float4 c = src.Sample(smp, uv * scale + offset);
    // Keying judges brightness as displayed (sRGB), so dark UI (menu panels) stays solid while true black goes.
    float3 shown = decode > 0.5 ? c.rgb : (encode > 0.5 || dstLinear > 0.5) ? toSrgb(saturate(c.rgb)) : c.rgb;
    if (decode > 0.5) c.rgb = toLinear(saturate(c.rgb));
    if (encode > 0.5) c.rgb = toSrgb(saturate(c.rgb));
    // The panel's middle, shown on its own layer at the aim point instead (PrxFrame::aimPatch).
    if (hole.z > 0 && at.x >= hole.x && at.x <= hole.z && at.y >= hole.y && at.y <= hole.w) c = 0;
    // A panel the game left see-through (key < 0). Colour with no coverage at all (HDRP grain, bloom or exposure over
    // the cleared camera) would be added over the game by the runtime's premultiplied blend; it fades out. Older
    // Unity UI shaders (2018: SrcAlpha, OneMinusSrcAlpha on alpha too) store alpha squared while the colour is
    // premultiplied by the true alpha, so a 50% HUD square came out 25%. Premultiplied colour is never brighter
    // than its alpha, so where it is the alpha is raised to it: bright UI and crosshairs get their coverage back,
    // and games that store it right (Unity 2022 and later, 50% stays 50%) are left as they are.
    if (key < 0) {
        c.rgb *= saturate(c.a * 64);
        c.a = max(c.a, saturate(max(c.r, max(c.g, c.b))));
    }
    // Comfort vignette: the clear centre shrinks as the strength rises; a wide smoothstep keeps the edge soft.
    if (vignette > 0) {
        float inner = lerp(1.25, 0.45, vignette);
        c.rgb *= 1 - smoothstep(inner, inner + 0.45, length((uv - 0.5) * 2));
    }
    // Laser beam: distance to the segment in height units; lime core with a soft edge, fading in from the hand.
    if (beamWidth > 0) {
        float2 k = float2(aspect, 1), a = beam.xy * k, b = beam.zw * k, q = at * k;
        float t = saturate(dot(q - a, b - a) / max(dot(b - a, b - a), 1e-8));
        float edge = 1 - smoothstep(beamWidth * 0.5, beamWidth, length(q - lerp(a, b, t)));
        float3 lime = watchLook.yzw;
        float k2 = edge * lerp(0.25, 0.9, t);
        c.rgb = lerp(c.rgb, dstLinear > 0.5 ? toLinear(lime) : lime, k2);
        c.a = max(c.a, k2);
    }
    // UI panel keying, for a panel the game filled opaque (an effect on its hidden camera drew black under the UI):
    // the colour is read as UI drawn over black, so its brightness stands in for the lost alpha. Black goes, bright UI
    // stays solid, and dim washes (damage flashes, tinted overlays, shadows) become translucent. The colour is already
    // premultiplied that way, which is how the runtime blends the layer.
    bool kept = at.x >= keep.x && at.x <= keep.z && at.y >= keep.y && at.y <= keep.w;
    if (key > 0 && !kept) c.a = min(c.a, saturate(max(shown.r, max(shown.g, shown.b)) * 2.5));
    // Aim reticle: a lime ring and dot with a dark outline, readable on bright and dark scenes alike.
    if (aim.z > 0) {
        float d = length((at - aim.xy) * float2(aspect, 1)) / aim.z, aa = 0.08;
        float ring = 1 - smoothstep(0.16, 0.16 + aa, abs(d - 0.8)), pip = 1 - smoothstep(0.18, 0.18 + aa, d);
        float halo = 1 - smoothstep(0.32, 0.32 + aa, min(abs(d - 0.8), d));
        float3 lime = watchLook.yzw, col = lime * saturate(ring + pip);
        if (dstLinear > 0.5) col = toLinear(col);
        float a = max(halo * 0.6, saturate(ring + pip));
        c.rgb = lerp(c.rgb, col, a);
        c.a = max(c.a, a);
    }
    // Watch face: dark disc, a grey rim and three round-ended bars (the menu glyph). Glanced at (watch.w, eased
    // 0..1) the rim thickens and rim and bars turn lime. The whole face fades with watchLook.x as the player looks.
    if (watch.z > 0 && watchLook.x > 0.004) {
        float2 k = float2(aspect, 1), q = (at - watch.xy) * k / watch.z;
        float d = length(q), aa = 0.06;
        if (d < 1 + aa) {
            float3 lime = watchLook.yzw, rest = float3(0.29, 0.29, 0.314), bar = float3(0.659, 0.659, 0.686);
            float3 black = float3(0.043, 0.043, 0.047);
            float lit = saturate(watch.w);
            float face = 1 - smoothstep(1 - aa, 1, d);
            float inner = lerp(0.84, 0.72, lit);
            float rim = face * smoothstep(inner, inner + aa, d);
            // Capsules: half length 0.36, radius 0.07, at y = -0.28, 0, 0.28.
            float y = q.y - clamp(round(q.y / 0.28), -1, 1) * 0.28;
            float2 e = float2(max(abs(q.x) - 0.36, 0), y);
            float bars = 1 - smoothstep(0.07, 0.07 + aa, length(e));
            float3 col = black;
            col = lerp(col, lerp(bar, lime, lit), bars);
            col = lerp(col, lerp(rest, lime, lit), rim);
            if (dstLinear > 0.5) col = toLinear(col);
            float a = face * 0.92 * saturate(watchLook.x);
            c.rgb = lerp(c.rgb, col, a);
            c.a = max(c.a, a);
        }
    }
    return c;
}
)";
const size_t kBlitShaderSize = sizeof(kBlitShader) - 1;

DXGI_FORMAT gpu_view_format(DXGI_FORMAT f, bool& linear) {
    linear = false;
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: linear = true; return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT: linear = true; return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: linear = true; return f;
    default: return f;
    }
}

bool gpu_format_linear(DXGI_FORMAT f) {
    bool linear;
    gpu_view_format(f, linear);
    return linear;
}

BlitParams gpu_params(bool flipY, bool srcLinear, bool dstLinear) {
    BlitParams p{flipY ? 1.f : 0.f, (dstLinear && !srcLinear) ? 1.f : 0.f, (!dstLinear && srcLinear) ? 1.f : 0.f, 0.f,
                 1.f, 1.f, 0.f, 0.f};
    p.dstLinear = dstLinear ? 1.f : 0.f;
    return p;
}

void gpu_overlay(BlitParams& p, const Overlay& o, float aspect) {
    p.vignette = o.vignette;
    for (int i = 0; i < 4; ++i) p.beam[i] = o.beam[i];
    p.beamWidth = o.beamWidth;
    for (int i = 0; i < 4; ++i) p.watch[i] = o.watch[i];
    p.watchLook[0] = o.watchAlpha;
    for (int i = 0; i < 3; ++i) p.watchLook[1 + i] = o.accent[i];
    p.key = o.key;
    for (int i = 0; i < 4; ++i) p.keep[i] = o.keep[i];
    for (int i = 0; i < 4; ++i) p.aim[i] = o.aim[i];
    for (int i = 0; i < 4; ++i) p.hole[i] = o.hole[i];
    if (o.crop[2] > 0) {
        p.scaleX = o.crop[2] - o.crop[0];
        p.scaleY = o.crop[3] - o.crop[1];
        p.offsetX = o.crop[0];
        p.offsetY = p.flip > 0.5f ? 1 - o.crop[3] : o.crop[1];  // the shader flips before it crops
    }
    p.aspect = aspect;
}

void gpu_mirror_crop(BlitParams& p, float eye, float screen) {
    float visible = eye / screen < 1 ? eye / screen : 1.f;
    p.scaleY = visible;
    p.offsetY = (1 - visible) / 2;
}

float gpu_opaque_fraction(const uint8_t* data, UINT w, UINT h, UINT pitch, DXGI_FORMAT format) {
    bool bytes = format == DXGI_FORMAT_R8G8B8A8_TYPELESS || format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                 format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                 format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    // 10-bit and half-float back buffers too (D3D12 games with HDR or wide-colour output): alpha over a half.
    bool tenBit = format == DXGI_FORMAT_R10G10B10A2_TYPELESS || format == DXGI_FORMAT_R10G10B10A2_UNORM;
    bool half = format == DXGI_FORMAT_R16G16B16A16_TYPELESS || format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if ((!bytes && !tenBit && !half) || !w || !h) return -1;
    int opaque = 0, total = 0;
    for (UINT y = h / 64; y < h; y += h / 32 + 1)
        for (UINT x = w / 128; x < w; x += w / 64 + 1, ++total) {
            const uint8_t* px = data + y * pitch + x * (half ? 8 : 4);
            if (bytes) opaque += px[3] > 128;
            else if (tenBit) opaque += (*(const uint32_t*)px >> 30) >= 2;
            else opaque += (*(const uint16_t*)(px + 6) & 0x7FFF) >= 0x3800;  // half-float alpha >= 0.5
        }
    return total ? (float)opaque / total : -1;
}

void gpu_write_bmp(const wchar_t* path, const uint8_t* data, UINT w, UINT h, UINT pitch, DXGI_FORMAT format) {
    bool rgba = format == DXGI_FORMAT_R8G8B8A8_TYPELESS || format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    bool bgra = format == DXGI_FORMAT_B8G8R8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    bool tenBit = format == DXGI_FORMAT_R10G10B10A2_TYPELESS || format == DXGI_FORMAT_R10G10B10A2_UNORM;
    if (!rgba && !bgra && !tenBit) {
        prx_log("capture: unsupported format %d", (int)format);
        return;
    }
    std::vector<uint8_t> pixels(w * h * 4);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = data + y * pitch;
        uint8_t* out = pixels.data() + y * w * 4;
        for (UINT x = 0; x < w; ++x) {
            if (tenBit) {  // Unreal's default backbuffer and scene target
                uint32_t v = ((const uint32_t*)row)[x];
                out[x * 4 + 0] = (uint8_t)(((v >> 20) & 1023) >> 2), out[x * 4 + 1] = (uint8_t)(((v >> 10) & 1023) >> 2);
                out[x * 4 + 2] = (uint8_t)((v & 1023) >> 2), out[x * 4 + 3] = (uint8_t)((v >> 30) * 85);
                continue;
            }
            out[x * 4 + 0] = row[x * 4 + (rgba ? 2 : 0)];
            out[x * 4 + 1] = row[x * 4 + 1];
            out[x * 4 + 2] = row[x * 4 + (rgba ? 0 : 2)];
            out[x * 4 + 3] = row[x * 4 + 3];
        }
    }
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = (LONG)w;
    ih.biHeight = -(LONG)h;  // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)pixels.size();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(pixels.data(), 1, pixels.size(), f);
    fclose(f);
}

void (*gpu_present_observer)(IDXGISwapChain*);
std::atomic<bool> gpu_grab_present, gpu_no_vsync;

namespace {
using PresentFn =HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
PresentFn g_present;
Present1Fn g_present1;
std::mutex g_targetMutex;
Gpu* g_target;

void on_present(IDXGISwapChain* sc, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return;
    {
        std::lock_guard lock(g_targetMutex);
        if (g_target) g_target->present(sc);
    }
    if (gpu_present_observer) gpu_present_observer(sc);
}

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    on_present(sc, flags);
    if (gpu_no_vsync) sync = 0;
    return g_present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    on_present(sc, flags);
    if (gpu_no_vsync) sync = 0;
    return g_present1(sc, sync, flags, p);
}
}  // namespace

bool gpu_hook_present(IUnknown* deviceOrQueue) {
    static bool hooked = false;
    if (hooked) return true;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ParallaxProbe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> sc;
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = d.Height = 8;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc = {1, 0};
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    bool made = hwnd && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->CreateSwapChainForHwnd(deviceOrQueue, hwnd, &d, nullptr, nullptr, &sc));
    if (made) {
        void** vtbl = *(void***)sc.Get();
        MH_Initialize();  // already initialised by the input hooks in most runs; both outcomes are fine
        hooked = MH_CreateHook(vtbl[8], (void*)&hook_present, (void**)&g_present) == MH_OK &&
                 MH_CreateHook(vtbl[22], (void*)&hook_present1, (void**)&g_present1) == MH_OK &&
                 MH_EnableHook(vtbl[8]) == MH_OK && MH_EnableHook(vtbl[22]) == MH_OK;
    }
    sc.Reset();
    if (hwnd) DestroyWindow(hwnd);
    prx_log("present hook: %s", hooked ? "on" : "failed");
    return hooked;
}

void gpu_set_present_target(Gpu* gpu) {
    std::lock_guard lock(g_targetMutex);
    g_target = gpu;
}
