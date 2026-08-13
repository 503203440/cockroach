#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "resource.h"

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static const UINT  TIMER_MS  = 20;              // ~50 fps
static const float PI        = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HWND      g_hwnd = NULL;
static HDC       g_memDC = NULL;
static HBITMAP   g_hDib = NULL;
static void*     g_dibBits = NULL;
static Bitmap*   g_bmp = NULL;
static Graphics* g_gfx = NULL;
static Bitmap*   g_imgRoach = NULL;
static int       g_canvas = 340;

// settings (from config.ini)
static DWORD     g_idleMs = 30 * 1000;          // idle time before spawning (default 30s)

// virtual screen
static int g_scrX = 0, g_scrY = 0, g_scrW = 0, g_scrH = 0;

// state
enum State { HIDDEN, CRAWL, FLEE };
enum CrawlSubState { SUB_RUN, SUB_PAUSE };
static State         g_state = HIDDEN;
static CrawlSubState g_subState = SUB_RUN;
static float         g_subTimer = 2.0f;
static const float   MAX_TURN_RATE = 30.0f * (3.14159265f / 180.0f); // 30 deg/sec limit

static DWORD  g_lastInput = 0;
static float  g_x = 0, g_y = 0;          // cockroach center (absolute screen coords)
static float  g_heading = 0;             // facing angle (radians)
static float  g_headingTarget = 0;
static float  g_speed = 0;
static float  g_legAnimT = 0.0f;           // Leg tripod gait phase (freezes during pause)
static float  g_antAnimT = 0.0f;           // Antenna sniffing phase (always active)
static bool   g_entering = false;
static LARGE_INTEGER g_freq, g_lastTick;

// tray
static const UINT WM_TRAY = WM_APP + 1;
static const UINT IDM_SPAWN = 1001;
static const UINT IDM_EXIT  = 1002;
static NOTIFYICONDATA g_nid = {0};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static float frand(float lo, float hi) {
    return lo + (hi - lo) * ((float)rand() / (float)RAND_MAX);
}
static float rndAngle() { return frand(0.0f, 2.0f * PI); }

static float normAngle(float a) {
    while (a >  PI) a -= 2.0f * PI;
    while (a < -PI) a += 2.0f * PI;
    return a;
}

// ---------------------------------------------------------------------------
// Embedded Resource Image Loader
// ---------------------------------------------------------------------------
static Bitmap* LoadBitmapFromResource(HMODULE hMod, int resId) {
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hRes) return NULL;
    DWORD resSize = SizeofResource(hMod, hRes);
    if (resSize == 0) return NULL;
    HGLOBAL hMem = LoadResource(hMod, hRes);
    if (!hMem) return NULL;
    void* pData = LockResource(hMem);
    if (!pData) return NULL;

    HGLOBAL hBuffer = GlobalAlloc(GMEM_MOVEABLE, resSize);
    if (!hBuffer) return NULL;
    void* pBuffer = GlobalLock(hBuffer);
    if (!pBuffer) { GlobalFree(hBuffer); return NULL; }
    memcpy(pBuffer, pData, resSize);
    GlobalUnlock(hBuffer);

    IStream* pStream = NULL;
    if (FAILED(CreateStreamOnHGlobal(hBuffer, TRUE, &pStream))) {
        GlobalFree(hBuffer);
        return NULL;
    }

    Bitmap* bmp = Bitmap::FromStream(pStream);
    pStream->Release();
    return bmp;
}

// ---------------------------------------------------------------------------
// Config file
// ---------------------------------------------------------------------------
static void GetExeDir(WCHAR* buf, int cap) {
    GetModuleFileNameW(NULL, buf, cap);
    WCHAR* slash = wcsrchr(buf, L'\\');
    if (slash) *(slash + 1) = 0;
}

static void Trim(char*& s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = 0;
}

