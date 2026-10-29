#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <unordered_map>
#include <mutex>
#include <vector>
#pragma comment(lib, "d3dcompiler.lib")
static std::unordered_map<uint64_t, uint32_t> shaderIds;
struct WorldShaderInfo
{
    UINT viewOffset = 0, projectionOffset = 0;
    bool particle = false;
    std::vector<UINT> clipOffsets;
};
static std::unordered_map<uint32_t, bool> flashShaders;
static std::unordered_map<uint32_t, WorldShaderInfo> worldShaderInfo;
static std::mutex shaderMutex;
struct HudInputLayout
{
    UINT uvSlot = 0, uvOffset = 0;
    bool hasUV = false;
};
static std::unordered_map<uint64_t, HudInputLayout> hudInputLayouts;
static bool diagnosticFrame = false, inEffects = false;
static unsigned drawSerial = 0;
static void pipelineCreated(reshade::api::device *, reshade::api::pipeline_layout, uint32_t n,
                            const reshade::api::pipeline_subobject *subs, reshade::api::pipeline pipe)
{
    for (uint32_t i = 0; i < n; i++)
    {
        if (subs[i].type == reshade::api::pipeline_subobject_type::input_layout)
        {
            HudInputLayout layout{};
            auto *elements = static_cast<const reshade::api::input_element *>(subs[i].data);
            for (UINT j = 0; j < subs[i].count; j++)
                if (elements[j].semantic && !_stricmp(elements[j].semantic, "TEXCOORD") &&
                    elements[j].semantic_index == 0 &&
                    elements[j].format == reshade::api::format::r32g32_float)
                {
                    layout.uvSlot = elements[j].buffer_binding;
                    layout.uvOffset = elements[j].offset;
                    layout.hasUV = true;
                }
            std::lock_guard<std::mutex> lock(shaderMutex);
            hudInputLayouts[pipe.handle] = layout;
            continue;
        }
        bool vs = subs[i].type == reshade::api::pipeline_subobject_type::vertex_shader;
        if (!vs && subs[i].type != reshade::api::pipeline_subobject_type::pixel_shader)
            continue;
        auto &s = *static_cast<const reshade::api::shader_desc *>(subs[i].data);
        if (!s.code || !s.code_size)
            continue;
        uint32_t hash = 2166136261;
        for (size_t j = 0; j < s.code_size; j++)
            hash = (hash ^ static_cast<const unsigned char *>(s.code)[j]) * 16777619;
        {
            std::lock_guard<std::mutex> lock(shaderMutex);
            shaderIds[pipe.handle] = hash;
        }
        if (vs)
        {
            ID3D11ShaderReflection *reflection = nullptr;
            if (SUCCEEDED(D3DReflect(s.code, s.code_size, __uuidof(ID3D11ShaderReflection),
                                     reinterpret_cast<void **>(&reflection))))
            {
                auto *buffer = reflection->GetConstantBufferByName("$Globals");
                D3D11_SHADER_VARIABLE_DESC scene{}, wv{}, wvp{};
                if (SUCCEEDED(buffer->GetVariableByName("World")->GetDesc(&wv)) &&
                    SUCCEEDED(buffer->GetVariableByName("WorldViewProjection")->GetDesc(&wvp)))
                {
                    WorldShaderInfo info;
                    info.viewOffset = wv.StartOffset;
                    info.projectionOffset = wvp.StartOffset;
                    info.clipOffsets.push_back(wvp.StartOffset);
                    D3D11_SHADER_VARIABLE_DESC desc{};
                    info.particle = SUCCEEDED(buffer->GetVariableByName("softParticleScale")->GetDesc(&desc));
                    if (SUCCEEDED(buffer->GetVariableByName("ViewProjection")->GetDesc(&desc)))
                        info.clipOffsets.push_back(desc.StartOffset);
                    auto *sv = buffer->GetVariableByName("scene");
                    if (SUCCEEDED(sv->GetDesc(&scene)))
                        for (auto name : {"Projection", "ViewProjection", "ViewProjection2"})
                        {
                            D3D11_SHADER_TYPE_DESC type{};
                            if (SUCCEEDED(sv->GetType()->GetMemberTypeByName(name)->GetDesc(&type)))
                                info.clipOffsets.push_back(scene.StartOffset + type.Offset);
                        }
                    std::lock_guard<std::mutex> lock(shaderMutex);
                    worldShaderInfo[hash] = info;
                }
                D3D11_SHADER_VARIABLE_DESC flash{};
                if (SUCCEEDED(
                        reflection->GetConstantBufferByName("para")->GetVariableByName("world0")->GetDesc(
                            &flash)))
                {
                    std::lock_guard<std::mutex> lock(shaderMutex);
                    flashShaders[hash] = true;
                }
                reflection->Release();
            }
        }
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%sshader-diagnostics\\%s-%08x.txt", ROOT, vs ? L"vs" : L"ps", hash);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            continue;
        ID3DBlob *asmCode = nullptr;
        if (SUCCEEDED(D3DDisassemble(s.code, s.code_size, 0, nullptr, &asmCode)))
        {
            FILE *f = nullptr;
            _wfopen_s(&f, path, L"wb");
            if (f)
            {
                fwrite(asmCode->GetBufferPointer(), 1, asmCode->GetBufferSize(), f);
                fclose(f);
            }
            asmCode->Release();
        }
    }
}
static void diagnoseDraw(reshade::api::command_list *cl, uint32_t count)
{
    if (!diagnosticFrame || inEffects || cl->get_device()->get_api() != reshade::api::device_api::d3d11)
        return;
    auto *c = reinterpret_cast<ID3D11DeviceContext *>(cl->get_native());
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    ID3D11DepthStencilState *ds = nullptr;
    ID3D11RenderTargetView *rt = nullptr;
    ID3D11DepthStencilView *dv = nullptr;
    ID3D11BlendState *bs = nullptr;
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    UINT ref, mask;
    float factors[4];
    c->OMGetDepthStencilState(&ds, &ref);
    c->OMGetBlendState(&bs, factors, &mask);
    c->OMGetRenderTargets(1, &rt, &dv);
    D3D11_DEPTH_STENCIL_DESC d{};
    if (ds)
        ds->GetDesc(&d);
    D3D11_BLEND_DESC blend{};
    if (bs)
        bs->GetDesc(&blend);
    D3D11_VIEWPORT vp{};
    UINT nv = 1;
    c->RSGetViewports(&nv, &vp);
    uint32_t v = 0, p = 0;
    {
        std::lock_guard<std::mutex> lock(shaderMutex);
        v = shaderIds[reinterpret_cast<uint64_t>(vs)];
        p = shaderIds[reinterpret_cast<uint64_t>(ps)];
    }
    log("DRAW %u n=%u vs=%08x ps=%08x depth=%d write=%d func=%d blend=%d src=%d dst=%d rt=%p dv=%p "
        "vp=%.0fx%.0f z=%.3f..%.3f",
        drawSerial++, count, v, p, d.DepthEnable, d.DepthWriteMask, d.DepthFunc,
        blend.RenderTarget[0].BlendEnable, blend.RenderTarget[0].SrcBlend, blend.RenderTarget[0].DestBlend,
        rt, dv, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth);
    if (vs)
        vs->Release();
    if (ps)
        ps->Release();
    if (ds)
        ds->Release();
    if (rt)
        rt->Release();
    if (dv)
        dv->Release();
    if (bs)
        bs->Release();
}
static bool captureDraw(reshade::api::command_list *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t, bool);
static void cleanBeforeEffects(reshade::api::effect_runtime *);
static bool drawCall(reshade::api::command_list *c, uint32_t n, uint32_t instances, uint32_t first,
                     uint32_t fi)
{
    diagnoseDraw(c, n);
    return captureDraw(c, n, instances, first, 0, fi, false);
}
static bool drawIndexedCall(reshade::api::command_list *c, uint32_t n, uint32_t instances, uint32_t first,
                            int32_t vo, uint32_t fi)
{
    diagnoseDraw(c, n);
    return captureDraw(c, n, instances, first, vo, fi, true);
}
static void beforeEffects(reshade::api::effect_runtime *r, reshade::api::command_list *,
                          reshade::api::resource_view, reshade::api::resource_view)
{
    inEffects = true;
    cleanBeforeEffects(r);
}
static void nextDiagnosticFrame()
{
    static int old = 0;
    int serial = GetPrivateProfileIntW(L"Headlook", L"Diagnose", 0, cfg);
    diagnosticFrame = serial != old;
    old = serial;
    inEffects = false;
    drawSerial = 0;
    if (diagnosticFrame)
        log("DIAGNOSTIC FRAME %d", serial);
}
