// FirstPersonCutscene.asi  --  GTA IV: The Complete Edition (1.2.0.59)
// ---------------------------------------------------------------------------
// Forces the first-person camera in the places the C06alt "First Person Mod"
// doesn't cover: cutscenes and the subway/trains. Dependency-free .asi
// (pattern scan + hooks, no ScriptHook); loads via FusionFix's ASI loader.
//
// How it works
//   * Camera: hook CopyCameraFrame at RVA 0x83E398 (after the position copy and
//     the rotation copy at 0x83E650). esi = the final camera matrix
//     (right@0 fwd@0x10 up@0x20 pos@0x30, fov@0x50). Overwriting it here reaches
//     the gameplay, vehicle AND cutscene cameras. stolen bytes 8B 47 50 89 46 50.
//   * Natives: self-contained invoker (FusionFix method). getNativeAddress is the
//     2nd .text match of "56 8B 35 ? ? ? ? 85 F6 75 06"; the table pointers are
//     read straight out of its body. Natives are only legal on the sim thread, so
//     they're called from a hook on CGame's per-frame process-chain call site
//     (same spot FusionFix uses). GTA IV natives return values through a trailing
//     output-pointer argument.
//   * Anchor: CPed::GetBoneMatrix (RVA 0x5E70B0) for the real head-bone matrix on
//     foot; the ped matrix read fresh on the render thread while in a vehicle
//     (the native poll is a frame stale and lags at speed); GET_CUTSCENE_PED_
//     POSITION slot (per-cutscene table, e.g. Vla4_a -> slot 10) in cutscenes.
//   * Look: an absolute world yaw seeded once (per FP session / per cutscene) and
//     moved only by the mouse, so strafing and the director's shot cuts don't
//     drag the view. In a vehicle the base yaw tracks the vehicle heading.
//   * Head: hidden via SET_DRAW_PLAYER_COMPONENT (HEAD/TEEF/FACE, + HAIR).
//
// Shipped surface: F7 -- toggle first person on/off, F5 -- save the current settings.
//
// Settings live in FirstPersonCutscene.ini next to the .asi (created with the
// defaults on first run; see the "settings (.ini)" section below). Everything
// else (live eye height/forward/FOV tuning, cutscene actor slot, head-bone
// rotation test, native invoker toggle, diagnostics dump) is gated behind a
// debug mode, off by default, toggled with Ctrl+F7.
//   F8 diagnostics dump   F9 recenter view
//   F11/F12 eye height -/+   Left/Right eye forward   PgUp/PgDn FOV
//   B head-bone rotation test   J force head-hide off   7/8 cutscene actor slot
//   F6 native invoker on/off   K/L/M/N aim options
//   F5 (SaveKey; works outside debug mode too) save the current settings into the
//   .ini, with an on-screen confirmation   Ctrl+F5 reload the .ini
// Eye height / forward / FOV are stored per context (on foot / car / train).
//
// Build: x86 DLL, /MT, no PCH, output extension .asi, next to GTAIV.exe.
// ---------------------------------------------------------------------------

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>

// Local build label, separate from the git tag. v0.2.0-beta is the last version
// actually pushed to GitHub; everything after it is unpublished local WIP until
// the next real tag. Bump the alphaN suffix each time a new build gets handed
// over, reset to alpha1 and bump the base version whenever a real tag lands.
#define FPMOD_VERSION "0.4.0-beta"

static uintptr_t g_moduleBase = 0;
static size_t    g_moduleSize = 0;
static uintptr_t g_hookReturn = 0;

static volatile uint8_t  g_enabled = 0;
static volatile int      g_debugMode = 0;   // off by default -- Ctrl+F7 toggles it (or DebugMode in the .ini).
                                             // Unlocks every live-tuning key except the on/off key itself.
static volatile float    g_eyeUp = 0.60f;
static volatile int      g_useNatives = 1;   // on by default now (F6 still toggles for debug)
// per-context tuning: [0] = on foot, [1] = in a car, [2] = on a train/subway
// (split out from [1] on 2026-09-13 -- user tuned trim/fov live while riding
// the subway to trim=0.33 fov=18, distinct from what feels right in a car)
static volatile float    g_eyeFwdV[3] = { 0.20f, 0.05f, 0.05f };    // forward nudge toward the nose (pulled back a bit in a vehicle)
static volatile float    g_eyeTrimV[3] = { 0.17f, 0.09f, 0.33f };   // up/down nudge (F11/F12)
static volatile float    g_fovBoostV[3] = { 21.0f, 30.0f, 18.0f };   // extra FOV degrees (PgUp/PgDn)
static volatile float    g_mouseSens = 0.0016f;
static volatile int      g_invertY = 0;         // .ini InvertY
static volatile float    g_maxPitch = 1.30f;    // look up/down limit, radians (~74.5 deg); .ini MaxPitchDegrees
static volatile int      g_toggleVk = VK_F7;    // first-person on/off key; .ini ToggleKey (Ctrl + it = debug mode)
static volatile int      g_saveVk = VK_F5;      // saves the current settings to the .ini (Ctrl + it reloads); 0 = disabled
static volatile int      g_showMsgs = 1;        // .ini ShowMessages: short on-screen text when settings are saved / reloaded
static char              g_toastText[128] = { 0 };
static volatile int      g_toastReq = 0;        // set by any thread, shown by the next sim-thread poll (natives are sim-thread only)
static uint32_t* g_frameCount = nullptr;
static uintptr_t         g_frameInsn = 0;      // the "inc [frameCount]" instruction, hook site
static uintptr_t         g_frameReturn = 0;
static uint8_t* g_userPause = nullptr;
static uint8_t* g_codePause = nullptr;
static uint32_t          g_lastLookFrame = 0;
static volatile LONG     g_mouseDX = 0, g_mouseDY = 0;   // from GetRawInputData hook (fallback)
static volatile int      g_mouseOK = 0;
static int32_t* g_diMouseX = nullptr;           // game's DirectInput mouse deltas
static int32_t* g_diMouseY = nullptr;

// CPed::GetBoneMatrix(this, rage::Matrix* out, int boneTag) -- __thiscall
typedef int(__thiscall* GetBoneMtx_t)(void* ped, void* outMtx, int boneTag);
static GetBoneMtx_t g_GetBoneMtx = nullptr;
static volatile int  g_boneRot = 0;            // 'B': use the head bone's own orientation as the FP base
static volatile int  g_aimAlign = 1;           // 'K': while the game's aim camera is live, take yaw/pitch from it
static volatile int  g_aimParallax = 1;        // 'L': ...and slide the eye onto its aim ray (crosshair == bullet line)
static volatile int  g_aimSide = 0;            // 'M': aim-cam shoulder: 0 = centered over the head, 1 = game default (right), 2 = left
static volatile int  g_aimVert = 0;            // 'N': also raise the aim ray to our eye height (experimental)
static const float   kAimShoulder = 0.475f;    // the game's own shoulder offset = aim cam local x @ +0x1C0
static volatile int  g_curWeapon = 0;          // GET_CURRENT_CHAR_WEAPON (weapon type id)
static volatile int  g_curWeaponSlot = -1;     // GET_WEAPONTYPE_SLOT of it (gun category)
static volatile int  g_aimActive = 0;          // the aim camera is live (set by the camera hook)
static volatile float g_aimFwdW[128] = { 0 };  // per-weapon extra eye-forward while aiming, metres (long guns' stocks clip the screen)
// fallback per weapon slot, so guns of one category inherit a tuned value. Slot 5 = assault
// rifles (M4 = weapon 15, AK47, ...): -0.08 tuned in-game 2026-09-19 -- pulling the eye BACK
// 8 cm is what stops the stock clipping the screen (other slots untuned = 0).
static volatile float g_aimFwdSlot[16] = { 0, 0, 0, 0, 0, -0.08f };
static const uintptr_t kAimCamVtblRva = 0xA9B98C;   // CCamAimWeapon vtable, 1.2.0.59 (found via the aim-cam pool dump)

// native-fed state (polled on the worker thread, read by the camera hook)
static volatile int   g_natOK = 0;
static volatile float g_headW[3] = { 0,0,0 };   // player head bone, world
static volatile float g_headMtx[16] = { 0 };    // full player head-bone matrix (world)
static volatile int   g_headMtxOK = 0;
static volatile float g_csPedW[3] = { 0,0,0 };   // chosen cutscene actor, world
static volatile int   g_csPedOK = 0;
#define CS_SLOTS 12
static volatile float g_csSlot[CS_SLOTS][3] = { {0} };
static volatile int   g_csSlotOK[CS_SLOTS] = { 0 };
static volatile int   g_csIndex = 0;             // which cutscene-ped slot the camera follows ('[' / ']')
static volatile int   g_csUserOverride = 0;      // 7/8 pinned a slot by hand -- stop auto-correcting to Niko
static volatile uint32_t g_nikoModel = 0;        // Niko's own model hash, from GET_CHAR_MODEL
static volatile uint32_t g_csSlotModel[CS_SLOTS] = { 0 };   // per-slot model hash (0 = unknown/not queried)
static volatile int      g_csModelMatch = -1;    // slot whose model == g_nikoModel this poll, -1 = none
static volatile int   g_inTrain = 0;
static volatile int   g_inCar = 0;
static volatile int   g_isRagdoll = 0;   // IS_PED_RAGDOLL(player) -- auto-switches to head-bone rotation
static volatile int   g_playerPed = 0;
static volatile float g_charHeading = 0;
static volatile int   g_headingOK = 0;
static volatile int   g_natLogged = 0;
static volatile int      g_pedRank = 0;
static volatile uint32_t g_hits = 0, g_applied = 0;
static volatile int      g_flipFwd = 0;
static volatile float    g_yawTrim = 0.0f;
static volatile float    g_pitchTrim = 0.0f;
static float             g_pedMtx[16] = { 0 };
static volatile int      g_fwdRow = 1;        // which camera row is "forward" (0/1/2)
static volatile int      g_fwdSign = 0;        // 0 = +row, 1 = -row
static float             g_dstRot[9] = { 0 };    // last seen shot-cam rotation rows
static volatile float    g_lookYaw = 0.0f;     // free-look, radians
static volatile float    g_lookPitch = 0.0f;
static volatile float    g_seedYaw = 0.0f;     // gameplay FP: absolute world yaw the view is built from
static volatile int      g_seedYawSet = 0;     // 0 = reseed on the next frame
static float             g_pedMtx3[16] = { 0 };

static volatile float g_pickX = 0, g_pickY = 0, g_pickZ = 0, g_pickD2 = -1;

static HINSTANCE g_selfInst = nullptr;
static char      g_logPath[MAX_PATH] = { 0 };
static char      g_iniPath[MAX_PATH] = { 0 };
static uint8_t** g_pCamPoolPtr = nullptr;
static uint8_t** g_pPedPoolPtr = nullptr;
static const char* g_cutsceneName = nullptr;
typedef void* (__cdecl* FindPlayerPed_t)(int);
static FindPlayerPed_t g_FindPlayerPed = nullptr;
static volatile float g_shotX = 0, g_shotY = 0, g_shotZ = 0;   // shot-cam pos seen by the hook

// ---- logging -----------------------------------------------------------
static void BuildLogPath()
{
    char p[MAX_PATH] = { 0 };
    GetModuleFileNameA(g_selfInst, p, MAX_PATH);
    char* d = strrchr(p, '.'); if (d) *d = 0;
    strcat_s(p, MAX_PATH, ".log"); strcpy_s(g_logPath, MAX_PATH, p);
}
static void BuildIniPath()
{
    char p[MAX_PATH] = { 0 };
    GetModuleFileNameA(g_selfInst, p, MAX_PATH);
    char* d = strrchr(p, '.'); if (d) *d = 0;
    strcat_s(p, MAX_PATH, ".ini"); strcpy_s(g_iniPath, MAX_PATH, p);
}
static void Log(const char* fmt, ...)
{
    if (!g_logPath[0]) return;
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") != 0 || !f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fprintf(f, "\n"); fclose(f);
}

// ---- GTA IV native invoker (self-contained, no ScriptHook) ----------
typedef void* (__stdcall* getNativeAddress_t)(uint32_t);
static getNativeAddress_t g_getNativeAddress = nullptr;
static uint32_t** g_pNatives = nullptr;     // -> native table (pairs: hash, handler)
static uint32_t* g_pNativeSize = nullptr;

struct NativeCtx
{
    void* pReturn;         // +00
    uint32_t nArgCount;       // +04
    void* pArgs;           // +08
    uint32_t nDataCount;      // +0C
    void* pOriginalData[4];// +10
    float    tempData[16];    // +20
    uint32_t stack[32];
    NativeCtx() {
        pReturn = stack; pArgs = stack; nArgCount = 0; nDataCount = 0;
        for (int i = 0; i < 4; i++) pOriginalData[i] = nullptr;
        for (int i = 0; i < 32; i++) stack[i] = 0;
        for (int i = 0; i < 16; i++) tempData[i] = 0.0f;
    }
    void pushI(int32_t v) { stack[nArgCount++] = (uint32_t)v; }
    void pushF(float v) { *(float*)&stack[nArgCount++] = v; }
    void pushP(void* v) { stack[nArgCount++] = (uint32_t)(uintptr_t)v; }
    int32_t resI() { return (int32_t)stack[0]; }
    float   resF() { return *(float*)&stack[0]; }
    // GTA IV vec3-output natives stash the caller's ptr in pOriginalData[] and
    // write the result into tempData[]; the copy-back happens here (like FF's GetResult).
    void flushVec() {
        while (nDataCount > 0) {
            nDataCount--;
            float* dst = (float*)pOriginalData[nDataCount];
            float* src = &tempData[nDataCount * 4];
            if (dst) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; }
        }
    }
};
typedef void (*NativeFn)(NativeCtx*);

