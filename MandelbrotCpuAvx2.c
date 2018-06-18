﻿#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <immintrin.h>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>


#define k_DemoName "Mandelbrot (CPU, AVX, Double-precision)"
#define k_DemoResolutionX 1280
#define k_DemoResolutionY 720
#define k_DemoRcpResolutionX (1.0 / k_DemoResolutionX)
#define k_DemoRcpResolutionY (1.0 / k_DemoResolutionY)
#define k_DemoAspectRatio ((double)k_DemoResolutionX / k_DemoResolutionY)

#define k_TileSize 40
#define k_NumTilesX (k_DemoResolutionX / k_TileSize)
#define k_NumTilesY (k_DemoResolutionY / k_TileSize)
#define k_NumTiles (k_NumTilesX * k_NumTilesY)

#define k_MaxNumThreads 32

typedef struct __declspec(align(32)) ComplexPacket
{
    __m256d re, im;
} ComplexPacket;

typedef struct Demo Demo;
typedef struct __declspec(align(64)) WorkerThread
{
    HANDLE handle;
    HANDLE beginEvent;
    HANDLE endEvent;
    Demo *demo;
} WorkerThread;

typedef struct Demo
{
    double zoom;
    double position[2];
    HWND window;
    HDC windowDevCtx;
    HDC memoryDevCtx;
    uint8_t *displayPtr;
    uint32_t numWorkerThreads;
    WorkerThread workerThreads[k_MaxNumThreads];
    volatile long drawTileIndex;
} Demo;

static __m256d s_0_5;
static __m256d s_1_0;
static __m256d s_100_0;

static double
GetTime(void)
{
    static LARGE_INTEGER counter0;
    static LARGE_INTEGER frequency;
    if (counter0.QuadPart == 0)
    {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter0);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart - counter0.QuadPart) / (double)frequency.QuadPart;
}

static void
UpdateFrameTime(HWND window, double *time, double *deltaTime)
{
    static double lastTime = -1.0;
    static double lastFpsTime = 0.0;
    static unsigned frameCount = 0;

    if (lastTime < 0.0)
    {
        lastTime = GetTime();
        lastFpsTime = lastTime;
    }

    *time = GetTime();
    *deltaTime = *time - lastTime;
    lastTime = *time;

    if ((*time - lastFpsTime) >= 1.0)
    {
        const double fps = frameCount / (*time - lastFpsTime);
        const double ms = (1.0 / fps) * 1000.0;
        char text[256];
        snprintf(text, sizeof(text), "[%.1f fps  %.3f ms] %s", fps, ms, k_DemoName);
        SetWindowText(window, text);
        lastFpsTime = *time;
        frameCount = 0;
    }
    frameCount++;
}

static LRESULT CALLBACK
ProcessWindowMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
        {
            PostQuitMessage(0);
            return 0;
        }
        break;
    }
    return DefWindowProc(window, message, wparam, lparam);
}

static void
MakeWindow(Demo *demo)
{
    WNDCLASS winclass = {
        .lpfnWndProc = ProcessWindowMessage,
        .hInstance = GetModuleHandle(NULL),
        .hCursor = LoadCursor(NULL, IDC_ARROW),
        .lpszClassName = k_DemoName
    };
    if (!RegisterClass(&winclass))
        assert(0);

    RECT rect = { 0, 0, k_DemoResolutionX, k_DemoResolutionY };
    if (!AdjustWindowRect(&rect, WS_OVERLAPPED | WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX, 0))
        assert(0);

    demo->window = CreateWindowEx(
        0, k_DemoName, k_DemoName, WS_OVERLAPPED | WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, NULL, 0);
    assert(demo->window);

    demo->windowDevCtx = GetDC(demo->window);
    assert(demo->windowDevCtx);

    BITMAPINFO bi = {
        .bmiHeader.biSize = sizeof(BITMAPINFOHEADER),
        .bmiHeader.biPlanes = 1,
        .bmiHeader.biBitCount = 32,
        .bmiHeader.biCompression = BI_RGB,
        .bmiHeader.biWidth = k_DemoResolutionX,
        .bmiHeader.biHeight = k_DemoResolutionY,
        .bmiHeader.biSizeImage = k_DemoResolutionX * k_DemoResolutionY,
    };
    HBITMAP hbm = CreateDIBSection(demo->windowDevCtx, &bi, DIB_RGB_COLORS, (void**)&demo->displayPtr, NULL, 0);
    assert(hbm);

    demo->memoryDevCtx = CreateCompatibleDC(demo->windowDevCtx);
    assert(demo->memoryDevCtx);

    SelectObject(demo->memoryDevCtx, hbm);
}

static __forceinline ComplexPacket
ComplexPacketMul(ComplexPacket a, ComplexPacket b)
{
    ComplexPacket ab;

    // ab.re = a.re * b.re - a.im * b.im;
    ab.re = _mm256_sub_pd(_mm256_mul_pd(a.re, b.re), _mm256_mul_pd(a.im, b.im));

    // ab.im = a.re * b.im + a.im * b.re;
    ab.im = _mm256_add_pd(_mm256_mul_pd(a.re, b.im), _mm256_mul_pd(a.im, b.re));

    return ab;
}