static void LoadConfig() {
    WCHAR dir[MAX_PATH];
    GetExeDir(dir, MAX_PATH);
    WCHAR path[MAX_PATH];
    wsprintfW(path, L"%sconfig.ini", dir);

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD sz = GetFileSize(h, NULL);
    if (sz == 0 || sz > 65536) { CloseHandle(h); return; }

    char* buf = new char[sz + 1];
    DWORD rd = 0;
    ReadFile(h, buf, sz, &rd, NULL);
    buf[rd] = 0;
    CloseHandle(h);

    char* line = buf;
    char* end = buf + rd;
    while (line < end) {
        char* nl = strchr(line, '\n');
        if (!nl) nl = end;
        char save = *nl;
        *nl = 0;

        char* key = line;
        Trim(key);
        if (*key && *key != '#' && *key != ';') {
            char* eq = strchr(key, '=');
            if (eq) {
                *eq = 0;
                char* val = eq + 1;
                Trim(key);
                Trim(val);
                if (_stricmp(key, "idle_seconds") == 0 || _stricmp(key, "idle_sec") == 0) {
                    double s = strtod(val, NULL);
                    if (s < 1.0) s = 1.0;
                    if (s > 86400.0) s = 86400.0;
                    g_idleMs = (DWORD)(s * 1000.0);
                } else if (_stricmp(key, "idle_minutes") == 0) {
                    double m = strtod(val, NULL);
                    if (m < 0.05) m = 0.05;
                    if (m > 1440) m = 1440;
                    g_idleMs = (DWORD)(m * 60000.0);
                }
            }
        }
        *nl = save;
        line = nl + 1;
    }
    delete[] buf;
}

// ---------------------------------------------------------------------------
// Graphics setup / teardown
// ---------------------------------------------------------------------------
static bool InitGraphics() {
    g_scrX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_scrY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_scrW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_scrH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g_memDC = CreateCompatibleDC(NULL);
    if (!g_memDC) return false;

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g_canvas;
    bi.bmiHeader.biHeight      = -g_canvas;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g_hDib = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &g_dibBits, NULL, 0);
    if (!g_hDib) return false;
    SelectObject(g_memDC, g_hDib);

    g_bmp = new Bitmap(g_canvas, g_canvas, PixelFormat32bppPARGB);
    g_gfx = new Graphics(g_bmp);
    g_gfx->SetSmoothingMode(SmoothingModeAntiAlias);
    g_gfx->SetInterpolationMode(InterpolationModeHighQualityBicubic);

    // Try loading from embedded EXE RCDATA resource first
    g_imgRoach = LoadBitmapFromResource(GetModuleHandle(NULL), IDR_ROACH_PNG);
    if (!g_imgRoach || g_imgRoach->GetLastStatus() != Ok) {
        if (g_imgRoach) { delete g_imgRoach; g_imgRoach = NULL; }
        // Fallback to external file if resource fails
        WCHAR dir[MAX_PATH];
        GetExeDir(dir, MAX_PATH);
        WCHAR imgPath[MAX_PATH];
        wsprintfW(imgPath, L"%scockroach.png", dir);
        g_imgRoach = Bitmap::FromFile(imgPath);
    }

    return true;
}

static void ShutdownGraphics() {
    if (g_imgRoach) { delete g_imgRoach; g_imgRoach = NULL; }
    if (g_gfx) { delete g_gfx; g_gfx = NULL; }
    if (g_bmp) { delete g_bmp; g_bmp = NULL; }
    if (g_hDib) { DeleteObject(g_hDib); g_hDib = NULL; }
    if (g_memDC) { DeleteDC(g_memDC); g_memDC = NULL; }
}

// Non-linear stance/swing kinetic shaping:
// Smooth stance push, rapid recovery swing
static float AsymSwing(float t) {
    float s = sinf(t);
    return (s >= 0.0f) ? powf(s, 0.75f) : -powf(-s, 1.25f);
}

