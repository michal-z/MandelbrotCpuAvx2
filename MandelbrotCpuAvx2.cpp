#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <immintrin.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>


#define k_DemoName "Mandelbrot (CPU, AVX2)"
#define k_DemoResolutionX 1280
#define k_DemoResolutionY 720
#define k_DemoRcpResolutionX (1.0f / k_DemoResolutionX)
#define k_DemoRcpResolutionY (1.0f / k_DemoResolutionY)
#define k_DemoAspectRatio ((float)k_DemoResolutionX / k_DemoResolutionY)

#define k_TileSize 16
#define k_NumTilesX (k_DemoResolutionX / k_TileSize)
#define k_NumTilesY (k_DemoResolutionY / k_TileSize)
#define k_NumTiles (k_NumTilesX * k_NumTilesY)

#define k_MaxNumThreads 32

struct alignas(32) ComplexPacket
{
    __m256 re, im;
};

struct alignas(64) WorkerThread
{
    float zoom;
    float position[2];
    uint8_t* displayPtr;
    HANDLE handle;
    HANDLE beginEvent;
    HANDLE endEvent;
};

struct Demo
{
    float zoom;
    float position[2];
    uint8_t* displayPtr;
    HWND window;
    HDC windowDevCtx;
    HDC memoryDevCtx;
    uint32_t numWorkerThreads;
    WorkerThread workerThreads[k_MaxNumThreads];
};

alignas(64) static uint32_t s_TileIndex[16];

static const __m256 s_0_5 = _mm256_set1_ps(0.5f);
static const __m256 s_1_0 = _mm256_set1_ps(1.0f);
static const __m256 s_100_0 = _mm256_set1_ps(100.0f);

static double
GetTime()
{
    static LARGE_INTEGER startCounter;
    static LARGE_INTEGER frequency;
    if (startCounter.QuadPart == 0)
    {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&startCounter);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart - startCounter.QuadPart) / (double)frequency.QuadPart;
}