static __forceinline ComplexPacket
ComplexPacketSqr(ComplexPacket a)
{
    ComplexPacket aa;

    // aa.re = a.re * a.re - a.im * a.im;
    aa.re = _mm256_sub_pd(_mm256_mul_pd(a.re, a.re), _mm256_mul_pd(a.im, a.im));

    // aa.im = 2.0f * a.re * a.im;
    aa.im = _mm256_mul_pd(_mm256_add_pd(a.re, a.re), a.im);

    return aa;
}

static __m256d
ComputeDistance(__m256d vcx, __m256d vcy, int bailout)
{
    ComplexPacket z = { _mm256_setzero_pd(), _mm256_setzero_pd() };
    ComplexPacket dz = { s_1_0, _mm256_setzero_pd() };
    __m256d m2, lessMask;
    do
    {
        m2 = _mm256_add_pd(_mm256_mul_pd(z.re, z.re), _mm256_mul_pd(z.im, z.im));
        lessMask = _mm256_cmp_pd(m2, s_100_0, _CMP_LE_OQ);
        if (_mm256_movemask_pd(lessMask) == 0)
            break;

        ComplexPacket dzN = ComplexPacketMul(z, dz);
        dzN.re = _mm256_add_pd(_mm256_add_pd(dzN.re, dzN.re), s_1_0);
        dzN.im = _mm256_add_pd(dzN.im, dzN.im);

        ComplexPacket zN = ComplexPacketSqr(z);
        zN.re = _mm256_add_pd(zN.re, vcx);
        zN.im = _mm256_add_pd(zN.im, vcy);

        z.re = _mm256_blendv_pd(z.re, zN.re, lessMask);
        z.im = _mm256_blendv_pd(z.im, zN.im, lessMask);
        dz.re = _mm256_blendv_pd(dz.re, dzN.re, lessMask);
        dz.im = _mm256_blendv_pd(dz.im, dzN.im, lessMask);
    } while (--bailout);

    __declspec(align(32)) double logTemp[4];
    _mm256_store_pd(logTemp, m2);
    logTemp[0] = log(logTemp[0]);
    logTemp[1] = log(logTemp[1]);
    logTemp[2] = log(logTemp[2]);
    logTemp[3] = log(logTemp[3]);
    __m256d logRes = _mm256_load_pd(logTemp);

    __m256d dzDot2 = _mm256_add_pd(_mm256_mul_pd(dz.re, dz.re), _mm256_mul_pd(dz.im, dz.im));

    __m256d dist = _mm256_sqrt_pd(_mm256_div_pd(m2, dzDot2));
    dist = _mm256_mul_pd(logRes, _mm256_mul_pd(dist, s_0_5));

    return _mm256_andnot_pd(lessMask, dist);
}

static void
DrawTile(Demo *demo, uint32_t tileIndex)
{
    uint32_t x0 = (tileIndex % k_NumTilesX) * k_TileSize;
    uint32_t y0 = (tileIndex / k_NumTilesX) * k_TileSize;
    uint32_t x1 = x0 + k_TileSize;
    uint32_t y1 = y0 + k_TileSize;
    uint8_t *displayPtr = demo->displayPtr;

    __m256d xOffsets = _mm256_set_pd(3.0f, 2.0f, 1.0f, 0.0f);
    __m256d rcpResX = _mm256_set1_pd(k_DemoRcpResolutionX);
    __m256d aspectRatio = _mm256_set1_pd(k_DemoAspectRatio);
    __m256d zoom = _mm256_broadcast_sd(&demo->zoom);
    __m256d posX = _mm256_broadcast_sd(&demo->position[0]);

    for (uint32_t y = y0; y < y1; ++y)
    {
        double cy = 2.0 * (y * k_DemoRcpResolutionY - 0.5);
        cy = (cy * demo->zoom) - demo->position[1];
        __m256d vcy = _mm256_broadcast_sd(&cy);

        for (uint32_t x = x0; x < x1; x += 4)
        {
            // vcx = 2.0 * (x * k_DemoRcpResolutionX - 0.5) * k_DemoAspectRatio;
            double xd = (double)x;
            __m256d vcx = _mm256_add_pd(_mm256_broadcast_sd(&xd), xOffsets);
            vcx = _mm256_sub_pd(_mm256_mul_pd(vcx, rcpResX), s_0_5);
            vcx = _mm256_mul_pd(_mm256_add_pd(vcx, vcx), aspectRatio);

            // vcx = (vcx * demo->zoom) - demo->position[0];
            vcx = _mm256_sub_pd(_mm256_mul_pd(vcx, zoom), posX);

            __m256d d = ComputeDistance(vcx, vcy, 256);
            d = _mm256_sqrt_pd(_mm256_sqrt_pd(_mm256_div_pd(d, zoom)));
            d = _mm256_min_pd(d, s_1_0);

            __declspec(align(32)) double ds[4];
            _mm256_store_pd(ds, d);
            uint32_t idx = (x + y * k_DemoResolutionX) * 4;
            displayPtr[idx +  0] = (uint8_t)(255.0 * ds[0]);
            displayPtr[idx +  1] = (uint8_t)(255.0 * ds[0]);
            displayPtr[idx +  2] = (uint8_t)(255.0 * ds[0]);
            displayPtr[idx +  3] = 255;
            displayPtr[idx +  4] = (uint8_t)(255.0 * ds[1]);
            displayPtr[idx +  5] = (uint8_t)(255.0 * ds[1]);
            displayPtr[idx +  6] = (uint8_t)(255.0 * ds[1]);
            displayPtr[idx +  7] = 255;
            displayPtr[idx +  8] = (uint8_t)(255.0 * ds[2]);
            displayPtr[idx +  9] = (uint8_t)(255.0 * ds[2]);
            displayPtr[idx + 10] = (uint8_t)(255.0 * ds[2]);
            displayPtr[idx + 11] = 255;
            displayPtr[idx + 12] = (uint8_t)(255.0 * ds[3]);
            displayPtr[idx + 13] = (uint8_t)(255.0 * ds[3]);
            displayPtr[idx + 14] = (uint8_t)(255.0 * ds[3]);
            displayPtr[idx + 15] = 255;
        }
    }
}

