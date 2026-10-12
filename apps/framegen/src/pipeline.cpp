#include "pipeline.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cctype>

namespace {

// Mirrors cbuffer Params in shaders/framegen.hlsl.
struct alignas(16) Params {
    float srcSize[2];
    float dstSize[2];
    float parentSize[2];
    float t;
    int searchRadius;
    float dstRect[4];
    float sharpness;
    float occlusionThreshold;
    float flowUvScale[2];
};
static_assert(sizeof(Params) == 64);

constexpr UINT kBlock = 8;          // must match BLOCK in the shader
constexpr UINT kCoarsestMin = 48;   // stop the pyramid near this many pixels
constexpr size_t kMaxLevels = 6;

Size Half(Size s) { return {std::max(1u, (s.width + 1) / 2), std::max(1u, (s.height + 1) / 2)}; }
Size Blocks(Size s) { return {(s.width + kBlock - 1) / kBlock, (s.height + kBlock - 1) / kBlock}; }

// Flow grids round up to whole blocks, so a grid covers slightly more than its
// level. This maps image UV on `level` to UV on that level's flow grid.
std::array<float, 2> FlowUvScale(Size level) {
    Size grid = Blocks(level);
    return {static_cast<float>(level.width) / (kBlock * grid.width),
            static_cast<float>(level.height) / (kBlock * grid.height)};
}
}  // namespace

Pipeline::Pipeline(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& shaderPath)
    : device_(device), context_(context) {
    csLuma_ = Compile(shaderPath, "CSLuma");
    csDownsample_ = Compile(shaderPath, "CSDownsample");
    csMotion_ = Compile(shaderPath, "CSMotion");
    csSynthesize_ = Compile(shaderPath, "CSSynthesize");
    csOutput_ = Compile(shaderPath, "CSOutput");

    D3D11_BUFFER_DESC bd{sizeof(Params), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                         D3D11_CPU_ACCESS_WRITE, 0, 0};
    Check(device_->CreateBuffer(&bd, nullptr, &params_), "CreateBuffer(params)");

    D3D11_SAMPLER_DESC sd{};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    Check(device_->CreateSamplerState(&sd, &linear_), "CreateSamplerState(linear)");
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    Check(device_->CreateSamplerState(&sd, &point_), "CreateSamplerState(point)");
}

ComPtr<ID3D11ComputeShader> Pipeline::Compile(const std::wstring& path, const char* entry) {
    std::string define = "PASS_" + std::string(entry);
    std::transform(define.begin(), define.end(), define.begin(),
                   [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    const D3D_SHADER_MACRO macros[] = {{define.c_str(), "1"}, {nullptr, nullptr}};

    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompileFromFile(path.c_str(), macros, nullptr, entry, "cs_5_0",
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        std::string msg = "shader " + std::string(entry) + " failed to compile";
        if (errors) msg += ":\n" + std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                               errors->GetBufferSize());
        throw std::runtime_error(msg);
    }
    ComPtr<ID3D11ComputeShader> cs;
    Check(device_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs),
          "CreateComputeShader");
    return cs;
}

Pipeline::Texture Pipeline::MakeTexture(Size size, DXGI_FORMAT format) {
    Texture t;
    t.size = size;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = size.width;
    td.Height = size.height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    Check(device_->CreateTexture2D(&td, nullptr, &t.tex), "CreateTexture2D");
    Check(device_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv), "CreateShaderResourceView");
    Check(device_->CreateUnorderedAccessView(t.tex.Get(), nullptr, &t.uav), "CreateUnorderedAccessView");
    return t;
}

void Pipeline::Resize(Size source, Size output, const RECT& image) {
    source_ = source;
    output_ = output;
    image_ = image;
    hasPrevious_ = false;

    for (Slot& slot : slots_) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = source.width;
        td.Height = source.height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // Desktop Duplication format
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Check(device_->CreateTexture2D(&td, nullptr, slot.color.ReleaseAndGetAddressOf()), "capture slot");
        Check(device_->CreateShaderResourceView(slot.color.Get(), nullptr,
                                                slot.colorSrv.ReleaseAndGetAddressOf()),
              "capture slot SRV");

        slot.luma.clear();
        for (Size s = Half(source); slot.luma.size() < kMaxLevels; s = Half(s)) {
            slot.luma.push_back(MakeTexture(s, DXGI_FORMAT_R16_FLOAT));
            if (std::min(s.width, s.height) <= kCoarsestMin) break;
        }
    }

    forward_.clear();
    backward_.clear();
    for (const Texture& level : slots_[0].luma) {
        forward_.push_back(MakeTexture(Blocks(level.size), DXGI_FORMAT_R16G16_FLOAT));
        backward_.push_back(MakeTexture(Blocks(level.size), DXGI_FORMAT_R16G16_FLOAT));
    }
    synth_ = MakeTexture(source, DXGI_FORMAT_R8G8B8A8_UNORM);
    out_ = MakeTexture(output, DXGI_FORMAT_R8G8B8A8_UNORM);
}