static void DrawLeg(float jx, float jy, float baseAng, float femLen, float tibLen, float tarLen, float phase, bool isLeft, int legType) {
    Pen femPen(Color(255, 65, 28, 10), 4.0f);
    femPen.SetStartCap(LineCapRound);
    femPen.SetEndCap(LineCapRound);

    Pen tibPen(Color(255, 55, 22, 8), 2.8f);
    tibPen.SetStartCap(LineCapRound);
    tibPen.SetEndCap(LineCapRound);

    Pen spinePen(Color(255, 40, 15, 5), 1.6f);
    Pen tarPen(Color(255, 45, 18, 6), 1.6f);
    tarPen.SetStartCap(LineCapRound);

    float t = g_legAnimT + phase;
    float sw = AsymSwing(t);

    // Femur hip angle
    float femAng = baseAng + 0.35f * sw;
    float kx = jx + cosf(femAng) * femLen;
    float ky = jy + sinf(femAng) * femLen;

    // Tibia knee angle with flexion lag during forward swing
    float kneeFlex = 0.50f * sinf(t + 0.85f);
    float tibAng = femAng + (isLeft ? -0.48f : 0.48f) + (isLeft ? kneeFlex : -kneeFlex);
    float ax = kx + cosf(tibAng) * tibLen;
    float ay = ky + sinf(tibAng) * tibLen;

    // Tarsus foot angle with claw grip lag
    float clawFlex = 0.35f * sinf(t + 1.70f);
    float tarAng = tibAng + (isLeft ? 0.38f : -0.38f) + (isLeft ? clawFlex : -clawFlex);
    float fx = ax + cosf(tarAng) * tarLen;
    float fy = ay + sinf(tarAng) * tarLen;

    // Femur
    g_gfx->DrawLine(&femPen, jx, jy, kx, ky);

    // Tibia
    g_gfx->DrawLine(&tibPen, kx, ky, ax, ay);

    // Tibia Spines (4 sharp spines along Tibia)
    for (int s = 1; s <= 4; s++) {
        float ratio = (float)s / 5.0f;
        float sx = kx + (ax - kx) * ratio;
        float sy = ky + (ay - ky) * ratio;
        float sAng = tibAng + (isLeft ? -1.3f : 1.3f);
        float spx = sx + cosf(sAng) * 6.5f;
        float spy = sy + sinf(sAng) * 6.5f;
        g_gfx->DrawLine(&spinePen, sx, sy, spx, spy);
    }

    // Tarsus
    g_gfx->DrawLine(&tarPen, ax, ay, fx, fy);
}

