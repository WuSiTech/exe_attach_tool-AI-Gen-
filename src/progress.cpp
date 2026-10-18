/* ============================================================================
 * progress.cpp —— 生成进度对话框
 *
 * 为什么要后台线程：构建时要拷贝 A/B 上百 MB，如果直接在窗口线程里做，
 * 消息循环就被占住 —— 窗口变"未响应"、进度条也不动。
 * 现在把构建放到后台线程，主线程只负责跑进度条和收进度消息。
 * ==========================================================================*/

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>

#include "progress.h"

#define PDM_SETSTATUS (WM_APP + 10)   /* wParam=阶段序号  lParam=百分比(0..100) */

static const wchar_t* kProgClass = L"WuSiProgressWnd";

struct ProgCtx {
    HWND hwnd;
    HWND bar;
    HWND text;
    HFONT font;
    bool  done;
    bool  ok;
};

static ProgCtx g_pc;
static bool    g_progClassReady = false;

/* ---- 阶段文案 ---------------------------------------------------------- */
static const wchar_t* StageText(int stage)
{
    switch (stage) {
    case 0: return L"正在准备…";
    case 1: return L"正在写入图标与版本信息…";
    case 2: return L"正在写入辅助程序 B…";
    case 3: return L"正在写入主程序 A…";
    case 4: return L"正在校验产物…";
    case 5: return L"生成完成";
    default: return L"正在生成…";
    }
}

static LRESULT CALLBACK ProgProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_pc.hwnd = hWnd;
        RECT rc;
        GetClientRect(hWnd, &rc);

        g_pc.text = CreateWindowExW(0, L"STATIC", L"正在准备…",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    16, 14, rc.right - 32, 20,
                                    hWnd, NULL, NULL, NULL);
        g_pc.bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
                                   WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                   16, 44, rc.right - 32, 20,
                                   hWnd, NULL, NULL, NULL);
        if (g_pc.font && g_pc.text) {
            SendMessageW(g_pc.text, WM_SETFONT, (WPARAM)g_pc.font, TRUE);
        }
        SendMessageW(g_pc.bar, PBM_SETRANGE32, 0, 100);
        SendMessageW(g_pc.bar, PBM_SETPOS, 0, 0);
        return 0;
    }

    case PDM_SETSTATUS: {
        int stage = (int)wp;
        int pct   = (int)lp;
        if (g_pc.text) SetWindowTextW(g_pc.text, StageText(stage));
        if (g_pc.bar) {
            if (pct < 0) SendMessageW(g_pc.bar, PBM_SETMARQUEE, TRUE, 40);
            else {
                SendMessageW(g_pc.bar, PBM_SETMARQUEE, FALSE, 0);
                SendMessageW(g_pc.bar, PBM_SETPOS, (WPARAM)pct, 0);
            }
        }
        return 0;
    }

    case WM_CLOSE:
        return 0;   /* 生成期间不允许关掉 */

    case WM_DESTROY:
        if (g_pc.font) { DeleteObject(g_pc.font); g_pc.font = NULL; }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

static void EnsureClass()
{
    if (g_progClassReady) return;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ProgProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kProgClass;
    RegisterClassExW(&wc);
    g_progClassReady = true;
}

HWND ProgressCreate(HWND parent)
{
    EnsureClass();
    ZeroMemory(&g_pc, sizeof(g_pc));

    /* 自己建一个字体。
       **不要**用 SendMessage(parent, WM_GETFONT) 去取主窗口的字体 ——
       那返回的是**对方进程**里的 HFONT，在本进程使用是非法句柄，
       会直接崩在 USER32 里（异常代码 0xc0000005）。这个坑踩过一次。 */
    if (!g_pc.font) {
        HDC dc = GetDC(NULL);
        int hgt = -MulDiv(9, dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96, 72);
        if (dc) ReleaseDC(NULL, dc);
        g_pc.font = CreateFontW(hgt, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE,
                                L"Microsoft YaHei UI");
        if (!g_pc.font)
            g_pc.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }

    const int W = 420, H = 120;
    RECT pr;
    GetWindowRect(parent, &pr);
    int x = pr.left + ((pr.right - pr.left) - W) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - H) / 2;

    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME,
                             kProgClass, L"正在生成",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             x, y, W, H, parent, NULL,
                             GetModuleHandleW(NULL), NULL);
    if (h) {
        ShowWindow(h, SW_SHOW);
        UpdateWindow(h);
    }
    return h;
}

void ProgressSet(HWND dlg, int stage, int pct)
{
    if (dlg) SendMessageW(dlg, PDM_SETSTATUS, (WPARAM)stage, (LPARAM)pct);
}

void ProgressPump(HWND dlg)
{
    if (dlg) UpdateWindow(dlg);
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void ProgressDestroy(HWND dlg)
{
    if (dlg) DestroyWindow(dlg);
}