void Pipeline::SetParams(Size src, Size dst, Size parent, float t, std::array<float, 2> flowUvScale) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    Check(context_->Map(params_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map(params)");
    Params p{};
    p.srcSize[0] = static_cast<float>(src.width);
    p.srcSize[1] = static_cast<float>(src.height);
    p.dstSize[0] = static_cast<float>(dst.width);
    p.dstSize[1] = static_cast<float>(dst.height);
    p.parentSize[0] = static_cast<float>(parent.width);
    p.parentSize[1] = static_cast<float>(parent.height);
    p.t = t;
    p.searchRadius = settings.searchRadius;
    p.dstRect[0] = static_cast<float>(image_.left);
    p.dstRect[1] = static_cast<float>(image_.top);
    p.dstRect[2] = static_cast<float>(image_.right - image_.left);
    p.dstRect[3] = static_cast<float>(image_.bottom - image_.top);
    p.sharpness = settings.sharpness;
    p.occlusionThreshold = settings.occlusionThreshold;
    p.flowUvScale[0] = flowUvScale[0];
    p.flowUvScale[1] = flowUvScale[1];
    *static_cast<Params*>(mapped.pData) = p;
    context_->Unmap(params_.Get(), 0);
}

void Pipeline::Dispatch(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srvs,
                        ID3D11UnorderedAccessView* uav, Size grid) {
    ID3D11SamplerState* samplers[] = {linear_.Get(), point_.Get()};
    context_->CSSetShader(cs, nullptr, 0);
    context_->CSSetConstantBuffers(0, 1, params_.GetAddressOf());
    context_->CSSetSamplers(0, 2, samplers);
    context_->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.begin());
    context_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context_->Dispatch((grid.width + 7) / 8, (grid.height + 7) / 8, 1);

    // Unbind so the next pass can read what this one wrote.
    ID3D11ShaderResourceView* nullSrvs[4] = {};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    context_->CSSetShaderResources(0, 4, nullSrvs);
    context_->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
}

void Pipeline::BuildPyramid(Slot& slot) {
    SetParams(source_, slot.luma[0].size);
    Dispatch(csLuma_.Get(), {slot.colorSrv.Get()}, slot.luma[0].uav.Get(), slot.luma[0].size);
    for (size_t l = 1; l < slot.luma.size(); ++l) {
        SetParams(slot.luma[l - 1].size, slot.luma[l].size);
        Dispatch(csDownsample_.Get(), {slot.luma[l - 1].srv.Get()}, slot.luma[l].uav.Get(), slot.luma[l].size);
    }
}

void Pipeline::EstimateMotion(Slot& from, Slot& to, std::vector<Texture>& flow) {
    for (size_t l = flow.size(); l-- > 0;) {
        const bool coarsest = l + 1 == flow.size();
        Size parent = coarsest ? Size{} : flow[l + 1].size;
        ID3D11ShaderResourceView* parentSrv = coarsest ? nullptr : flow[l + 1].srv.Get();
        // The parent grid belongs to level l + 1.
        SetParams(from.luma[l].size, flow[l].size, parent, 0,
                  coarsest ? std::array<float, 2>{1, 1} : FlowUvScale(from.luma[l + 1].size));
        Dispatch(csMotion_.Get(), {from.luma[l].srv.Get(), to.luma[l].srv.Get(), parentSrv},
                 flow[l].uav.Get(), flow[l].size);
    }
}

bool Pipeline::CommitCapture() {
    current_ = 1 - current_;
    Slot& cur = slots_[current_];
    Slot& prev = slots_[1 - current_];
    BuildPyramid(cur);
    const bool ready = hasPrevious_;
    if (ready) {
        EstimateMotion(prev, cur, forward_);
        EstimateMotion(cur, prev, backward_);
    }
    hasPrevious_ = true;
    return ready;
}

void Pipeline::RenderGenerated(float t, ID3D11Texture2D* backBuffer) {
    SetParams(source_, source_, {}, t, FlowUvScale(slots_[0].luma[0].size));
    Dispatch(csSynthesize_.Get(),
             {slots_[1 - current_].colorSrv.Get(), slots_[current_].colorSrv.Get(), forward_[0].srv.Get(),
              backward_[0].srv.Get()},
             synth_.uav.Get(), source_);
    Output(synth_.srv.Get(), backBuffer);
}

void Pipeline::RenderCurrent(ID3D11Texture2D* backBuffer) {
    Output(slots_[current_].colorSrv.Get(), backBuffer);
}

void Pipeline::Output(ID3D11ShaderResourceView* frame, ID3D11Texture2D* backBuffer) {
    SetParams(source_, output_);
    Dispatch(csOutput_.Get(), {frame}, out_.uav.Get(), output_);
    context_->CopyResource(backBuffer, out_.tex.Get());
}