// FusionFix's table walk -- deterministic, no reliance on getNativeAddress
static NativeFn NatFnTable(uint32_t hash)
{
    if (!g_pNatives || !g_pNativeSize) return nullptr;
    __try {
        uint32_t* nat = *g_pNatives;
        uint32_t  sz = *g_pNativeSize;
        if (!nat || !sz || !hash) return nullptr;
        uint32_t idx = hash % sz, tmp = hash;
        uint32_t h = nat[2 * idx];
        if (h != hash) {
            while (h) {
                tmp = (tmp >> 1) + 1;
                idx = (tmp + idx) % sz;
                h = nat[2 * idx];
                if (h == hash) break;
            }
        }
        if (h != hash) return nullptr;
        return (NativeFn)nat[2 * idx + 1];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static NativeFn NatFn(uint32_t hash)
{
    NativeFn f = NatFnTable(hash);
    if (f) return f;
    if (!g_getNativeAddress) return nullptr;
    __try { return (NativeFn)g_getNativeAddress(hash); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ---- module / pattern ------------------------------------------------
static bool GetMainModuleRange(uintptr_t& base, size_t& size)
{
    HMODULE h = GetModuleHandleA(nullptr);
    if (!h) return false;
    auto dos = (IMAGE_DOS_HEADER*)h;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (IMAGE_NT_HEADERS*)((uint8_t*)h + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    base = (uintptr_t)h; size = nt->OptionalHeader.SizeOfImage;
    return true;
}
static uintptr_t FindPattern(const uint8_t* pat, const char* mask)
{
    const size_t len = strlen(mask);
    if (!len || len > g_moduleSize) return 0;
    for (uintptr_t i = 0; i + len <= g_moduleSize; ++i)
    {
        const uint8_t* p = (const uint8_t*)(g_moduleBase + i);
        bool ok = true;
        for (size_t j = 0; j < len; ++j)
            if (mask[j] == 'x' && p[j] != pat[j]) { ok = false; break; }
        if (ok) return g_moduleBase + i;
    }
    return 0;
}
static int FindAll(const uint8_t* pat, const char* mask, uintptr_t* out, int maxHits)
{
    const size_t len = strlen(mask); int n = 0;
    if (!len || len > g_moduleSize) return 0;
    for (uintptr_t i = 0; i + len <= g_moduleSize; ++i)
    {
        const uint8_t* p = (const uint8_t*)(g_moduleBase + i);
        bool ok = true;
        for (size_t j = 0; j < len; ++j)
            if (mask[j] == 'x' && p[j] != pat[j]) { ok = false; break; }
        if (ok) { if (n < maxHits) out[n] = g_moduleBase + i; ++n; }
    }
    return n;
}
// .text (executable code) section range of the main module
static bool GetTextRange(uintptr_t& lo, uintptr_t& hi)
{
    auto dos = (IMAGE_DOS_HEADER*)g_moduleBase;
    auto nt = (IMAGE_NT_HEADERS*)(g_moduleBase + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)
        {
            lo = g_moduleBase + sec->VirtualAddress;
            hi = lo + sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}
static uintptr_t FindPatternText(const uint8_t* pat, const char* mask)
{
    uintptr_t out[1];
    uintptr_t lo, hi;
    if (!GetTextRange(lo, hi)) return FindPattern(pat, mask);
    const size_t len = strlen(mask);
    for (uintptr_t a = lo; a + len <= hi; ++a)
    {
        const uint8_t* p = (const uint8_t*)a;
        bool ok = true;
        for (size_t j = 0; j < len; ++j)
            if (mask[j] == 'x' && p[j] != pat[j]) { ok = false; break; }
        if (ok) { (void)out; return a; }
    }
    return 0;
}
static int FindAllInText(const uint8_t* pat, const char* mask, uintptr_t* out, int maxHits)
{
    uintptr_t lo, hi;
    if (!GetTextRange(lo, hi)) return FindAll(pat, mask, out, maxHits);
    const size_t len = strlen(mask); int n = 0;
    for (uintptr_t a = lo; a + len <= hi; ++a)
    {
        const uint8_t* p = (const uint8_t*)a;
        bool ok = true;
        for (size_t j = 0; j < len; ++j)
            if (mask[j] == 'x' && p[j] != pat[j]) { ok = false; break; }
        if (ok) { if (n < maxHits) out[n] = a; ++n; }
    }
    return n;
}

// ---- pools ------------------------------------------------------
struct Pool { uint8_t* storage; uint8_t* flags; int32_t size; int32_t stride; };
static bool ReadPool(uint8_t** pp, Pool& p, int minStride)
{
    if (!pp) return false;
    uint8_t* pool = *pp;
    if (!pool) return false;
    p.storage = *(uint8_t**)(pool + 0);
    p.flags = *(uint8_t**)(pool + 4);
    p.size = *(int32_t*)(pool + 8);
    p.stride = *(int32_t*)(pool + 0xC);
    return p.storage && p.flags && p.size > 0 && p.size <= 4096
        && p.stride >= minStride && p.stride <= 0x4000;
}

// ---- mouse look: hook GetRawInputData -----------------------------
typedef UINT(WINAPI* GetRawInputData_t)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static GetRawInputData_t g_origGRID = nullptr;

static UINT WINAPI HookGetRawInputData(HRAWINPUT hr, UINT cmd, LPVOID pData, PUINT pcb, UINT cbh)
{
    UINT r = g_origGRID(hr, cmd, pData, pcb, cbh);
    if (cmd == RID_INPUT && pData && r != (UINT)-1)
    {
        RAWINPUT* ri = (RAWINPUT*)pData;
        if (ri->header.dwType == RIM_TYPEMOUSE)
        {
            LONG dx = ri->data.mouse.lLastX, dy = ri->data.mouse.lLastY;
            if (dx || dy) {
                InterlockedExchangeAdd(&g_mouseDX, dx);
                InterlockedExchangeAdd(&g_mouseDY, dy);
                g_mouseOK = 1;
            }
        }
    }
    return r;
}

static void InstallMouseHook()
{
    HMODULE u = GetModuleHandleA("user32.dll");
    if (!u) return;
    void* target = (void*)GetProcAddress(u, "GetRawInputData");
    if (!target) { Log("GetRawInputData not found"); return; }
    {
        uint8_t* b = (uint8_t*)target;
        Log("GetRawInputData @ %p bytes %02X %02X %02X %02X %02X %02X %02X %02X",
            target, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        // expect hotpatch prologue 8B FF 55 8B EC (mov edi,edi; push ebp; mov ebp,esp)
        if (!(b[0] == 0x8B && b[1] == 0xFF)) { Log("  unexpected prologue - skipping mouse hook"); return; }
    }

    static uint8_t s_tramp[32];
    memcpy(s_tramp, target, 5);                       // save 5 stolen bytes
    s_tramp[5] = 0xE9;                                // jmp back
    *(int32_t*)(s_tramp + 6) = (int32_t)(((uintptr_t)target + 5) - ((uintptr_t)s_tramp + 10));
    DWORD op;
    VirtualProtect(s_tramp, sizeof(s_tramp), PAGE_EXECUTE_READWRITE, &op);
    g_origGRID = (GetRawInputData_t)(void*)s_tramp;

    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &op);
    *(uint8_t*)target = 0xE9;
    *(int32_t*)((uint8_t*)target + 1) = (int32_t)((uintptr_t)&HookGetRawInputData - ((uintptr_t)target + 5));
    VirtualProtect(target, 5, op, &op);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    Log("GetRawInputData hooked");
}

static void PollNatives();     // fwd decl -- called here so natives run on the main thread

static float WrapPi(float a)
{
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}

// The game's aim camera (CCamAimWeapon) only exists in the camera pool while
// aiming. Its frame is the game's real aim ray and the ped's heading follows it.
// Returns the frame matrix (right@0 fwd@4 up@8 pos@12, floats) or null.
static const float* FindAimCamFrame()
{
    Pool cp;
    if (!ReadPool(g_pCamPoolPtr, cp, 0x100)) return nullptr;
    const uintptr_t want = g_moduleBase + kAimCamVtblRva;
    for (int i = 0; i < cp.size; ++i)
    {
        if (cp.flags[i] & 0x80) continue;
        uint8_t* o = cp.storage + (size_t)i * cp.stride;
        if (*(uintptr_t*)o != want) continue;
        const float* m = (const float*)(o + 0x10);
        const float l = m[4] * m[4] + m[5] * m[5] + m[6] * m[6];
        if (l < 0.9f || l > 1.1f) return nullptr;
        return m;
    }
    return nullptr;
}

// The aim camera's pivot = head + a shoulder offset the game rotates into world space
// every tick from a LOCAL offset vector at +0x1C0 (x right, y fwd, z up; the game's
// value is x = 0.475, measured: world offset at +0x1B0 == right-vector * 0.475 at every
// heading). That pivot is what the bullet ray goes through. Rewriting the local x makes
// the aim ray start over the head (0) or the left shoulder (-0.475) instead of the
// right one, so the eye no longer has to slide sideways to line up. Sim thread, once per
// tick after the game's own update, so the next tick's camera update reads our value.
static void ApplyAimCamOffset()
{
    if (!g_enabled || !g_aimAlign || g_aimSide == 1) return;
    if (g_inCar || g_inTrain || g_isRagdoll || (g_cutsceneName && g_cutsceneName[0])) return;
    Pool cp;
    if (!ReadPool(g_pCamPoolPtr, cp, 0x100) || cp.stride < 0x1D0) return;
    const uintptr_t want = g_moduleBase + kAimCamVtblRva;
    __try {
        for (int i = 0; i < cp.size; ++i)
        {
            if (cp.flags[i] & 0x80) continue;
            uint8_t* o = cp.storage + (size_t)i * cp.stride;
            if (*(uintptr_t*)o != want) continue;
            float* ofs = (float*)(o + 0x1C0);
            ofs[0] = (g_aimSide == 2) ? -kAimShoulder : 0.0f;
            if (g_aimVert) ofs[2] = g_eyeTrimV[0];
            break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Extra eye-forward while aiming for the current weapon. A long gun's stock ends up
// beside/behind the eye and pokes into view (clips at the screen edge); pushing the eye
// forward puts the stock behind the camera plane. Per-weapon value first, then the value
// tuned for its slot (category), else 0. Moving along the view direction keeps the eye on
// the aim ray, so accuracy is unaffected.
static float EffectiveAimFwd()
{
    const int w = g_curWeapon, sl = g_curWeaponSlot;
    if (w >= 0 && w < 128 && g_aimFwdW[w] != 0.f) return g_aimFwdW[w];
    if (sl >= 0 && sl < 16) return g_aimFwdSlot[sl];
    return 0.f;
}

static void AdjustAimFwd(float d)
{
    const int w = g_curWeapon, sl = g_curWeaponSlot;
    if (w < 0 || w >= 128) return;
    float v = EffectiveAimFwd() + d;
    if (v > 0.60f) v = 0.60f;
    if (v < -0.40f) v = -0.40f;
    if (fabsf(v) < 0.001f) v = 0.f;
    g_aimFwdW[w] = v;
    if (sl >= 0 && sl < 16) g_aimFwdSlot[sl] = v;
    Log("aimFwd weapon=%d slot=%d = %.2f", w, sl, v);
}

// ---- hook body -------------------------------------------------
static uint32_t g_lastPollFrame = 0;
static void __cdecl OnFinalCam(float* dst)     // dst = final cam matrix, fully populated
{
    ++g_hits;
    __try
    {
        bool inCs = g_cutsceneName && g_cutsceneName[0] != '\0';
        g_shotX = dst[12]; g_shotY = dst[13]; g_shotZ = dst[14];   // pre-override cam pos
        for (int i = 0; i < 3; ++i) { g_dstRot[i] = dst[i]; g_dstRot[3 + i] = dst[4 + i]; g_dstRot[6 + i] = dst[8 + i]; }

        if (!g_enabled) return;

        // ---- head-anchored FP (gameplay, vehicles, subway AND cutscenes, via natives) ----
        // pick the anchor: cutscene actor if in a cutscene, else the real head bone
        float hx, hy, hz;
        if (inCs && g_csPedOK)
        {
            hx = g_csPedW[0]; hy = g_csPedW[1]; hz = g_csPedW[2] + g_eyeUp;   // actor pos + eye
        }
        else if (!inCs && (g_inCar || g_inTrain) && g_FindPlayerPed && g_headMtxOK)
        {
            // in a vehicle: the cached head-bone pos is a frame stale -> at speed
            // the camera visibly lags the car. Read the ped matrix fresh this
            // frame; the seated head sits ~0.55 above the ped matrix origin.
            void* ped = g_FindPlayerPed(0);
            float* m = ped ? *(float**)((char*)ped + 0x20) : nullptr;
            if (m)
            {
                hx = m[12]; hy = m[13]; hz = m[14] + 0.55f;
                // Exiting a train: IS_CHAR_IN_ANY_TRAIN stays true for a second or
                // two into the stand-up-and-step-out animation, so this fixed
                // seated offset keeps getting applied while Niko's real skeleton
                // is already rising toward standing height -- pins the camera at
                // waist level while he's visibly stood up (2026-09-15, subway exit
                // F8 dump: real head bone climbed 0.4+ units above this formula's
                // output over several samples while inTrain was still 1, then
                // matched up again once it cleared). While genuinely seated the
                // real bone reads BELOW this formula every time (tuned to sit a
                // touch higher on purpose) -- so preferring whichever is higher
                // only ever kicks in once he's actually standing.
                //
                // HEIGHT ONLY, not X/Y: g_headMtx is a frame (or more) stale --
                // it's only refreshed once per SIM tick, same staleness this whole
                // branch exists to dodge for the ped root (see the fresh-read
                // comment above). Taking its X/Y too made the camera visibly lag
                // and warp sideways off Niko's real position on a fast-moving
                // train (2026-09-15 follow-up test) -- X/Y must stay on the
                // fresh-this-frame root read; only Z borrows the cached bone.
                if (g_headMtx[14] > hz) hz = g_headMtx[14];
            }
            else { hx = g_headMtx[12]; hy = g_headMtx[13]; hz = g_headMtx[14]; }
        }
        else if (!inCs && g_headMtxOK)
        {
            // Same staleness fix as the vehicle branch above: g_headMtx is only
            // refreshed once per SIM tick (the frame-hook poll), but this runs
            // once per RENDERED frame. Under a heavier/uneven renderer (RTX Remix
            // adds real GPU latency and can decouple render pacing from the sim
            // tick) that gap becomes visible -- the camera trails a frame or more
            // behind where the head bone actually is right now. GetBoneMatrix is a
            // plain function pointer, not a script native, so it's exactly as
            // legal to call from this render-thread hook as FindPlayerPed already
            // is elsewhere in this function -- read it fresh here instead of
            // trusting the cached copy.
            bool freshOK = false;
            if (g_GetBoneMtx && g_FindPlayerPed)
            {
                void* pd = g_FindPlayerPed(0);
                if (pd)
                {
                    float m[16];
                    g_GetBoneMtx(pd, m, 1205);
                    if (m[12] || m[13] || m[14]) { hx = m[12]; hy = m[13]; hz = m[14]; freshOK = true; }
                }
            }
            if (!freshOK) { hx = g_headMtx[12]; hy = g_headMtx[13]; hz = g_headMtx[14]; }
        }
        else if (g_natOK)
        {
            hx = g_headW[0]; hy = g_headW[1]; hz = g_headW[2];                // head bone (already at head)
        }
        else if (g_FindPlayerPed)
        {
            void* ped = g_FindPlayerPed(0);
            if (!ped) return;
            float* m = *(float**)((char*)ped + 0x20);
            if (!m) return;
            hx = m[12]; hy = m[13]; hz = m[14] + g_eyeUp;
        }
        else return;
        const int vi = g_inTrain ? 2 : (g_inCar ? 1 : 0);                    // 0 foot, 1 car, 2 train
        hz += g_eyeTrimV[vi];                                                 // F11/F12 fine nudge

        // Aim alignment. The game aims from its own aim camera (CCamAimWeapon), not
        // from the final matrix we overwrite, and that camera's yaw/pitch drift away
        // from our own mouse integration (measured: 0-10 deg yaw, -8..+3 deg pitch
        // apart, plus a ~0.4 m shoulder offset) -- so shots miss the crosshair.
        // While the aim camera is live, take its yaw/pitch, converging from our old
        // view over ~80 ms so entering aim doesn't snap.
        static float s_offYaw = 0.f, s_offPitch = 0.f, s_eyeW = 0.f, s_shift[3] = { 0.f, 0.f, 0.f };
        static bool  s_aimWas = false;
        static LONGLONG s_aimQpc = 0; static double s_qpcInv = 0.0;
        const float* aimF = nullptr;
        if (g_aimAlign && g_seedYawSet && !inCs && !g_inCar && !g_inTrain && !g_isRagdoll && !g_boneRot)
            aimF = FindAimCamFrame();
        {
            if (s_qpcInv == 0.0) { LARGE_INTEGER qf; QueryPerformanceFrequency(&qf); s_qpcInv = 1.0 / (double)qf.QuadPart; }
            LARGE_INTEGER qn; QueryPerformanceCounter(&qn);
            float dt = s_aimQpc ? (float)((qn.QuadPart - s_aimQpc) * s_qpcInv) : 0.f;
            s_aimQpc = qn.QuadPart;
            if (dt < 0.f || dt > 0.25f) dt = 0.f;
            const float k = expf(-dt / 0.08f);      // dt==0 on repeat calls within a frame -> no-op
            if (aimF)
            {
                const float aimYaw = atan2f(-aimF[4], aimF[5]);
                const float aimPitch = asinf(fmaxf(-1.f, fminf(1.f, aimF[6])));
                if (!s_aimWas)
                {
                    s_offYaw = WrapPi(g_seedYaw + g_lookYaw - aimYaw);
                    s_offPitch = g_lookPitch - aimPitch;
                    s_aimWas = true;
                }
                s_offYaw *= k; s_offPitch *= k;
                g_lookYaw = WrapPi(aimYaw + s_offYaw - g_seedYaw);
                g_lookPitch = fmaxf(-1.40f, fminf(1.40f, aimPitch + s_offPitch));   // game's aim pitch limit is ~1.396
                s_eyeW += (1.f - s_eyeW) * (1.f - k);
            }
            else { s_aimWas = false; s_eyeW *= k; }
        }

        g_aimActive = aimF ? 1 : 0;

        // consume mouse ONCE per rendered frame (this hook fires several times/frame).
        // When GTA IV's idle camera takes over it rolls the shot cam and drives the
        // mouse-delta globals to animate its drift -- detect that (incoming up.z
        // gone negative) and freeze look input so the view doesn't get dragged /
        // flipped. Our own pitch is hard-clamped to +-1.30 rad (74.5 deg), so our
        // own math can never produce a negative up.z -- 0.60 was too strict and
        // falsely tripped on ordinary steep look-down (cos(74.5 deg) =~ 0.27).
        bool camUpright = g_dstRot[8] > -0.20f;
        uint32_t fc = g_frameCount ? *g_frameCount : (g_hits >> 3);
        if (aimF)
        {
            // the aim camera owns yaw/pitch right now -- drain so nothing bursts when it ends
            g_lastLookFrame = fc;
            InterlockedExchange(&g_mouseDX, 0);
            InterlockedExchange(&g_mouseDY, 0);
        }
        else if (camUpright && fc != g_lastLookFrame)
        {
            g_lastLookFrame = fc;
            float mdx = 0, mdy = 0;
            if (g_diMouseX) mdx += (float)*g_diMouseX;
            if (g_diMouseY) mdy += (float)*g_diMouseY;
            mdx += (float)InterlockedExchange(&g_mouseDX, 0);
            mdy += (float)InterlockedExchange(&g_mouseDY, 0);
            if (mdx > -1.5f && mdx < 1.5f) mdx = 0.0f;   // reject sub-pixel jitter
            if (mdy > -1.5f && mdy < 1.5f) mdy = 0.0f;
            if (mdx > 200.0f) mdx = 200.0f; if (mdx < -200.0f) mdx = -200.0f;
            if (mdy > 200.0f) mdy = 200.0f; if (mdy < -200.0f) mdy = -200.0f;
            g_lookYaw -= mdx * g_mouseSens;
            g_lookPitch -= mdy * g_mouseSens * (g_invertY ? -1.0f : 1.0f);
            if (g_lookPitch > g_maxPitch) g_lookPitch = g_maxPitch;
            if (g_lookPitch < -g_maxPitch) g_lookPitch = -g_maxPitch;
        }
        else if (!camUpright)
        {
            // idle cam: drain the mouse deltas so they don't burst when it ends
            if (g_diMouseX) { volatile int32_t t = *g_diMouseX; (void)t; }
            InterlockedExchange(&g_mouseDX, 0);
            InterlockedExchange(&g_mouseDY, 0);
        }

        dst[12] = hx;
        dst[13] = hy;
        dst[14] = hz;
        g_pickX = hx; g_pickY = hy; g_pickZ = hz;

        // base orientation:
        //  - g_boneRot (manual) or g_isRagdoll (auto): the head bone's own matrix
        //    (true head tracking) -- so getting knocked down / ragdolling actually
        //    tumbles the view instead of it staying locked to one direction.
        //    g_fwdRow picks which bone axis is "look", g_fwdSign flips it.
        //  - else g_headingOK: Niko's heading (locks yaw to his facing)
        //  - else: the incoming shot/gameplay camera direction
        float R[3], F[3];
        if ((g_boneRot || g_isRagdoll) && g_headMtxOK && !inCs)
        {
            int fr = g_fwdRow & 3; if (fr > 2) fr = 0;
            int rr = (fr + 1) % 3;
            F[0] = g_headMtx[fr * 4 + 0]; F[1] = g_headMtx[fr * 4 + 1]; F[2] = g_headMtx[fr * 4 + 2];
            // the bone matrix's "rr" row is the LEFT vector, not right (its up row
            // matches R x F only once negated -- confirmed against the bone's own
            // up row: without this, R x F pointed down and the view was upside down)
            R[0] = -g_headMtx[rr * 4 + 0]; R[1] = -g_headMtx[rr * 4 + 1]; R[2] = -g_headMtx[rr * 4 + 2];
        }
        else if (inCs)
        {
            // cutscene: FREE LOOK. an absolute world yaw seeded once from the first
            // shot's direction, then moved only by the mouse -- so the view PERSISTS
            // through the director's shot cuts instead of snapping to each new angle.
            if (!g_seedYawSet)
            {
                g_seedYaw = atan2f(-g_dstRot[3], g_dstRot[4]);
                g_seedYawSet = 1;
            }
            float y = g_seedYaw;
            F[0] = -sinf(y); F[1] = cosf(y); F[2] = 0.f;
            R[0] = cosf(y);  R[1] = sinf(y); R[2] = 0.f;
        }
        else if (g_useNatives && g_headingOK && (g_inCar || g_inTrain))
        {
            // in a vehicle / train: the view follows the vehicle's heading each
            // frame (auto-centers through turns); the mouse is a free offset on top.
            float y = g_charHeading * 0.01745329f;   // deg -> rad
            F[0] = -sinf(y); F[1] = cosf(y); F[2] = 0.f;
            R[0] = cosf(y);  R[1] = sinf(y); R[2] = 0.f;
        }
        else
        {
            // on foot: an ABSOLUTE world yaw seeded once, then moved only by the
            // mouse. Walking / strafing (A,D) turns the character + chase-cam but
            // must NOT drag the view.
            if (!g_seedYawSet)
            {
                if (g_useNatives && g_headingOK) g_seedYaw = g_charHeading * 0.01745329f;
                else g_seedYaw = atan2f(-g_dstRot[3], g_dstRot[4]);
                g_seedYawSet = 1;
            }
            float y = g_seedYaw;
            F[0] = -sinf(y); F[1] = cosf(y); F[2] = 0.f;
            R[0] = cosf(y);  R[1] = sinf(y); R[2] = 0.f;
        }
        if (g_fwdSign) { R[0] = -R[0]; R[1] = -R[1]; R[2] = -R[2]; F[0] = -F[0]; F[1] = -F[1]; F[2] = -F[2]; }

        float cy = cosf(g_lookYaw), sy = sinf(g_lookYaw);
        { float x = R[0] * cy - R[1] * sy, y = R[0] * sy + R[1] * cy; R[0] = x; R[1] = y; }
        { float x = F[0] * cy - F[1] * sy, y = F[0] * sy + F[1] * cy; F[0] = x; F[1] = y; }

        if (g_lookPitch != 0.0f)
        {
            float cp = cosf(g_lookPitch), sp = sinf(g_lookPitch);
            float ux = R[1] * F[2] - R[2] * F[1], uy = R[2] * F[0] - R[0] * F[2], uz = R[0] * F[1] - R[1] * F[0];
            F[0] = F[0] * cp + ux * sp; F[1] = F[1] * cp + uy * sp; F[2] = F[2] * cp + uz * sp;
        }

        float U[3] = { R[1] * F[2] - R[2] * F[1], R[2] * F[0] - R[0] * F[2], R[0] * F[1] - R[1] * F[0] };
        float ul = sqrtf(U[0] * U[0] + U[1] * U[1] + U[2] * U[2]); if (ul > 1e-4f) { U[0] /= ul; U[1] /= ul; U[2] /= ul; }
        R[0] = F[1] * U[2] - F[2] * U[1]; R[1] = F[2] * U[0] - F[0] * U[2]; R[2] = F[0] * U[1] - F[1] * U[0];
        float rl = sqrtf(R[0] * R[0] + R[1] * R[1] + R[2] * R[2]); if (rl > 1e-4f) { R[0] /= rl; R[1] /= rl; R[2] /= rl; }

        dst[0] = R[0]; dst[1] = R[1]; dst[2] = R[2];
        dst[4] = F[0]; dst[5] = F[1]; dst[6] = F[2];
        dst[8] = U[0]; dst[9] = U[1]; dst[10] = U[2];

        // nudge toward eyes/mouth: forward a touch along the view direction
        dst[12] = hx + F[0] * g_eyeFwdV[vi];
        dst[13] = hy + F[1] * g_eyeFwdV[vi];
        dst[14] = hz + F[2] * g_eyeFwdV[vi];
        g_pickX = dst[12]; g_pickY = dst[13]; g_pickZ = dst[14];

        // Parallax: slide the eye sideways/vertically onto the aim ray (the component
        // of the aim camera's offset perpendicular to the view), so the crosshair ray
        // and the game's aim ray are the same line. Fades in/out with the aim camera.
        if (aimF && g_aimParallax)
        {
            const float dx = aimF[12] - dst[12], dy = aimF[13] - dst[13], dz = aimF[14] - dst[14];
            const float along = dx * F[0] + dy * F[1] + dz * F[2];
            const float sx = dx - along * F[0], sy = dy - along * F[1], sz = dz - along * F[2];
            if (sx * sx + sy * sy + sz * sz < 4.0f) { s_shift[0] = sx; s_shift[1] = sy; s_shift[2] = sz; }
        }
        if (g_aimParallax && s_eyeW > 0.001f)
        {
            dst[12] += s_shift[0] * s_eyeW; dst[13] += s_shift[1] * s_eyeW; dst[14] += s_shift[2] * s_eyeW;
            g_pickX = dst[12]; g_pickY = dst[13]; g_pickZ = dst[14];
        }

        // Weapon-aware eye-forward while aiming (fades with the aim camera).
        if (s_eyeW > 0.001f)
        {
            const float af = EffectiveAimFwd() * s_eyeW;
            if (af != 0.f)
            {
                dst[12] += F[0] * af; dst[13] += F[1] * af; dst[14] += F[2] * af;
                g_pickX = dst[12]; g_pickY = dst[13]; g_pickZ = dst[14];
            }
        }

        // FOV: dst[20] (matrix+0x50) is the field of view, already copied in by
        // the stolen bytes. widen it a touch.
        if (g_fovBoostV[vi] != 0.0f)
        {
            float f = dst[20] + g_fovBoostV[vi];
            if (f > 5.0f && f < 140.0f) dst[20] = f;
        }
        ++g_applied;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) void RotStub()
{
    __asm
    {
        mov    eax, [edi + 0x50]              // stolen 1: mov eax,[edi+50]
        mov[esi + 0x50], eax              // stolen 2: mov [esi+50],eax
        pushad
        sub    esp, 0x20
        movups[esp + 0x00], xmm0
        movups[esp + 0x10], xmm1
        push   esi
        call   OnFinalCam
        add    esp, 4
        movups xmm0, [esp + 0x00]
        movups xmm1, [esp + 0x10]
        add    esp, 0x20
        popad
        jmp    dword ptr[g_hookReturn]       // -> 0x83E39E
    }
}

// ---- per-frame game-thread hook (natives are only safe on the sim thread) ----
// We hook the same CALL that FusionFix uses for onGameProcessEvent: the first E8
// in CGame's per-frame process chain. That runs on the simulation thread, once
// per frame, which is where GTA IV natives are legal to call.
static volatile int  g_frameHookHits = 0;
static DWORD         g_gameThreadId = 0;
typedef void(__cdecl* voidfn_t)();
static voidfn_t      g_origGameProcess = nullptr;

// ---- hide the player's head via SET_DRAW_PLAYER_COMPONENT (proper native) -----
// components: 0 HEAD, 7 HAIR, 9 TEEF, 10 FACE -- hair always goes with the head,
// nobody wants it floating in mid-air once the head's gone.
static volatile int   g_hideHead = 1;   // auto-hides whenever first person is on

static void OnGameFrame()
{
    ++g_frameHookHits;
    g_gameThreadId = GetCurrentThreadId();
    if (g_useNatives) PollNatives();
    ApplyAimCamOffset();
}

__declspec(naked) void FrameStub()
{
    __asm
    {
        call  dword ptr[g_origGameProcess]    // run the real CGame::Process first
        pushad
        sub   esp, 0x80
        movups[esp + 0x00], xmm0
        movups[esp + 0x10], xmm1
        movups[esp + 0x20], xmm2
        movups[esp + 0x30], xmm3
        movups[esp + 0x40], xmm4
        movups[esp + 0x50], xmm5
        movups[esp + 0x60], xmm6
        movups[esp + 0x70], xmm7
        call  OnGameFrame
        movups xmm0, [esp + 0x00]
        movups xmm1, [esp + 0x10]
        movups xmm2, [esp + 0x20]
        movups xmm3, [esp + 0x30]
        movups xmm4, [esp + 0x40]
        movups xmm5, [esp + 0x50]
        movups xmm6, [esp + 0x60]
        movups xmm7, [esp + 0x70]
        add   esp, 0x80
        popad
        ret
    }
}

static bool AddrIsExecutable(uintptr_t a)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD p = mbi.Protect & 0xFF;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
        p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

static bool InstallFrameHook()
{
    // FusionFix pattern for the CGame process-chain call site:
    //   call;call;call; mov ecx,imm; call;call;call;call; mov ecx,imm
    // FindPattern scans the whole image, so several byte-coincidences match.
    // We validate: calls #2..#7 must target inside the main module, and call #1's
    // target must at least be committed executable memory (FusionFix may already
    // have redirected #1 to its own handler in another module).
    static const uint8_t pat[] = {
        0xE8,0,0,0,0, 0xE8,0,0,0,0, 0xE8,0,0,0,0, 0xB9,0,0,0,0,
        0xE8,0,0,0,0, 0xE8,0,0,0,0, 0xE8,0,0,0,0, 0xE8,0,0,0,0, 0xB9
    };
    const char* msk = "x????x????x????x????x????x????x????x????x";
    static const int callOff[7] = { 0, 5, 10, 20, 25, 30, 35 };

    uintptr_t hits[64];
    int n = FindAllInText(pat, msk, hits, 64);
    Log("game-process pattern: %d raw match(es) in .text", n);

    uintptr_t modLo = g_moduleBase, modHi = g_moduleBase + g_moduleSize;
    uintptr_t chosen = 0; uintptr_t chosenOrig = 0;
    for (int i = 0; i < n && i < 64; ++i)
    {
        uintptr_t m = hits[i];
        bool good = true;
        uintptr_t t0 = 0;
        for (int c = 0; c < 7; ++c)
        {
            int off = callOff[c];
            int32_t rel = *(int32_t*)(m + off + 1);
            uintptr_t tgt = m + off + 5 + rel;
            if (c == 0) { t0 = tgt; continue; }          // #1 checked separately
            if (tgt < modLo || tgt >= modHi) { good = false; break; }
        }
        if (!good) continue;
        if (!(t0 >= modLo && t0 < modHi) && !AddrIsExecutable(t0)) continue;
        Log("  candidate site=0x%p  call#1 -> 0x%p", (void*)m, (void*)t0);
        if (!chosen) { chosen = m; chosenOrig = t0; }
    }

    if (!chosen) { Log("no valid game-process call site - native polling disabled"); return false; }

    uint8_t* call = (uint8_t*)chosen;
    g_origGameProcess = (voidfn_t)chosenOrig;
    int32_t newRel = (int32_t)((uintptr_t)&FrameStub - (chosen + 5));
    DWORD op = 0;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &op)) return false;
    *(int32_t*)(call + 1) = newRel;
    VirtualProtect(call, 5, op, &op);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    Log("game-process hook: site=0x%p orig=0x%p", (void*)chosen, (void*)g_origGameProcess);
    return true;
}

static bool InstallHook()
{
    uintptr_t site = g_moduleBase + 0x83E398;
    const uint8_t want[6] = { 0x8B,0x47,0x50, 0x89,0x46,0x50 };
    if (memcmp((void*)site, want, 6) != 0)
    {
        uint8_t* p = (uint8_t*)site;
        Log("83E398 mismatch: %02X %02X %02X %02X %02X %02X",
            p[0], p[1], p[2], p[3], p[4], p[5]);
        return false;
    }
    g_hookReturn = site + 6;
    int32_t rel = (int32_t)((uintptr_t)&RotStub - (site + 5));
    DWORD op = 0;
    if (!VirtualProtect((void*)site, 6, PAGE_EXECUTE_READWRITE, &op)) return false;
    uint8_t* p = (uint8_t*)site;
    p[0] = 0xE9; memcpy(p + 1, &rel, 4); p[5] = 0x90;   // jmp rel32 + nop
    VirtualProtect((void*)site, 6, op, &op);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 6);
    Log("hook installed at RVA 0x83E398");
    return true;
}

static void ResolveStuff()
{
    {
        static const uint8_t pat[] = {
            0x8B,0x0D,0,0,0,0, 0xE8,0,0,0,0, 0x85,0xC0, 0x74,0, 0x80,0x7C,0x24,0,0, 0x75
        };
        uintptr_t m = FindPattern(pat, "xx????x????xxx?xxx??x");
        if (m) g_pCamPoolPtr = (uint8_t**)(*(uint32_t*)(m + 2));
        Log("camPool = 0x%p", (void*)g_pCamPoolPtr);
    }
    {
        static const uint8_t pat[] = { 0x8B,0x3D,0,0,0,0, 0x8B,0xF1, 0x8B,0x47 };
        uintptr_t m = FindPattern(pat, "xx????xxxx");
        if (m) g_pPedPoolPtr = (uint8_t**)(*(uint32_t*)(m + 2));
        Log("pedPool = 0x%p", (void*)g_pPedPoolPtr);
    }
    {
        static const uint8_t pat[] = {
            0x8B,0x44,0x24,0x04, 0x85,0xC0, 0x75,0x00, 0xA1,0,0,0,0, 0x83,0xF8,0x00, 0x74
        };
        uintptr_t m = FindPattern(pat, "xxxxxxx?x????xx?x");
        g_FindPlayerPed = (FindPlayerPed_t)m;
        Log("FindPlayerPed = 0x%p", (void*)m);
    }
    {
        // FusionFix: mouse axis deltas  74 3D C7 05 <X:4> <imm:4> C7 05 <Y:4> <imm:4>
        static const uint8_t pat[] = {
            0x74,0x3D, 0xC7,0x05, 0,0,0,0, 0,0,0,0, 0xC7,0x05
        };
        uintptr_t m = FindPattern(pat, "xxxx????????xx");
        if (m)
        {
            g_diMouseX = *(int32_t**)(m + 4);
            g_diMouseY = *(int32_t**)(m + 14);
        }
        Log("diMouseX = 0x%p  diMouseY = 0x%p", (void*)g_diMouseX, (void*)g_diMouseY);
    }
    {
        // FusionFix: CTimer::m_frameCount   FF 05 <addr> F3 0F 2C C0 F3 0F 10 05
        static const uint8_t pat[] = {
            0xFF,0x05, 0,0,0,0, 0xF3,0x0F,0x2C,0xC0, 0xF3,0x0F,0x10,0x05
        };
        uintptr_t m = FindPattern(pat, "xx????xxxxxxxx");
        if (m) { g_frameCount = *(uint32_t**)(m + 2); g_frameInsn = m; }
        Log("frameCount = 0x%p  insn = 0x%p", (void*)g_frameCount, (void*)g_frameInsn);
    }
    {
        // FusionFix: getNativeAddress -- pattern "56 8B 35 ? ? ? ? 85 F6 75 06", 2nd .text match IS the function
        static const uint8_t pat[] = { 0x56, 0x8B,0x35, 0,0,0,0, 0x85,0xF6, 0x75,0x06 };
        uintptr_t hits[8];
        int n = FindAllInText(pat, "xxx????xxxx", hits, 8);
        Log("getNativeAddress pattern: %d .text match(es)  [0]=0x%p [1]=0x%p",
            n, (void*)(n > 0 ? hits[0] : 0), (void*)(n > 1 ? hits[1] : 0));
        if (n >= 2)      g_getNativeAddress = (getNativeAddress_t)hits[1];
        else if (n == 1) g_getNativeAddress = (getNativeAddress_t)hits[0];
        Log("getNativeAddress = 0x%p", (void*)g_getNativeAddress);

        // getNativeAddress body:  56  8B 35 <sizeVar>  85 F6 75 06 ... 8B 1D <tableVar>
        // pull the exact globals it uses so our table walk hits the SAME (live) table.
        if (g_getNativeAddress)
        {
            uint8_t* g = (uint8_t*)g_getNativeAddress;
            if (g[0] == 0x56 && g[1] == 0x8B && g[2] == 0x35)
                g_pNativeSize = *(uint32_t**)(g + 3);
            for (int off = 8; off < 0x40; ++off)
                if (g[off] == 0x8B && g[off + 1] == 0x1D) { g_pNatives = *(uint32_t***)(g + off + 2); break; }
            Log("native globals from getNA: size=0x%p natives=0x%p", (void*)g_pNativeSize, (void*)g_pNatives);
        }
    }
    if (!g_pNatives || !g_pNativeSize)
    {
        // FusionFix fallback patterns (may point at a secondary/dead table)
        static const uint8_t pa[] = { 0x8B,0x35, 0,0,0,0, 0x85,0xF6, 0x75,0x06, 0x33,0xC0, 0x5E, 0xC2,0x04,0x00, 0x53,0x57, 0x8B,0x7C,0x24,0x10 };
        uintptr_t m = FindPatternText(pa, "xx????xxxxxxxxxxxxxxxx");
        if (m && !g_pNativeSize) g_pNativeSize = *(uint32_t**)(m + 2);
        static const uint8_t pb[] = { 0x8B,0x1D, 0,0,0,0, 0x8B,0xCF, 0x8B,0x04,0xD3, 0x3B,0xC7, 0x74,0x19, 0x8D,0x64,0x24,0x00, 0x85,0xC0 };
        uintptr_t m2 = FindPatternText(pb, "xx????xxxxxxxxxxxxxxx");
        if (m2 && !g_pNatives) g_pNatives = *(uint32_t***)(m2 + 2);
    }
    {
        Log("ms_pNatives ptr=0x%p (*=0x%p)  ms_dwNativeTableSize ptr=0x%p (*=%u)",
            (void*)g_pNatives, (void*)(g_pNatives ? *g_pNatives : 0),
            (void*)g_pNativeSize, (unsigned)(g_pNativeSize ? *g_pNativeSize : 0));
        // dump a few table slots so we can eyeball hash/handler pairs
        __try {
            if (g_pNatives && *g_pNatives && g_pNativeSize && *g_pNativeSize) {
                uint32_t* nt = *g_pNatives; uint32_t sz = *g_pNativeSize;
                for (uint32_t k = 0; k < 4 && k < sz; ++k)
                    Log("  nat[%u] hash=0x%08X handler=0x%08X", k, nt[2 * k], nt[2 * k + 1]);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("  native table dump faulted"); }
    }
    {
        static const uint8_t pat[] = {
            0x68,0,0,0,0, 0xE8,0,0,0,0, 0x83,0xC4,0x00, 0x85,0xC0, 0x0F,0x85,0,0,0,0, 0xF3,0x0F,0x10,0x3D
        };
        uintptr_t m = FindPattern(pat, "x????x????xx?xxxx????xxxx");
        if (m) g_cutsceneName = *(const char**)(m + 1);
        Log("cutsceneName = 0x%p", (void*)g_cutsceneName);
    }
    {
        // FusionFix: CTimer::m_UserPause / m_CodePause   0A 05 <user> 0A 05 <code> 75 38
        static const uint8_t pat[] = {
            0x0A,0x05, 0,0,0,0, 0x0A,0x05, 0,0,0,0, 0x75,0x38
        };
        uintptr_t m = FindPattern(pat, "xx????xx????xx");
        if (m) { g_userPause = *(uint8_t**)(m + 2); g_codePause = *(uint8_t**)(m + 8); }
        Log("userPause = 0x%p  codePause = 0x%p", (void*)g_userPause, (void*)g_codePause);
    }
    {
        // CPed::GetBoneMatrix(this, rage::Matrix* out, int boneTag) -- __thiscall, ret 8.
        // Called internally by GET_PED_BONE_POSITION. Gives the full bone matrix
        // (right@0 up@0x10 fwd@0x20 pos@0x30), not just position.
        static const uint8_t pat[] = {
            0x57, 0x8B,0xF9, 0x8B,0x07, 0xFF,0x90,0xA0,0,0,0, 0x85,0xC0, 0x74
        };
        uintptr_t m = FindPatternText(pat, "xxxxxxxx???xxx");
        g_GetBoneMtx = (GetBoneMtx_t)m;
        Log("GetBoneMatrix = 0x%p", (void*)m);
    }
}

// ---- poll game natives (worker thread) -----------------------------
static volatile int g_natDbgPed = -1, g_natDbgBoneCalls = 0;
static volatile int g_natTimer = -1;

static void PollNativesInner()
{
    static NativeFn fTimer = NatFn(0x022B2DA9);   // GET_GAME_TIMER() -> ms
    static NativeFn fGetPlayerChar = NatFn(0x511454A9);   // GET_PLAYER_CHAR(player,&ped)
    static NativeFn fBonePos = NatFn(0x43475BB3);   // GET_PED_BONE_POSITION(ped,tag,ox,oy,oz,&v)
    static NativeFn fInTrain = NatFn(0x22434C20);   // IS_CHAR_IN_ANY_TRAIN(ped)
    static NativeFn fInCar = NatFn(0x71184DA3);   // IS_CHAR_IN_ANY_CAR(ped)
    static NativeFn fRagdoll = NatFn(0x3E251ADE);   // IS_PED_RAGDOLL(ped)
    static NativeFn fDrawComp = NatFn(0x3EFE3DC8);   // SET_DRAW_PLAYER_COMPONENT(comp,draw)
    static NativeFn fCsPed = NatFn(0x366B549F);   // GET_CUTSCENE_PED_POSITION(idx,&v)
    static NativeFn fHeading = NatFn(0x057A3AC7);   // GET_CHAR_HEADING(ped) -> float
    static NativeFn fCharModel = NatFn(0x0A3D60CE);   // GET_CHAR_MODEL(ped,&model)
    static NativeFn fPedModelFromIdx = NatFn(0x124D4571);   // GET_PED_MODEL_FROM_INDEX(idx,&model) -- guess: cutscene slot idx, same space as GET_CUTSCENE_PED_POSITION

    static NativeFn fPrintNow = NatFn(0x0CA539D6);    // PRINT_STRING_WITH_LITERAL_STRING_NOW(gxt,text,ms,flag)
    static NativeFn fCurWeapon = NatFn(0x5AB8289F);   // GET_CURRENT_CHAR_WEAPON(ped,&weapon)
    static NativeFn fWeapSlot = NatFn(0x5E4F6DE3);    // GET_WEAPONTYPE_SLOT(weapon,&slot)

    if (!g_natLogged)
    {
        g_natLogged = 1;
        Log("nat resolve (table then getNA): timer=%p tableTimer=%p char=%p bone=%p cs=%p heading=%p",
            fTimer, NatFnTable(0x022B2DA9), fGetPlayerChar, fBonePos, fCsPed, fHeading);
    }

    // GTA IV ABI: value-returning natives take a trailing OUTPUT-POINTER arg
    // (void return in FusionFix's wrappers). vec3 outputs go via tempData[] and
    // need flushVec() to copy back into the caller's buffer.
    if (fTimer) { uint32_t t = 0; NativeCtx a; a.pushP(&t); fTimer(&a); g_natTimer = (int)t; }

    int ped = 0;
    if (fGetPlayerChar) { NativeCtx a; a.pushI(0); a.pushP(&ped); fGetPlayerChar(&a); }
    g_playerPed = ped;
    g_natDbgPed = ped;

    // Niko's own model hash, cached once -- the discriminator for "which cutscene
    // slot is actually him", independent of spawn-order timing.
    if (!g_nikoModel && fCharModel && ped)
    {
        uint32_t m = 0; NativeCtx a; a.pushI(ped); a.pushP(&m); fCharModel(&a);
        if (m) { g_nikoModel = m; Log("nikoModel = 0x%08X", m); }
    }

    if (fBonePos && ped)
    {
        float v[3] = { 0,0,0 };
        NativeCtx a; a.pushI(ped); a.pushI(1205);
        a.pushF(0); a.pushF(0); a.pushF(0); a.pushP(v);
        fBonePos(&a);
        a.flushVec();
        ++g_natDbgBoneCalls;
        if (v[0] || v[1] || v[2]) { g_headW[0] = v[0]; g_headW[1] = v[1]; g_headW[2] = v[2]; g_natOK = 1; }
    }

    // full head-bone matrix (orientation + position) for the real ped
    if (g_GetBoneMtx && g_FindPlayerPed)
    {
        void* pd = g_FindPlayerPed(0);
        if (pd)
        {
            float m[16] = { 0 };
            int ok = g_GetBoneMtx(pd, m, 1205);
            // valid if the position row is non-zero and finite
            if (m[12] || m[13] || m[14])
            {
                for (int i = 0; i < 16; ++i) g_headMtx[i] = m[i];
                g_headMtxOK = 1;
            }
            else g_headMtxOK = 0;
        }
    }
    if (fInTrain && ped) { NativeCtx a; a.pushI(ped); fInTrain(&a); g_inTrain = a.resI() ? 1 : 0; }
    if (fInCar && ped) { NativeCtx a; a.pushI(ped); fInCar(&a); g_inCar = a.resI() ? 1 : 0; }
    if (g_toastReq && fPrintNow)
    {
        g_toastReq = 0;
        static char s_toast[128];
        strncpy_s(s_toast, sizeof(s_toast), g_toastText, _TRUNCATE);
        NativeCtx a; a.pushP((void*)"STRING"); a.pushP(s_toast); a.pushI(3500); a.pushI(1);
        fPrintNow(&a);
    }
    if (fCurWeapon && ped)
    {
        uint32_t w = 0; NativeCtx a; a.pushI(ped); a.pushP(&w); fCurWeapon(&a);
        static int s_lastW = -1;
        if ((int)w != s_lastW)
        {
            s_lastW = (int)w; g_curWeapon = (int)w;
            int32_t sl = -1;
            if (fWeapSlot) { NativeCtx b; b.pushI((int32_t)w); b.pushP(&sl); fWeapSlot(&b); }
            g_curWeaponSlot = sl;
            Log("weapon changed: type=%d slot=%d", (int)w, (int)sl);
        }
    }
    if (fRagdoll && ped)
    {
        NativeCtx a; a.pushI(ped); fRagdoll(&a);
        int rd = a.resI() ? 1 : 0;
        if (rd != g_isRagdoll)
        {
            g_lookYaw = 0.0f; g_lookPitch = 0.0f;   // don't add a stale mouse offset to the tumble
            if (!rd) g_seedYawSet = 0;              // back on your feet -- reseed the look yaw
        }
        g_isRagdoll = rd;
    }

    // hide / restore the head + teeth/face + hair via the native. apply on change,
    // and re-apply every ~2 s so it survives a model reload.
    if (fDrawComp)
    {
        static int   lastState = -1;      // -1 none, 0 shown, 1 hidden
        static uint32_t nextReapply = 0;
        int want = (g_enabled && g_hideHead) ? 1 : 0;
        uint32_t now = (uint32_t)g_natTimer;
        if (want != lastState || (want != 0 && now >= nextReapply))
        {
            lastState = want;
            nextReapply = now + 2000;
            static const int comps[4] = { 0, 9, 10, 7 };   // HEAD, TEEF, FACE, HAIR
            for (int i = 0; i < 4; ++i) { NativeCtx a; a.pushI(comps[i]); a.pushI(want ? 0 : 1); fDrawComp(&a); }
        }
    }
    if (fHeading && ped) {
        float h = 0; NativeCtx a; a.pushI(ped); a.pushP(&h); fHeading(&a);
        if (h > -400.f && h < 400.f) { g_charHeading = h; g_headingOK = 1; }
    }

    bool inCs = g_cutsceneName && g_cutsceneName[0] != '\0';

    // per-cutscene "which slot is Niko" table (from testing / user override with 7/8).
    // -1 = unknown cutscene -- we don't know which slot is Niko, so we ride
    // whatever slot spawns first and keep re-checking every poll for the real one.
    static const struct { const char* name; int idx; } kCsNiko[] = {
        { "Vla4_a", 10 },
    };
    static char s_lastCs[64] = { 0 };
    static int  s_nikoIdx = -1;          // known/guessed Niko slot for the RUNNING cutscene
    static int  s_prevSlotOK[CS_SLOTS] = { 0 };   // last poll's validity, to catch late spawns
    static int  s_staleIdx = -1;                  // staleness tracker, shared with the block below
    static int  s_staleSinceMs = -1;
    static int  s_staleFlagged = 0;
    // Per-slot movement tracking: tells an actively-animated cutscene actor
    // apart from a frozen leftover sharing the same model hash (the
    // "duplicate gameplay Niko" ghost flagged since the very start of this
    // investigation -- a real Niko-modeled body can sit inert nearby while
    // the cutscene director puppeteers a DIFFERENT Niko-modeled slot). Only
    // consulted when 2+ slots read Niko's hash at once; with a single match
    // it changes nothing.
    static float s_slotLastPos[CS_SLOTS][3] = { {0} };
    static int   s_slotMoveMs[CS_SLOTS];
    static bool  s_slotMoveInit = false;
    if (!s_slotMoveInit) { for (int i = 0; i < CS_SLOTS; ++i) s_slotMoveMs[i] = -1; s_slotMoveInit = true; }
    // Softens the trusted-slot catch-up jump: while the zero-read hold above
    // keeps the camera glued to the last known position, Niko can keep
    // walking during that gap -- so the first fresh read after a hold can
    // land noticeably further away, a visible pop (2026-09-13, alpha12
    // follow-up). Ramp from the held position to the fresh one over
    // kTrustedRampMs instead of snapping; ordinary poll-to-poll movement
    // (no hold happened) is untouched, so normal walking stays crisp.
    // 150ms still read as a perceptible delay in testing -- cut to 50ms
    // (alpha14): enough to kill the hard pop, fast enough to feel instant.
    static bool  s_trustedWasHeld = false;
    static float s_trustedRampFrom[3] = { 0,0,0 };
    static int   s_trustedRampStartMs = -1;
    const int kTrustedRampMs = 50;
    if (inCs && _stricmp(s_lastCs, g_cutsceneName) != 0)
    {
        strncpy_s(s_lastCs, g_cutsceneName, _TRUNCATE);
        s_nikoIdx = -1;
        for (const auto& e : kCsNiko)
            if (_stricmp(e.name, g_cutsceneName) == 0) { s_nikoIdx = e.idx; break; }
        g_csUserOverride = 0;
        g_csIndex = (s_nikoIdx >= 0) ? s_nikoIdx : -1;   // -1 = "not picked yet, take first valid"
        for (int i = 0; i < CS_SLOTS; ++i) s_prevSlotOK[i] = 0;
        s_staleIdx = -1; s_staleSinceMs = -1; s_staleFlagged = 0;   // don't carry staleness across cutscenes
        for (int i = 0; i < CS_SLOTS; ++i)   // don't carry movement history across cutscenes either
        {
            s_slotMoveMs[i] = -1;
            s_slotLastPos[i][0] = s_slotLastPos[i][1] = s_slotLastPos[i][2] = 0.0f;
        }
        s_trustedWasHeld = false; s_trustedRampStartMs = -1;   // don't carry catch-up ramps across cutscenes either
        g_seedYawSet = 0;   // reseed the free-look yaw ONCE per cutscene (persists across shot cuts)
        Log("cutscene '%s' -> nikoIdx=%d (known=%d)", g_cutsceneName, s_nikoIdx, s_nikoIdx >= 0);
    }
    static int s_wasInCs = 0;
    if (!inCs && s_wasInCs) { g_seedYawSet = 0; }   // cutscene just ended -> reseed gameplay look
    s_wasInCs = inCs;
    if (!inCs) { s_lastCs[0] = 0; s_nikoIdx = -1; g_csUserOverride = 0; }

    if (inCs && fCsPed)
    {
        // scan every cutscene-ped slot; the camera follows g_csIndex
        for (int i = 0; i < CS_SLOTS; ++i)
        {
            float v[3] = { 0,0,0 };
            NativeCtx a; a.pushI(i); a.pushP(v);
            fCsPed(&a);
            a.flushVec();
            if (v[0] || v[1] || v[2])
            {
                if (i == s_nikoIdx && s_trustedWasHeld)
                {
                    // just recovered from a held blip -- start a short ramp
                    // from where we were frozen instead of snapping straight
                    // to the fresh (possibly-moved-on) position.
                    s_trustedRampFrom[0] = g_csSlot[i][0]; s_trustedRampFrom[1] = g_csSlot[i][1]; s_trustedRampFrom[2] = g_csSlot[i][2];
                    s_trustedRampStartMs = g_natTimer;
                    s_trustedWasHeld = false;
                }
                if (i == s_nikoIdx && s_trustedRampStartMs >= 0 && g_natTimer - s_trustedRampStartMs < kTrustedRampMs)
                {
                    float t = (g_natTimer - s_trustedRampStartMs) / (float)kTrustedRampMs;
                    g_csSlot[i][0] = s_trustedRampFrom[0] + (v[0] - s_trustedRampFrom[0]) * t;
                    g_csSlot[i][1] = s_trustedRampFrom[1] + (v[1] - s_trustedRampFrom[1]) * t;
                    g_csSlot[i][2] = s_trustedRampFrom[2] + (v[2] - s_trustedRampFrom[2]) * t;
                }
                else
                {
                    if (i == s_nikoIdx) s_trustedRampStartMs = -1;
                    g_csSlot[i][0] = v[0]; g_csSlot[i][1] = v[1]; g_csSlot[i][2] = v[2];
                }
                g_csSlotOK[i] = 1;
            }
            else if (i == s_nikoIdx && g_csSlotOK[i])
            {
                // Trusted table slot momentarily read (0,0,0) -- confirmed
                // (2026-09-13, alpha11 testing) that a shot transition can
                // make EVERY slot report zero for a poll or two while the
                // engine reloads, even though the real actor hasn't gone
                // anywhere (the very next poll reads the same position as
                // before the blip). That was forcing a full invalid ->
                // reselect round trip through kCsNiko, costing a visible
                // ~half-second flash to the on-foot fallback camera. Once
                // we've had a good read for the trusted slot, hold it
                // through a blip instead of dropping it -- g_csSlot/
                // g_csSlotOK stay exactly as they already are.
                s_trustedWasHeld = true;
            }
            else g_csSlotOK[i] = 0;
        }

        // Per-slot model hash -- if GET_PED_MODEL_FROM_INDEX really does take a
        // cutscene-slot index (unverified, same index space as GET_CUTSCENE_PED_
        // POSITION), this tells us definitively which slot is Niko, no spawn-order
        // guessing required. Only bother once we know Niko's own model hash and only
        // for slots that are currently valid.
        int modelMatch = -1;
        int modelMatchMoving = -1;   // among niko-hash matches, one that's actually moved recently
        const int kMovingMs = 2000;  // shorter than kStaleMs below -- "is this animating right now"
        if (fPedModelFromIdx && g_nikoModel)
        {
            for (int i = 0; i < CS_SLOTS; ++i)
            {
                if (!g_csSlotOK[i]) { g_csSlotModel[i] = 0; continue; }
                uint32_t m = 0; NativeCtx a; a.pushI(i); a.pushP(&m); fPedModelFromIdx(&a);
                // Testing (2026-09-13) showed a slot's REAL occupant can swap
                // across a shot-cut (position jumps to a new actor) while this
                // reported model hash stays frozen on whatever it first read --
                // i.e. slot 0 read Niko's hash the whole cutscene even though
                // the actor actually standing there was Dave in shot 1 and only
                // became Niko in shot 2. Log every time the reported value for
                // a slot actually changes, so we can see whether it ever
                // updates on its own or is simply stuck from first read.
                if (m != g_csSlotModel[i])
                    Log("csSlotModel[%d] 0x%08X -> 0x%08X%s", i, g_csSlotModel[i], m,
                        (m == g_nikoModel && m) ? "  [NIKO MODEL]" : "");
                g_csSlotModel[i] = m;
                if (m == g_nikoModel)
                {
                    if (modelMatch < 0) modelMatch = i;

                    // Also seen 2026-09-13: two slots can both read Niko's
                    // hash at once -- a leftover/decoy gameplay-Niko body
                    // sitting inert nearby while the cutscene director
                    // actually puppeteers a DIFFERENT Niko-modeled slot.
                    // First-match alone can't tell them apart; movement can
                    // -- the decoy just sits there, the real one animates.
                    volatile float* p = g_csSlot[i];
                    bool neverSeen = (s_slotMoveMs[i] < 0 && !s_slotLastPos[i][0] && !s_slotLastPos[i][1] && !s_slotLastPos[i][2]);
                    float dx = p[0] - s_slotLastPos[i][0], dy = p[1] - s_slotLastPos[i][1], dz = p[2] - s_slotLastPos[i][2];
                    if (neverSeen || dx * dx + dy * dy + dz * dz > 0.0001f) s_slotMoveMs[i] = g_natTimer;
                    s_slotLastPos[i][0] = p[0]; s_slotLastPos[i][1] = p[1]; s_slotLastPos[i][2] = p[2];

                    bool moving = (s_slotMoveMs[i] >= 0 && (g_natTimer - s_slotMoveMs[i]) < kMovingMs);
                    if (moving && modelMatchMoving < 0) modelMatchMoving = i;
                }
            }
        }
        int modelPick = (modelMatchMoving >= 0) ? modelMatchMoving : modelMatch;
        g_csModelMatch = modelMatch;   // raw first-match, unchanged meaning for the heartbeat log

        {
            // Niko may not have spawned into his slot yet when the cutscene starts
            // (his position reads zero for the first few frames). Once his known
            // slot lights up, snap onto him -- even if the camera is already riding
            // another actor -- so a wrong early pick self-corrects instead of
            // sticking for the whole scene.
            //
            // Confirmed via testing (2026-09-13): making this ALWAYS win, even over
            // a manual 7/8 pin, went too far the other way. g_nikoModel is known
            // almost from the start of a cutscene, so modelMatch resolves within a
            // poll or two and immediately stomps any manual pin right back to
            // Niko's slot -- 7/8 looked completely dead (log showed "csIndex=1 (8,
            // pinned)" instantly followed by "csIndex 1 -> 0 (modelMatch)", over and
            // over, every time the keys were pressed). Fix: only auto-snap when
            // there's no LIVE manual pin. A pin sticks as long as its slot is still
            // valid; once that slot goes stale/invalid (the original M13 problem --
            // a wrong early guess locking the camera for the whole scene), control
            // falls through to nikoReady/modelMatch/spawn-order exactly as if no pin
            // had ever been set.
            int cur = g_csIndex;
            bool curPinValid = (g_csUserOverride && cur >= 0 && cur < CS_SLOTS && g_csSlotOK[cur]);
            bool nikoReady = (s_nikoIdx >= 0 && s_nikoIdx < CS_SLOTS && g_csSlotOK[s_nikoIdx]);
            // REVERSED (2026-09-13) after direct visual confirmation: a manual pin
            // to Vla4_a's slot 10 (the kCsNiko table's answer) held for 2+ minutes,
            // ~60 F8 samples, and multiple staleness cycles -- every single one
            // showed Niko, snapping back within a second or two whenever staleness
            // briefly kicked the view to the real on-foot fallback. Slot 0 (what
            // modelMatch picks, since it happens to carry Niko's cached hash) was
            // separately, repeatedly confirmed to be Vlad the entire scene, moving
            // and all. So the earlier "kCsNiko points at a decoy" diagnosis that
            // justified preferring modelMatch was wrong -- what actually looked
            // like a wrong-character snap was staleness on the CORRECT slot (10),
            // not the table being bad. The model hash is the one with a confirmed
            // false positive here, not the table. Table wins when a cutscene has a
            // known entry; model hash stays the fallback for cutscenes that don't
            // (e.g. intro, which has no kCsNiko row and is unaffected by this).
            if (curPinValid)
            {
                // manual pin still alive on a valid slot -- respect it, don't fight
                // the 7/8 keys every poll.
            }
            else if (nikoReady) g_csIndex = s_nikoIdx;
            else if (modelPick >= 0) g_csIndex = modelPick;   // fallback when there's no known-cutscene entry
            else if (!g_csUserOverride)
            {
                // No fixed Niko slot and no model match (either the native guess is
                // wrong, or we just don't have g_nikoModel yet). Fall back to spawn-
                // order guessing: Niko tends to load later than the NPCs he's sharing
                // the scene with, so ride whichever slot JUST went valid this poll.
                // Unreliable in scenes with more than one late arrival -- kept only
                // as a last resort under the model check above, and only when the
                // user hasn't manually pinned a slot themselves.
                int lateArrival = -1;
                for (int i = 0; i < CS_SLOTS; ++i)
                    if (g_csSlotOK[i] && !s_prevSlotOK[i]) { lateArrival = i; break; }
                if (lateArrival >= 0) g_csIndex = lateArrival;
                else if (cur < 0 || cur >= CS_SLOTS || !g_csSlotOK[cur])
                {
                    int fallback = -1;
                    for (int i = 0; i < CS_SLOTS; ++i) if (g_csSlotOK[i]) { fallback = i; break; }
                    g_csIndex = fallback;
                }
            }
            if (g_csIndex != cur)
            {
                const char* why = nikoReady
                    ? "kCsNiko"
                    : ((modelPick >= 0) ? ((modelPick != modelMatch) ? "modelMatchMoving" : "modelMatch") : "spawnOrder");
                Log("csIndex %d -> %d (%s) model=0x%08X niko=0x%08X", cur, g_csIndex, why,
                    (g_csIndex >= 0 && g_csIndex < CS_SLOTS) ? g_csSlotModel[g_csIndex] : 0, g_nikoModel);
            }
        }
        for (int i = 0; i < CS_SLOTS; ++i) s_prevSlotOK[i] = g_csSlotOK[i];

        // Staleness check: GET_CUTSCENE_PED_POSITION doesn't zero out when an actor
        // is no longer driven by the current shot -- it just holds his LAST position,
        // frozen, forever (confirmed via F8: csSlot[0]'s model kept matching Niko all
        // scene, but its position sat bit-identical for 30+ seconds while every other
        // slot kept moving). That's almost certainly the same "extra frozen Niko"
        // Zolika's trainer showed and the user had to keep out of frame -- a stale
        // read, not a missing one. Detect it: if the currently-selected slot's
        // position hasn't moved for kStaleMs, treat it as unavailable so OnFinalCam's
        // existing fallback chain (real player head-bone) takes over, same tradeoff
        // Zolika shipped with.
        // Time-based (via GET_GAME_TIMER), not poll-count-based -- a poll-count
        // threshold (originally 12 polls, under half a second) falsely fired on
        // Niko just standing still at his mark for a moment before his walk-cycle
        // kicks in, kicking the camera off a perfectly live character. 4 seconds
        // comfortably outlasts an idle/dialogue beat while still escaping a beat
        // where he's genuinely not present (observed dead freezes ran 30+ seconds).
        const int kStaleMs = 4000;
        static float s_staleLastPos[3] = { 0,0,0 };
        int idx = g_csIndex;
        bool slotValid = (idx >= 0 && idx < CS_SLOTS && g_csSlotOK[idx]);
        // Vla4_a testing (2026-09-13) showed real Niko can legitimately stand
        // frozen through a dialogue beat for 20+ seconds straight -- well past
        // kStaleMs -- which kept kicking the camera to the on-foot fallback
        // ("gameplay Niko at the door") and back, over and over, even though
        // the table's pick (slot 10) was continuously, provably correct via
        // 60+ F8 samples across multiple staleness cycles. A kCsNiko entry
        // only gets added after exactly that kind of manual confirmation, so
        // once we're riding the table's slot, a frozen position just means
        // "he's standing still" -- skip the staleness bailout entirely for
        // it. Untested cutscenes with no table entry (e.g. intro) still get
        // full protection, since a modelMatch/spawn-order guess there can
        // still be genuinely wrong and needs the escape hatch.
        bool trustedSlot = (s_nikoIdx >= 0 && idx == s_nikoIdx);
        bool stale = false;
        if (trustedSlot)
        {
            // verified table slot -- never bail on staleness, see above.
        }
        else if (!g_csUserOverride && slotValid)
        {
            volatile float* p = g_csSlot[idx];
            bool sameAsLast = false;
            if (idx == s_staleIdx)
            {
                float dx = p[0] - s_staleLastPos[0], dy = p[1] - s_staleLastPos[1], dz = p[2] - s_staleLastPos[2];
                sameAsLast = (dx * dx + dy * dy + dz * dz < 0.0001f);
            }
            if (idx != s_staleIdx || !sameAsLast)
            {
                s_staleIdx = idx; s_staleSinceMs = g_natTimer; s_staleFlagged = 0;
                s_staleLastPos[0] = p[0]; s_staleLastPos[1] = p[1]; s_staleLastPos[2] = p[2];
            }
            stale = (s_staleSinceMs >= 0 && g_natTimer >= s_staleSinceMs
                && (g_natTimer - s_staleSinceMs) >= kStaleMs);
            if (stale && !s_staleFlagged)
            {
                s_staleFlagged = 1;
                Log("csIndex %d stale (frozen %dms+ at %.1f,%.1f,%.1f) -- falling back off cutscene ped",
                    idx, g_natTimer - s_staleSinceMs, p[0], p[1], p[2]);
            }
        }
        else { s_staleIdx = -1; s_staleSinceMs = -1; s_staleFlagged = 0; }

        if (slotValid && !stale) {
            g_csPedW[0] = g_csSlot[idx][0]; g_csPedW[1] = g_csSlot[idx][1]; g_csPedW[2] = g_csSlot[idx][2];
            g_csPedOK = 1;
        }
        else g_csPedOK = 0;
    }
    else if (!inCs) { g_csPedOK = 0; for (int i = 0; i < CS_SLOTS; ++i) g_csSlotOK[i] = 0; }
}

static volatile int      g_pollExc = 0;
static volatile uint32_t g_pollExcCode = 0;
static volatile uintptr_t g_pollExcAddr = 0;

static int PollFilter(EXCEPTION_POINTERS* ep)
{
    g_pollExcCode = ep->ExceptionRecord->ExceptionCode;
    g_pollExcAddr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

static void PollNatives()
{
    if (!g_getNativeAddress && !(g_pNatives && g_pNativeSize)) return;
    if (g_codePause && *g_codePause) return;         // game code paused - not safe
    if (g_userPause && *g_userPause) return;
    __try { PollNativesInner(); }
    __except (PollFilter(GetExceptionInformation()))
    {
        if ((++g_pollExc % 500) == 1)
            Log("PollNatives exception #%d code=0x%08X at 0x%p", g_pollExc,
                (unsigned)g_pollExcCode, (void*)g_pollExcAddr);
    }
}

// ---- settings (.ini) ---------------------------------------------------
// FirstPersonCutscene.ini next to the .asi. Created on first run from the compiled-in
// defaults (so the file always shows the real defaults); a key that is missing or
// unparsable just keeps its default. Table-driven: one row per setting drives the load,
// the F5 save and the default-file writer, so they can't drift apart. Plain Win32
// profile API, no dependencies.
// short on-screen text (shown by the next sim-thread poll); no-op if ShowMessages = 0
static void Toast(const char* text)
{
    if (!g_showMsgs) return;
    strncpy_s(g_toastText, sizeof(g_toastText), text, _TRUNCATE);
    g_toastReq = 1;
}

enum IniType { IT_FLOAT, IT_BOOL, IT_SPECIAL };
enum IniSpecial { SP_NONE, SP_START_ENABLED, SP_TOGGLE_KEY, SP_SAVE_KEY, SP_DEBUG, SP_MAX_PITCH_DEG, SP_AIM_SIDE };
struct IniEntry
{
    const char* sec; const char* key; IniType type;
    volatile float* pf; volatile int* pi; float lo, hi; int dec;
    IniSpecial sp;
    bool reload;      // applied again on Ctrl+F5 (startup-only settings are not)
    bool save;        // written by F5
    const char* comment;
};
#define INI_F(sec, key, ptr, lo, hi, dec, rl, sv, cmt) { sec, key, IT_FLOAT, ptr, nullptr, lo, hi, dec, SP_NONE, rl, sv, cmt }
#define INI_B(sec, key, ptr, rl, sv, cmt)              { sec, key, IT_BOOL, nullptr, ptr, 0.f, 1.f, 0, SP_NONE, rl, sv, cmt }
#define INI_S(sec, key, sp, rl, sv, cmt)               { sec, key, IT_SPECIAL, nullptr, nullptr, 0.f, 0.f, 0, sp, rl, sv, cmt }
#define INI_SLOT(i, cmt) INI_F("AimEyeForward", "Slot" #i, &g_aimFwdSlot[i], -0.4f, 0.6f, 3, true, true, cmt)

static const IniEntry kIni[] =
{
    INI_S("General", "StartEnabled", SP_START_ENABLED, false, false,
          "1 = first person is already on when the game loads; 0 = press ToggleKey to turn it on."),
    INI_S("General", "ToggleKey", SP_TOGGLE_KEY, true, false,
          "Key that turns first person on/off: F1-F12, a letter or digit, or a hex virtual-key code such as 0x76. Ctrl + this key toggles debug mode."),
    INI_S("General", "SaveKey", SP_SAVE_KEY, true, false,
          "Key that saves your current in-game settings into this file (Ctrl + it reloads the file). Same key names as ToggleKey, or none to disable. Works with or without debug mode."),
    INI_B("General", "ShowMessages", &g_showMsgs, true, true,
          "1 = show a short on-screen message when settings are saved or reloaded."),
    INI_S("General", "DebugMode", SP_DEBUG, false, false,
          "1 = debug mode starts on. It unlocks the live-tuning keys (see the README): tune with them in game, then press SaveKey to keep the values."),
    INI_B("General", "HideHead", &g_hideHead, true, true,
          "Hide Niko's head and hair so they don't block the view. Turn off only for debugging."),
    INI_F("General", "MouseSensitivity", &g_mouseSens, 0.0001f, 0.05f, 4, true, true,
          "Look speed in radians per mouse count. Raise it to look around faster, lower it for slower."),
    INI_B("General", "InvertY", &g_invertY, true, true,
          "1 = invert vertical mouse look."),
    INI_S("General", "MaxPitchDegrees", SP_MAX_PITCH_DEG, true, true,
          "How far up/down you can look, in degrees (30 - 89)."),

    INI_F("OnFoot", "EyeHeight", &g_eyeTrimV[0], -1.0f, 1.5f, 3, true, true,
          "Camera height offset in metres, added to the head (on foot) or the seat position (car, train). Positive = higher."),
    INI_F("OnFoot", "EyeForward", &g_eyeFwdV[0], -0.5f, 1.0f, 3, true, true,
          "Camera offset forward along the view, in metres. Positive = toward the nose / away from the back of your head."),
    INI_F("OnFoot", "FovBoost", &g_fovBoostV[0], -20.0f, 50.0f, 1, true, true,
          "Degrees added to the game's own field of view (negative narrows it). Ultra-wide, very large or very small screens usually want a different value: raise it for a wider view, lower it if the edges look stretched."),

    INI_F("Car", "EyeHeight", &g_eyeTrimV[1], -1.0f, 1.5f, 3, true, true, nullptr),
    INI_F("Car", "EyeForward", &g_eyeFwdV[1], -0.5f, 1.0f, 3, true, true, nullptr),
    INI_F("Car", "FovBoost", &g_fovBoostV[1], -20.0f, 50.0f, 1, true, true, nullptr),

    INI_F("Train", "EyeHeight", &g_eyeTrimV[2], -1.0f, 1.5f, 3, true, true, nullptr),
    INI_F("Train", "EyeForward", &g_eyeFwdV[2], -0.5f, 1.0f, 3, true, true, nullptr),
    INI_F("Train", "FovBoost", &g_fovBoostV[2], -20.0f, 50.0f, 1, true, true, nullptr),

    INI_B("Aim", "AlignToAimCamera", &g_aimAlign, true, true,
          "While aiming (hold RMB, on foot) take yaw/pitch from the game's aim camera so shots land on the crosshair. Turning this off brings the missed shots back."),
    INI_B("Aim", "EyeOnAimRay", &g_aimParallax, true, true,
          "Also slide the eye onto the game's aim ray so the crosshair is exactly the bullet line."),
    INI_S("Aim", "AimSide", SP_AIM_SIDE, true, true,
          "Where the aim ray starts: center = over your head (default), right = the game's own over-the-right-shoulder view, left = over the left shoulder."),
    INI_B("Aim", "AimRayToEyeHeight", &g_aimVert, true, true,
          "Experimental: also raise the aim ray to your eye height."),

    INI_SLOT(0, "Extra eye offset while aiming, in metres (+ forward, - back), per weapon slot (category), so long guns' stocks don't clip the screen. Slot5 = assault rifles (M4, AK47...). The log prints \"weapon changed: type=.. slot=..\" for each gun you equip. Add a Weapon<type> = value line to override a single weapon."),
    INI_SLOT(1, nullptr), INI_SLOT(2, nullptr), INI_SLOT(3, nullptr), INI_SLOT(4, nullptr),
    INI_SLOT(5, nullptr), INI_SLOT(6, nullptr), INI_SLOT(7, nullptr), INI_SLOT(8, nullptr),
    INI_SLOT(9, nullptr), INI_SLOT(10, nullptr), INI_SLOT(11, nullptr), INI_SLOT(12, nullptr),
    INI_SLOT(13, nullptr), INI_SLOT(14, nullptr), INI_SLOT(15, nullptr),
};
#undef INI_F
#undef INI_B
#undef INI_S
#undef INI_SLOT

static const char* IniSecNote(const char* sec)
{
    if (!strcmp(sec, "General")) return "Basic switches and mouse look.";
    if (!strcmp(sec, "OnFoot")) return "Camera tuning on foot. OnFoot / Car / Train each have their own eye height, eye forward and FOV boost.";
    if (!strcmp(sec, "Car")) return "Camera tuning in a car (same keys as [OnFoot]).";
    if (!strcmp(sec, "Train")) return "Camera tuning on the subway / trains (same keys as [OnFoot]).";
    if (!strcmp(sec, "Aim")) return "Aiming (hold RMB, on foot): how the camera lines up with where the bullets go.";
    if (!strcmp(sec, "AimEyeForward")) return "Per-weapon eye offset while aiming.";
    return nullptr;
}

// value text of one key, with any inline ; comment and whitespace stripped
static bool IniRaw(const char* sec, const char* key, char* out, int outSz)
{
    static const char kMissing[] = "\x01<missing>";
    GetPrivateProfileStringA(sec, key, kMissing, out, outSz, g_iniPath);
    if (strcmp(out, kMissing) == 0) return false;
    char* c = strchr(out, ';'); if (c) *c = 0;
    char* s = out; while (*s == ' ' || *s == '\t') ++s;
    if (s != out) memmove(out, s, strlen(s) + 1);
    size_t n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\t' || out[n - 1] == '\r')) out[--n] = 0;
    return out[0] != 0;
}

// locale-independent (accepts '.' or ',' as the decimal mark)
static bool ParseFloat(const char* s, float& out)
{
    while (*s == ' ' || *s == '\t') ++s;
    bool neg = false;
    if (*s == '-') { neg = true; ++s; } else if (*s == '+') ++s;
    long long mant = 0; int frac = 0, digits = 0; bool dot = false;
    for (; *s; ++s)
    {
        if (*s >= '0' && *s <= '9') { if (digits < 17) { mant = mant * 10 + (*s - '0'); if (dot) ++frac; } ++digits; }
        else if ((*s == '.' || *s == ',') && !dot) dot = true;
        else break;
    }
    if (digits == 0) return false;
    double v = (double)mant;
    for (int i = 0; i < frac; ++i) v /= 10.0;
    out = (float)(neg ? -v : v);
    return true;
}

static int ParseBool(const char* s)   // -1 = not a boolean
{
    if (!_stricmp(s, "1") || !_stricmp(s, "true") || !_stricmp(s, "yes") || !_stricmp(s, "on")) return 1;
    if (!_stricmp(s, "0") || !_stricmp(s, "false") || !_stricmp(s, "no") || !_stricmp(s, "off")) return 0;
    return -1;
}

static int VkFromName(const char* s)  // 0 = not recognised
{
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9')
    {
        const int n = atoi(s + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (s[0] && !s[1])
    {
        char c = s[0]; if (c >= 'a' && c <= 'z') c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        const int v = (int)strtol(s, nullptr, 16);
        if (v > 0 && v < 256) return v;
    }
    return 0;
}

static void VkName(int vk, char* out, int sz)
{
    if (vk >= VK_F1 && vk <= VK_F24) snprintf(out, sz, "F%d", vk - VK_F1 + 1);
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) snprintf(out, sz, "%c", vk);
    else snprintf(out, sz, "0x%02X", vk);
}

static void FmtFloat(char* out, int sz, float v, int dec)
{
    snprintf(out, sz, "%.*f", dec, (double)v);
    for (char* c = out; *c; ++c) if (*c == ',') *c = '.';
}

static void FormatEntry(const IniEntry& e, char* out, int sz)
{
    switch (e.type)
    {
    case IT_FLOAT: FmtFloat(out, sz, *e.pf, e.dec); return;
    case IT_BOOL:  snprintf(out, sz, "%d", *e.pi ? 1 : 0); return;
    default: break;
    }
    switch (e.sp)
    {
    case SP_START_ENABLED: snprintf(out, sz, "%d", g_enabled ? 1 : 0); break;
    case SP_TOGGLE_KEY:    VkName(g_toggleVk, out, sz); break;
    case SP_SAVE_KEY:      if (g_saveVk) VkName(g_saveVk, out, sz); else snprintf(out, sz, "none"); break;
    case SP_DEBUG:         snprintf(out, sz, "%d", g_debugMode ? 1 : 0); break;
    case SP_MAX_PITCH_DEG: FmtFloat(out, sz, g_maxPitch / 0.01745329f, 1); break;
    case SP_AIM_SIDE:      snprintf(out, sz, "%s", g_aimSide == 2 ? "left" : (g_aimSide == 1 ? "right" : "center")); break;
    default:               out[0] = 0; break;
    }
}

static bool ApplyEntry(const IniEntry& e, const char* raw)
{
    float f = 0.f;
    switch (e.type)
    {
    case IT_FLOAT:
        if (!ParseFloat(raw, f)) return false;
        if (f < e.lo) f = e.lo;
        if (f > e.hi) f = e.hi;
        *e.pf = f;
        return true;
    case IT_BOOL:
    {
        const int b = ParseBool(raw);
        if (b < 0) return false;
        *e.pi = b;
        return true;
    }
    default: break;
    }
    switch (e.sp)
    {
    case SP_START_ENABLED: { const int b = ParseBool(raw); if (b < 0) return false; g_enabled = (uint8_t)b; return true; }
    case SP_TOGGLE_KEY:    { const int v = VkFromName(raw); if (!v) return false; g_toggleVk = v; return true; }
    case SP_SAVE_KEY:
        if (!_stricmp(raw, "none") || !_stricmp(raw, "off") || !_stricmp(raw, "disabled")) { g_saveVk = 0; return true; }
        { const int v = VkFromName(raw); if (!v) return false; g_saveVk = v; return true; }
    case SP_DEBUG:         { const int b = ParseBool(raw); if (b < 0) return false; g_debugMode = b; return true; }
    case SP_MAX_PITCH_DEG:
        if (!ParseFloat(raw, f)) return false;
        if (f < 30.f) f = 30.f;
        if (f > 89.f) f = 89.f;
        g_maxPitch = f * 0.01745329f;
        return true;
    case SP_AIM_SIDE:
        if (!_stricmp(raw, "center") || !_stricmp(raw, "centre") || !_stricmp(raw, "middle") || !strcmp(raw, "0")) { g_aimSide = 0; return true; }
        if (!_stricmp(raw, "right") || !_stricmp(raw, "game") || !_stricmp(raw, "default") || !strcmp(raw, "1")) { g_aimSide = 1; return true; }
        if (!_stricmp(raw, "left") || !strcmp(raw, "2")) { g_aimSide = 2; return true; }
        return false;
    default: return false;
    }
}

static void WriteDefaultIni()
{
    FILE* f = nullptr;
    if (fopen_s(&f, g_iniPath, "w") != 0 || !f) { Log("could not create %s", g_iniPath); return; }
    fprintf(f,
        "; FirstPersonCutscene.ini -- settings for the GTA IV first-person mod (" FPMOD_VERSION ").\n"
        "; The values below are the defaults. Delete a line (or this whole file) to get the default back.\n"
        "; Edit while the game runs, then press Ctrl+SaveKey (Ctrl+F5 by default) to reload.\n"
        "; In debug mode (Ctrl+ToggleKey) you can also tune live with the keys from the README and press\n"
        "; SaveKey (F5) to write the current values back into this file. Lines starting with ; are comments.\n");
    const char* lastSec = "";
    for (const IniEntry& e : kIni)
    {
        if (strcmp(e.sec, lastSec) != 0)
        {
            lastSec = e.sec;
            fprintf(f, "\n[%s]\n", e.sec);
            const char* note = IniSecNote(e.sec);
            if (note) fprintf(f, "; %s\n", note);
        }
        char v[64]; FormatEntry(e, v, sizeof(v));
        if (e.comment) fprintf(f, "\n; %s\n", e.comment);
        fprintf(f, "%s = %s\n", e.key, v);
    }
    fclose(f);
    Log("created default settings file %s", g_iniPath);
}

// startup = true: apply everything (creating the file with the defaults if it doesn't exist);
// false (Ctrl+F5): re-apply only the live-tunable rows, leaving the on/off state alone.
static bool LoadIni(bool startup)
{
    if (!g_iniPath[0]) return false;
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES)
    {
        if (startup) { WriteDefaultIni(); return true; }
        Log("no .ini to reload (%s)", g_iniPath);
        return false;
    }
    int applied = 0;
    char raw[96];
    for (const IniEntry& e : kIni)
    {
        if (!startup && !e.reload) continue;
        if (IniRaw(e.sec, e.key, raw, sizeof(raw)) && ApplyEntry(e, raw)) ++applied;
    }
    for (int w = 0; w < 128; ++w)   // per-weapon overrides: Weapon<type> = metres
    {
        char key[16]; snprintf(key, sizeof(key), "Weapon%d", w);
        float v = 0.f;
        if (IniRaw("AimEyeForward", key, raw, sizeof(raw)) && ParseFloat(raw, v))
        {
            if (v < -0.4f) v = -0.4f;
            if (v > 0.6f) v = 0.6f;
            g_aimFwdW[w] = v; ++applied;
        }
    }
    char kn[16]; VkName(g_toggleVk, kn, sizeof(kn));
    Log("settings %s: %d value(s) from %s", startup ? "loaded" : "reloaded", applied, g_iniPath);
    Log("  toggle=%s enabled=%d debug=%d hideHead=%d sens=%.4f invertY=%d maxPitch=%.1fdeg",
        kn, (int)g_enabled, (int)g_debugMode, (int)g_hideHead, (double)g_mouseSens, (int)g_invertY,
        (double)(g_maxPitch / 0.01745329f));
    Log("  foot[h %.3f f %.3f fov %.1f] car[h %.3f f %.3f fov %.1f] train[h %.3f f %.3f fov %.1f]",
        (double)g_eyeTrimV[0], (double)g_eyeFwdV[0], (double)g_fovBoostV[0],
        (double)g_eyeTrimV[1], (double)g_eyeFwdV[1], (double)g_fovBoostV[1],
        (double)g_eyeTrimV[2], (double)g_eyeFwdV[2], (double)g_fovBoostV[2]);
    Log("  aim: align=%d eyeOnRay=%d side=%d vert=%d  slot5=%.3f weapon15=%.3f",
        (int)g_aimAlign, (int)g_aimParallax, (int)g_aimSide, (int)g_aimVert, (double)g_aimFwdSlot[5], (double)g_aimFwdW[15]);
    return true;
}

// write one value; for a key that already exists the API keeps the "Key =" text and puts
// the value straight after it, so lead with a space to keep the file's "Key = value" look
static bool IniPut(const char* sec, const char* key, const char* val)
{
    char raw[32], v[80];   // raw must fit IniRaw's "missing" sentinel, or absent keys look present
    if (IniRaw(sec, key, raw, sizeof(raw))) snprintf(v, sizeof(v), " %s", val);
    else snprintf(v, sizeof(v), "%s", val);
    return WritePrivateProfileStringA(sec, key, v, g_iniPath) != 0;
}

// F5: write the live tuning back into the .ini (WritePrivateProfileString edits values in
// place, so the file's comments and any keys we don't touch are preserved).
static bool SaveIni()
{
    if (!g_iniPath[0]) return false;
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) WriteDefaultIni();
    bool ok = true;
    char v[64];
    for (const IniEntry& e : kIni)
    {
        if (!e.save) continue;
        FormatEntry(e, v, sizeof(v));
        ok = IniPut(e.sec, e.key, v) && ok;
    }
    char raw[16];
    for (int w = 0; w < 128; ++w)
    {
        char key[16]; snprintf(key, sizeof(key), "Weapon%d", w);
        if (g_aimFwdW[w] != 0.f || IniRaw("AimEyeForward", key, raw, sizeof(raw)))
        {
            FmtFloat(v, sizeof(v), g_aimFwdW[w], 3);
            ok = IniPut("AimEyeForward", key, v) && ok;
        }
    }
    Log(ok ? "settings saved to %s" : "settings NOT saved (write failed) to %s", g_iniPath);
    return ok;
}

// the save-key action: Ctrl + key reloads the .ini, the key alone saves the current settings
static void SaveKeyAction(bool ctrl)
{
    if (ctrl) Toast(LoadIni(false) ? "First person: settings reloaded" : "First person: no .ini found to reload");
    else      Toast(SaveIni() ? "First person: settings saved to the .ini" : "First person: could not write the .ini");
}

// Aim-accuracy diagnostics. The game aims from its own camera objects
// (CCamAimWeapon etc. -- each has its own frame at +0x10 and pitch/heading
// fields), not from the final camera matrix we overwrite, so shots can diverge
// from our view. Dumps every live camera-pool object next to our view angles.
static void DumpAimState(const char* tag)
{
    const bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    Log("  aim[%s] rmb=%d lmb=%d charHdg=%.1fdeg seedYaw=%.2f lookYaw=%.2f lookPitch=%.2f inCs=%d ragdoll=%d align=%d parallax=%d side=%d vert=%d weapon=%d slot=%d aimFwd=%.2f",
        tag, (int)rmb, (int)lmb, g_charHeading, g_seedYaw, g_lookYaw, g_lookPitch,
        (int)(g_cutsceneName && g_cutsceneName[0]), g_isRagdoll, g_aimAlign, g_aimParallax, g_aimSide, g_aimVert,
        g_curWeapon, g_curWeaponSlot, EffectiveAimFwd());
    __try {
        Pool cp;
        if (ReadPool(g_pCamPoolPtr, cp, 0x100))
        {
            for (int i = 0; i < cp.size; ++i)
            {
                if (cp.flags[i] & 0x80) continue;
                uint8_t* o = cp.storage + i * cp.stride;
                const uintptr_t vt = *(uintptr_t*)o;
                const float* m = (const float*)(o + 0x10);
                float f144 = 0, f148 = 0;
                if (cp.stride >= 0x150) { f144 = *(float*)(o + 0x144); f148 = *(float*)(o + 0x148); }
                Log("    cam[%2d] vt=+0x%X fwd=(%.2f,%.2f,%.2f) pos=(%.1f,%.1f,%.1f) fov=%.1f f144=%.3f f148=%.3f",
                    i, (unsigned)(vt - g_moduleBase), m[4], m[5], m[6], m[12], m[13], m[14], m[20], f144, f148);
                // F8 only: raw floats of the aim camera, to locate its own yaw/pitch fields
                if (tag[0] == 'F' && vt == g_moduleBase + kAimCamVtblRva && cp.stride >= 0x1D0)
                    for (int r = 0x110; r < 0x1D0; r += 0x20)
                    {
                        const float* q = (const float*)(o + r);
                        Log("      +0x%03X: %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f", r, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
                    }
                // F8 only: work out the aim cam's shoulder offset (camera-space R/F/U from its
                // pivot at +0x140) and its yaw/pitch, then flag every float in the object
                // that equals one of them -- points straight at the fields we'd need to zero.
                if (tag[0] == 'F' && vt == g_moduleBase + kAimCamVtblRva)
                {
                    const int lim = cp.stride < 0x400 ? cp.stride : 0x400;
                    const float px = *(float*)(o + 0x140), py = *(float*)(o + 0x144), pz = *(float*)(o + 0x148);
                    const float dx = m[12] - px, dy = m[13] - py, dz = m[14] - pz;
                    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    const float yaw = atan2f(-m[4], m[5]);
                    const float pit = asinf(fmaxf(-1.f, fminf(1.f, m[6])));
                    const float offR = dx * m[0] + dy * m[1] + dz * m[2];
                    const float offF = dx * m[4] + dy * m[5] + dz * m[6];
                    const float offU = dx * m[8] + dy * m[9] + dz * m[10];
                    Log("      aimcam stride=0x%X pivot=(%.2f,%.2f,%.2f) yaw=%.4f pitch=%.4f offset R/F/U=(%.3f,%.3f,%.3f) dist=%.3f",
                        (unsigned)cp.stride, px, py, pz, yaw, pit, offR, offF, offU, dist);
                    {
                        const float qx = px - g_headMtx[12], qy = py - g_headMtx[13], qz = pz - g_headMtx[14];
                        Log("      pivot-from-head R/F/U=(%.3f,%.3f,%.3f) localOfs@+0x1C0=(%.3f,%.3f,%.3f) worldOfs@+0x1B0=(%.3f,%.3f,%.3f)",
                            qx * m[0] + qy * m[1] + qz * m[2], qx * m[4] + qy * m[5] + qz * m[6], qx * m[8] + qy * m[9] + qz * m[10],
                            *(float*)(o + 0x1C0), *(float*)(o + 0x1C4), *(float*)(o + 0x1C8),
                            *(float*)(o + 0x1B0), *(float*)(o + 0x1B4), *(float*)(o + 0x1B8));
                    }
                    const float D2R = 0.01745329f;
                    struct Want { const char* n; float v; };
                    const Want want[] = {
                        { "yaw", yaw }, { "-yaw", -yaw }, { "yaw+pi", yaw + 3.14159265f }, { "yawDeg", yaw / D2R },
                        { "pitch", pit }, { "-pitch", -pit }, { "pitchDeg", pit / D2R },
                        { "offR", offR }, { "-offR", -offR }, { "offF", offF }, { "-offF", -offF },
                        { "offU", offU }, { "-offU", -offU }, { "dist", dist }
                    };
                    for (int off = 0x60; off + 4 <= lim; off += 4)
                    {
                        const float v = *(float*)(o + off);
                        for (int w = 0; w < 14; ++w)
                            if (fabsf(want[w].v) > 0.05f && fabsf(v - want[w].v) < 0.0025f * fmaxf(1.f, fabsf(want[w].v)))
                                Log("        +0x%03X = %.4f  ~ %s (%.4f)", off, v, want[w].n, want[w].v);
                    }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("    cam pool dump faulted"); }
}

static DWORD WINAPI Worker(LPVOID)
{
    Log("Worker started");
    const int N = 22;
    bool k[N] = { 0 };
    int vk[N] = { VK_F7 /* replaced each loop by ToggleKey */, VK_F8, VK_F9, VK_F11, VK_F12,
                        VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_F6,
                        '7' /* cs slot - */, '8' /* cs slot + */, 'B' /* head-bone rotation */,
                        'J' /* hide head */,
                        VK_NEXT /* PgDn: FOV - */, VK_PRIOR /* PgUp: FOV + */,
                        '0' /* un-pin -- hand control back to auto (Niko when available) */,
                        'K' /* aim align on/off */, 'L' /* aim parallax on/off */,
                        'M' /* aim side: center / game / left */, 'N' /* aim vertical on/off */,
                        VK_F5 /* save the live tuning to the .ini / Ctrl+F5 reload it */ };
    int dn = 0; uint32_t tick = 0;
    bool lmbPrev = false;
    for (;;)
    {
        bool d[N];
        vk[0] = g_toggleVk;
        vk[21] = g_saveVk;
        for (int i = 0; i < N; ++i) d[i] = vk[i] && (GetAsyncKeyState(vk[i]) & 0x8000) != 0;
        const int vi = g_inTrain ? 2 : (g_inCar ? 1 : 0);   // which tuning set the keys edit
        const char* ctx = vi == 2 ? "train" : (vi == 1 ? "car" : "foot");

        // debug mode: log the aim state on every fire-button press
        const bool lmbNow = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (g_debugMode && lmbNow && !lmbPrev) DumpAimState("SHOT");
        lmbPrev = lmbNow;

        // The toggle key (F7 by default, ToggleKey in the .ini) is the only key that works
        // without debug mode. Ctrl + it switches debug mode itself -- everything below only
        // fires while that's on.
        if (d[0] && !k[0])
        {
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            char kn[16]; VkName(g_toggleVk, kn, sizeof(kn));
            if (ctrl)
            {
                g_debugMode = !g_debugMode;
                Log("Ctrl+%s debugMode=%d", kn, g_debugMode);
            }
            else
            {
                g_enabled = g_enabled ? 0 : 1; g_seedYawSet = 0; g_lookYaw = g_lookPitch = 0;
                Log("%s enabled=%d", kn, g_enabled);
            }
        }

        // SaveKey (F5 by default) works with or without debug mode: press it after tuning to
        // write the current settings into the .ini; Ctrl + it reloads the .ini
        if (d[21] && !k[21]) SaveKeyAction((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);

        if (g_debugMode)
        {
        if (d[9] && !k[9]) { g_useNatives = !g_useNatives; Log("F6 useNatives=%d", g_useNatives); }
        if (d[12] && !k[12]) {
            g_boneRot = !g_boneRot; g_lookYaw = g_lookPitch = 0;
            Log("B boneRot=%d (look reset)", g_boneRot);
        }
        if (d[13] && !k[13]) { g_hideHead = !g_hideHead; Log("J hideHead=%d", g_hideHead); }
        if (d[17] && !k[17]) { g_aimAlign = !g_aimAlign; Log("K aimAlign=%d", g_aimAlign); }
        if (d[18] && !k[18]) { g_aimParallax = !g_aimParallax; Log("L aimParallax=%d", g_aimParallax); }
        if (d[19] && !k[19]) { g_aimSide = (g_aimSide + 1) % 3; Log("M aimSide=%d (0 center, 1 game/right, 2 left)", g_aimSide); }
        if (d[20] && !k[20]) { g_aimVert = !g_aimVert; Log("N aimVert=%d", g_aimVert); }
        if (d[14] && !k[14]) { g_fovBoostV[vi] -= 3.0f; if (g_fovBoostV[vi] < -20.0f) g_fovBoostV[vi] = -20.0f; Log("PgDn fov[%s]=%.0f", ctx, g_fovBoostV[vi]); }
        if (d[15] && !k[15]) { g_fovBoostV[vi] += 3.0f; if (g_fovBoostV[vi] > 50.0f) g_fovBoostV[vi] = 50.0f; Log("PgUp fov[%s]=%.0f", ctx, g_fovBoostV[vi]); }
        // arrows: up/down = mouse sensitivity, left/right = eye forward
        if (d[7] && !k[7]) { g_mouseSens *= 1.25f; Log("mouseSens=%.5f", g_mouseSens); }
        if (d[8] && !k[8]) { g_mouseSens *= 0.80f; Log("mouseSens=%.5f", g_mouseSens); }
        if (g_boneRot)
        {
            if (d[5] && !k[5]) { g_fwdRow = (g_fwdRow + 1) % 3; g_lookYaw = g_lookPitch = 0; Log("fwdRow=%d", g_fwdRow); }
            if (d[6] && !k[6]) { g_fwdSign = !g_fwdSign; g_lookYaw = g_lookPitch = 0; Log("fwdSign=%d", g_fwdSign); }
        }
        else
        {
            // while aiming (or with Ctrl held) these tune the CURRENT WEAPON's aim eye-forward
            // (long guns clipping the screen); otherwise the normal eye-forward of the context
            const bool wpn = g_aimActive || (GetAsyncKeyState(VK_CONTROL) & 0x8000);
            if (d[5] && !k[5]) { if (wpn) AdjustAimFwd(-0.02f); else { g_eyeFwdV[vi] -= 0.03f; Log("eyeFwd[%s]=%.2f", ctx, g_eyeFwdV[vi]); } }
            if (d[6] && !k[6]) { if (wpn) AdjustAimFwd(+0.02f); else { g_eyeFwdV[vi] += 0.03f; Log("eyeFwd[%s]=%.2f", ctx, g_eyeFwdV[vi]); } }
        }
        }   // g_debugMode

        if (g_debugMode && d[1] && !k[1])
        {
            ++dn;
            Log("==== F8 #%d hits=%u applied=%u rank=%d eyeUp=%.2f cs='%s' ====",
                dn, g_hits, g_applied, g_pedRank, g_eyeUp,
                (g_cutsceneName && g_cutsceneName[0]) ? g_cutsceneName : "");
            Log("  shotCam pos=(%.1f, %.1f, %.1f)", g_shotX, g_shotY, g_shotZ);
            Log("  finalPos=(%.1f, %.1f, %.1f) fwdSign=%d foot[trim %.2f fwd %.2f fov %.0f] car[trim %.2f fwd %.2f fov %.0f] train[trim %.2f fwd %.2f fov %.0f] yaw=%.2f pitch=%.2f",
                g_pickX, g_pickY, g_pickZ, g_fwdSign,
                g_eyeTrimV[0], g_eyeFwdV[0], g_fovBoostV[0], g_eyeTrimV[1], g_eyeFwdV[1], g_fovBoostV[1],
                g_eyeTrimV[2], g_eyeFwdV[2], g_fovBoostV[2],
                g_lookYaw, g_lookPitch);
            Log("  hideHead=%d inCar=%d inTrain=%d ragdoll=%d incomingUp.z=%.2f",
                g_hideHead, g_inCar, g_inTrain, g_isRagdoll, g_dstRot[8]);
            Log("  shotRot r0=(%.2f,%.2f,%.2f) r1=(%.2f,%.2f,%.2f) r2=(%.2f,%.2f,%.2f)",
                g_dstRot[0], g_dstRot[1], g_dstRot[2], g_dstRot[3], g_dstRot[4], g_dstRot[5],
                g_dstRot[6], g_dstRot[7], g_dstRot[8]);
            Log("  headMtx ok=%d boneRot=%d fwdRow=%d pos=(%.2f,%.2f,%.2f)", g_headMtxOK, g_boneRot, g_fwdRow,
                g_headMtx[12], g_headMtx[13], g_headMtx[14]);
            Log("    row0=(%.2f,%.2f,%.2f) row1=(%.2f,%.2f,%.2f) row2=(%.2f,%.2f,%.2f)",
                g_headMtx[0], g_headMtx[1], g_headMtx[2], g_headMtx[4], g_headMtx[5], g_headMtx[6],
                g_headMtx[8], g_headMtx[9], g_headMtx[10]);
            Log("  pedMtx r0=(%.2f,%.2f,%.2f) r1=(%.2f,%.2f,%.2f) r2=(%.2f,%.2f,%.2f)",
                g_pedMtx3[0], g_pedMtx3[1], g_pedMtx3[2], g_pedMtx3[4], g_pedMtx3[5], g_pedMtx3[6],
                g_pedMtx3[8], g_pedMtx3[9], g_pedMtx3[10]);
            DumpAimState("F8");
            __try {
                Pool cp; if (ReadPool(g_pCamPoolPtr, cp, 0x100))
                {
                    float* m0 = (float*)(cp.storage + 0x10);
                    Log("  cam0 pos=(%.1f, %.1f, %.1f)", m0[12], m0[13], m0[14]);
                }
                if (g_FindPlayerPed) {
                    void* gp = g_FindPlayerPed(0);
                    if (gp) {
                        float* gm = *(float**)((char*)gp + 0x20);
                        if (gm) Log("  gameplayNiko pos=(%.1f, %.1f, %.1f)", gm[12], gm[13], gm[14]);
                    }
                }
                Pool pp;
                if (ReadPool(g_pPedPoolPtr, pp, 0x200)) {
                    float sx = g_shotX, sy = g_shotY, sz = g_shotZ;
                    for (int i = 0; i < pp.size; ++i) {
                        if (pp.flags[i] & 0x80) continue;
                        float* pm = *(float**)(pp.storage + i * pp.stride + 0x20);
                        if (!pm) continue;
                        float dx = pm[12] - sx, dy = pm[13] - sy, dz = pm[14] - sz;
                        float d = dx * dx + dy * dy + dz * dz;
                        if (d < 400.0f)     // within 20m of shot cam
                            Log("  ped[%3d] pos=(%.1f, %.1f, %.1f) up.z=%.2f dist=%.1f",
                                i, pm[12], pm[13], pm[14], pm[10], (d > 0 ? d : 0));
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (g_cutsceneName && g_cutsceneName[0]) {
                Log("  --- cutscene ped slots (active idx=%d, nikoModel=0x%08X) ---", g_csIndex, g_nikoModel);
                for (int i = 0; i < CS_SLOTS; ++i)
                    if (g_csSlotOK[i])
                        Log("    csSlot[%2d] pos=(%.1f, %.1f, %.1f) model=0x%08X%s%s", i,
                            g_csSlot[i][0], g_csSlot[i][1], g_csSlot[i][2], g_csSlotModel[i],
                            (g_csSlotModel[i] == g_nikoModel && g_csSlotModel[i]) ? "  [NIKO MODEL]" : "",
                            (i == g_csIndex) ? "  <-- camera" : "");
            }
        }
        if (g_debugMode)
        {
        if (d[2] && !k[2]) {
            // F9 = recenter: reseed the view yaw to Niko's current facing, reset look
            g_seedYawSet = 0; g_lookYaw = g_lookPitch = 0;
            Log("F9 recenter");
        }
        if (d[3] && !k[3]) { g_eyeTrimV[vi] -= 0.03f; Log("F11 eyeTrim[%s]=%.2f", ctx, g_eyeTrimV[vi]); }
        if (d[4] && !k[4]) { g_eyeTrimV[vi] += 0.03f; Log("F12 eyeTrim[%s]=%.2f", ctx, g_eyeTrimV[vi]); }
        if (d[10] && !k[10]) { g_csUserOverride = 1; g_csIndex = (g_csIndex + CS_SLOTS - 1) % CS_SLOTS; Log("csIndex=%d (7, pinned)", g_csIndex); }
        if (d[11] && !k[11]) { g_csUserOverride = 1; g_csIndex = (g_csIndex + 1) % CS_SLOTS; Log("csIndex=%d (8, pinned)", g_csIndex); }
        // 7/8 just step the raw slot index -- no idea which one is Niko. Rather
        // than hunting for him by hand, 0 drops the pin and hands control right
        // back to the auto logic, which snaps onto him immediately if his slot
        // is currently valid (same modelMatch/nikoReady check PollNativesInner
        // already runs every poll -- clearing the override just lets it act).
        if (d[16] && !k[16]) { g_csUserOverride = 0; Log("0 un-pinned -- back to auto (niko when available)"); }
        }   // g_debugMode
        for (int i = 0; i < N; ++i) k[i] = d[i];

        if (++tick % 60 == 0)
            Log("alive en=%d nat=%d fhh=%d gtid=%lu timer=%d ped=%d hdg=%.0f(%d) natOK=%d inTrain=%d inCar=%d ragdoll=%d head=(%.1f,%.1f,%.1f) csOK=%d csIdx=%d csPed=(%.1f,%.1f,%.1f) nikoModel=0x%08X modelMatch=%d exc=%d ecode=0x%08X eaddr=0x%p cs='%s'",
                g_enabled, g_useNatives, g_frameHookHits, (unsigned long)g_gameThreadId,
                g_natTimer, g_natDbgPed, g_charHeading, g_headingOK,
                g_natOK, g_inTrain, g_inCar, g_isRagdoll,
                g_headW[0], g_headW[1], g_headW[2], g_csPedOK, g_csIndex,
                g_csPedW[0], g_csPedW[1], g_csPedW[2],
                g_nikoModel, g_csModelMatch, g_pollExc,
                (unsigned)g_pollExcCode, (void*)g_pollExcAddr,
                (g_cutsceneName && g_cutsceneName[0]) ? g_cutsceneName : "");
        Sleep(30);
    }
    return 0;
}

static DWORD WINAPI Init(LPVOID)
{
    Log("=== Init  FirstPersonCutscene " FPMOD_VERSION " ===");
    LoadIni(true);      // creates FirstPersonCutscene.ini with the defaults on first run
    Sleep(4000);
    if (!GetMainModuleRange(g_moduleBase, g_moduleSize)) { Log("module range failed"); return 0; }
    Log("module base=0x%p size=0x%X", (void*)g_moduleBase, (unsigned)g_moduleSize);

    ResolveStuff();
    bool ok = InstallHook();
    InstallMouseHook();
    bool fh = InstallFrameHook();
    Log("game-process hook: %d", fh);
    Log(ok ? "init ok -- toggle key enables first person" : "camera hook FAILED");
    CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_selfInst = h;
        DisableThreadLibraryCalls(h);
        BuildLogPath();
        BuildIniPath();
        Log("=== DllMain attach ===");
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