static void DrawCockroach() {
    // -----------------------------------------------------------------------
    // 1. Tail Cerci & Abdomen Tip (0.8x scaled)
    // -----------------------------------------------------------------------
    Pen cerciPen(Color(255, 45, 18, 6), 2.4f);
    cerciPen.SetStartCap(LineCapRound);
    cerciPen.SetEndCap(LineCapRound);
    float cerciWig = sinf(g_antAnimT * 2.0f) * 1.4f;
    g_gfx->DrawLine(&cerciPen, -60.0f, -5.0f, -78.0f, -13.0f + cerciWig);
    g_gfx->DrawLine(&cerciPen, -60.0f,  5.0f, -78.0f,  13.0f - cerciWig);

    // -----------------------------------------------------------------------
    // 2. Dynamic 6 Spiny Legs (0.8x Scaled Biomechanical Fluid Kinematics)
    // -----------------------------------------------------------------------
    DrawLeg( 29.0f, -7.0f, -0.65f, 19.0f, 24.0f, 14.0f, 0.0f, true, 0);
    DrawLeg( 29.0f,  7.0f,  0.65f, 19.0f, 24.0f, 14.0f, 3.14159f, false, 0);

    DrawLeg(  7.0f, -8.0f, -1.45f, 22.0f, 29.0f, 17.0f, 3.14159f, true, 1);
    DrawLeg(  7.0f,  8.0f,  1.45f, 22.0f, 29.0f, 17.0f, 0.0f, false, 1);

    DrawLeg(-24.0f, -8.0f, -2.30f, 30.0f, 41.0f, 22.0f, 0.0f, true, 2);
    DrawLeg(-24.0f,  8.0f,  2.30f, 30.0f, 41.0f, 22.0f, 3.14159f, false, 2);

    // -----------------------------------------------------------------------
    // 3. Photorealistic Photo Torso (Scaled destW = 70.0f)
    // -----------------------------------------------------------------------
    if (g_imgRoach && g_imgRoach->GetLastStatus() == Ok) {
        float wobble = (g_subState == SUB_RUN) ? sinf(g_legAnimT * 2.5f) * 2.5f : 0.0f;

        Matrix oldTransform;
        g_gfx->GetTransform(&oldTransform);

        g_gfx->RotateTransform(90.0f + wobble);

        float imgW = (float)g_imgRoach->GetWidth();
        float imgH = (float)g_imgRoach->GetHeight();

        // Source crop rectangle for pure body
        RectF srcRect(imgW * 0.28f, imgH * 0.16f, imgW * 0.44f, imgH * 0.72f);

        // Destination rect on canvas (70px wide x ~113px long)
        float destW = 70.0f;
        float destH = destW * (srcRect.Height / srcRect.Width);
        RectF destRect(-destW / 2.0f, -destH / 2.0f, destW, destH);

        GraphicsPath clipPath;
        clipPath.AddEllipse(-destW * 0.46f, -destH * 0.48f, destW * 0.92f, destH * 0.96f);
        g_gfx->SetClip(&clipPath, CombineModeIntersect);

        g_gfx->DrawImage(g_imgRoach, destRect, srcRect.X, srcRect.Y, srcRect.Width, srcRect.Height, UnitPixel);

        g_gfx->ResetClip();
        g_gfx->SetTransform(&oldTransform);
    } else {
        // Procedural fallback
        SolidBrush abdoBrush(Color(255, 55, 22, 8));
        g_gfx->FillEllipse(&abdoBrush, -65.0f, -12.0f, 26.0f, 24.0f);
    }

    // -----------------------------------------------------------------------
    // 4. Dynamic Whip Antennae (0.8x Scaled Reach ~136px)
    // -----------------------------------------------------------------------
    Pen antPen1(Color(255, 35, 12, 4), 2.5f);
    antPen1.SetStartCap(LineCapRound);
    antPen1.SetEndCap(LineCapRound);

    Pen antPen2(Color(255, 35, 12, 4), 1.4f);
    antPen2.SetStartCap(LineCapRound);
    antPen2.SetEndCap(LineCapRound);

    // Antenna Sockets
    SolidBrush antJoint(Color(255, 35, 12, 4));
    g_gfx->FillEllipse(&antJoint, 32.0f, -5.5f, 5.0f, 5.0f);
    g_gfx->FillEllipse(&antJoint, 32.0f,  1.0f, 5.0f, 5.0f);

    float ampMult = (g_subState == SUB_PAUSE) ? 3.0f : 1.6f;

    // Left Antenna: independent frequency 1.4f & phase
    float lWig1 = sinf(g_antAnimT * 1.4f + 0.5f) * (4.3f + 2.4f * sinf(g_antAnimT * 0.35f)) * ampMult;
    float lWig2 = cosf(g_antAnimT * 1.1f + 1.2f) * (3.3f + 1.8f * cosf(g_antAnimT * 0.28f)) * ampMult;

    // Right Antenna: independent frequency 1.7f, phase +2.1f & amplitude modulation
    float rWig1 = sinf(g_antAnimT * 1.7f + 2.1f) * (4.8f + 2.8f * cosf(g_antAnimT * 0.42f)) * ampMult;
    float rWig2 = cosf(g_antAnimT * 1.3f + 0.8f) * (3.0f + 1.6f * sinf(g_antAnimT * 0.31f)) * ampMult;

    // Draw Left Antenna
    g_gfx->DrawBezier(&antPen1, 34.0f, -4.0f, 53.0f, -12.0f, 74.0f, -18.0f + lWig1, 96.0f + lWig2, -26.0f + lWig1);
    g_gfx->DrawBezier(&antPen2, 96.0f + lWig2, -26.0f + lWig1, 110.0f, -32.0f, 122.0f, -37.0f + lWig1, 136.0f, -42.0f + lWig2);

    // Draw Right Antenna
    g_gfx->DrawBezier(&antPen1, 34.0f,  4.0f, 53.0f,  12.0f, 74.0f,  18.0f + rWig1, 96.0f + rWig2,  26.0f + rWig1);
    g_gfx->DrawBezier(&antPen2, 96.0f + rWig2,  26.0f + rWig1, 110.0f,  32.0f, 122.0f,  37.0f + rWig1, 136.0f,  42.0f + rWig2);
}

