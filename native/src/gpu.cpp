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
    float4 beam; float beamWidth; float aspect; float dstLinear;
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
    if (decode > 0.5) c.rgb = toLinear(saturate(c.rgb));
    if (encode > 0.5) c.rgb = toSrgb(saturate(c.rgb));
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
        float3 lime = float3(0.776, 1, 0.102);
        c.rgb = lerp(c.rgb, dstLinear > 0.5 ? toLinear(lime) : lime, edge * lerp(0.25, 0.9, t));
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
    p.aspect = aspect;
}

void gpu_mirror_crop(BlitParams& p, float eye, float screen) {
    float visible = eye / screen < 1 ? eye / screen : 1.f;
    p.scaleY = visible;
    p.offsetY = (1 - visible) / 2;
}

void gpu_write_bmp(const wchar_t* path, const uint8_t* data, UINT w, UINT h, UINT pitch, DXGI_FORMAT format) {
    bool rgba = format == DXGI_FORMAT_R8G8B8A8_TYPELESS || format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    bool bgra = format == DXGI_FORMAT_B8G8R8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!rgba && !bgra) {
        prx_log("capture: unsupported format %d", (int)format);
        return;
    }
    std::vector<uint8_t> pixels(w * h * 4);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = data + y * pitch;
        uint8_t* out = pixels.data() + y * w * 4;
        for (UINT x = 0; x < w; ++x) {
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

namespace {
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
PresentFn g_present;
Present1Fn g_present1;
std::mutex g_targetMutex;
Gpu* g_target;

void on_present(IDXGISwapChain* sc, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return;
    std::lock_guard lock(g_targetMutex);
    if (g_target) g_target->present(sc);
}

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    on_present(sc, flags);
    return g_present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    on_present(sc, flags);
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
