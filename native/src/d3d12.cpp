#include "gpu.h"
#include <MinHook.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <map>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteFn g_execute;
thread_local ID3D12CommandQueue* t_lastQueue;

void STDMETHODCALLTYPE hook_execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    if (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) t_lastQueue = queue;
    g_execute(queue, count, lists);
}

// Without Unity's plugin interface we cannot ask which state a Unity texture is in. An eye target was last written
// by its camera, so it sits in RENDER_TARGET; every barrier we add is undone before our list ends, so Unity's own
// tracking stays true. The UI comes from our own copy of the backbuffer made at Present, where the state is known.
constexpr D3D12_RESOURCE_STATES kUnityState = D3D12_RESOURCE_STATE_RENDER_TARGET;
constexpr int kRing = 16, kBlitsPerList = 2;

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    return b;
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    auto b = transition(r, from, to);
    list->ResourceBarrier(1, &b);
}

class D3D12 final : public Gpu {
public:
    explicit D3D12(ID3D12Device* device) : device_(device) {}

    ~D3D12() override {
        std::lock_guard lock(mutex_);
        wait_idle();
        mirror_.Reset();
        if (event_) CloseHandle(event_);
    }

    bool init() {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, sizeof(BlitParams) / 4};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs{2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, errors;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_))))
            return false;
        vs_ = compile("vs", "vs_5_0");
        ps_ = compile("ps", "ps_5_0");
        if (!vs_ || !ps_) return false;

        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRing * kBlitsPerList,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap_)))) return false;
        hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRing * kBlitsPerList, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)))) return false;
        srvStep_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rtvStep_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (auto& c : ring_) {
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.alloc))) ||
                FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.alloc.Get(), nullptr,
                                                  IID_PPV_ARGS(&c.list))))
                return false;
            c.list->Close();
        }
        return event_ != nullptr;
    }

    bool ready() override {
        std::lock_guard lock(mutex_);
        return queue_ != nullptr;
    }

    bool create_session(XrSession* session) override {
        XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        if (!xr_ok(xrGetD3D12GraphicsRequirementsKHR(g_xr.instance, g_xr.system, &req), "graphics requirements"))
            return false;
        if (!adapter_matches(device_->GetAdapterLuid(), req.adapterLuid)) return false;
        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        binding.device = device_.Get();
        binding.queue = queue_.Get();
        XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, g_xr.system};
        return xr_ok(xrCreateSession(g_xr.instance, &sci, session), "xrCreateSession");
    }

    bool bind_images(XrSwapchain swapchain, int64_t format) override {
        uint32_t count = 0;
        xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr);
        std::vector<XrSwapchainImageD3D12KHR> imgs(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(swapchain, count, &count, (XrSwapchainImageBaseHeader*)imgs.data());
        std::lock_guard lock(mutex_);
        auto& images = images_[swapchain];
        for (auto& img : imgs) images.push_back({img.texture, (DXGI_FORMAT)format});
        return true;
    }

    void forget_images(XrSwapchain swapchain) override {
        std::lock_guard lock(mutex_);
        wait_idle();
        images_.erase(swapchain);
    }

    bool size(void* texture, int& w, int& h) override {
        auto desc = ((ID3D12Resource*)texture)->GetDesc();
        w = (int)desc.Width;
        h = (int)desc.Height;
        return desc.SampleDesc.Count == 1;
    }

    // Swapchain images are in RENDER_TARGET when acquired and must be again when released.
    void draw(void* texture, XrSwapchain swapchain, uint32_t index, bool flipY, const Overlay& overlay,
              const wchar_t* capture) override {
        std::lock_guard lock(mutex_);
        if (!queue_) return;
        auto& img = images_[swapchain][index];
        auto* src = (ID3D12Resource*)texture;
        auto srcState = src == uiCopy_.Get() || own_.count(src) ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : kUnityState;
        auto* list = begin();
        auto desc = img.texture->GetDesc();
        blit(list, 0, src, srcState, img.texture, img.format, D3D12_RESOURCE_STATE_RENDER_TARGET, (UINT)desc.Width,
             desc.Height, flipY, &overlay, nullptr);
        ComPtr<ID3D12Resource> readback;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        if (capture) readback = copy_to_readback(list, img.texture, fp);
        // Opacity probe: read the previous copy once the GPU passed it, then copy this image.
        bool probing = false;
        if (overlay.probe) {
            if (probeBuf_ && fence_->GetCompletedValue() >= probeDone_) {
                void* data = nullptr;
                if (SUCCEEDED(probeBuf_->Map(0, nullptr, &data))) {
                    probeResult_ = gpu_opaque_fraction((const uint8_t*)data + probeFp_.Offset, probeFp_.Footprint.Width,
                                                       probeFp_.Footprint.Height, probeFp_.Footprint.RowPitch, probeFormat_);
                    probeBuf_->Unmap(0, nullptr);
                }
                probeBuf_.Reset();
            }
            if (!probeBuf_) {
                probeBuf_ = copy_to_readback(list, img.texture, probeFp_);
                probeFormat_ = img.format;
                probing = probeBuf_ != nullptr;
            }
        }
        uint64_t done = submit();
        if (probing) probeDone_ = done;
        if (readback) save_bmp(readback.Get(), fp, img.format, done, capture);
    }

    // Our own textures rest in PIXEL_SHADER_RESOURCE; each has an upload buffer reused once the GPU has read it.
    void* upload(void* texture, const void* rgba, int w, int h) override {
        std::lock_guard lock(mutex_);
        if (!queue_) return nullptr;
        auto it = own_.find((ID3D12Resource*)texture);
        if (it != own_.end()) {
            auto d = it->second.tex->GetDesc();
            if ((int)d.Width != w || (int)d.Height != h) {
                wait_idle();
                own_.erase(it);
                it = own_.end();
            }
        }
        if (it == own_.end()) {
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = (UINT64)w, d.Height = (UINT)h, d.DepthOrArraySize = d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc = {1, 0};
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            ComPtr<ID3D12Resource> tex;
            if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                        nullptr, IID_PPV_ARGS(&tex))))
                return nullptr;
            it = own_.emplace(tex.Get(), Own{tex}).first;
        }
        Own& o = it->second;
        auto td = o.tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        device_->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        wait_for(o.done);
        if (!o.upload) {
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total;
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc = {1, 0};
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                        nullptr, IID_PPV_ARGS(&o.upload))))
                return nullptr;
        }
        uint8_t* data = nullptr;
        if (FAILED(o.upload->Map(0, nullptr, (void**)&data))) return nullptr;
        for (int y = 0; y < h; ++y)
            memcpy(data + fp.Offset + (size_t)y * fp.Footprint.RowPitch, (const uint8_t*)rgba + (size_t)y * w * 4, (size_t)w * 4);
        o.upload->Unmap(0, nullptr);
        auto* list = begin();
        barrier(list, o.tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{o.tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{o.upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(list, o.tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        o.done = submit();
        return o.tex.Get();
    }

    float opaque_fraction() override {
        float f = probeResult_;
        probeResult_ = -1;
        return f;
    }

    void capture_backbuffer() override {}

    void* backbuffer_copy() override { return uiCopy_.Get(); }

    // Held, not just pointed at: the game may free its eye texture (a new size, VR switched off) before the next
    // Present, which would then read a freed object. Another texture takes its place only once the GPU has run the
    // lists of ours that drew from the old one.
    void set_mirror(void* texture, const float* crop, bool flipY) override {
        std::lock_guard lock(mutex_);
        mirrorCrop_[2] = 0;  // the whole image unless cropped
        if (crop) memcpy(mirrorCrop_, crop, sizeof(mirrorCrop_));
        mirrorFlip_ = flipY;
        if (mirror_.Get() != texture) {
            if (mirror_) wait_idle();
            mirror_ = (ID3D12Resource*)texture;
        }
    }

    // Present is where the backbuffer's state is known (PRESENT), so the UI grab and the desktop mirror happen here.
    void present(IDXGISwapChain* swapchain) override {
        std::lock_guard lock(mutex_);
        ComPtr<ID3D12Device> owner;
        if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != device_.Get()) return;
        if (!queue_) {
            // The game submits its frame on the presenting thread right before Present; that queue owns the swapchain.
            ComPtr<ID3D12Device> queueOwner;
            if (!t_lastQueue || FAILED(t_lastQueue->GetDevice(IID_PPV_ARGS(&queueOwner))) || queueOwner != owner) return;
            queue_ = t_lastQueue;
            prx_log("D3D12 queue captured");
        }
        ComPtr<IDXGISwapChain3> sc3;
        ComPtr<ID3D12Resource> back;
        if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc3))) ||
            FAILED(sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back))))
            return;
        auto bd = back->GetDesc();
        if (!ensure_ui_copy(bd)) return;
        auto* list = begin();
        D3D12_RESOURCE_BARRIER pre[2] = {
            transition(back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(uiCopy_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
        list->ResourceBarrier(2, pre);
        list->CopyResource(uiCopy_.Get(), back.Get());
        D3D12_RESOURCE_BARRIER post[2] = {
            transition(back.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
            transition(uiCopy_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(2, post);
        if (auto* eye = mirror_.Get()) {
            auto ed = eye->GetDesc();
            BlitParams crop{};
            gpu_mirror_crop(crop, (float)ed.Width / ed.Height, (float)bd.Width / bd.Height, mirrorCrop_);
            blit(list, 1, eye, kUnityState, back.Get(), bd.Format, D3D12_RESOURCE_STATE_PRESENT, (UINT)bd.Width,
                 bd.Height, mirrorFlip_, nullptr, &crop);
        }
        submit();
    }

private:
    struct Image {
        ID3D12Resource* texture;
        DXGI_FORMAT format;
    };
    struct Cmd {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        uint64_t fence = 0;
    };

    ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
        ComPtr<ID3DBlob> code, errors;
        if (FAILED(D3DCompile(kBlitShader, kBlitShaderSize, "blit", nullptr, nullptr, entry, target,
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
            prx_log("blit shader %s: %s", entry, errors ? (const char*)errors->GetBufferPointer() : "?");
            return nullptr;
        }
        return code;
    }

    ID3D12PipelineState* pipeline(DXGI_FORMAT rtv) {
        auto& pso = pipelines_[rtv];
        if (pso) return pso.Get();
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root_.Get();
        d.VS = {vs_->GetBufferPointer(), vs_->GetBufferSize()};
        d.PS = {ps_->GetBufferPointer(), ps_->GetBufferSize()};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = rtv;
        d.SampleDesc = {1, 0};
        if (FAILED(device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
            prx_log("blit pipeline for format %d failed", (int)rtv);
            pipelines_.erase(rtv);
            return nullptr;
        }
        return pso.Get();
    }

    void wait_for(uint64_t value) {
        if (fence_->GetCompletedValue() >= value) return;
        fence_->SetEventOnCompletion(value, event_);
        WaitForSingleObject(event_, 2000);
    }

    void wait_idle() {
        if (queue_ && fence_) wait_for(fenceValue_);
    }

    ID3D12GraphicsCommandList* begin() {
        auto& c = ring_[next_];
        wait_for(c.fence);
        c.alloc->Reset();
        c.list->Reset(c.alloc.Get(), nullptr);
        return c.list.Get();
    }

    uint64_t submit() {
        auto& c = ring_[next_];
        c.list->Close();
        ID3D12CommandList* lists[] = {c.list.Get()};
        queue_->ExecuteCommandLists(1, lists);
        queue_->Signal(fence_.Get(), ++fenceValue_);
        c.fence = fenceValue_;
        next_ = (next_ + 1) % kRing;
        return fenceValue_;
    }

    void blit(ID3D12GraphicsCommandList* list, int n, ID3D12Resource* src, D3D12_RESOURCE_STATES srcState,
              ID3D12Resource* dst, DXGI_FORMAT dstFormat, D3D12_RESOURCE_STATES dstState, UINT w, UINT h, bool flipY,
              const Overlay* overlay, const BlitParams* crop) {
        auto* pso = pipeline(dstFormat);
        if (!pso) return;
        auto sd = src->GetDesc();
        bool srcLinear;
        D3D12_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = gpu_view_format(sd.Format, srcLinear);
        vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        vd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        vd.Texture2D.MipLevels = 1;
        int slot = next_ * kBlitsPerList + n;
        auto srvCpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        auto srvGpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        srvCpu.ptr += (SIZE_T)slot * srvStep_;
        srvGpu.ptr += (UINT64)slot * srvStep_;
        auto rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (SIZE_T)slot * rtvStep_;
        device_->CreateShaderResourceView(src, &vd, srvCpu);
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = dstFormat;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device_->CreateRenderTargetView(dst, &rd, rtv);

        auto p = gpu_params(flipY, srcLinear, gpu_format_linear(dstFormat));
        if (overlay) gpu_overlay(p, *overlay, (float)w / h);
        if (crop) {
            p.scaleX = crop->scaleX;
            p.scaleY = crop->scaleY;
            p.offsetX = crop->offsetX;
            p.offsetY = crop->offsetY;
        }
        barrier(list, src, srcState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barrier(list, dst, dstState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(root_.Get());
        list->SetPipelineState(pso);
        list->SetGraphicsRoot32BitConstants(0, sizeof(p) / 4, &p, 0);
        list->SetGraphicsRootDescriptorTable(1, srvGpu);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
        D3D12_RECT scissor{0, 0, (LONG)w, (LONG)h};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        barrier(list, dst, D3D12_RESOURCE_STATE_RENDER_TARGET, dstState);
        barrier(list, src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, srcState);
    }

    bool ensure_ui_copy(const D3D12_RESOURCE_DESC& back) {
        if (uiCopy_) {
            auto have = uiCopy_->GetDesc();
            if (have.Width == back.Width && have.Height == back.Height && have.Format == back.Format) return true;
            wait_idle();
            uiCopy_.Reset();
        }
        D3D12_RESOURCE_DESC d = back;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        d.MipLevels = 1;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                    IID_PPV_ARGS(&uiCopy_)))) {
            prx_log("backbuffer copy: format %d not copyable", (int)back.Format);
            return false;
        }
        return true;
    }

    ComPtr<ID3D12Resource> copy_to_readback(ID3D12GraphicsCommandList* list, ID3D12Resource* tex,
                                            D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp) {
        auto td = tex->GetDesc();
        UINT64 total = 0;
        device_->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc = {1, 0};
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buf;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&buf))))
            return nullptr;
        barrier(list, tex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{buf.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(list, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        return buf;
    }

    void save_bmp(ID3D12Resource* buf, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, DXGI_FORMAT format, uint64_t done,
                  const wchar_t* path) {
        wait_for(done);
        void* data = nullptr;
        if (FAILED(buf->Map(0, nullptr, &data))) return;
        gpu_write_bmp(path, (const uint8_t*)data + fp.Offset, fp.Footprint.Width, fp.Footprint.Height,
                      fp.Footprint.RowPitch, format);
        buf->Unmap(0, nullptr);
    }

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3DBlob> vs_, ps_;
    std::map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> pipelines_;
    ComPtr<ID3D12DescriptorHeap> srvHeap_, rtvHeap_;
    UINT srvStep_ = 0, rtvStep_ = 0;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12Resource> probeBuf_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT probeFp_{};
    DXGI_FORMAT probeFormat_ = DXGI_FORMAT_UNKNOWN;
    uint64_t probeDone_ = 0;
    float probeResult_ = -1;
    uint64_t fenceValue_ = 0;
    HANDLE event_ = nullptr;
    Cmd ring_[kRing];
    int next_ = 0;
    ComPtr<ID3D12Resource> uiCopy_;
    struct Own {
        ComPtr<ID3D12Resource> tex, upload;
        uint64_t done = 0;
    };
    std::map<ID3D12Resource*, Own> own_;
    std::map<XrSwapchain, std::vector<Image>> images_;
    ComPtr<ID3D12Resource> mirror_;
    float mirrorCrop_[4]{};
    bool mirrorFlip_ = true;
    // Present can run on a different thread from the render-thread events.
    std::mutex mutex_;
};
}  // namespace

bool gpu_hook_queue(ID3D12CommandQueue* probe) {
    static bool hooked = false;
    if (hooked) return true;
    void* target = (*(void***)probe)[10];  // ID3D12CommandQueue::ExecuteCommandLists
    MH_Initialize();
    hooked = MH_CreateHook(target, (void*)&hook_execute, (void**)&g_execute) == MH_OK && MH_EnableHook(target) == MH_OK;
    prx_log("queue hook: %s", hooked ? "on" : "failed");
    return hooked;
}

Gpu* gpu_create_d3d12(ID3D12Device* device) {
    auto* gpu = new D3D12(device);
    if (gpu->init()) return gpu;
    delete gpu;
    return nullptr;
}
