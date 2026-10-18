// tests/chk_click.cpp —— 验证子类化后复选框能否被点击切换
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
    Sleep(800);

    /* 给复选框发鼠标按下/抬起 —— 这正是用户点击时系统投递给控件的那条路径 */
    int ids[] = { 1005, 1006, 1007, 1016, 1008 };
    const char* names[] = { "管理员", "无窗口", "清理", "删除自身", "日志" };

    for (int k = 0; k < 5; ++k) {
        HWND h = GetDlgItem(g_main, ids[k]);
        if (!h) { printf("%-8s 控件不存在\n", names[k]); continue; }

        RECT r; GetClientRect(h, &r);
        POINT p = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
        LPARAM lp = MAKELPARAM(p.x, p.y);

        ULONG_PTR before = GetWindowLongPtrW(h, GWLP_USERDATA);
        SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        Sleep(30);
        SendMessageW(h, WM_LBUTTONUP, 0, lp);
        Sleep(120);
        ULONG_PTR after = GetWindowLongPtrW(h, GWLP_USERDATA);

        bool chkBefore = (before & 0x8) != 0;
        bool chkAfter  = (after  & 0x8) != 0;
        printf("%-8s 勾选 %d -> %d  %s\n", names[k], chkBefore ? 1 : 0, chkAfter ? 1 : 0,
               chkBefore != chkAfter ? "切换成功 ✔" : "没反应 ✘");

        /* 再点一次，应切回来 */
        SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        Sleep(30);
        SendMessageW(h, WM_LBUTTONUP, 0, lp);
        Sleep(120);
        ULONG_PTR after2 = GetWindowLongPtrW(h, GWLP_USERDATA);
        bool chkAfter2 = (after2 & 0x8) != 0;
        printf("         再点一次 %d -> %d  %s\n", chkAfter ? 1 : 0, chkAfter2 ? 1 : 0,
               chkAfter != chkAfter2 ? "来回切换正常 ✔" : "第二次没反应 ✘");
    }

    TerminateProcess(OpenProcess(PROCESS_TERMINATE, FALSE, pi.dwProcessId), 0);
    return 0;
}
