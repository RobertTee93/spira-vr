#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <d3d11.h>
#include <mutex>
#include "reshade.hpp"
#include "openvr.h"
#include "MinHook.h"

// FFX Steam executable SHA256 is checked by the launcher before this loads.
// This addon changes only the field view matrix, never persistent camera/save data.
static wchar_t ROOT[MAX_PATH]; // Set by the reversible launcher for this process only.
static wchar_t cfg[MAX_PATH], logpath[MAX_PATH];
static vr::IVRSystem *hmd = nullptr;
static vr::IVRCompositor *compositor = nullptr;
static ID3D11Texture2D *frameTexture = nullptr;
static bool outputVR = false;
static float fovScale = 1;
static bool started = false, enabled = false, poseValid = false, centerValid = false;
static float relative[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, center[3][3];
static std::mutex cameraState;
static vr::HmdMatrix34_t trackedPose{}, cameraPose{};
static bool cameraPoseValid = false;
static bool headsetStandby = false;
static float nativeBaseView[16]{}, nativeHeadView[16]{};
struct CameraRecord
{
    float view[16]{};
    vr::HmdMatrix34_t pose{};
    bool valid = false;
};
static CameraRecord cameraHistory[256];
static unsigned cameraHistoryCursor = 0;
static void rememberCamera(const float *view)
{
    auto &record = cameraHistory[cameraHistoryCursor++ % 256];
    memcpy(record.view, view, 64);
    record.pose = cameraPose;
    record.valid = cameraPoseValid;
}
static unsigned long calls = 0, changed = 0;
static int centerSerial = -1;
static float previewYaw = 0;
static uintptr_t base = 0;
using LookAt = void(__cdecl *)(float *, const float *, const float *, const float *, float);
static LookAt original = nullptr;
using Projection = void(__cdecl *)(float *, const void *);
static Projection originalProjection = nullptr;
static void log(const char *fmt, ...)
{
    FILE *f = nullptr;
    _wfopen_s(&f, logpath, L"a");
    if (!f)
        return;
    fprintf(f, "%llu ", GetTickCount64());
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fputc('\n', f);
    fclose(f);
}
#include "diagnostics.hpp"
#include "vr-output.hpp"
static bool normalize(float *v)
{
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!std::isfinite(n) || n < 0.0001f)
        return false;
    for (int i = 0; i < 3; i++)
        v[i] /= n;
    return true;
}
static void cross(const float *a, const float *b, float *r)
{
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}
static void __cdecl hooked(float *out, const float *eye, const float *target, const float *up, float roll)
{
    std::lock_guard<std::mutex> cameraLock(cameraState);
    if (reinterpret_cast<uintptr_t>(out) != base + 0x1F11440)
    {
        original(out, eye, target, up, roll);
        return;
    }
    ++calls;
    if (calls == 1)
        log("Field camera observed: eye %.3f %.3f %.3f target %.3f %.3f %.3f up %.3f %.3f %.3f roll %.3f",
            eye[0], eye[1], eye[2], target[0], target[1], target[2], up[0], up[1], up[2], roll);
    if (!enabled || (!poseValid && previewYaw == 0))
    {
        original(out, eye, target, up, roll);
        cameraPose = trackedPose;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                cameraPose.m[i][j] = centerValid ? center[i][j] : float(i == j);
        cameraPoseValid = poseValid;
        rememberCamera(out);
        return;
    }
    original(out, eye, target, up, roll);
    memcpy(nativeBaseView, out, sizeof(nativeBaseView));
    float rot[3][3];
    memcpy(rot, relative, sizeof(rot));
    if (previewYaw != 0)
    {
        float a = previewYaw * 0.01745329252f;
        float t[3][3] = {{cosf(a), 0, sinf(a)}, {0, 1, 0}, {-sinf(a), 0, cosf(a)}};
        memcpy(rot, t, sizeof(rot));
    }
    // FFX uses positive camera Z and screen-down Y; OpenVR uses back Z and up Y.
    float old[16];
    memcpy(old, out, sizeof(old));
    float sign[3] = {1, -1, -1};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 3; j++)
        {
            out[i * 4 + j] = 0;
            for (int k = 0; k < 3; k++)
                out[i * 4 + j] += old[i * 4 + k] * rot[k][j] * sign[k] * sign[j];
        }
    ++changed;
    memcpy(nativeHeadView, out, sizeof(nativeHeadView));
    cameraPose = trackedPose;
    cameraPoseValid = poseValid;
    rememberCamera(out);
}
static void __cdecl projection(float *out, const void *in)
{
    originalProjection(out, in);
}
static void initialize()
{
    started = true;
    wcscpy_s(cfg, ROOT);
    wcscat_s(cfg, L"control.ini");
    wcscpy_s(logpath, ROOT);
    wcscat_s(logpath, L"headlook.log");
    base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    unsigned char expected[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x30};
    void *address = reinterpret_cast<void *>(base + 0x3C1BE0);
    if (memcmp(address, expected, sizeof(expected)))
    {
        log("REJECT: camera signature mismatch");
        return;
    }
    MH_STATUS a = MH_Initialize(), b = MH_CreateHook(address, &hooked, reinterpret_cast<void **>(&original));
    MH_STATUS c = b == MH_OK ? MH_EnableHook(address) : b;
    log("Hook initialize=%d create=%d enable=%d", a, b, c);
    unsigned char pexpected[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x1c};
    void *pa = reinterpret_cast<void *>(base + 0x3C3D60);
    if (!memcmp(pa, pexpected, 6) &&
        MH_CreateHook(pa, &projection, reinterpret_cast<void **>(&originalProjection)) == MH_OK)
        log("Projection hook=%d", MH_EnableHook(pa));
    // Private filename prevents ReShade's separate VR post-processing runtime from
    // processing these already post-processed frames again (including this addon).
    wchar_t dll[MAX_PATH];
    wcscpy_s(dll, ROOT);
    wcscat_s(dll, L"FFXHeadlookVR.dll");
    HMODULE vrmod = LoadLibraryW(dll);
    if (!vrmod)
    {
        log("OpenVR DLL load failed %lu", GetLastError());
        return;
    }
    auto init =
        reinterpret_cast<uint32_t(__cdecl *)(vr::EVRInitError *, vr::EVRApplicationType, const char *)>(
            GetProcAddress(vrmod, "VR_InitInternal2"));
    auto iface = reinterpret_cast<void *(__cdecl *)(const char *, vr::EVRInitError *)>(
        GetProcAddress(vrmod, "VR_GetGenericInterface"));
    if (!init || !iface)
    {
        log("OpenVR missing exports");
        return;
    }
    vr::EVRInitError err = vr::VRInitError_None;
    init(&err, vr::VRApplication_Scene, nullptr);
    log("OpenVR init=%d", err);
    if (err == vr::VRInitError_None)
    {
        hmd = static_cast<vr::IVRSystem *>(iface(vr::IVRSystem_Version, &err));
        log("OpenVR system=%p error=%d", hmd, err);
    }
    if (hmd)
    {
        compositor = static_cast<vr::IVRCompositor *>(iface(vr::IVRCompositor_Version, &err));
        log("Compositor=%p error=%d", compositor, err);
        if (compositor)
            compositor->SetTrackingSpace(vr::TrackingUniverseSeated);
        float l, r, t, b;
        hmd->GetProjectionRaw(vr::Eye_Left, &l, &r, &t, &b);
        log("HMD tangent bounds left=%.5f right=%.5f top=%.5f bottom=%.5f", l, r, t, b);
    }
}
static void present(reshade::api::effect_runtime *runtime)
{
    static bool inside = false;
    if (inside)
        return;
    inside = true;
    struct Guard
    {
        bool &v;
        ~Guard()
        {
            v = false;
        }
    } guard{inside};
    if (!started)
        initialize();
    static ULONGLONG lastLog = 0;
    static bool f8down = false, f9down = false;
    bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0, f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    {
        std::lock_guard<std::mutex> cameraLock(cameraState);
        if (foregroundPid == GetCurrentProcessId())
        {
            if (f8 && !f8down)
            {
                centerValid = false;
                log("F8 recenter requested");
            }
            if (f9 && !f9down)
            {
                enabled = !enabled;
                WritePrivateProfileStringW(L"Headlook", L"Enabled", enabled ? L"1" : L"0", cfg);
                log("F9 headlook enabled=%d", enabled);
            }
        }
        f8down = f8;
        f9down = f9;
        static ULONGLONG lastConfig = 0;
        auto now = GetTickCount64();
        if (now - lastConfig > 250)
        {
            lastConfig = now;
            enabled = GetPrivateProfileIntW(L"Headlook", L"Enabled", 0, cfg) != 0;
            outputVR = GetPrivateProfileIntW(L"Headlook", L"OutputVR", 0, cfg) != 0;
            fovScale = GetPrivateProfileIntW(L"Headlook", L"FovPercent", 100, cfg) / 100.0f;
            previewYaw = float(GetPrivateProfileIntW(L"Headlook", L"PreviewYaw", 0, cfg));
            int s = GetPrivateProfileIntW(L"Headlook", L"Recenter", 0, cfg);
            autoProjection = GetPrivateProfileIntW(L"Headlook", L"AutoProjection", 1, cfg) != 0;
            anchorUI = GetPrivateProfileIntW(L"Headlook", L"AnchorUI", 1, cfg) != 0;
            stereoStrength = GetPrivateProfileIntW(L"Headlook", L"StereoPercent", 0, cfg) / 100.f;
            if (s != centerSerial)
            {
                centerSerial = s;
                centerValid = false;
            }
        }
    }
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
    if (compositor && outputVR && runtime->get_device()->get_api() == reshade::api::device_api::d3d11)
    {
        headsetStandby = hmd->GetTrackedDeviceActivityLevel(0) == vr::k_EDeviceActivityLevel_Standby;
        renderStereo(runtime);
        if (!headsetStandby)
            compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        else
            hmd->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, 0, poses,
                                                 vr::k_unMaxTrackedDeviceCount);
    }
    else if (hmd)
        hmd->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, 0, poses,
                                             vr::k_unMaxTrackedDeviceCount);
    if (hmd)
    {
        std::lock_guard<std::mutex> cameraLock(cameraState);
        auto &p = poses[vr::k_unTrackedDeviceIndex_Hmd];
        poseValid = p.bPoseIsValid && p.bDeviceIsConnected;
        if (poseValid)
            trackedPose = p.mDeviceToAbsoluteTracking;
        if (poseValid)
        {
            if (!centerValid)
            {
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++)
                        center[i][j] = p.mDeviceToAbsoluteTracking.m[i][j];
                centerValid = true;
                log("Head orientation recentered");
            }
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                {
                    relative[i][j] = 0;
                    for (int k = 0; k < 3; k++)
                        relative[i][j] += center[k][i] * p.mDeviceToAbsoluteTracking.m[k][j];
                }
        }
    }
    if (GetTickCount64() - lastLog > 5000)
    {
        std::lock_guard<std::mutex> cameraLock(cameraState);
        lastLog = GetTickCount64();
        log("Status enabled=%d pose=%d fieldCalls=%lu modified=%lu forward=%.3f %.3f %.3f", enabled,
            poseValid, calls, changed, -relative[0][2], -relative[1][2], -relative[2][2]);
    }
    nextDiagnosticFrame();
}
extern "C" __declspec(dllexport) const char *NAME = "SpiraVR";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Head-controlled FFX in SteamVR, with anchored UI and optional experimental stereo.";
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        const DWORD rootLength = GetEnvironmentVariableW(L"SPIRA_VR_HOME", ROOT, MAX_PATH);
        if (rootLength == 0 || rootLength >= MAX_PATH - 32)
            return FALSE;
        if (ROOT[rootLength - 1] != L'\\')
            wcscat_s(ROOT, L"\\");
        if (!reshade::register_addon(module))
            return FALSE;
        reshade::register_event<reshade::addon_event::reshade_present>(present);
        reshade::register_event<reshade::addon_event::init_pipeline>(pipelineCreated);
        reshade::register_event<reshade::addon_event::init_resource>(createdBuffer);
        reshade::register_event<reshade::addon_event::map_buffer_region>(mappedBuffer);
        reshade::register_event<reshade::addon_event::unmap_buffer_region>(unmappedBuffer);
        reshade::register_event<reshade::addon_event::update_buffer_region>(updatedBuffer);
        reshade::register_event<reshade::addon_event::update_buffer_region_command>(updatedBufferCommand);
        reshade::register_event<reshade::addon_event::destroy_resource>(destroyedBuffer);
        reshade::register_event<reshade::addon_event::draw>(drawCall);
        reshade::register_event<reshade::addon_event::draw_indexed>(drawIndexedCall);
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(beforeEffects);
        reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(clearUiDepth);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        reshade::unregister_addon(module);
    }
    return TRUE;
}
