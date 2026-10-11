#pragma once

#include "common.h"

#include <array>
#include <vector>

struct PipelineSettings {
    int searchRadius = 4;             // coarsest-level exhaustive search, in pixels
    float sharpness = 0.0f;           // 0..1
    float occlusionThreshold = 0.12f; // luma disagreement that stops blending
};

// GPU work between capture and present: luma pyramids, bidirectional motion
// estimation, frame synthesis, and the output resample.
class Pipeline {
public:
    Pipeline(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& shaderPath);

    // `source`: captured region size. `output`: back buffer size.
    // `image`: where the source lands inside the output (aspect fit).
    void Resize(Size source, Size output, const RECT& image);

    // Texture the next capture should be copied into (B8G8R8A8, source size).
    ID3D11Texture2D* CaptureTarget() const { return slots_[1 - current_].color.Get(); }

    // Commits the captured frame as current. Returns true when a previous
    // frame exists and motion fields between them are ready.
    bool CommitCapture();
    void ForgetHistory() { hasPrevious_ = false; }

    void RenderGenerated(float t, ID3D11Texture2D* backBuffer);
    void RenderCurrent(ID3D11Texture2D* backBuffer);

    PipelineSettings settings;

private:
    struct Texture {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        Size size;
    };
    struct Slot {
        ComPtr<ID3D11Texture2D> color;
        ComPtr<ID3D11ShaderResourceView> colorSrv;
        std::vector<Texture> luma;  // pyramid, finest first
    };

    Texture MakeTexture(Size size, DXGI_FORMAT format);
    ComPtr<ID3D11ComputeShader> Compile(const std::wstring& path, const char* entry);
    void Dispatch(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srvs,
                  ID3D11UnorderedAccessView* uav, Size grid);
    void SetParams(Size src, Size dst, Size parent = {}, float t = 0, std::array<float, 2> flowUvScale = {1, 1});
    void BuildPyramid(Slot& slot);
    void EstimateMotion(Slot& from, Slot& to, std::vector<Texture>& flow);
    void Output(ID3D11ShaderResourceView* frame, ID3D11Texture2D* backBuffer);

    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
    ComPtr<ID3D11ComputeShader> csLuma_, csDownsample_, csMotion_, csSynthesize_, csOutput_;
    ComPtr<ID3D11Buffer> params_;
    ComPtr<ID3D11SamplerState> linear_, point_;

    Size source_, output_;
    RECT image_{};
    Slot slots_[2];
    int current_ = 0;
    bool hasPrevious_ = false;
    std::vector<Texture> forward_, backward_;  // per pyramid level, finest first
    Texture synth_, out_;
};
