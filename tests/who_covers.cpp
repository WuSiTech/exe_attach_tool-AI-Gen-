// tests/who_covers.cpp —— 找出到底是谁盖在控件上面
#include <windows.h>
#include <stdio.h>
#include <vector>
#include <string>

static HWND g_main = NULL;

static BOOL CALLBACK EnumTopProc(HWND h, LPARAM)
{
    wchar_t buf[512]; GetWindowTextW(h, buf, 512);
    if (std::wstring(buf).find(L"WuSi Attacher") != std::wstring::npos) { g_main = h; return FALSE; }
    return TRUE;
}

static void Describe(HWND h, const char* tag)
{
    if (!h) { printf("  %s: (NULL)\n", tag); return; }
    wchar_t cls[256] = {0}, txt[256] = {0};
    GetClassNameW(h, cls, 256);
    SendMessageW(h, WM_GETTEXT, 255, (LPARAM)txt);
    RECT r; GetWindowRect(h, &r);
    LONG st = GetWindowLongW(h, GWL_STYLE);
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    printf("  %s: hwnd=%p id=%d cls=%ls text=[%ls]\n", tag, (void*)h,
           GetDlgCtrlID(h), cls, txt);
    printf("      rect=(%ld,%ld)-(%ld,%ld) size=%ldx%ld style=0x%08X ex=0x%08X parent=%p\n",
           r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top,
           (unsigned)st, (unsigned)ex, (void*)GetParent(h));
    printf("      visible=%d enabled=%d 是主窗口=%d\n",
           IsWindowVisible(h) ? 1 : 0, IsWindowEnabled(h) ? 1 : 0, h == g_main ? 1 : 0);
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

    printf("=== 主窗口 ===\n");
    Describe(g_main, "主窗口");

    HWND btn = GetDlgItem(g_main, 1012);   /* 生成 */
    printf("\n=== 「生成」按钮 ===\n");
    Describe(btn, "生成按钮");

    printf("\n=== 按钮中心点上，WindowFromPoint 是谁 ===\n");
    if (btn) {
        RECT r; GetClientRect(btn, &r);
        POINT p = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
        ClientToScreen(btn, &p);
        printf("  屏幕坐标 = %ld,%ld\n", p.x, p.y);
        HWND hit = WindowFromPoint(p);
        Describe(hit, "命中窗口");
        /* 沿父链往上走 */
        printf("  --- 父链 ---\n");
        HWND cur = hit;
        for (int i = 0; i < 6 && cur; ++i) {
            Describe(cur, "链上");
            cur = GetParent(cur);
        }
    }

    printf("\n=== 枚举主窗口所有直接子控件 ===\n");
    struct Ctx { int n; } ctx = { 0 };
    EnumChildWindows(g_main, [](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = (Ctx*)lp;
        if (GetParent(h) != g_main) return TRUE;
        RECT r; GetWindowRect(h, &r);
        wchar_t cls[128] = {0}; GetClassNameW(h, cls, 128);
        c->n++;
        if ((r.right - r.left) > 400 && (r.bottom - r.top) > 100)
            printf("  [大控件] id=%d cls=%ls size=%ldx%ld @(%ld,%ld)\n",
                   GetDlgCtrlID(h), cls, r.right - r.left, r.bottom - r.top,
                   r.left, r.top);
        return TRUE;
    }, (LPARAM)&ctx);
    printf("  直接子控件数 = %d\n", ctx.n);

    TerminateProcess(OpenProcess(PROCESS_TERMINATE, FALSE, pi.dwProcessId), 0);
    return 0;
}
