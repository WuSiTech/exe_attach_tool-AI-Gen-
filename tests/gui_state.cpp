// tests/gui_state.cpp —— 勾选一个复选框后截图，验证自绘的选中态
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
        g_main = h; return FALSE;
    }
    return TRUE;
}

static void SaveBmp(const wchar_t* path, int w, int h, const void* bits)
{
    BITMAPFILEHEADER fh; BITMAPINFOHEADER ih;
    ZeroMemory(&fh, sizeof(fh)); ZeroMemory(&ih, sizeof(ih));
    int stride = w * 4, imgSize = stride * h;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + imgSize;
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -h;
    ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB; ih.biSizeImage = imgSize;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(f, &fh, sizeof(fh), &wr, NULL);
    WriteFile(f, &ih, sizeof(ih), &wr, NULL);
    WriteFile(f, bits, imgSize, &wr, NULL);
    CloseHandle(f);
    printf("已保存 %ls (%dx%d)\n", path, w, h);
}

int main(void)
{
    { typedef BOOL (WINAPI *F)(void); HMODULE u = GetModuleHandleW(L"user32.dll");
      F f = u ? (F)(void*)GetProcAddress(u, "SetProcessDPIAware") : NULL; if (f) f(); }

    std::wstring cmd = L"\"WuSi_Attacher.exe\"";
    std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(NULL, &c[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("启动失败 %lu\n", GetLastError()); return 1;
    }
    for (int i = 0; i < 40 && !g_main; ++i) { EnumWindows(EnumTopProc, 0); if (!g_main) Sleep(250); }
    if (!g_main) { printf("没找到窗口\n"); return 1; }
    Sleep(600);

    /* 用真实鼠标消息测试：父窗口的 WM_LBUTTONDOWN 才会触发自绘复选框切换 */
    {
        HWND c2 = GetDlgItem(g_main, 1005);
        if (c2) {
            printf("控件 1005 样式 = 0x%08X (type=0x%X，0xB 即 BS_OWNERDRAW)\n",
                   (unsigned)GetWindowLongW(c2, GWL_STYLE),
                   (unsigned)(GetWindowLongW(c2, GWL_STYLE) & BS_TYPEMASK));
            RECT r; GetWindowRect(c2, &r);
            POINT pt = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
            ScreenToClient(g_main, &pt);
            printf("  点击点(客户区) = %ld,%ld\n", pt.x, pt.y);

            LPARAM lpDown = MAKELPARAM(pt.x, pt.y);
            SendMessageW(g_main, WM_LBUTTONDOWN, MK_LBUTTON, lpDown);
            SendMessageW(g_main, WM_LBUTTONUP, 0, lpDown);
            Sleep(150);
            GetWindowRect(c2, &r);
            printf("  点击一次后：控件还在，准备截图\n");
            SendMessageW(g_main, WM_LBUTTONDOWN, MK_LBUTTON, lpDown);
            SendMessageW(g_main, WM_LBUTTONUP, 0, lpDown);
            Sleep(150);
            printf("  再点一次（应切回未选中）\n");
            /* 最后留成选中态，方便截图对比 */
            SendMessageW(g_main, WM_LBUTTONDOWN, MK_LBUTTON, lpDown);
            SendMessageW(g_main, WM_LBUTTONUP, 0, lpDown);
            Sleep(150);
        }
    }

    /* 再挑两个也点上，形成"选中/未选中"对比 */
    {
        int ids[] = { 1006, 1016 };
        for (int k = 0; k < 2; ++k) {
            HWND c2 = GetDlgItem(g_main, ids[k]);
            if (!c2) continue;
            RECT r; GetWindowRect(c2, &r);
            POINT pt = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
            ScreenToClient(g_main, &pt);
            LPARAM lpDown = MAKELPARAM(pt.x, pt.y);
            SendMessageW(g_main, WM_LBUTTONDOWN, MK_LBUTTON, lpDown);
            SendMessageW(g_main, WM_LBUTTONUP, 0, lpDown);
            Sleep(100);
        }
    }
    /* 让「退出」按钮进入悬停态不易模拟，这里只验证选中态 */
    RedrawWindow(g_main, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    Sleep(400);

    RECT r; GetWindowRect(g_main, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    if (!PrintWindow(g_main, mem, 2)) BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);

    std::vector<unsigned char> bits((size_t)w * h * 4);
    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(mem, bmp, 0, h, &bits[0], &bi, DIB_RGB_COLORS);
    SaveBmp(L"gui_checked.bmp", w, h, &bits[0]);

    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, screen);
    TerminateProcess(OpenProcess(PROCESS_TERMINATE, FALSE, pi.dwProcessId), 0);
    return 0;
}
