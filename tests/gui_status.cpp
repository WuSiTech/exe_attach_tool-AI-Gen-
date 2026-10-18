// tests/gui_status.cpp —— 验证状态行换文字后是否会残留旧文字（重叠 bug）
#include <windows.h>
#include <stdio.h>
#include <vector>
#include <string>

static HWND g_main = NULL;

static BOOL CALLBACK EnumTopProc(HWND h, LPARAM)
{
    wchar_t buf[512];
    GetWindowTextW(h, buf, 512);
    if (std::wstring(buf).find(L"WuSi Attacher") != std::wstring::npos) { g_main = h; return FALSE; }
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
    printf("已保存 %ls\n", path);
}

static void Shot(const wchar_t* name)
{
    RECT r; GetWindowRect(g_main, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(g_main, mem, 2);
    std::vector<unsigned char> bits((size_t)w * h * 4);
    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(mem, bmp, 0, h, &bits[0], &bi, DIB_RGB_COLORS);
    SaveBmp(name, w, h, &bits[0]);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, screen);
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
    Sleep(700);

    HWND st = GetDlgItem(g_main, 1014);   /* IDC_STATUS */
    if (!st) { printf("找不到状态行\n"); return 1; }

    /* 模拟"先长后短"的切换：长文字短文字都属于同一控件 */
    const wchar_t* seq[] = {
        L"就绪。选择 A 和 B，点「生成」。",
        L"请先选择存在的主程序 A.exe",
        L"就绪。选择 A 和 B，点「生成」。",
        L"生成成功",
    };
    for (int i = 0; i < 4; ++i) {
        SetWindowTextW(st, seq[i]);
        InvalidateRect(st, NULL, TRUE);
        RedrawWindow(g_main, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        Sleep(200);
        wchar_t cur[256] = {0};
        /* 跨进程不能用 GetWindowTextW 读属主窗口的文本，走 WM_GETTEXT */
        SendMessageW(st, WM_GETTEXT, 255, (LPARAM)cur);
        printf("[%d] 窗口存活=%d 状态文字长度=%d 内容=「%ls」\n", i,
               IsWindow(g_main) ? 1 : 0,
               (int)SendMessageW(st, WM_GETTEXTLENGTH, 0, 0), cur);
    }

    Shot(L"gui_status.bmp");
    TerminateProcess(OpenProcess(PROCESS_TERMINATE, FALSE, pi.dwProcessId), 0);
    return 0;
}