static void
UpdateFrameTime(HWND window, double& o_Time, float& o_DeltaTime)
{
    static double lastTime = -1.0;
    static double lastFpsTime = 0.0;
    static unsigned frameCount = 0;

    if (lastTime < 0.0)
    {
        lastTime = GetTime();
        lastFpsTime = lastTime;
    }

    o_Time = GetTime();
    o_DeltaTime = (float)(o_Time - lastTime);
    lastTime = o_Time;

    if ((o_Time - lastFpsTime) >= 1.0)
    {
        const double fps = frameCount / (o_Time - lastFpsTime);
        const double ms = (1.0 / fps) * 1000.0;
        char text[256];
        snprintf(text, sizeof(text), "[%.1f fps  %.3f ms] %s", fps, ms, k_DemoName);
        SetWindowText(window, text);
        lastFpsTime = o_Time;
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
InitializeWindow(Demo& demo)
{
    WNDCLASS winclass = {};
    winclass.lpfnWndProc = ProcessWindowMessage;
    winclass.hInstance = GetModuleHandle(nullptr);
    winclass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    winclass.lpszClassName = k_DemoName;
    if (!RegisterClass(&winclass))
        assert(0);

    RECT rect = { 0, 0, k_DemoResolutionX, k_DemoResolutionY };
    if (!AdjustWindowRect(&rect, WS_OVERLAPPED | WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX, 0))
        assert(0);

    demo.window = CreateWindowEx(
        0, k_DemoName, k_DemoName, WS_OVERLAPPED | WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, nullptr, 0);
    assert(demo.window);

    demo.windowDevCtx = GetDC(demo.window);
    assert(demo.windowDevCtx);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bi.bmiHeader.biWidth = k_DemoResolutionX;
    bi.bmiHeader.biHeight = k_DemoResolutionY;
    bi.bmiHeader.biSizeImage = k_DemoResolutionX * k_DemoResolutionY;
    HBITMAP hbm = CreateDIBSection(demo.windowDevCtx, &bi, DIB_RGB_COLORS, (void**)&demo.displayPtr, nullptr, 0);
    assert(hbm);

    demo.memoryDevCtx = CreateCompatibleDC(demo.windowDevCtx);
    assert(demo.memoryDevCtx);

    SelectObject(demo.memoryDevCtx, hbm);
}

static __forceinline ComplexPacket
Mul(ComplexPacket a, ComplexPacket b)
{
    ComplexPacket ab;

    // ab.re = a.re * b.re - a.im * b.im;
    ab.re = _mm256_sub_ps(_mm256_mul_ps(a.re, b.re), _mm256_mul_ps(a.im, b.im));

    // ab.im = a.re * b.im + a.im * b.re;
    ab.im = _mm256_add_ps(_mm256_mul_ps(a.re, b.im), _mm256_mul_ps(a.im, b.re));

    return ab;
}

static __forceinline ComplexPacket
Sqr(ComplexPacket a)
{
    ComplexPacket aa;

    // aa.re = a.re * a.re - a.im * a.im;
    aa.re = _mm256_sub_ps(_mm256_mul_ps(a.re, a.re), _mm256_mul_ps(a.im, a.im));

    // aa.im = 2.0f * a.re * a.im;
    aa.im = _mm256_mul_ps(_mm256_add_ps(a.re, a.re), a.im);

    return aa;
}

#ifndef __INTEL_COMPILER
static __forceinline __m256 __vectorcall
_mm256_log_ps(__m256 v)
{
    alignas(32) float temp[8];
    _mm256_store_ps(temp, v);
    temp[0] = logf(temp[0]);
    temp[1] = logf(temp[1]);
    temp[2] = logf(temp[2]);
    temp[3] = logf(temp[3]);
    temp[4] = logf(temp[4]);
    temp[5] = logf(temp[5]);
    temp[6] = logf(temp[6]);
    temp[7] = logf(temp[7]);
    return _mm256_load_ps(temp);
}
#endif

static __m256
ComputeDistance(__m256 vcx, __m256 vcy, uint32_t bailout)
{
    ComplexPacket z = { _mm256_setzero_ps(), _mm256_setzero_ps() };
    ComplexPacket dz = { s_1_0, _mm256_setzero_ps() };
    __m256 m2, lessMask;

    do
    {
        m2 = _mm256_add_ps(_mm256_mul_ps(z.re, z.re), _mm256_mul_ps(z.im, z.im));
        lessMask = _mm256_cmp_ps(m2, s_100_0, _CMP_LE_OQ);
        if (_mm256_movemask_ps(lessMask) == 0)
            break;

        ComplexPacket dzN = Mul(z, dz);
        dzN.re = _mm256_add_ps(_mm256_add_ps(dzN.re, dzN.re), s_1_0);
        dzN.im = _mm256_add_ps(dzN.im, dzN.im);

        ComplexPacket zN = Sqr(z);
        zN.re = _mm256_add_ps(zN.re, vcx);
        zN.im = _mm256_add_ps(zN.im, vcy);

        z.re = _mm256_blendv_ps(z.re, zN.re, lessMask);
        z.im = _mm256_blendv_ps(z.im, zN.im, lessMask);
        dz.re = _mm256_blendv_ps(dz.re, dzN.re, lessMask);
        dz.im = _mm256_blendv_ps(dz.im, dzN.im, lessMask);
    } while (--bailout);

    __m256 logRes = _mm256_log_ps(m2);
    __m256 dzDot2 = _mm256_add_ps(_mm256_mul_ps(dz.re, dz.re), _mm256_mul_ps(dz.im, dz.im));

    __m256 dist = _mm256_sqrt_ps(_mm256_div_ps(m2, dzDot2));
    dist = _mm256_mul_ps(logRes, _mm256_mul_ps(dist, s_0_5));

    return _mm256_andnot_ps(lessMask, dist);
}

static void
DrawTile(uint32_t tileIndex, uint8_t* displayPtr, float zoom, float positionX, float positionY)
{
    const uint32_t x0 = (tileIndex % k_NumTilesX) * k_TileSize;
    const uint32_t y0 = (tileIndex / k_NumTilesX) * k_TileSize;
    const uint32_t x1 = x0 + k_TileSize;
    const uint32_t y1 = y0 + k_TileSize;

    const __m256 xOffsets = _mm256_set_ps(7.0f, 6.0f, 5.0f, 4.0f, 3.0f, 2.0f, 1.0f, 0.0f);
    const __m256 rcpResX = _mm256_set1_ps(k_DemoRcpResolutionX);
    const __m256 aspectRatio = _mm256_set1_ps(k_DemoAspectRatio);
    const __m256 vzoom = _mm256_broadcast_ss(&zoom);
    const __m256 vposx = _mm256_broadcast_ss(&positionX);

    for (uint32_t y = y0; y < y1; ++y)
    {
        float cy = 2.0f * (y * k_DemoRcpResolutionY - 0.5f);
        cy = (cy * zoom) - positionY;
        const __m256 vcy = _mm256_broadcast_ss(&cy);

        for (uint32_t x = x0; x < x1; x += 8)
        {
            // vcx = 2.0f * (x * k_DemoRcpResolutionX - 0.5f) * k_DemoAspectRatio;
            const float xf = (float)x;
            __m256 vcx = _mm256_add_ps(_mm256_broadcast_ss(&xf), xOffsets);
            vcx = _mm256_sub_ps(_mm256_mul_ps(vcx, rcpResX), s_0_5);
            vcx = _mm256_mul_ps(_mm256_add_ps(vcx, vcx), aspectRatio);

            // vcx = (vcx * vzoom) - vposx;
            vcx = _mm256_sub_ps(_mm256_mul_ps(vcx, vzoom), vposx);

            __m256 d = ComputeDistance(vcx, vcy, 64);
            d = _mm256_sqrt_ps(_mm256_sqrt_ps(_mm256_div_ps(d, vzoom)));
            d = _mm256_min_ps(d, s_1_0);

            alignas(32) float ds[8];
            _mm256_store_ps(ds, d);
            const uint32_t idx = (x + y * k_DemoResolutionX) * 4;
            displayPtr[idx +  0] = (uint8_t)(255.0f * ds[0]);
            displayPtr[idx +  1] = (uint8_t)(255.0f * ds[0]);
            displayPtr[idx +  2] = (uint8_t)(255.0f * ds[0]);
            displayPtr[idx +  3] = 255;
            displayPtr[idx +  4] = (uint8_t)(255.0f * ds[1]);
            displayPtr[idx +  5] = (uint8_t)(255.0f * ds[1]);
            displayPtr[idx +  6] = (uint8_t)(255.0f * ds[1]);
            displayPtr[idx +  7] = 255;
            displayPtr[idx +  8] = (uint8_t)(255.0f * ds[2]);
            displayPtr[idx +  9] = (uint8_t)(255.0f * ds[2]);
            displayPtr[idx + 10] = (uint8_t)(255.0f * ds[2]);
            displayPtr[idx + 11] = 255;
            displayPtr[idx + 12] = (uint8_t)(255.0f * ds[3]);
            displayPtr[idx + 13] = (uint8_t)(255.0f * ds[3]);
            displayPtr[idx + 14] = (uint8_t)(255.0f * ds[3]);
            displayPtr[idx + 15] = 255;
            displayPtr[idx + 16] = (uint8_t)(255.0f * ds[4]);
            displayPtr[idx + 17] = (uint8_t)(255.0f * ds[4]);
            displayPtr[idx + 18] = (uint8_t)(255.0f * ds[4]);
            displayPtr[idx + 19] = 255;
            displayPtr[idx + 20] = (uint8_t)(255.0f * ds[5]);
            displayPtr[idx + 21] = (uint8_t)(255.0f * ds[5]);
            displayPtr[idx + 22] = (uint8_t)(255.0f * ds[5]);
            displayPtr[idx + 23] = 255;
            displayPtr[idx + 24] = (uint8_t)(255.0f * ds[6]);
            displayPtr[idx + 25] = (uint8_t)(255.0f * ds[6]);
            displayPtr[idx + 26] = (uint8_t)(255.0f * ds[6]);
            displayPtr[idx + 27] = 255;
            displayPtr[idx + 28] = (uint8_t)(255.0f * ds[7]);
            displayPtr[idx + 29] = (uint8_t)(255.0f * ds[7]);
            displayPtr[idx + 30] = (uint8_t)(255.0f * ds[7]);
            displayPtr[idx + 31] = 255;
        }
    }
}

static void
DrawTiles(uint8_t* displayPtr, float zoom, float positionX, float positionY)
{
    for (;;)
    {
        const uint32_t idx = (uint32_t)_InterlockedIncrement(s_TileIndex) - 1;
        if (idx >= k_NumTiles)
            break;

        DrawTile(idx, displayPtr, zoom, positionX, positionY);
    }
}

static DWORD WINAPI
DrawTilesThread(void* param)
{
    WorkerThread& thread = *(WorkerThread*)param;

    for (;;)
    {
        WaitForSingleObject(thread.beginEvent, INFINITE);

        DrawTiles(thread.displayPtr, thread.zoom, thread.position[0], thread.position[1]);

        SetEvent(thread.endEvent);
    }
}

static void
Draw(Demo& demo)
{
    s_TileIndex[0] = 0;

    HANDLE waitList[k_MaxNumThreads];
    for (uint32_t i = 0; i < demo.numWorkerThreads; ++i)
    {
        demo.workerThreads[i].zoom = demo.zoom;
        demo.workerThreads[i].position[0] = demo.position[0];
        demo.workerThreads[i].position[1] = demo.position[1];

        waitList[i] = demo.workerThreads[i].endEvent;
        SetEvent(demo.workerThreads[i].beginEvent);
    }
    DrawTiles(demo.displayPtr, demo.zoom, demo.position[0], demo.position[1]);

    WaitForMultipleObjects(demo.numWorkerThreads, waitList, TRUE, INFINITE);

    BitBlt(demo.windowDevCtx, 0, 0, k_DemoResolutionX, k_DemoResolutionY, demo.memoryDevCtx, 0, 0, SRCCOPY);
}

static void
InitializeWorkerThreads(Demo& demo)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    demo.numWorkerThreads = (uint32_t)(si.dwNumberOfProcessors - 1);

    for (uint32_t i = 0; i < demo.numWorkerThreads; ++i)
    {
        demo.workerThreads[i].displayPtr = demo.displayPtr;
        demo.workerThreads[i].handle = nullptr;
        demo.workerThreads[i].beginEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        demo.workerThreads[i].endEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

        assert(demo.workerThreads[i].beginEvent);
        assert(demo.workerThreads[i].endEvent);
    }

    for (uint32_t i = 0; i < demo.numWorkerThreads; ++i)
    {
        demo.workerThreads[i].handle = CreateThread(nullptr, 0, DrawTilesThread, (void*)&demo.workerThreads[i], 0, nullptr);
        assert(demo.workerThreads[i].handle);
    }
}

int CALLBACK
WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    SetProcessDPIAware();

    Demo demo = {};
    demo.zoom = 0.8;
    demo.position[0] = 0.5;
    demo.position[1] = 0.1;

    InitializeWindow(demo);
    InitializeWorkerThreads(demo);

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
            double time;
            float deltaTime;
            UpdateFrameTime(demo.window, time, deltaTime);

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

            Draw(demo);
        }
    }

    return 0;
}
// vim: ts=4 sw=4 expandtab:
