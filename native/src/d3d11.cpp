#include "gpu.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <cstring>
#include <map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kBlitShader, kBlitShaderSize, "blit", nullptr, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        prx_log("blit shader %s: %s", entry, errors ? (const char*)errors->GetBufferPointer() : "?");
        return nullptr;
    }
    return code;
}

class D3D11 final : public Gpu {
public:
    explicit D3D11(ID3D11Device* device) : device_(device) { device->GetImmediateContext(&ctx_); }

    bool init() {
        auto vsCode = compile("vs", "vs_4_0");
        auto psCode = compile("ps", "ps_4_0");
        if (!vsCode || !psCode) return false;
        if (FAILED(device_->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs_)) ||
            FAILED(device_->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps_)))
            return false;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&sd, &sampler_))) return false;
        D3D11_BUFFER_DESC bd{sizeof(BlitParams), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
        if (FAILED(device_->CreateBuffer(&bd, nullptr, &params_))) return false;
        // A private context state lets us draw on Unity's immediate context without disturbing its cached state.
        ComPtr<ID3D11Device1> device1;
        if (FAILED(device_.As(&device1))) {
            prx_log("D3D11.1 unavailable");
            return false;
        }
        D3D_FEATURE_LEVEL level = device_->GetFeatureLevel();
        UINT flags = (device_->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                         ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        return SUCCEEDED(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
                                                           __uuidof(ID3D11Device1), nullptr, &state_));
    }

    bool ready() override { return true; }

    void* upload(void* texture, const void* rgba, int w, int h) override {
        auto it = own_.find(texture);
        if (it != own_.end()) {
            D3D11_TEXTURE2D_DESC d;
            it->second->GetDesc(&d);
            if ((int)d.Width != w || (int)d.Height != h) {
                own_.erase(it);
                it = own_.end();
            }
        }
        if (it == own_.end()) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = (UINT)w, td.Height = (UINT)h, td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> tex;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex))) return nullptr;
            it = own_.emplace(tex.Get(), tex).first;
        }
        ctx_->UpdateSubresource(it->second.Get(), 0, nullptr, rgba, (UINT)w * 4, 0);
        return it->first;
    }

    bool create_session(XrSession* session) override {
        XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        if (!xr_ok(xrGetD3D11GraphicsRequirementsKHR(g_xr.instance, g_xr.system, &req), "graphics requirements"))
            return false;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(device_.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter))) adapter->GetDesc(&ad);
        if (!adapter_matches(ad.AdapterLuid, req.adapterLuid)) return false;
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = device_.Get();
        XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, g_xr.system};
        return xr_ok(xrCreateSession(g_xr.instance, &sci, session), "xrCreateSession");
    }

    bool bind_images(XrSwapchain swapchain, int64_t format) override {
        uint32_t count = 0;
        xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(swapchain, count, &count, (XrSwapchainImageBaseHeader*)imgs.data());
        auto& images = images_[swapchain];
        for (auto& img : imgs) {
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = (DXGI_FORMAT)format;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            Image image{img.texture, nullptr, rd.Format};
            if (FAILED(device_->CreateRenderTargetView(img.texture, &rd, &image.rtv))) {
                prx_log("swapchain RTV failed");
                images_.erase(swapchain);
                return false;
            }
            images.push_back(image);
        }
        return true;
    }

    void forget_images(XrSwapchain swapchain) override { images_.erase(swapchain); }

    bool size(void* texture, int& w, int& h) override {
        D3D11_TEXTURE2D_DESC desc;
        ((ID3D11Texture2D*)texture)->GetDesc(&desc);
        w = (int)desc.Width;
        h = (int)desc.Height;
        return true;
    }

    void draw(void* texture, XrSwapchain swapchain, uint32_t index, bool flipY, const Overlay& overlay,
              const wchar_t* capture) override {
        auto& img = images_[swapchain][index];
        D3D11_TEXTURE2D_DESC desc;
        img.texture->GetDesc(&desc);
        BlitParams p{};
        if (!params_for((ID3D11Texture2D*)texture, flipY, gpu_format_linear(img.format), p)) return;
        gpu_overlay(p, overlay, (float)desc.Width / desc.Height);
        blit((ID3D11Texture2D*)texture, img.rtv.Get(), (int)desc.Width, (int)desc.Height, p);
        if (capture) save_bmp(img.texture, capture);
        if (overlay.probe) probe(img.texture);
    }

    float opaque_fraction() override {
        float f = probeResult_;
        probeResult_ = -1;
        return f;
    }

    // Reads the previous copy if the GPU is done with it, then copies this image for the next call.
    void probe(ID3D11Texture2D* tex) {
        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        if (probe_) {
            D3D11_MAPPED_SUBRESOURCE m;
            HRESULT hr = ctx_->Map(probe_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
            if (SUCCEEDED(hr)) {
                D3D11_TEXTURE2D_DESC have;
                probe_->GetDesc(&have);
                probeResult_ = gpu_opaque_fraction((const uint8_t*)m.pData, have.Width, have.Height, m.RowPitch, have.Format);
                ctx_->Unmap(probe_.Get(), 0);
            }
            D3D11_TEXTURE2D_DESC have;
            probe_->GetDesc(&have);
            if (have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format) probe_.Reset();
        }
        if (!probe_) {
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            desc.ArraySize = 1;
            desc.MipLevels = 1;
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, &probe_))) return;
        }
        ctx_->CopySubresourceRegion(probe_.Get(), 0, 0, 0, 0, tex, 0, nullptr);
    }

    void capture_backbuffer() override {
        ComPtr<ID3D11RenderTargetView> rtv;
        ctx_->OMGetRenderTargets(1, &rtv, nullptr);
        if (!rtv) return;
        ComPtr<ID3D11Resource> res;
        rtv->GetResource(&res);
        ComPtr<ID3D11Texture2D> src;
        if (SUCCEEDED(res.As(&src))) grab(src.Get());
    }

    void grab(ID3D11Texture2D* src) {
        D3D11_TEXTURE2D_DESC desc;
        src->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC have{};
        if (backbufferCopy_) backbufferCopy_->GetDesc(&have);
        if (!backbufferCopy_ || have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format) {
            D3D11_TEXTURE2D_DESC copy = desc;
            copy.SampleDesc = {1, 0};
            copy.MipLevels = 1;
            copy.ArraySize = 1;
            copy.Usage = D3D11_USAGE_DEFAULT;
            copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copy.CPUAccessFlags = 0;
            copy.MiscFlags = 0;
            backbufferCopy_.Reset();
            if (FAILED(device_->CreateTexture2D(&copy, nullptr, &backbufferCopy_))) {
                prx_log("backbuffer copy: format %d not copyable", (int)desc.Format);
                return;
            }
        }
        if (desc.SampleDesc.Count > 1) ctx_->ResolveSubresource(backbufferCopy_.Get(), 0, src, 0, desc.Format);
        else ctx_->CopySubresourceRegion(backbufferCopy_.Get(), 0, 0, 0, 0, src, 0, nullptr);
    }

    void* backbuffer_copy() override { return backbufferCopy_.Get(); }

    void set_mirror(void* texture) override { mirror_ = texture; }

    void present(IDXGISwapChain* swapchain) override {
        auto* eye = (ID3D11Texture2D*)mirror_.load();
        if (!eye && !gpu_grab_present) return;
        ComPtr<ID3D11Device> owner;
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != device_.Get() ||
            FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back))))
            return;
        if (gpu_grab_present) grab(back.Get());
        if (!eye) return;
        D3D11_TEXTURE2D_DESC bd, ed;
        back->GetDesc(&bd);
        eye->GetDesc(&ed);
        // Backbuffers are UNORM; Unity writes them through sRGB views in linear-space games, so the bytes are sRGB.
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = bd.Format;
        rd.ViewDimension = bd.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(device_->CreateRenderTargetView(back.Get(), &rd, &rtv))) return;
        BlitParams p{};
        if (!params_for(eye, true, gpu_format_linear(bd.Format), p)) return;
        gpu_mirror_crop(p, (float)ed.Width / ed.Height, (float)bd.Width / bd.Height);
        blit(eye, rtv.Get(), (int)bd.Width, (int)bd.Height, p);
    }

private:
    struct Image {
        ID3D11Texture2D* texture;
        ComPtr<ID3D11RenderTargetView> rtv;
        DXGI_FORMAT format;
    };

    bool params_for(ID3D11Texture2D* src, bool flipY, bool dstLinear, BlitParams& p) {
        D3D11_TEXTURE2D_DESC desc;
        src->GetDesc(&desc);
        if (desc.SampleDesc.Count > 1) {
            if (!warnedMsaa_) prx_log("blit: source is multisampled, skipped");
            warnedMsaa_ = true;
            return false;
        }
        p = gpu_params(flipY, gpu_format_linear(desc.Format), dstLinear);
        return true;
    }

    void blit(ID3D11Texture2D* src, ID3D11RenderTargetView* dst, int w, int h, const BlitParams& p) {
        D3D11_TEXTURE2D_DESC desc;
        src->GetDesc(&desc);
        bool linear;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = gpu_view_format(desc.Format, linear);
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        // Created per draw: Unity may recreate the texture at the same address, a cached view would go stale.
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device_->CreateShaderResourceView(src, &vd, &srv))) {
            prx_log("blit: CreateShaderResourceView failed for format %d", (int)desc.Format);
            return;
        }
        ComPtr<ID3D11DeviceContext1> ctx1;
        ctx_.As(&ctx1);
        ComPtr<ID3DDeviceContextState> previous;
        ctx1->SwapDeviceContextState(state_.Get(), &previous);
        ctx_->UpdateSubresource(params_.Get(), 0, nullptr, &p, 0, 0);
        D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        ctx_->PSSetShaderResources(0, 1, srv.GetAddressOf());
        ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        ctx_->PSSetConstantBuffers(0, 1, params_.GetAddressOf());
        ctx_->OMSetRenderTargets(1, &dst, nullptr);
        ctx_->RSSetViewports(1, &vp);
        ctx_->Draw(3, 0);
        // Our state must not keep Unity's texture or the swapchain image bound while swapped out.
        ctx_->ClearState();
        ctx1->SwapDeviceContextState(previous.Get(), nullptr);
    }

    void save_bmp(ID3D11Texture2D* tex, const wchar_t* path) {
        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        desc.ArraySize = 1;
        desc.MipLevels = 1;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging))) return;
        ctx_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
        gpu_write_bmp(path, (const uint8_t*)m.pData, desc.Width, desc.Height, m.RowPitch, desc.Format);
        ctx_->Unmap(staging.Get(), 0);
    }

    std::map<void*, ComPtr<ID3D11Texture2D>> own_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> params_;
    ComPtr<ID3DDeviceContextState> state_;
    ComPtr<ID3D11Texture2D> backbufferCopy_, probe_;
    float probeResult_ = -1;
    std::map<XrSwapchain, std::vector<Image>> images_;
    std::atomic<void*> mirror_{nullptr};
    bool warnedMsaa_ = false;
};
}  // namespace

bool adapter_matches(LUID game, LUID runtime) {
    if ((runtime.HighPart == 0 && runtime.LowPart == 0) || memcmp(&game, &runtime, sizeof(LUID)) == 0) return true;
    prx_log("game adapter luid %08lx:%08lx, runtime wants %08lx:%08lx", game.HighPart, game.LowPart, runtime.HighPart,
            runtime.LowPart);
    prx_log("game renders on a different GPU than the headset runtime");
    return false;
}

Gpu* gpu_create_d3d11(ID3D11Device* device) {
    auto* gpu = new D3D11(device);
    if (gpu->init()) return gpu;
    delete gpu;
    return nullptr;
}