static void
DrawTiles(Demo *demo)
{
    for (;;)
    {
        const uint32_t idx = (uint32_t)_InterlockedIncrement(&demo->drawTileIndex) - 1;
        if (idx >= k_NumTiles)
            break;

        DrawTile(demo, idx);
    }
}

static DWORD WINAPI
DrawTilesThread(void *param)
{
    WorkerThread *thread = (WorkerThread *)param;
    Demo *demo = thread->demo;

    for (;;)
    {
        WaitForSingleObject(thread->beginEvent, INFINITE);

        DrawTiles(demo);

        SetEvent(thread->endEvent);
    }
}

static void
Draw(Demo *demo)
{
    demo->drawTileIndex = 0;

    HANDLE waitList[k_MaxNumThreads];
    for (uint32_t i = 0; i < demo->numWorkerThreads; ++i)
    {
        waitList[i] = demo->workerThreads[i].endEvent;
        SetEvent(demo->workerThreads[i].beginEvent);
    }
    DrawTiles(demo);

    WaitForMultipleObjects(demo->numWorkerThreads, waitList, TRUE, INFINITE);

    BitBlt(demo->windowDevCtx, 0, 0, k_DemoResolutionX, k_DemoResolutionY, demo->memoryDevCtx, 0, 0, SRCCOPY);
}

static void
SpawnThreads(Demo *demo)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    demo->numWorkerThreads = (uint32_t)(si.dwNumberOfProcessors - 1);

    for (uint32_t i = 0; i < demo->numWorkerThreads; ++i)
    {
        demo->workerThreads[i].demo = demo;
        demo->workerThreads[i].handle = NULL;
        demo->workerThreads[i].beginEvent = CreateEventEx(NULL, NULL, 0, EVENT_ALL_ACCESS);
        demo->workerThreads[i].endEvent = CreateEventEx(NULL, NULL, 0, EVENT_ALL_ACCESS);

        assert(demo->workerThreads[i].beginEvent);
        assert(demo->workerThreads[i].endEvent);
    }

    for (uint32_t i = 0; i < demo->numWorkerThreads; ++i)
    {
        demo->workerThreads[i].handle = CreateThread(NULL, 0, DrawTilesThread, (void*)&demo->workerThreads[i], 0, NULL);
        assert(demo->workerThreads[i].handle);
    }
}

int CALLBACK
WinMain(HINSTANCE h, HINSTANCE hp, LPSTR cmd, int f)
{
    (void)h; (void)hp; (void)cmd; (void)f;

    SetProcessDPIAware();

    Demo demo = { .zoom = 0.8, .position = { 0.5, 0.1 } };
    MakeWindow(&demo);
    SpawnThreads(&demo);

    s_0_5 = _mm256_set1_pd(0.5);
    s_1_0 = _mm256_set1_pd(1.0);
    s_100_0 = _mm256_set1_pd(100.0);

    for (;;)
    {
        MSG msg = { 0 };
        if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
        {
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                break;
        }
        else
        {
            double time, deltaTime;
            UpdateFrameTime(demo.window, &time, &deltaTime);

            if (GetAsyncKeyState('A') & 0x8000)
                demo.zoom -= deltaTime * demo.zoom;
            if (GetAsyncKeyState('Z') & 0x8000)
                demo.zoom += deltaTime * demo.zoom;

            if (GetAsyncKeyState(VK_LEFT) & 0x8000)
                demo.position[0] += deltaTime * demo.zoom;
            else if (GetAsyncKeyState(VK_RIGHT) & 0x8000)
                demo.position[0] -= deltaTime * demo.zoom;

            if (GetAsyncKeyState(VK_UP) & 0x8000)
                demo.position[1] -= deltaTime * demo.zoom;
            if (GetAsyncKeyState(VK_DOWN) & 0x8000)
                demo.position[1] += deltaTime * demo.zoom;

            Draw(&demo);
        }
    }

    return 0;
}
// vim: set ts=4 sw=4 expandtab:
