#include "common.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
const char kShader[] = R"(
cbuffer Params : register(b0) { float flip; float decode; float encode; float pad; };
Texture2D src : register(t0);
SamplerState smp : register(s0);

void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 toLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 toSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    if (flip > 0.5) uv.y = 1 - uv.y;
    float4 c = src.Sample(smp, uv);
    if (decode > 0.5) c.rgb = toLinear(saturate(c.rgb));
    if (encode > 0.5) c.rgb = toSrgb(saturate(c.rgb));
    return c;
}
)";

struct Params { float flip, decode, encode, pad; };

ComPtr<ID3D11VertexShader> g_vs;
ComPtr<ID3D11PixelShader> g_ps;
ComPtr<ID3D11SamplerState> g_sampler;
ComPtr<ID3D11Buffer> g_params;
ComPtr<ID3DDeviceContextState> g_state;
bool g_warnedMsaa = false;

ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "blit", nullptr, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        prx_log("blit shader %s: %s", entry, errors ? (const char*)errors->GetBufferPointer() : "?");
        return nullptr;
    }
    return code;
}

// Typeless 8-bit sources are read raw so the bytes Unity stored reach the shader unchanged. `linear` reports whether
// the sampled value is linear light: sRGB views decode in hardware, and float targets hold linear values.
DXGI_FORMAT view_format(DXGI_FORMAT f, bool& linear) {
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
}  // namespace

bool blit_init(ID3D11Device* device) {
    auto vsCode = compile("vs", "vs_4_0");
    auto psCode = compile("ps", "ps_4_0");
    if (!vsCode || !psCode) return false;
    if (FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &g_vs)) ||
        FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &g_ps)))
        return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &g_sampler))) return false;

    D3D11_BUFFER_DESC bd{sizeof(Params), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    if (FAILED(device->CreateBuffer(&bd, nullptr, &g_params))) return false;

    // A private context state lets us draw on Unity's immediate context without disturbing its cached state.
    ComPtr<ID3D11Device1> device1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) {
        prx_log("D3D11.1 unavailable");
        return false;
    }
    D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                     ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    return SUCCEEDED(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
                                                       __uuidof(ID3D11Device1), nullptr, &g_state));
}

void blit_shutdown() {
    g_vs.Reset();
    g_ps.Reset();
    g_sampler.Reset();
    g_params.Reset();
    g_state.Reset();
}

void blit_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11RenderTargetView* dst, int w, int h,
               bool flipY, bool dstSrgb) {
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);
    if (desc.SampleDesc.Count > 1) {
        if (!g_warnedMsaa) prx_log("blit: source is multisampled, skipped");
        g_warnedMsaa = true;
        return;
    }
    bool hwLinear;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = view_format(desc.Format, hwLinear);
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);
    // Created per draw: Unity may recreate the texture at the same address, a cached view would go stale.
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(src, &vd, &srv))) {
        prx_log("blit: CreateShaderResourceView failed for format %d", (int)desc.Format);
        return;
    }

    ComPtr<ID3D11DeviceContext1> ctx1;
    ctx->QueryInterface(IID_PPV_ARGS(&ctx1));
    ComPtr<ID3DDeviceContextState> previous;
    ctx1->SwapDeviceContextState(g_state.Get(), &previous);

    Params p{flipY ? 1.f : 0.f, (dstSrgb && !hwLinear) ? 1.f : 0.f, (!dstSrgb && hwLinear) ? 1.f : 0.f, 0.f};
    ctx->UpdateSubresource(g_params.Get(), 0, nullptr, &p, 0, 0);
    D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
    ctx->PSSetSamplers(0, 1, g_sampler.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, g_params.GetAddressOf());
    ctx->OMSetRenderTargets(1, &dst, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->Draw(3, 0);
    // Our state must not keep Unity's texture or the swapchain image bound while swapped out.
    ctx->ClearState();
    ctx1->SwapDeviceContextState(previous.Get(), nullptr);
}

void blit_save_bmp(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const wchar_t* path) {
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!rgba && !bgra) {
        prx_log("capture: unsupported format %d", (int)desc.Format);
        return;
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.ArraySize = 1;
    desc.MipLevels = 1;
    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return;
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;

    std::vector<uint8_t> pixels(desc.Width * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
        const uint8_t* row = (const uint8_t*)m.pData + y * m.RowPitch;
        uint8_t* out = pixels.data() + y * desc.Width * 4;
        for (UINT x = 0; x < desc.Width; ++x) {
            out[x * 4 + 0] = row[x * 4 + (rgba ? 2 : 0)];
            out[x * 4 + 1] = row[x * 4 + 1];
            out[x * 4 + 2] = row[x * 4 + (rgba ? 0 : 2)];
            out[x * 4 + 3] = row[x * 4 + 3];
        }
    }
    ctx->Unmap(staging.Get(), 0);

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = (LONG)desc.Width;
    ih.biHeight = -(LONG)desc.Height;  // top-down
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