static void RenderFrame() {
    g_gfx->ResetTransform();
    g_gfx->Clear(Color(0, 0, 0, 0));

    float half = g_canvas / 2.0f;
    g_gfx->TranslateTransform(half, half);
    g_gfx->RotateTransform(g_heading * 180.0f / PI);

    DrawCockroach();
    g_gfx->ResetTransform();

    BitmapData bd;
    Rect rc(0, 0, g_canvas, g_canvas);
    if (g_bmp->LockBits(&rc, ImageLockModeRead, PixelFormat32bppPARGB, &bd) == Ok) {
        memcpy(g_dibBits, bd.Scan0, (size_t)g_canvas * g_canvas * 4);
        g_bmp->UnlockBits(&bd);
    }

    POINT pt = { (LONG)(g_x - half), (LONG)(g_y - half) };
    SIZE sz = { g_canvas, g_canvas };
    POINT src = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hwnd, NULL, &pt, &sz, g_memDC, &src, 0, &blend, ULW_ALPHA);
}

// ---------------------------------------------------------------------------
// State machine & Realistic Movement AI
// ---------------------------------------------------------------------------
static void ShowRoach() { ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); }

static void HideRoach() {
    ShowWindow(g_hwnd, SW_HIDE);
    g_state = HIDDEN;
}

static void Spawn() {
    int edge = rand() % 4;
    switch (edge) {
        case 0: g_x = (float)g_scrX - 40.0f;             g_y = frand((float)g_scrY, (float)(g_scrY + g_scrH)); g_heading = 0.0f;      break;
        case 1: g_x = (float)(g_scrX + g_scrW) + 40.0f;  g_y = frand((float)g_scrY, (float)(g_scrY + g_scrH)); g_heading = PI;       break;
        case 2: g_x = frand((float)g_scrX, (float)(g_scrX + g_scrW)); g_y = (float)g_scrY - 40.0f;          g_heading = PI / 2.0f;  break;
        case 3: g_x = frand((float)g_scrX, (float)(g_scrX + g_scrW)); g_y = (float)(g_scrY + g_scrH) + 40.0f; g_heading = -PI / 2.0f; break;
    }
    g_headingTarget = g_heading;
    g_speed = frand(130.0f, 210.0f);
    g_entering = true;
    g_subState = SUB_RUN;
    g_subTimer = frand(1.5f, 3.0f);
    g_state = CRAWL;

    LASTINPUTINFO lii = {0};
    lii.cbSize = sizeof(lii);
    GetLastInputInfo(&lii);
    g_lastInput = lii.dwTime;

    ShowRoach();
}

static void StartFlee() {
    g_state = FLEE;
    g_speed = frand(600.0f, 850.0f);

    float dl = g_x - g_scrX;
    float dr = (g_scrX + g_scrW) - g_x;
    float dt = g_y - g_scrY;
    float db = (g_scrY + g_scrH) - g_y;

    float dmin = dl; g_heading = PI;
    if (dr < dmin) { dmin = dr; g_heading = 0.0f; }
    if (dt < dmin) { dmin = dt; g_heading = -PI / 2.0f; }
    if (db < dmin) { dmin = db; g_heading =  PI / 2.0f; }
}

