#include <wrl/client.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <vector>
#include <unordered_set>
using Microsoft::WRL::ComPtr;
static float scaleX = .35f, scaleY = .35f, sourceTanX = 1.08f, sourceTanY = 1.28f;
static bool autoProjection = true, anchorUI = true;
static float stereoUnitsPerMetre = 100, stereoStrength = 0, panelWidth = 2.5f, panelDistance = 2.0f;
static unsigned worldDraws = 0, uiDraws = 0;
static bool worldCaptured = false, cbCaptured = false, projectionValid = false;
static float hardwareProjection[16] = {};
static ComPtr<ID3D11Device> gameDevice;
static ComPtr<ID3D11Texture2D> cleanWorld, uiLayer, colourCopy, eyes[2], desktopImage;
static ComPtr<ID3D11ShaderResourceView> uiSrv, colourSrv;
static ComPtr<ID3D11ShaderResourceView> frameSrv, previousDepthSrv, worldDepthSrv;
static ComPtr<ID3D11Texture2D> previousDepth, worldDepth;
static ComPtr<ID3D11Texture2D> worldDepthSource, frameDepth;
static ComPtr<ID3D11ShaderResourceView> frameDepthSrv;
static bool frameDepthValid = false;
static int stableWorldDepth = 1;
static unsigned frameDepthStage = 0;
struct FrameView
{
    vr::HmdMatrix34_t pose{};
    float projection[16]{}, cx = 0, cy = 0;
    bool valid = false;
};
static FrameView frameView, previousView, displayView;
static bool haveWorld = false;
static unsigned worldGapFrames = 0;
static bool haveSceneWorld = false;
static float matchedCameraError = 0;
static unsigned matchedCameraAge = 0;
static bool freshFlatFrame = false, finalWorldPresented = false;
static std::unordered_set<IUnknown *> backBufferIdentities;
static bool finalScreenTarget(ID3D11Texture2D *texture)
{
    ComPtr<IUnknown> identity;
    if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&identity))))
        return false;
    return backBufferIdentities.count(identity.Get()) != 0;
}
static bool delayedDLSS(reshade::api::effect_runtime *r)
{
    static ULONGLONG checked = 0;
    static bool delayed = false;
    if (GetTickCount64() - checked > 2000)
    {
        checked = GetTickCount64();
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t *name = wcsrchr(path, L'\\');
        if (name)
            wcscpy_s(name + 1, MAX_PATH - size_t(name + 1 - path), L"dlss5-feed.cfg");
        FILE *f = nullptr;
        _wfopen_s(&f, path, L"r");
        int active = 0, async = 0;
        if (f)
        {
            char line[256];
            while (fgets(line, sizeof(line), f))
            {
                sscanf_s(line, "enabled=%d", &active);
                sscanf_s(line, "async_home=%d", &async);
            }
            fclose(f);
        }
        delayed = active && async;
    }
    auto technique = r->find_technique("DLSS5_Feed.fx", "DLSS5_Feed");
    return delayed && technique.handle && r->get_technique_state(technique);
}
static bool copyDepth(ID3D11DeviceContext *c, ID3D11ShaderResourceView *src, ComPtr<ID3D11Texture2D> &texture,
                      ComPtr<ID3D11ShaderResourceView> &srv)
{
    if (!src)
        return false;
    ComPtr<ID3D11Resource> res;
    src->GetResource(&res);
    ComPtr<ID3D11Texture2D> input;
    if (FAILED(res.As(&input)))
        return false;
    D3D11_TEXTURE2D_DESC desc{}, old{};
    input->GetDesc(&desc);
    if (texture)
        texture->GetDesc(&old);
    if (!texture || desc.Width != old.Width || desc.Height != old.Height || desc.Format != old.Format)
    {
        srv.Reset();
        texture.Reset();
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        src->GetDesc(&view);
        if (FAILED(gameDevice->CreateTexture2D(&desc, nullptr, &texture)) ||
            FAILED(gameDevice->CreateShaderResourceView(texture.Get(), &view, &srv)))
            return false;
    }
    c->CopyResource(texture.Get(), input.Get());
    return true;
}
static ComPtr<ID3D11RenderTargetView> uiRtv;
static ComPtr<ID3D11RenderTargetView> worldRtv;
static ComPtr<ID3D11Texture2D> uiDepth;
static ComPtr<ID3D11DepthStencilView> uiDepthView;
static ComPtr<ID3D11Texture2D> uiDepthSource;
static bool uiDepthCopied = false;
static unsigned particleDraws = 0, flashDraws = 0;
static float worldScaleX = 1, worldScaleY = 1;
static ComPtr<ID3D11UnorderedAccessView> eyeUav[2], desktopUav;
static ComPtr<ID3D11ComputeShader> stereoShader;
static ComPtr<ID3D11Buffer> stereoConstants, readbackConstants;
static ComPtr<ID3D11SamplerState> linearSampler, pointSampler;
static std::unordered_map<ID3D11BlendState *, ComPtr<ID3D11BlendState>> alphaBlends;
static UINT outputWidth = 0, outputHeight = 0, eyeWidth = 0, eyeHeight = 0;
static thread_local bool internalDraw = false;
static bool gpuReady = false;
static reshade::api::device *gameApiDevice = nullptr;
static float appliedNativeX = 0, appliedNativeY = 0, targetNativeX = 0, targetNativeY = 0;
static float tangents[2][4];
static float opticalCenterX = 0, opticalCenterY = 0;
static WorldShaderInfo capturedShader{};
static vr::HmdMatrix34_t matchedCameraPose{};
static bool matchedCameraValid = false;
static void captureWorldDepth(ID3D11DeviceContext *c, unsigned stage = 3)
{
    frameDepthValid = false;
    if (!worldDepthSource || !projectionValid)
        return;
    D3D11_TEXTURE2D_DESC d{}, old{};
    worldDepthSource->GetDesc(&d);
    if (d.SampleDesc.Count != 1)
        return;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN, textureFormat = d.Format;
    switch (d.Format)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        textureFormat = DXGI_FORMAT_R24G8_TYPELESS;
        viewFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        break;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        textureFormat = DXGI_FORMAT_R32_TYPELESS;
        viewFormat = DXGI_FORMAT_R32_FLOAT;
        break;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        textureFormat = DXGI_FORMAT_R32G8X24_TYPELESS;
        viewFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        break;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        textureFormat = DXGI_FORMAT_R16_TYPELESS;
        viewFormat = DXGI_FORMAT_R16_UNORM;
        break;
    default:
        return;
    }
    if (frameDepth)
        frameDepth->GetDesc(&old);
    if (!frameDepth || old.Width != d.Width || old.Height != d.Height || old.Format != textureFormat)
    {
        frameDepth.Reset();
        frameDepthSrv.Reset();
        d.Format = textureFormat;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.CPUAccessFlags = 0;
        d.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = viewFormat;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(gameDevice->CreateTexture2D(&d, nullptr, &frameDepth)) ||
            FAILED(gameDevice->CreateShaderResourceView(frameDepth.Get(), &sd, &frameDepthSrv)))
            return;
        log("Direct world depth %ux%u format=%u", d.Width, d.Height, viewFormat);
    }
    c->CopyResource(frameDepth.Get(), worldDepthSource.Get());
    frameDepthValid = true;
    frameDepthStage = stage;
}
static bool clearUiDepth(reshade::api::command_list *cl, reshade::api::resource_view view, const float *depth,
                         const uint8_t *stencil, uint32_t rectCount, const reshade::api::rect *)
{
    if (internalDraw || cl->get_device() != gameApiDevice)
        return false;
    auto *original = reinterpret_cast<ID3D11DepthStencilView *>(view.handle);
    if (!original)
        return false;
    ComPtr<ID3D11Resource> res;
    original->GetResource(&res);
    // FFX reuses its world depth surface for interface rendering. Preserve the
    // geometry depth before that clear, not after the interface has finished.
    if (stableWorldDepth && outputVR && gpuReady && depth && worldDraws && projectionValid &&
        !worldCaptured && res.Get() == worldDepthSource.Get())
    {
        auto *c = reinterpret_cast<ID3D11DeviceContext *>(cl->get_native());
        captureWorldDepth(c, 1);
    }
    if (!uiDepthCopied || !uiDepthSource || !uiDepthView || res.Get() != uiDepthSource.Get())
        return false;
    if (rectCount)
    {
        uiDepthCopied = false;
        return false;
    }
    auto *c = reinterpret_cast<ID3D11DeviceContext *>(cl->get_native());
    UINT flags = (depth ? D3D11_CLEAR_DEPTH : 0) | (stencil ? D3D11_CLEAR_STENCIL : 0);
    internalDraw = true;
    c->ClearDepthStencilView(uiDepthView.Get(), flags, depth ? *depth : 1, stencil ? *stencil : 0);
    internalDraw = false;
    return false;
}
static bool extractProjection(const float *view, const float *wvp, float *out)
{
    double a[4][8] = {};
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++)
            a[i][j] = view[i * 4 + j];
        a[i][i + 4] = 1;
    }
    for (int k = 0; k < 4; k++)
    {
        int pivot = k;
        for (int i = k + 1; i < 4; i++)
            if (fabs(a[i][k]) > fabs(a[pivot][k]))
                pivot = i;
        if (fabs(a[pivot][k]) < 1e-10)
            return false;
        for (int j = 0; j < 8; j++)
            std::swap(a[k][j], a[pivot][j]);
        double divisor = a[k][k];
        for (int j = 0; j < 8; j++)
            a[k][j] /= divisor;
        for (int i = 0; i < 4; i++)
            if (i != k)
            {
                double f = a[i][k];
                for (int j = 0; j < 8; j++)
                    a[i][j] -= f * a[k][j];
            }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
        {
            double value = 0;
            for (int k = 0; k < 4; k++)
                value += wvp[i * 4 + k] * a[k][j + 4];
            out[i * 4 + j] = float(value);
        }
    // The engine's View matrix contains scale, so derive ray geometry from the
    // actual world-to-clip transform instead of trusting scene.Projection.
    float m[16];
    memcpy(m, out, sizeof(m));
    float normW = sqrtf(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]);
    if (!std::isfinite(normW) || normW < .00001f)
        return false;
    float normW2 = normW * normW;
    float cx = (m[0] * m[12] + m[1] * m[13] + m[2] * m[14]) / normW2;
    float cy = (m[4] * m[12] + m[5] * m[13] + m[6] * m[14]) / normW2;
    float az = (m[8] * m[12] + m[9] * m[13] + m[10] * m[14]) / normW2;
    float fx2 = 0, fy2 = 0;
    for (int j = 0; j < 3; j++)
    {
        float x = m[j] - cx * m[12 + j], y = m[4 + j] - cy * m[12 + j];
        fx2 += x * x;
        fy2 += y * y;
    }
    if (!projectionValid && fx2 > 1e-10f && fy2 > 1e-10f)
    {
        float gpu[3][3];
        for (int j = 0; j < 3; j++)
        {
            gpu[0][j] = (m[j] - cx * m[12 + j]) / sqrtf(fx2);
            gpu[1][j] = (m[4 + j] - cy * m[12 + j]) / sqrtf(fy2);
            gpu[2][j] = m[12 + j] / normW;
        }
        // Phyre's world/clip handedness differs from the native FFX view matrix.
        const float clipSign[3] = {-1, 1, -1}, worldSign[3] = {1, 1, -1};
        float best = 1e9f;
        unsigned age = 0;
        std::lock_guard<std::mutex> lock(cameraState);
        matchedCameraPose = cameraPose;
        matchedCameraValid = cameraPoseValid;
        for (unsigned t = 0; t < (std::min)(cameraHistoryCursor, 256u); t++)
        {
            auto &rec = cameraHistory[(cameraHistoryCursor - 1 - t) % 256];
            if (!rec.valid)
                continue;
            float error = 0;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                {
                    float d = rec.view[i * 4 + j] - gpu[j][i] * clipSign[j] * worldSign[i];
                    error += d * d;
                }
            if (error < best)
            {
                best = error;
                age = t;
                if (error < .0005f)
                {
                    matchedCameraPose = rec.pose;
                    matchedCameraValid = true;
                }
            }
        }
        if (diagnosticFrame)
            log("Camera pose match error=%.8f age=%u", best, age);
        matchedCameraError = best;
        matchedCameraAge = age;
    }
    if (diagnosticFrame && !projectionValid)
    {
        std::lock_guard<std::mutex> lock(cameraState);
        log("CAM GPU X %.4f %.4f %.4f Y %.4f %.4f %.4f Z %.4f %.4f %.4f", m[0] / sqrtf(fx2),
            m[1] / sqrtf(fx2), m[2] / sqrtf(fx2), m[4] / sqrtf(fy2), m[5] / sqrtf(fy2), m[6] / sqrtf(fy2),
            m[12] / normW, m[13] / normW, m[14] / normW);
        log("CAM native X %.4f %.4f %.4f Y %.4f %.4f %.4f Z %.4f %.4f %.4f", nativeHeadView[0],
            nativeHeadView[4], nativeHeadView[8], nativeHeadView[1], nativeHeadView[5], nativeHeadView[9],
            nativeHeadView[2], nativeHeadView[6], nativeHeadView[10]);
    }
    memset(out, 0, 64);
    out[0] = sqrtf(fx2) / normW;
    out[5] = sqrtf(fy2) / normW;
    out[10] = az;
    out[11] = (m[11] - az * m[15]) / normW;
    out[14] = 1;
    opticalCenterX = cx;
    opticalCenterY = cy;
    return std::isfinite(out[0]) && std::isfinite(out[5]) && out[0] > .05f && out[5] > .05f;
}
static bool reloadStereoShader()
{
    if (!gameDevice)
        return false;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%svr-output.hlsl", ROOT);
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompileFromFile(path, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                    0, &code, &errors);
    if (FAILED(hr))
    {
        log("Stereo shader compile failed %lx %s", hr,
            errors ? static_cast<const char *>(errors->GetBufferPointer()) : "");
        return false;
    }
    ComPtr<ID3D11ComputeShader> replacement;
    if (FAILED(gameDevice->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                               &replacement)))
        return false;
    stereoShader = replacement;
    log("Stereo shader loaded");
    return true;
}
static bool ensureOutput(ID3D11Texture2D *src)
{
    ComPtr<IUnknown> identity;
    if (SUCCEEDED(src->QueryInterface(IID_PPV_ARGS(&identity))))
        backBufferIdentities.insert(identity.Get());
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    if (gpuReady && d.Width == outputWidth && d.Height == outputHeight)
        return true;
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    {
        log("Unsupported VR source format %d", d.Format);
        return false;
    }
    src->GetDevice(gameDevice.ReleaseAndGetAddressOf());
    outputWidth = d.Width;
    outputHeight = d.Height;
    if (!stereoShader)
    {
        if (!reloadStereoShader())
            return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 176;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(gameDevice->CreateBuffer(&bd, nullptr, &stereoConstants)))
            return false;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(gameDevice->CreateSamplerState(&sd, &linearSampler)))
            return false;
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        if (FAILED(gameDevice->CreateSamplerState(&sd, &pointSampler)))
            return false;
    }
    d.Usage = D3D11_USAGE_DEFAULT;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    d.SampleDesc = {1, 0};
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(gameDevice->CreateTexture2D(&d, nullptr, cleanWorld.ReleaseAndGetAddressOf())))
        return false;
    if (FAILED(
            gameDevice->CreateRenderTargetView(cleanWorld.Get(), nullptr, worldRtv.ReleaseAndGetAddressOf())))
        return false;
    if (FAILED(gameDevice->CreateShaderResourceView(cleanWorld.Get(), nullptr,
                                                    frameSrv.ReleaseAndGetAddressOf())))
        return false;
    haveWorld = false;
    haveSceneWorld = false;
    worldGapFrames = 0;
    previousView.valid = false;
    previousDepth.Reset();
    previousDepthSrv.Reset();
    worldDepth.Reset();
    worldDepthSrv.Reset();
    if (FAILED(gameDevice->CreateTexture2D(&d, nullptr, colourCopy.ReleaseAndGetAddressOf())))
        return false;
    if (FAILED(gameDevice->CreateShaderResourceView(colourCopy.Get(), nullptr,
                                                    colourSrv.ReleaseAndGetAddressOf())))
        return false;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(gameDevice->CreateTexture2D(&d, nullptr, uiLayer.ReleaseAndGetAddressOf())))
        return false;
    if (FAILED(gameDevice->CreateShaderResourceView(uiLayer.Get(), nullptr, uiSrv.ReleaseAndGetAddressOf())))
        return false;
    if (FAILED(gameDevice->CreateRenderTargetView(uiLayer.Get(), nullptr, uiRtv.ReleaseAndGetAddressOf())))
        return false;
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(gameDevice->CreateTexture2D(&d, nullptr, desktopImage.ReleaseAndGetAddressOf())))
        return false;
    if (FAILED(gameDevice->CreateUnorderedAccessView(desktopImage.Get(), nullptr,
                                                     desktopUav.ReleaseAndGetAddressOf())))
        return false;
    hmd->GetRecommendedRenderTargetSize(&eyeWidth, &eyeHeight);
    // Use the headset's aspect; cap supersampling to limit 32-bit game memory usage.
    float reduction = (std::min)(1.f, 2048.f / float((std::max)(eyeWidth, eyeHeight)));
    eyeWidth = UINT(eyeWidth * reduction);
    eyeHeight = UINT(eyeHeight * reduction);
    d.Width = eyeWidth;
    d.Height = eyeHeight;
    for (int e = 0; e < 2; e++)
    {
        hmd->GetProjectionRaw(vr::EVREye(e), &tangents[e][0], &tangents[e][1], &tangents[e][2],
                              &tangents[e][3]);
        if (FAILED(gameDevice->CreateTexture2D(&d, nullptr, eyes[e].ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(gameDevice->CreateUnorderedAccessView(eyes[e].Get(), nullptr,
                                                         eyeUav[e].ReleaseAndGetAddressOf())))
            return false;
    }
    sourceTanX = (std::max)({fabsf(tangents[0][0]), fabsf(tangents[0][1]), fabsf(tangents[1][0]),
                             fabsf(tangents[1][1])}) *
                 1.08f;
    sourceTanY = (std::max)({fabsf(tangents[0][2]), fabsf(tangents[0][3]), fabsf(tangents[1][2]),
                             fabsf(tangents[1][3])}) *
                 1.08f;
    ComPtr<ID3D11DeviceContext> ctx;
    gameDevice->GetImmediateContext(&ctx);
    float clear[4] = {};
    ctx->ClearRenderTargetView(uiRtv.Get(), clear);
    gpuReady = true;
    log("Stereo resources: source %ux%u, each eye %ux%u, tangent half extents %.4f %.4f", outputWidth,
        outputHeight, eyeWidth, eyeHeight, sourceTanX, sourceTanY);
    return true;
}
static uint32_t shaderId(ID3D11VertexShader *shader)
{
    std::lock_guard<std::mutex> lock(shaderMutex);
    auto it = shaderIds.find(reinterpret_cast<uint64_t>(shader));
    return it == shaderIds.end() ? 0 : it->second;
}
struct BufferShadow
{
    std::vector<unsigned char> bytes;
    void *mapped = nullptr;
    size_t offset = 0, length = 0;
    bool valid = false;
};
static std::unordered_map<uint64_t, BufferShadow> bufferShadows;
static std::unordered_map<UINT, ComPtr<ID3D11Buffer>> wideBuffers;
static std::mutex shadowMutex;
static unsigned shadowMisses = 0, wideDraws = 0;
static bool battleHudSeen = false;
static unsigned markerDraws = 0, markerShadowMisses = 0;
static bool constantBuffer(reshade::api::device *d, reshade::api::resource r, D3D11_BUFFER_DESC &desc)
{
    if (d != gameApiDevice)
        return false;
    auto rd = d->get_resource_desc(r);
    if (rd.type != reshade::api::resource_type::buffer)
        return false;
    reinterpret_cast<ID3D11Buffer *>(r.handle)->GetDesc(&desc);
    return (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0 ||
           ((desc.BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER)) &&
            desc.ByteWidth <= 1048576);
}
static void createdBuffer(reshade::api::device *d, const reshade::api::resource_desc &desc,
                          const reshade::api::subresource_data *data, reshade::api::resource_usage,
                          reshade::api::resource r)
{
    if (internalDraw || desc.type != reshade::api::resource_type::buffer || !data || !data->data)
        return;
    D3D11_BUFFER_DESC bd{};
    if (!constantBuffer(d, r, bd))
        return;
    std::lock_guard<std::mutex> lock(shadowMutex);
    auto &s = bufferShadows[r.handle];
    s.bytes.resize(bd.ByteWidth);
    memcpy(s.bytes.data(), data->data, bd.ByteWidth);
    s.valid = true;
}
static void mappedBuffer(reshade::api::device *d, reshade::api::resource r, uint64_t offset, uint64_t size,
                         reshade::api::map_access access, void **data)
{
    if (internalDraw || access == reshade::api::map_access::read_only || !data || !*data)
        return;
    D3D11_BUFFER_DESC bd{};
    if (!constantBuffer(d, r, bd))
        return;
    if (offset >= bd.ByteWidth)
        return;
    std::lock_guard<std::mutex> lock(shadowMutex);
    auto &s = bufferShadows[r.handle];
    s.bytes.resize(bd.ByteWidth);
    s.mapped = *data;
    s.offset = size_t(offset);
    s.length = size_t((std::min)(size, uint64_t(bd.ByteWidth - offset)));
}
static void unmappedBuffer(reshade::api::device *, reshade::api::resource r)
{
    if (internalDraw)
        return;
    std::lock_guard<std::mutex> lock(shadowMutex);
    auto it = bufferShadows.find(r.handle);
    if (it == bufferShadows.end() || !it->second.mapped)
        return;
    auto &s = it->second;
    memcpy(s.bytes.data() + s.offset, s.mapped, s.length);
    s.mapped = nullptr;
    s.valid = true;
}
static bool updatedBuffer(reshade::api::device *d, const void *data, reshade::api::resource r,
                          uint64_t offset, uint64_t size)
{
    if (internalDraw || !data)
        return false;
    D3D11_BUFFER_DESC bd{};
    if (!constantBuffer(d, r, bd) || offset >= bd.ByteWidth)
        return false;
    std::lock_guard<std::mutex> lock(shadowMutex);
    auto &s = bufferShadows[r.handle];
    s.bytes.resize(bd.ByteWidth);
    memcpy(s.bytes.data() + size_t(offset), data, size_t((std::min)(size, uint64_t(bd.ByteWidth - offset))));
    s.valid = true;
    return false;
}
static bool updatedBufferCommand(reshade::api::command_list *c, const void *data, reshade::api::resource r,
                                 uint64_t offset, uint64_t size)
{
    return updatedBuffer(c->get_device(), data, r, offset, size);
}
static void destroyedBuffer(reshade::api::device *d, reshade::api::resource r)
{
    if (d == gameApiDevice)
    {
        std::lock_guard<std::mutex> lock(shadowMutex);
        bufferShadows.erase(r.handle);
    }
}
static bool drawWideWorld(ID3D11DeviceContext *c, WorldShaderInfo info, uint32_t n, uint32_t instances,
                          uint32_t first, int32_t offset, uint32_t fi, bool indexed)
{
    ComPtr<ID3D11Buffer> cb;
    c->VSGetConstantBuffers(0, 1, &cb);
    if (!cb)
        return false;
    std::vector<unsigned char> bytes;
    {
        std::lock_guard<std::mutex> lock(shadowMutex);
        auto it = bufferShadows.find(reinterpret_cast<uint64_t>(cb.Get()));
        if (it != bufferShadows.end() && it->second.valid)
            bytes = it->second.bytes;
    }
    if (bytes.size() < (std::max)(info.viewOffset, info.projectionOffset) + 64)
    {
        ++shadowMisses;
        return false;
    }
    float projection[16];
    float previousCenterX = opticalCenterX, previousCenterY = opticalCenterY;
    bool perspective =
        extractProjection(reinterpret_cast<float *>(bytes.data() + info.viewOffset),
                          reinterpret_cast<float *>(bytes.data() + info.projectionOffset), projection);
    if (!perspective && (!info.particle || !projectionValid))
        return false;
    float sx = perspective ? 1 / (sourceTanX * projection[0]) : worldScaleX,
          sy = perspective ? 1 / (sourceTanY * projection[5]) : worldScaleY;
    if (!autoProjection)
        sx = sy = 1;
    // Preserve Z/W depth, but widen the clip-space X/Y rows. Original buffers are
    // never edited; only this draw uses a private constant-buffer copy.
    auto widen = [&](size_t byteOffset) {
        if (byteOffset + 64 > bytes.size())
            return;
        float *m = reinterpret_cast<float *>(bytes.data() + byteOffset);
        for (int j = 0; j < 4; j++)
        {
            m[j] *= sx;
            m[4 + j] *= sy;
        }
    };
    std::sort(info.clipOffsets.begin(), info.clipOffsets.end());
    info.clipOffsets.erase(std::unique(info.clipOffsets.begin(), info.clipOffsets.end()),
                           info.clipOffsets.end());
    for (UINT clipOffset : info.clipOffsets)
        widen(clipOffset);
    auto &replacement = wideBuffers[UINT(bytes.size())];
    if (!replacement)
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = UINT(bytes.size());
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(gameDevice->CreateBuffer(&bd, nullptr, &replacement)))
            return false;
    }
    internalDraw = true;
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(c->Map(replacement.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
    {
        internalDraw = false;
        return false;
    }
    memcpy(map.pData, bytes.data(), bytes.size());
    c->Unmap(replacement.Get(), 0);
    ComPtr<ID3D11Buffer> psCb;
    c->PSGetConstantBuffers(0, 1, &psCb);
    ID3D11Buffer *b = replacement.Get();
    c->VSSetConstantBuffers(0, 1, &b);
    bool shared = psCb.Get() == cb.Get();
    if (shared)
        c->PSSetConstantBuffers(0, 1, &b);
    if (indexed)
        c->DrawIndexedInstanced(n, instances, first, offset, fi);
    else
        c->DrawInstanced(n, instances, first, fi);
    b = cb.Get();
    c->VSSetConstantBuffers(0, 1, &b);
    if (shared)
        c->PSSetConstantBuffers(0, 1, &b);
    internalDraw = false;
    if (!projectionValid && perspective)
    {
        memcpy(hardwareProjection, projection, sizeof(projection));
        hardwareProjection[0] *= sx;
        hardwareProjection[5] *= sy;
        opticalCenterX *= sx;
        opticalCenterY *= sy;
        projectionValid = true;
        worldScaleX = sx;
        worldScaleY = sy;
        ComPtr<ID3D11DepthStencilView> dsv;
        c->OMGetRenderTargets(0, nullptr, &dsv);
        worldDepthSource.Reset();
        if (dsv)
        {
            ComPtr<ID3D11Resource> res;
            dsv->GetResource(&res);
            res.As(&worldDepthSource);
        }
        frameView.pose = matchedCameraPose;
        frameView.valid = matchedCameraValid;
        memcpy(frameView.projection, hardwareProjection, sizeof(hardwareProjection));
        frameView.cx = opticalCenterX;
        frameView.cy = opticalCenterY;
    }
    else
    {
        opticalCenterX = previousCenterX;
        opticalCenterY = previousCenterY;
    }
    if (info.particle)
        ++particleDraws;
    // A clear between world passes is not the final scene. Subsequent geometry
    // invalidates that early snapshot until the next clear or first HUD draw.
    if (stableWorldDepth && !worldCaptured)
    {
        frameDepthValid = false;
        frameDepthStage = 0;
    }
    ++worldDraws;
    ++wideDraws;
    return true;
}
static std::vector<unsigned char> inspectBufferRange(ID3D11DeviceContext *c, ID3D11Buffer *buffer, UINT start,
                                                     UINT count)
{
    std::vector<unsigned char> data;
    if (!buffer || !count || count > 1048576)
        return data;
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    if (start >= desc.ByteWidth || count > desc.ByteWidth - start)
        return data;
    D3D11_BUFFER_DESC staging{};
    staging.ByteWidth = count;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> copy;
    if (FAILED(gameDevice->CreateBuffer(&staging, nullptr, &copy)))
        return data;
    D3D11_BOX box{start, 0, 0, start + count, 1, 1};
    c->CopySubresourceRegion(copy.Get(), 0, 0, 0, 0, buffer, 0, &box);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(c->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &map)))
    {
        data.resize(count);
        memcpy(data.data(), map.pData, count);
        c->Unmap(copy.Get(), 0);
    }
    return data;
}
static std::vector<unsigned char> hudBufferRange(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT start,
                                                 UINT count)
{
    if (!b || !count || count > 4096)
        return {};
    {
        std::lock_guard<std::mutex> lock(shadowMutex);
        auto it = bufferShadows.find(reinterpret_cast<uint64_t>(b));
        if (it != bufferShadows.end() && it->second.valid &&
            uint64_t(start) + count <= it->second.bytes.size())
            return {it->second.bytes.begin() + start, it->second.bytes.begin() + start + count};
    }
    // Some FFX geometry predates the first presentation and therefore has no CPU
    // shadow. Read only this tiny draw's range, never the whole vertex arena.
    D3D11_BUFFER_DESC bd{};
    b->GetDesc(&bd);
    if (uint64_t(start) + count > bd.ByteWidth)
        return {};
    static std::unordered_map<UINT, ComPtr<ID3D11Buffer>> stagingBuffers;
    auto &staging = stagingBuffers[count];
    if (!staging)
    {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = count;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(gameDevice->CreateBuffer(&d, nullptr, &staging)))
            return {};
    }
    D3D11_BOX box{start, 0, 0, start + count, 1, 1};
    c->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, b, 0, &box);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(c->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return {};
    std::vector<unsigned char> data(count);
    memcpy(data.data(), map.pData, count);
    c->Unmap(staging.Get(), 0);
    if (diagnosticFrame)
        log("HUD buffer fallback bytes=%u usage=%u bind=%x range=%u+%u", bd.ByteWidth, bd.Usage, bd.BindFlags,
            start, count);
    return data;
}
static constexpr UINT maxCursorQuads = 32, maxCursorIndices = maxCursorQuads * 6;
static bool hudPositions(ID3D11DeviceContext *c, UINT n, UINT first, int offset, bool indexed,
                         float (*positions)[3], float (*uv)[2] = nullptr)
{
    if (!n || n > maxCursorIndices)
        return false;
    UINT vertices[maxCursorIndices]{}, low = UINT_MAX, high = 0;
    ComPtr<ID3D11Buffer> ib;
    std::vector<unsigned char> indices;
    UINT indexSize = 0, indexStart = 0;
    if (indexed)
    {
        DXGI_FORMAT format;
        c->IAGetIndexBuffer(&ib, &format, &indexStart);
        if (format != DXGI_FORMAT_R16_UINT && format != DXGI_FORMAT_R32_UINT)
            return false;
        indexSize = format == DXGI_FORMAT_R16_UINT ? 2 : 4;
        indices = hudBufferRange(c, ib.Get(), indexStart + first * indexSize, n * indexSize);
        if (indices.size() != n * indexSize)
            return false;
    }
    for (UINT i = 0; i < n; i++)
    {
        int64_t vertex = first + i;
        if (indexed)
        {
            vertex = indexSize == 2 ? reinterpret_cast<const uint16_t *>(indices.data())[i]
                                    : reinterpret_cast<const uint32_t *>(indices.data())[i];
            vertex += offset;
        }
        if (vertex < 0 || vertex > UINT_MAX)
            return false;
        vertices[i] = UINT(vertex);
        low = (std::min)(low, vertices[i]);
        high = (std::max)(high, vertices[i]);
    }
    auto readStream = [&](UINT slot, UINT elementOffset, UINT components, float *out, UINT outputStride) {
        ComPtr<ID3D11Buffer> vb;
        UINT stride = 0, start = 0;
        c->IAGetVertexBuffers(slot, 1, &vb, &stride, &start);
        if (!vb || stride < elementOffset + components * 4 || high - low > maxCursorIndices)
            return false;
        uint64_t begin = uint64_t(start) + uint64_t(low) * stride + elementOffset;
        if (begin > UINT_MAX)
            return false;
        UINT count = (high - low) * stride + components * 4;
        auto data = hudBufferRange(c, vb.Get(), UINT(begin), count);
        if (data.size() != count)
        {
            ++markerShadowMisses;
            return false;
        }
        for (UINT i = 0; i < n; i++)
            memcpy(out + i * outputStride, data.data() + (vertices[i] - low) * stride, components * 4);
        return true;
    };
    if (!readStream(0, 0, 3, &positions[0][0], 3))
        return false;
    if (uv)
    {
        ComPtr<ID3D11InputLayout> layout;
        c->IAGetInputLayout(&layout);
        HudInputLayout info{};
        {
            std::lock_guard<std::mutex> lock(shaderMutex);
            auto it = hudInputLayouts.find(reinterpret_cast<uint64_t>(layout.Get()));
            if (it != hudInputLayouts.end())
                info = it->second;
        }
        if (!info.hasUV || !readStream(info.uvSlot, info.uvOffset, 2, &uv[0][0], 2))
            return false;
    }
    return true;
}
static bool worldMarker(ID3D11DeviceContext *c, uint32_t hash, UINT n, UINT first, int offset, bool indexed)
{
    if (hash != 0x70b23021 || !projectionValid || (n != 3 && n != 6 && n != 18))
        return false;
    float p[24][3]{};
    if (!hudPositions(c, n, first, offset, indexed, p))
        return false;
    // These three health-bar batches precede the overhead arrows in the verified
    // battle renderer. They distinguish battle markers from minimap triangles.
    if (n == 18)
    {
        if (p[0][0] >= 200 && p[0][0] <= 400 && p[0][1] >= 300 && p[0][1] <= 500)
            battleHudSeen = true;
        return false;
    }
    if (!battleHudSeen)
        return false;
    float minX = p[0][0], maxX = minX, minY = p[0][1], maxY = minY, area = 0;
    UINT unique = 0;
    for (UINT i = 0; i < n; i++)
    {
        if (!std::isfinite(p[i][0]) || !std::isfinite(p[i][1]) || fabsf(p[i][2] - 1) > .01f)
            return false;
        minX = (std::min)(minX, p[i][0]);
        maxX = (std::max)(maxX, p[i][0]);
        minY = (std::min)(minY, p[i][1]);
        maxY = (std::max)(maxY, p[i][1]);
        bool exists = false;
        for (UINT j = 0; j < i; j++)
            if (fabsf(p[i][0] - p[j][0]) < .01f && fabsf(p[i][1] - p[j][1]) < .01f)
                exists = true;
        if (!exists)
            ++unique;
    }
    for (UINT i = 0; i + 2 < n; i += 3)
        area += fabsf((p[i + 1][0] - p[i][0]) * (p[i + 2][1] - p[i][1]) -
                      (p[i + 1][1] - p[i][1]) * (p[i + 2][0] - p[i][0])) *
                .5f;
    float width = maxX - minX, height = maxY - minY;
    bool triangle = width > 2 && width < 100 && height > 2 && height < 150 &&
                    (unique == 3 || area < width * height * .85f);
    if (diagnosticFrame)
        log("Marker geometry n=%u unique=%u size=%.2fx%.2f ratio=%.3f selected=%d", n, unique, width, height,
            width * height > 0 ? area / (width * height) : 0, triangle);
    return triangle;
}
static UINT texturedWorldMarkers(ID3D11DeviceContext *c, uint32_t hash, UINT n, UINT first, int offset,
                                 bool indexed)
{
    // Enemy selection combines cursors and other HUD sprites into a 66-index
    // batch. Classify individual quads rather than rejecting that larger batch.
    if (hash != 0x8281a370 || !projectionValid || !battleHudSeen || n > maxCursorIndices || n % 6)
        return 0;
    ComPtr<ID3D11ShaderResourceView> srv;
    c->PSGetShaderResources(0, 1, &srv);
    if (!srv)
        return 0;
    ComPtr<ID3D11Resource> resource;
    srv->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture)))
        return 0;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Width != 1024 || desc.Height != 1024 || desc.Format != DXGI_FORMAT_BC3_UNORM)
        return 0;
    float p[maxCursorIndices][3]{}, uv[maxCursorIndices][2]{};
    if (!hudPositions(c, n, first, offset, indexed, p, uv))
        return 0;
    UINT mask = 0;
    for (UINT q = 0; q < n / 6; q++)
    {
        float minU = 1e9f, minV = 1e9f, maxU = -1e9f, maxV = -1e9f, minX = 1e9f, minY = 1e9f, maxX = -1e9f,
              maxY = -1e9f;
        bool flat = true;
        for (UINT i = q * 6; i < q * 6 + 6; i++)
        {
            minU = (std::min)(minU, uv[i][0] * 1024);
            maxU = (std::max)(maxU, uv[i][0] * 1024);
            minV = (std::min)(minV, uv[i][1] * 1024);
            maxV = (std::max)(maxV, uv[i][1] * 1024);
            minX = (std::min)(minX, p[i][0]);
            maxX = (std::max)(maxX, p[i][0]);
            minY = (std::min)(minY, p[i][1]);
            maxY = (std::max)(maxY, p[i][1]);
            flat &= fabsf(p[i][2] - 1) < .01f;
        }
        // Verified battle atlas: green/red target cursors. The same sprite also
        // appears at the fixed left edge of the turn-order strip (x=647).
        bool cursor = minU >= 560 && maxU <= 636 && maxU - minU > 20 &&
                      ((minV >= 450 && maxV <= 506) || (minV >= 514 && maxV <= 570));
        bool turnOrder = fabsf(minX - 647) < 2 && minY >= -340 && maxY < 100;
        bool selected = cursor && flat && !turnOrder && maxX - minX < 100 && maxY - minY < 100;
        if (selected)
            mask |= 1u << q;
        if (diagnosticFrame)
            log("Cursor quad=%u p=%.2f %.2f..%.2f %.2f uv=%.2f %.2f..%.2f %.2f cursor=%d turnOrder=%d "
                "world=%d",
                q, minX, minY, maxX, maxY, minU, minV, maxU, maxV, cursor, turnOrder, selected);
    }
    return mask;
}
static bool routingMarkerSegments = false;
static void inspectHudTexture(ID3D11DeviceContext *c, ID3D11Texture2D *texture,
                              const wchar_t *label = nullptr)
{
    static std::unordered_set<ID3D11Texture2D *> seen;
    if (!label && seen.count(texture))
        return;
    if (!label)
        seen.insert(texture);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    UINT rowBytes = 0, rows = desc.Height;
    UINT format = desc.Format;
    if (format == 28 || format == 29 || format == 87 || format == 91)
    {
        rowBytes = desc.Width * 4;
    }
    else if (format >= 70 && format <= 84)
    {
        UINT block = (format <= 72 || format >= 79 && format <= 81) ? 8 : 16;
        rowBytes = ((desc.Width + 3) / 4) * block;
        rows = (desc.Height + 3) / 4;
    }
    else
        return;
    UINT limit = label ? 4096 : 2048;
    if (desc.Width > limit || desc.Height > limit || desc.SampleDesc.Count != 1)
        return;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(gameDevice->CreateTexture2D(&desc, nullptr, &copy)))
        return;
    c->CopyResource(copy.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(c->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return;
    wchar_t path[MAX_PATH];
    if (label)
        swprintf_s(path, L"%s%s.dds", ROOT, label);
    else
        swprintf_s(path, L"%shud-texture-%p.dds", ROOT, texture);
    FILE *f = nullptr;
    _wfopen_s(&f, path, L"wb");
    if (f)
    {
        uint32_t header[37]{};
        header[0] = 0x20534444;
        header[1] = 124;
        header[2] = 0x100f;
        header[3] = desc.Height;
        header[4] = desc.Width;
        header[5] = rowBytes;
        header[7] = 1;
        header[19] = 32;
        header[20] = 4;
        header[21] = 0x30315844;
        header[27] = 0x1000;
        header[32] = format;
        header[33] = 3;
        header[35] = 1;
        fwrite(header, 1, sizeof(header), f);
        for (UINT y = 0; y < rows; y++)
            fwrite(static_cast<unsigned char *>(map.pData) + y * map.RowPitch, 1, rowBytes, f);
        fclose(f);
    }
    c->Unmap(copy.Get(), 0);
}
static void inspectHud(ID3D11DeviceContext *c, uint32_t hash, WorldShaderInfo info, UINT n, UINT first,
                       int offset, bool indexed)
{
    if (!diagnosticFrame || n > 24)
        return;
    ComPtr<ID3D11Buffer> cb;
    c->VSGetConstantBuffers(0, 1, &cb);
    std::vector<unsigned char> bytes;
    {
        std::lock_guard<std::mutex> lock(shadowMutex);
        auto it = bufferShadows.find(reinterpret_cast<uint64_t>(cb.Get()));
        if (it != bufferShadows.end() && it->second.valid)
            bytes = it->second.bytes;
    }
    if (info.projectionOffset + 64 <= bytes.size())
    {
        auto *m = reinterpret_cast<float *>(bytes.data() + info.projectionOffset);
        log("HUD vs=%08x n=%u WVP %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f "
            "%.5f %.5f %.5f",
            hash, n, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13],
            m[14], m[15]);
    }
    ComPtr<ID3D11Buffer> vb, ib;
    UINT stride = 0, start = 0;
    c->IAGetVertexBuffers(0, 1, &vb, &stride, &start);
    if (stride < 12 || stride > 256)
        return;
    UINT vertex = first;
    if (indexed)
    {
        DXGI_FORMAT format;
        UINT ibStart;
        c->IAGetIndexBuffer(&ib, &format, &ibStart);
        UINT size = format == DXGI_FORMAT_R16_UINT ? 2 : 4;
        auto data = inspectBufferRange(c, ib.Get(), ibStart + first * size, size);
        if (data.empty())
            return;
        vertex = size == 2 ? *reinterpret_cast<const uint16_t *>(data.data())
                           : *reinterpret_cast<const uint32_t *>(data.data());
        if (int64_t(vertex) + offset < 0)
            return;
        vertex += offset;
    }
    auto data = inspectBufferRange(c, vb.Get(), start + vertex * stride, stride);
    if (data.size() >= 12)
    {
        auto *f = reinterpret_cast<const float *>(data.data());
        log("HUD vertex stride=%u p=%.3f %.3f %.3f tail=%.6f %.6f", stride, f[0], f[1], f[2],
            f[stride / 4 - 2], f[stride / 4 - 1]);
    }
    ComPtr<ID3D11ShaderResourceView> srv;
    c->PSGetShaderResources(0, 1, &srv);
    if (srv)
    {
        ComPtr<ID3D11Resource> resource;
        srv->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resource.As(&texture)))
        {
            D3D11_TEXTURE2D_DESC d{};
            texture->GetDesc(&d);
            log("HUD texture=%p size=%ux%u format=%u", texture.Get(), d.Width, d.Height, d.Format);
            if (hash == 0x8281a370 || hash == 0x70b23021)
                inspectHudTexture(c, texture.Get());
        }
    }
}
static bool captureDraw(reshade::api::command_list *cl, uint32_t n, uint32_t instances, uint32_t first,
                        int32_t offset, uint32_t firstInstance, bool indexed)
{
    if (internalDraw || inEffects || !gpuReady || !outputVR || !n || cl->get_device() != gameApiDevice)
        return false;
    auto *c = reinterpret_cast<ID3D11DeviceContext *>(cl->get_native());
    ComPtr<ID3D11VertexShader> vs;
    c->VSGetShader(&vs, nullptr, nullptr);
    uint32_t hash = shaderId(vs.Get());
    WorldShaderInfo info{};
    bool world = false, flash = false;
    {
        std::lock_guard<std::mutex> lock(shaderMutex);
        auto it = worldShaderInfo.find(hash);
        if (it != worldShaderInfo.end())
        {
            info = it->second;
            world = true;
        }
        flash = flashShaders.count(hash) != 0;
    }
    bool hud = hash == 0x70b23021 || hash == 0x8281a370 || hash == 0xdb2199d1 || hash == 0x173beeaa;
    if (hud)
        inspectHud(c, hash, info, n, first, offset, indexed);
    hud = hud || flash;
    if (!world && !hud)
    {
        // Observe actual full-screen presentation, rather than assuming offscreen
        // geometry means that the back buffer contains a new world image.
        if (n <= 6)
        {
            ComPtr<ID3D11RenderTargetView> target;
            c->OMGetRenderTargets(1, &target, nullptr);
            if (target)
            {
                ComPtr<ID3D11Resource> res;
                target->GetResource(&res);
                ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(res.As(&tex)) && finalScreenTarget(tex.Get()))
                {
                    D3D11_VIEWPORT viewport{};
                    UINT count = 1;
                    c->RSGetViewports(&count, &viewport);
                    if (viewport.Width == outputWidth && viewport.Height == outputHeight)
                    {
                        freshFlatFrame = true;
                        if (hash == 0x3e9c4ca0 && worldDraws && projectionValid)
                            finalWorldPresented = true;
                    }
                }
            }
        }
        return false;
    }
    ComPtr<ID3D11DepthStencilState> ds;
    UINT ref;
    c->OMGetDepthStencilState(&ds, &ref);
    D3D11_DEPTH_STENCIL_DESC depthState{};
    if (ds)
        ds->GetDesc(&depthState);
    if (hud && !flash && depthState.DepthEnable)
    {
        hud = false;
        info.particle = true;
    }
    if (hud)
        world = false;
    if (hud && !anchorUI)
        return false;
    if (world && !depthState.DepthEnable)
        return false;
    D3D11_VIEWPORT vp{};
    UINT nv = 1;
    c->RSGetViewports(&nv, &vp);
    if (!flash && (vp.Width != outputWidth || vp.Height != outputHeight))
        return false;
    ComPtr<ID3D11RenderTargetView> rt;
    ComPtr<ID3D11DepthStencilView> dv;
    c->OMGetRenderTargets(1, &rt, &dv);
    if (!rt)
        return false;
    ComPtr<ID3D11Resource> resource;
    rt->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture)))
        return false;
    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);
    if (td.Width != outputWidth || td.Height != outputHeight || td.SampleDesc.Count != 1)
        return false;
    // Flash constructs text/filter surfaces at full screen size. Only its final
    // draws to the swap-chain image belong on the VR panel.
    if (flash && !finalScreenTarget(texture.Get()))
        return false;
    if (world)
        return drawWideWorld(c, info, n, instances, first, offset, firstInstance, indexed);
    UINT texturedMarkers =
        (!flash && !routingMarkerSegments) ? texturedWorldMarkers(c, hash, n, first, offset, indexed) : 0;
    if (texturedMarkers)
    {
        if (!worldCaptured)
        {
            if (stableWorldDepth && !frameDepthValid)
                captureWorldDepth(c, 2);
            c->CopyResource(cleanWorld.Get(), texture.Get());
            worldCaptured = true;
        }
        routingMarkerSegments = true;
        for (UINT q = 0; q < n / 6; q++)
        {
            bool handled = false;
            if (texturedMarkers & (1u << q))
            {
                ID3D11RenderTargetView *target = worldRtv.Get();
                c->OMSetRenderTargets(1, &target, nullptr);
                info.particle = true;
                handled = drawWideWorld(c, info, 6, instances, first + q * 6, offset, firstInstance, indexed);
                target = rt.Get();
                c->OMSetRenderTargets(1, &target, dv.Get());
                if (handled)
                    ++markerDraws;
            }
            if (!handled)
            {
                handled = captureDraw(cl, 6, instances, first + q * 6, offset, firstInstance, indexed);
                if (!handled)
                {
                    internalDraw = true;
                    if (indexed)
                        c->DrawIndexedInstanced(6, instances, first + q * 6, offset, firstInstance);
                    else
                        c->DrawInstanced(6, instances, first + q * 6, firstInstance);
                    internalDraw = false;
                }
            }
        }
        routingMarkerSegments = false;
        return true;
    }
    if (!flash && worldMarker(c, hash, n, first, offset, indexed))
    {
        if (!worldCaptured)
        {
            if (stableWorldDepth && !frameDepthValid)
                captureWorldDepth(c, 2);
            c->CopyResource(cleanWorld.Get(), texture.Get());
            worldCaptured = true;
        }
        ID3D11RenderTargetView *target = worldRtv.Get();
        c->OMSetRenderTargets(1, &target, nullptr);
        info.particle = true;
        bool drawn = drawWideWorld(c, info, n, instances, first, offset, firstInstance, indexed);
        target = rt.Get();
        c->OMSetRenderTargets(1, &target, dv.Get());
        if (drawn)
        {
            ++markerDraws;
            return true;
        }
    }
    if (worldDraws && !worldCaptured)
    {
        if (stableWorldDepth && !frameDepthValid)
            captureWorldDepth(c, 2);
        c->CopyResource(cleanWorld.Get(), texture.Get());
        worldCaptured = true;
    }
    // Duplicate only verified screen-space shaders; preserve the original game draw.
    ComPtr<ID3D11BlendState> blend;
    float factors[4];
    UINT mask;
    c->OMGetBlendState(&blend, factors, &mask);
    auto &correctAlpha = alphaBlends[blend.Get()];
    if (!correctAlpha)
    {
        D3D11_BLEND_DESC bd{};
        if (blend)
            blend->GetDesc(&bd);
        else
        {
            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        }
        auto &t = bd.RenderTarget[0];
        t.SrcBlendAlpha = D3D11_BLEND_ONE;
        t.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (!blend)
            t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        else if (t.RenderTargetWriteMask & 7)
            t.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
        gameDevice->CreateBlendState(&bd, &correctAlpha);
    }
    if (!correctAlpha)
        return false;
    // Flash uses depth/stencil for UI clipping. Replay against a private snapshot
    // so neither its mask nor its original draw is changed by capturing it.
    ID3D11DepthStencilView *drawDepth = nullptr;
    if (flash && dv)
    {
        ComPtr<ID3D11Resource> dr;
        dv->GetResource(&dr);
        ComPtr<ID3D11Texture2D> dt;
        if (SUCCEEDED(dr.As(&dt)))
        {
            D3D11_TEXTURE2D_DESC dd{}, existing{};
            dt->GetDesc(&dd);
            if (uiDepth)
                uiDepth->GetDesc(&existing);
            if (!uiDepth || memcmp(&dd, &existing, sizeof(dd)))
            {
                uiDepthView.Reset();
                uiDepth.Reset();
                D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
                dv->GetDesc(&vd);
                if (SUCCEEDED(gameDevice->CreateTexture2D(&dd, nullptr, &uiDepth)))
                    gameDevice->CreateDepthStencilView(uiDepth.Get(), &vd, &uiDepthView);
            }
            if (uiDepthView)
            {
                if (!uiDepthCopied || uiDepthSource.Get() != dt.Get())
                {
                    c->CopyResource(uiDepth.Get(), dt.Get());
                    uiDepthSource = dt;
                    uiDepthCopied = true;
                }
                drawDepth = uiDepthView.Get();
            }
        }
    }
    internalDraw = true;
    ID3D11RenderTargetView *target = uiRtv.Get();
    c->OMSetRenderTargets(1, &target, drawDepth);
    c->OMSetBlendState(correctAlpha.Get(), factors, mask);
    if (indexed)
        c->DrawIndexedInstanced(n, instances, first, offset, firstInstance);
    else
        c->DrawInstanced(n, instances, first, firstInstance);
    target = rt.Get();
    c->OMSetRenderTargets(1, &target, dv.Get());
    c->OMSetBlendState(blend.Get(), factors, mask);
    internalDraw = false;
    ++uiDraws;
    if (flash)
        ++flashDraws;
    return false;
}
static void cleanBeforeEffects(reshade::api::effect_runtime *r)
{
    if (gpuReady && outputVR && r->get_device() == gameApiDevice && (!stableWorldDepth || !frameDepthValid))
    {
        ComPtr<ID3D11DeviceContext> c;
        gameDevice->GetImmediateContext(&c);
        captureWorldDepth(c.Get(), 3);
    }
    if (gpuReady && worldCaptured && projectionValid && anchorUI && outputVR &&
        r->get_device() == gameApiDevice)
    {
        ComPtr<ID3D11DeviceContext> c;
        gameDevice->GetImmediateContext(&c);
        auto *back = reinterpret_cast<ID3D11Texture2D *>(r->get_current_back_buffer().handle);
        c->CopyResource(back, cleanWorld.Get());
    }
}
struct StereoParams
{
    float bounds[4], projection[4], settings[4], panel[4], rotation[3][4], output[4], warp[3][4];
};
static void dumpStereoInputs(ID3D11DeviceContext *c, const StereoParams &p)
{
    if (!haveWorld || !worldDepth)
        return;
    inspectHudTexture(c, colourCopy.Get(), L"stereo-colour");
    D3D11_TEXTURE2D_DESC desc{};
    worldDepth->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R24G8_TYPELESS && desc.Format != DXGI_FORMAT_R32_TYPELESS)
        return;
    UINT format = desc.Format;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(gameDevice->CreateTexture2D(&desc, nullptr, &copy)))
        return;
    c->CopyResource(copy.Get(), worldDepth.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(c->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sstereo-depth.bin", ROOT);
    FILE *f = nullptr;
    _wfopen_s(&f, path, L"wb");
    if (f)
    {
        UINT header[] = {desc.Width, desc.Height, format};
        fwrite(header, 1, sizeof(header), f);
        for (UINT y = 0; y < desc.Height; y++)
            fwrite(static_cast<unsigned char *>(map.pData) + y * map.RowPitch, 4, desc.Width, f);
        fclose(f);
    }
    c->Unmap(copy.Get(), 0);
    StereoParams captured = p;
    captured.output[0] = float(eyeWidth);
    captured.output[1] = float(eyeHeight);
    captured.output[2] = 0;
    swprintf_s(path, L"%sstereo-params.bin", ROOT);
    _wfopen_s(&f, path, L"wb");
    if (f)
    {
        fwrite(&captured, 1, sizeof(captured), f);
        fclose(f);
    }
}
static int stableTransitions = 0;
static FILE *transitionTrace = nullptr;
static ULONGLONG transitionTraceEnd = 0;
static void transitionOptions()
{
    static ULONGLONG checked = 0;
    static int serial = 0;
    ULONGLONG now = GetTickCount64();
    if (transitionTrace && now >= transitionTraceEnd)
    {
        fclose(transitionTrace);
        transitionTrace = nullptr;
        log("Transition trace complete");
    }
    if (now - checked < 250)
        return;
    checked = now;
    stableTransitions = GetPrivateProfileIntW(L"Headlook", L"StableTransitions", 0, cfg);
    stableWorldDepth = GetPrivateProfileIntW(L"Headlook", L"StableWorldDepth", 1, cfg);
    static int shaderSerial = 0;
    int requestedShader = GetPrivateProfileIntW(L"Headlook", L"ReloadShader", 0, cfg);
    if (requestedShader != shaderSerial && gameDevice)
    {
        shaderSerial = requestedShader;
        reloadStereoShader();
    }
    int requested = GetPrivateProfileIntW(L"Headlook", L"TraceTransitions", 0, cfg);
    if (requested == serial)
        return;
    serial = requested;
    if (transitionTrace)
    {
        fclose(transitionTrace);
        transitionTrace = nullptr;
    }
    if (!serial)
        return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%stransition-%d.csv", ROOT, serial);
    _wfopen_s(&transitionTrace, path, L"w");
    if (!transitionTrace)
        return;
    unsigned seconds =
        (std::max)(5, (std::min)(60, int(GetPrivateProfileIntW(L"Headlook", L"TraceSeconds", 30, cfg))));
    transitionTraceEnd = now + seconds * 1000;
    fprintf(transitionTrace, "time,mode,field,gap,world,particles,ui,flash,raw,fresh,poseValid,matchError,"
                             "matchAge,fx,fy,cx,cy,depthA,depthB,depthStage");
    for (int i = 0; i < 16; i++)
        fprintf(transitionTrace, ",base%d", i);
    for (int i = 0; i < 9; i++)
        fprintf(transitionTrace, ",render%d", i);
    for (int i = 0; i < 9; i++)
        fprintf(transitionTrace, ",head%d", i);
    fprintf(transitionTrace, "\n");
    log("Transition trace started serial=%d seconds=%u mode=%d", serial, seconds, stableTransitions);
}
static void traceTransition(bool field, bool raw, const StereoParams &p, const vr::HmdMatrix34_t &head)
{
    if (!transitionTrace)
        return;
    fprintf(transitionTrace, "%llu,%d,%d,%u,%u,%u,%u,%u,%d,%d,%d,%.9g,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
            GetTickCount64(), stableTransitions, field, worldGapFrames, worldDraws, particleDraws, uiDraws,
            flashDraws, raw, freshFlatFrame, displayView.valid, matchedCameraError, matchedCameraAge,
            p.projection[0], p.projection[1], displayView.cx, displayView.cy, p.projection[2],
            p.projection[3]);
    fprintf(transitionTrace, ",%u", frameDepthStage);
    {
        std::lock_guard<std::mutex> lock(cameraState);
        for (float f : nativeBaseView)
            fprintf(transitionTrace, ",%.9g", f);
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            fprintf(transitionTrace, ",%.9g", displayView.pose.m[i][j]);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            fprintf(transitionTrace, ",%.9g", head.m[i][j]);
    fprintf(transitionTrace, "\n");
    static unsigned frames = 0;
    if (++frames % 30 == 0)
        fflush(transitionTrace);
}
static bool renderStereo(reshade::api::effect_runtime *r)
{
    transitionOptions();
    gameApiDevice = r->get_device();
    auto *src = reinterpret_cast<ID3D11Texture2D *>(r->get_current_back_buffer().handle);
    if (!ensureOutput(src))
        return false;
    ComPtr<ID3D11DeviceContext> c;
    gameDevice->GetImmediateContext(&c);
    reshade::api::resource_view depthView{}, depthSrgb{};
    auto var = r->find_texture_variable("DLSS5_Feed.fx", "DLSS5_Depth");
    if (var.handle)
        r->get_texture_binding(var, &depthView, &depthSrgb);
    // The world's own DSV is stable across menu/UI depth-buffer changes.
    auto *depth = frameDepthValid ? frameDepthSrv.Get() : nullptr;
    bool field = worldDraws > 0 && projectionValid && (worldCaptured || finalWorldPresented);
    bool split = field && worldCaptured && uiDraws && anchorUI;
    if (field)
    {
        worldGapFrames = 0;
        if (worldDraws > particleDraws)
            haveSceneWorld = true;
    }
    else if (worldGapFrames < 6000)
        ++worldGapFrames;
    // The title/save screens need their complete current image, including layers
    // outside the captured HUD shaders. Only use it when something was drawn this
    // frame; an empty zone transition must not resurrect a cached save screen.
    bool rawPanel = !field && (!haveWorld || freshFlatFrame || uiDraws > 0);
    // Optional live comparison: a one/two-frame drawing gap should keep the
    // cached world's projection rather than put that world on the flat UI panel.
    if (stableTransitions && haveSceneWorld && worldGapFrames <= 2 && flashDraws < 8)
        rawPanel = false;
    bool capturedPanel = anchorUI && uiDraws > 0;
    bool delayed = delayedDLSS(r), depthCopied = false;
    if (field)
    {
        c->CopyResource(colourCopy.Get(), src);
        bool previous = delayed && previousView.valid;
        displayView = previous ? previousView : frameView;
        depthCopied =
            copyDepth(c.Get(), previous ? previousDepthSrv.Get() : depth, worldDepth, worldDepthSrv);
        if (delayed)
            copyDepth(c.Get(), depth, previousDepth, previousDepthSrv);
        previousView = frameView;
        haveWorld = true;
    }
    else
    {
        // A menu can stop all world drawing. Keep the last correctly projected world
        // behind its panel instead of suddenly putting the world itself on the panel.
        c->CopyResource(cleanWorld.Get(), src);
        previousView.valid = false;
        depthCopied = worldDepthSrv != nullptr;
    }
    vr::HmdMatrix34_t submitPose{};
    float panelRotation[3][3]{};
    {
        std::lock_guard<std::mutex> lock(cameraState);
        submitPose = trackedPose;
    }
    vr::TrackedDevicePose_t late[vr::k_unMaxTrackedDeviceCount]{};
    float frequency =
        hmd->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
    float photons = hmd->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                       vr::Prop_SecondsFromVsyncToPhotons_Float);
    float since = 0;
    hmd->GetTimeSinceLastVsync(&since, nullptr);
    float prediction =
        (std::max)(0.f, (std::min)(.05f, (frequency > 1 ? 1 / frequency : 0) - since + photons));
    hmd->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, prediction, late,
                                         vr::k_unMaxTrackedDeviceCount);
    if (late[0].bPoseIsValid && late[0].bDeviceIsConnected)
        submitPose = late[0].mDeviceToAbsoluteTracking;
    {
        std::lock_guard<std::mutex> lock(cameraState);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                for (int k = 0; k < 3; k++)
                    panelRotation[i][j] += (centerValid ? center[k][i] : float(k == i)) * submitPose.m[k][j];
    }
    // Preserve all compute state touched here, including the other mods' bindings.
    ComPtr<ID3D11ComputeShader> oldShader;
    ID3D11ClassInstance *oldClasses[256] = {};
    UINT classCount = 256;
    c->CSGetShader(&oldShader, oldClasses, &classCount);
    ID3D11ShaderResourceView *oldSrvs[3] = {};
    ID3D11SamplerState *oldSamplers[2] = {};
    ComPtr<ID3D11Buffer> oldCb;
    ComPtr<ID3D11UnorderedAccessView> oldUav;
    c->CSGetShaderResources(0, 3, oldSrvs);
    c->CSGetSamplers(0, 2, oldSamplers);
    c->CSGetConstantBuffers(0, 1, &oldCb);
    c->CSGetUnorderedAccessViews(0, 1, &oldUav);
    c->CSSetShader(stereoShader.Get(), nullptr, 0);
    ID3D11Buffer *cb = stereoConstants.Get();
    c->CSSetConstantBuffers(0, 1, &cb);
    ID3D11SamplerState *samplers[] = {linearSampler.Get(), pointSampler.Get()};
    c->CSSetSamplers(0, 2, samplers);
    ID3D11ShaderResourceView *srvs[] = {colourSrv.Get(), worldDepthSrv.Get(),
                                        rawPanel ? frameSrv.Get() : uiSrv.Get()};
    c->CSSetShaderResources(0, 3, srvs);
    StereoParams p{};
    const float *proj = displayView.projection;
    p.projection[0] = haveWorld ? 1.f / fabsf(proj[0]) : sourceTanX;
    p.projection[1] = haveWorld ? 1.f / fabsf(proj[5]) : sourceTanY;
    float wz = proj[14], B = proj[11];
    bool depthUsable = haveWorld && depthCopied && fabsf(wz) > .1f && fabsf(B) > .001f;
    p.projection[2] = depthUsable ? proj[10] / wz : 1;
    p.projection[3] = depthUsable ? B / wz : -1;
    p.settings[1] = .02f;
    p.settings[2] = haveWorld ? 1.f : 0.f;
    p.settings[3] = depthUsable ? 1.f : 0.f;
    p.panel[0] = panelWidth;
    p.panel[1] = panelWidth * float(outputHeight) / float(outputWidth);
    p.panel[2] = panelDistance;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
        {
            p.rotation[i][j] = panelRotation[i][j];
            if (displayView.valid && enabled)
                for (int k = 0; k < 3; k++)
                    p.warp[i][j] += displayView.pose.m[k][i] * submitPose.m[k][j];
            else
                p.warp[i][j] = float(i == j);
        }
    p.rotation[0][3] = displayView.cx;
    p.rotation[1][3] = displayView.cy;
    p.rotation[2][3] = rawPanel ? 1.f : 0.f;
    p.output[0] = float(eyeWidth);
    p.output[1] = float(eyeHeight);
    p.output[3] = (rawPanel || capturedPanel) ? 1.f : 0.f;
    traceTransition(field, rawPanel, p, submitPose);
    for (int e = 0; e < 2; e++)
    {
        memcpy(p.bounds, tangents[e], sizeof(p.bounds));
        float eyeX = hmd->GetEyeToHeadTransform(vr::EVREye(e)).m[0][3];
        p.panel[3] = eyeX;
        p.settings[0] = eyeX * stereoUnitsPerMetre * stereoStrength;
        c->UpdateSubresource(stereoConstants.Get(), 0, nullptr, &p, 0, 0);
        ID3D11UnorderedAccessView *uav = eyeUav[e].Get();
        c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        c->Dispatch((eyeWidth + 7) / 8, (eyeHeight + 7) / 8, 1);
    }
    if (split)
    {
        p.output[0] = float(outputWidth);
        p.output[1] = float(outputHeight);
        p.output[2] = 1;
        c->UpdateSubresource(stereoConstants.Get(), 0, nullptr, &p, 0, 0);
        ID3D11UnorderedAccessView *u = desktopUav.Get();
        c->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        c->Dispatch((outputWidth + 7) / 8, (outputHeight + 7) / 8, 1);
    }
    ID3D11UnorderedAccessView *nullUav = nullptr;
    c->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ID3D11ShaderResourceView *nullSrvs[3] = {};
    c->CSSetShaderResources(0, 3, nullSrvs);
    if (split)
        c->CopyResource(src, desktopImage.Get());
    c->CSSetShader(oldShader.Get(), oldClasses, classCount);
    c->CSSetShaderResources(0, 3, oldSrvs);
    c->CSSetSamplers(0, 2, oldSamplers);
    cb = oldCb.Get();
    c->CSSetConstantBuffers(0, 1, &cb);
    ID3D11UnorderedAccessView *uav = oldUav.Get();
    c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    for (auto *x : oldSrvs)
        if (x)
            x->Release();
    for (auto *x : oldSamplers)
        if (x)
            x->Release();
    for (UINT i = 0; i < classCount; i++)
        if (oldClasses[i])
            oldClasses[i]->Release();
    static int eyeCapture = 0;
    int requestedCapture = GetPrivateProfileIntW(L"Headlook", L"CaptureEyes", 0, cfg);
    if (requestedCapture != eyeCapture)
    {
        eyeCapture = requestedCapture;
        if (eyeCapture)
        {
            inspectHudTexture(c.Get(), eyes[0].Get(), L"vr-eye-left");
            inspectHudTexture(c.Get(), eyes[1].Get(), L"vr-eye-right");
            dumpStereoInputs(c.Get(), p);
            log("Captured eyes serial=%d field=%d rawPanel=%d capturedPanel=%d UI=%u", eyeCapture, field,
                rawPanel, capturedPanel, uiDraws);
        }
    }
    if (!headsetStandby)
    {
        c->Flush();
        int errors[2];
        for (int e = 0; e < 2; e++)
        {
            vr::VRTextureWithPose_t tex{};
            tex.handle = eyes[e].Get();
            tex.eType = vr::TextureType_DirectX;
            tex.eColorSpace = vr::ColorSpace_Gamma;
            tex.mDeviceToAbsoluteTracking = submitPose;
            errors[e] = compositor->Submit(vr::EVREye(e), &tex, nullptr, vr::Submit_TextureWithPose);
        }
        static int last = -1;
        int result = errors[0] * 1000 + errors[1];
        if (last != result)
        {
            log("Stereo submit left=%d right=%d", errors[0], errors[1]);
            last = result;
        }
        compositor->PostPresentHandoff();
    }
    static ULONGLONG lastLog = 0;
    if (GetTickCount64() - lastLog > 5000)
    {
        lastLog = GetTickCount64();
        log("Stereo field=%d worldDraws=%u UI=%u split=%d depth=%d scale=%.4f %.4f sourceTan=%.4f %.4f",
            field, worldDraws, uiDraws, split, depthUsable, scaleX, scaleY, p.projection[0], p.projection[1]);
        log("Wide draws=%u shadow misses=%u depth A=%.6f B=%.6f centre=%.5f %.5f", wideDraws, shadowMisses,
            p.projection[2], p.projection[3], opticalCenterX, opticalCenterY);
    }
    static ULONGLONG report = 0;
    if (GetTickCount64() - report > 5000)
    {
        report = GetTickCount64();
        log("VR corrections particles=%u flash=%u markers=%u markerMisses=%u directDepth=%d depthStage=%u "
            "stereo=%.2f delayedDLSS=%d renderPose=%d cachedWorld=%d standby=%d prediction=%.4f",
            particleDraws, flashDraws, markerDraws, markerShadowMisses, frameDepthValid, frameDepthStage,
            stereoStrength, delayed, displayView.valid, haveWorld && !field, headsetStandby, prediction);
    }
    float clear[4] = {};
    c->ClearRenderTargetView(uiRtv.Get(), clear);
    worldDraws = uiDraws = particleDraws = flashDraws = markerDraws = frameDepthStage = 0;
    battleHudSeen = false;
    worldCaptured = cbCaptured = projectionValid = freshFlatFrame = finalWorldPresented = uiDepthCopied =
        frameDepthValid = false;
    return true;
}
