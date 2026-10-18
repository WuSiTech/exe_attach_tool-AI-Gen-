// tests/gui_shot.cpp —— 单独截取 WuSi_Attacher 主窗口，并报告 DPI 感知状态
#include <windows.h>
#include <stdio.h>
#include <vector>
#include <string>

static HWND g_main = NULL;

static BOOL CALLBACK EnumTopProc(HWND h, LPARAM)
{
    wchar_t buf[512];
    GetWindowTextW(h, buf, 512);
    if (std::wstring(buf).find(L"WuSi Attacher") != std::wstring::npos) {
        g_main = h;
        return FALSE;
    }
    return TRUE;
}

/* 用 WIC 太啰嗦，这里手写 32bpp BMP，方便直接看 */
static void SaveBmp(const wchar_t* path, int w, int h, const void* bits)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    ZeroMemory(&fh, sizeof(fh));
    ZeroMemory(&ih, sizeof(ih));
    int stride = w * 4;
    int imgSize = stride * h;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + imgSize;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;              /* top-down */
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = imgSize;

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(f, &fh, sizeof(fh), &wr, NULL);
    WriteFile(f, &ih, sizeof(ih), &wr, NULL);
    WriteFile(f, bits, imgSize, &wr, NULL);
    CloseHandle(f);
    printf("已保存截图: %ls (%dx%d)\n", path, w, h);
}

int main(void)
{
    /* 必须让本工具也 DPI 感知，否则 PrintWindow 的输出会被系统按缩放比虚拟化，
       截出来的图只有真实尺寸的 1/1.5，看起来像是内容被裁掉。 */
    {
        typedef BOOL (WINAPI *Fn)(void);
        HMODULE u = GetModuleHandleW(L"user32.dll");
        Fn fn = u ? (Fn)(void*)GetProcAddress(u, "SetProcessDPIAware") : NULL;
        if (fn) fn();
    }

    /* 真实物理分辨率（不受 DPI 虚拟化影响） */
    {
        DEVMODEW dm;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm))
            printf("真实显示模式 = %lux%lu @ %luHz\n",
                   (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight,
                   (unsigned long)dm.dmDisplayFrequency);
    }
    printf("GetSystemMetrics 屏幕 = %dx%d\n",
           GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    printf("\n");

    std::wstring exe = L"WuSi_Attacher.exe";
    std::wstring cmd = L"\"" + exe + L"\"";
    std::vector<wchar_t> c(cmd.begin(), cmd.end());
    c.push_back(0);

    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(exe.c_str(), &c[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("启动失败 err=%lu\n", GetLastError());
        return 1;
    }

    for (int i = 0; i < 40 && !g_main; ++i) { EnumWindows(EnumTopProc, 0); if (!g_main) Sleep(250); }
    if (!g_main) { printf("没找到主窗口\n"); return 1; }
    Sleep(800);

    DWORD pid = 0;
    GetWindowThreadProcessId(g_main, &pid);
    printf("窗口 pid=%lu\n", (unsigned long)pid);
    {
        typedef BOOL (WINAPI *Fn)(void);
        HMODULE u = GetModuleHandleW(L"user32.dll");
        Fn fn = u ? (Fn)(void*)GetProcAddress(u, "IsProcessDPIAware") : NULL;
        printf("本进程 DPI 感知 = %d (1=是)\n", (fn && fn()) ? 1 : 0);
        Fn sfn = u ? (Fn)(void*)GetProcAddress(u, "SetProcessDPIAware") : NULL;
        printf("SetProcessDPIAware 可用 = %d\n", sfn ? 1 : 0);
    }

    /* 直接量窗口所在线程的真实 DPI 与感知上下文 */
    {
        typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
        typedef HANDLE (WINAPI *GetWindowDpiAwarenessContextFn)(HWND);
        typedef int (WINAPI *GetAwarenessFromDpiAwarenessContextFn)(HANDLE);
        HMODULE u = GetModuleHandleW(L"user32.dll");
        GetDpiForWindowFn gd = u ? (GetDpiForWindowFn)(void*)GetProcAddress(u, "GetDpiForWindow") : NULL;
        GetWindowDpiAwarenessContextFn ga =
            u ? (GetWindowDpiAwarenessContextFn)(void*)GetProcAddress(u, "GetWindowDpiAwarenessContext") : NULL;
        GetAwarenessFromDpiAwarenessContextFn gf = u
            ? (GetAwarenessFromDpiAwarenessContextFn)(void*)GetProcAddress(u, "GetAwarenessFromDpiAwarenessContext") : NULL;
        if (gd) printf("GetDpiForWindow(打包器窗口) = %u\n", gd(g_main));
        if (ga && gf) {
            HANDLE ctx = ga(g_main);
            printf("窗口 DPI 感知 = %d  (0=Unaware 1=System 2=PerMonitor)\n", gf(ctx));
        }
        /* 顺便量一下同窗口的客户区，跟 GetClientRect 对照 */
        printf("GetDpiForWindow(本测试窗口=桌面) = %u\n", gd ? gd(GetDesktopWindow()) : 0);
    }

    RECT r;
    GetWindowRect(g_main, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    printf("窗口外框 = %dx%d\n", w, h);
    RECT cr; GetClientRect(g_main, &cr);
    printf("客户区 = %dx%d\n", cr.right, cr.bottom);

    /* 截取窗口（含非客户区用 PW_RENDERFULLCONTENT，失败则退回客户区） */
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    BOOL pr = PrintWindow(g_main, mem, 2);   /* PW_RENDERFULLCONTENT */
    if (!pr) {
        printf("PrintWindow 失败 err=%lu，改用 BitBlt 屏幕区域\n", GetLastError());
        BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);
    } else {
        printf("PrintWindow 成功\n");
    }

    std::vector<unsigned char> bits(w * h * 4);
    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(mem, bmp, 0, h, &bits[0], &bi, DIB_RGB_COLORS);

    SaveBmp(L"gui_window.bmp", w, h, &bits[0]);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);

    TerminateProcess(OpenProcess(PROCESS_TERMINATE, FALSE, pi.dwProcessId), 0);
    return 0;
}