static void UpdateCrawl(float dt) {
    POINT ptRoach = { (LONG)g_x, (LONG)g_y };
    HMONITOR hMon = MonitorFromPoint(ptRoach, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };

    float monMinX = (float)g_scrX + 40.0f;
    float monMaxX = (float)(g_scrX + g_scrW) - 40.0f;
    float monMinY = (float)g_scrY + 40.0f;
    float monMaxY = (float)(g_scrY + g_scrH) - 40.0f;

    if (GetMonitorInfoW(hMon, &mi)) {
        monMinX = (float)mi.rcMonitor.left + 40.0f;
        monMaxX = (float)mi.rcMonitor.right - 40.0f;
        monMinY = (float)mi.rcMonitor.top + 40.0f;
        monMaxY = (float)mi.rcMonitor.bottom - 40.0f;
    }

    float cx = (monMinX + monMaxX) / 2.0f;
    float cy = (monMinY + monMaxY) / 2.0f;

    // Layer 3: Emergency Fast Return if off-screen (outside all virtual screens)
    bool isOffScreen = g_x < (float)g_scrX - 20.0f || g_x > (float)(g_scrX + g_scrW) + 20.0f ||
                       g_y < (float)g_scrY - 20.0f || g_y > (float)(g_scrY + g_scrH) + 20.0f;
    if (isOffScreen && !g_entering) {
        g_heading = atan2f(cy - g_y, cx - g_x);
        g_headingTarget = g_heading;
        g_speed = 350.0f; // High-speed return dash
        g_subState = SUB_RUN;
        g_x += cosf(g_heading) * g_speed * dt;
        g_y += sinf(g_heading) * g_speed * dt;
        g_legAnimT += g_speed * dt * 0.06f;
        g_antAnimT += dt * 4.0f;
        return;
    }

    if (g_entering) {
        bool inside = g_x > monMinX + 20.0f && g_x < monMaxX - 20.0f &&
                      g_y > monMinY + 20.0f && g_y < monMaxY - 20.0f;
        if (inside) {
            g_entering = false;
            g_headingTarget = g_heading;
        }
    }

    g_subTimer -= dt;

    // Antennae sweep/sniff at gentle organic speed (1.5x clock)
    g_antAnimT += dt * 1.5f;

    if (g_subState == SUB_RUN) {
        // Layer 1: Wide early warning steering margin (180px) on current monitor
        float margin = 180.0f;
        bool nearEdge = g_x < monMinX + margin || g_x > monMaxX - margin ||
                         g_y < monMinY + margin || g_y > monMaxY - margin;
        
        if (nearEdge && !g_entering) {
            g_headingTarget = atan2f(cy - g_y, cx - g_x);
        }

        // Natural responsive steering (~140-250 deg/s)
        float diff = normAngle(g_headingTarget - g_heading);
        float maxTurn = (nearEdge ? 4.5f : 2.5f) * dt; // Responsive natural turn rate
        if (diff > maxTurn) diff = maxTurn;
        else if (diff < -maxTurn) diff = -maxTurn;
        g_heading += diff;

        float vx = cosf(g_heading) * g_speed;
        float vy = sinf(g_heading) * g_speed;

        g_x += vx * dt;
        g_y += vy * dt;

        // Layer 2: Hard Physical Inner Boundary Clamp per active monitor
        if (!g_entering) {
            if (g_x < monMinX) { g_x = monMinX; g_headingTarget = atan2f(cy - g_y, cx - g_x); g_heading = g_headingTarget; }
            if (g_x > monMaxX) { g_x = monMaxX; g_headingTarget = atan2f(cy - g_y, cx - g_x); g_heading = g_headingTarget; }
            if (g_y < monMinY) { g_y = monMinY; g_headingTarget = atan2f(cy - g_y, cx - g_x); g_heading = g_headingTarget; }
            if (g_y > monMaxY) { g_y = monMaxY; g_headingTarget = atan2f(cy - g_y, cx - g_x); g_heading = g_headingTarget; }
        }

        // Leg tripod gait updates ONLY when running
        g_legAnimT += g_speed * dt * 0.06f;

        if (g_subTimer <= 0.0f && !g_entering) {
            g_subState = SUB_PAUSE;
            int rnd = rand() % 100;
            if (rnd < 35) {
                g_subTimer = frand(0.8f, 1.5f);  // Micro-pause (35%)
            } else if (rnd < 85) {
                g_subTimer = frand(1.8f, 3.5f);  // Active sniffing (50%)
            } else {
                g_subTimer = frand(4.5f, 7.5f);  // Deep rest & exploration (15%)
            }
        }
    } else { // SUB_PAUSE (Stationary pause & antennae sniffing)
        // Legs stay 100% frozen (g_legAnimT is NOT incremented)!

        if (g_subTimer <= 0.0f) {
            // Pick gentle target angle offset (+-15 to +-25 deg)
            float angleOffset = frand(-25.0f, 25.0f) * (3.14159265f / 180.0f);
            g_headingTarget = normAngle(g_heading + angleOffset);
            g_speed = frand(120.0f, 220.0f);
            g_subState = SUB_RUN;
            g_subTimer = frand(1.0f, 4.0f);
        }
    }
}

static void UpdateFlee(float dt) {
    float vx = cosf(g_heading) * g_speed;
    float vy = sinf(g_heading) * g_speed;
    g_x += vx * dt;
    g_y += vy * dt;
    g_legAnimT += g_speed * dt * 0.04f;
    g_antAnimT += dt * 6.0f;

    if (g_x < g_scrX - 60 || g_x > g_scrX + g_scrW + 60 ||
        g_y < g_scrY - 60 || g_y > g_scrY + g_scrH + 60) {
        HideRoach();
    }
}

static void Tick() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)(now.QuadPart - g_lastTick.QuadPart) / (float)g_freq.QuadPart;
    g_lastTick = now;
    if (dt > 0.1f) dt = 0.1f;

    LASTINPUTINFO lii = {0};
    lii.cbSize = sizeof(lii);
    GetLastInputInfo(&lii);
    DWORD cur = lii.dwTime;
    DWORD idle = GetTickCount() - cur;

    switch (g_state) {
        case HIDDEN:
            if (idle >= g_idleMs) Spawn();
            break;
        case CRAWL:
            if (cur != g_lastInput) StartFlee();
            else UpdateCrawl(dt);
            break;
        case FLEE:
            UpdateFlee(dt);
            break;
    }
    g_lastInput = cur;

    if (g_state != HIDDEN) {
        RenderFrame();
    }
}

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------
static void AddTray() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"蟑螂小恶作剧");
    Shell_NotifyIcon(NIM_ADD, &g_nid);
}

static void RemoveTray() {
    Shell_NotifyIcon(NIM_DELETE, &g_nid);
}

static void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SPAWN, L"立即放出一只蟑螂");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);
    if (cmd == IDM_SPAWN) Spawn();
    else if (cmd == IDM_EXIT) PostMessage(g_hwnd, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------------------
// Window proc
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            Tick();
            return 0;
        case WM_TRAY:
            if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) ShowTrayMenu();
            return 0;
        case WM_DISPLAYCHANGE:
        case 0x02E0: // WM_DPICHANGED
            g_scrX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            g_scrY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            g_scrW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            g_scrH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            return 0;
        case WM_DESTROY:
            RemoveTray();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    HANDLE hMutex = CreateMutexW(NULL, FALSE, L"Global\\CockroachPrank_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }

    // Enable Per-Monitor V2 DPI Awareness for multi-monitor mixed DPI support
    typedef BOOL(WINAPI *SetProcessDpiAwarenessContextProc)(HANDLE);
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        SetProcessDpiAwarenessContextProc setDpi = (SetProcessDpiAwarenessContextProc)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setDpi) {
            setDpi((HANDLE)-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
        } else {
            SetProcessDPIAware();
        }
    } else {
        SetProcessDPIAware();
    }

    srand((unsigned)GetTickCount());

    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gsi, NULL) != Ok) return 1;

    LoadConfig();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"RoachWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
               WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    g_hwnd = CreateWindowExW(ex, wc.lpszClassName, L"", WS_POPUP,
                             0, 0, g_canvas, g_canvas, NULL, NULL, hInst, NULL);
    if (!g_hwnd) { GdiplusShutdown(token); return 1; }

    if (!InitGraphics()) { GdiplusShutdown(token); return 1; }

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_lastTick);

    LASTINPUTINFO lii = {0};
    lii.cbSize = sizeof(lii);
    GetLastInputInfo(&lii);
    g_lastInput = lii.dwTime;

    AddTray();
    SetTimer(g_hwnd, 1, TIMER_MS, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    KillTimer(g_hwnd, 1);
    ShutdownGraphics();
    GdiplusShutdown(token);
    return 0;
}
