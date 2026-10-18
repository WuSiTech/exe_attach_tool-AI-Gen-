// tests/gui_build.cpp —— 验证点「生成」时窗口不卡死、有进度对话框、完成后有提示
#include <windows.h>
#include <stdio.h>
#include <vector>
#include <string>

static HWND g_main = NULL;

static BOOL CALLBACK ETP(HWND h, LPARAM)
{
    wchar_t b[512]; GetWindowTextW(h, b, 512);
    if (std::wstring(b).find(L"WuSi Attacher") != std::wstring::npos) { g_main = h; return FALSE; }
    return TRUE;
}
static BOOL CALLBACK FindProg(HWND h, LPARAM lp)
{
    wchar_t b[256]; GetWindowTextW(h, b, 256);
    if (std::wstring(b).find(L"正在生成") != std::wstring::npos) { *(HWND*)lp = h; return FALSE; }
    return TRUE;
}

int main(int argc, char** argv)
{
    { typedef BOOL (WINAPI *F)(void); HMODULE u = GetModuleHandleW(L"user32.dll");
      F f = u ? (F)(void*)GetProcAddress(u, "SetProcessDPIAware") : NULL; if (f) f(); }

    std::wstring cmd = L"\"WuSi_Attacher.exe\"";
    std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(NULL, &c[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("启动失败\n"); return 1;
    }
    for (int i = 0; i < 40 && !g_main; ++i) { EnumWindows(ETP, 0); if (!g_main) Sleep(250); }
    if (!g_main) { printf("没找到窗口\n"); return 1; }
    Sleep(900);

    /* 填 A/B 路径（用命令行参数传入） */
    if (argc >= 3) {
        wchar_t wa[512], wb[512];
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wa, 512);
        MultiByteToWideChar(CP_ACP, 0, argv[2], -1, wb, 512);
        SetWindowTextW(GetDlgItem(g_main, 1001), wa);   /* A */
        SetWindowTextW(GetDlgItem(g_main, 1003), wb);   /* B */
    }
    /* 第三个参数：输出目录。沙箱禁止写桌面，所以验证时必须指到可写目录。 */
    if (argc >= 4) {
        wchar_t wd[512];
        MultiByteToWideChar(CP_ACP, 0, argv[3], -1, wd, 512);
        SetWindowTextW(GetDlgItem(g_main, 1011), wd);   /* 输出目录 */
    }
    SetWindowTextW(GetDlgItem(g_main, 1009), L"ProgTest.exe");
    Sleep(300);

    printf("点击「生成」...\n");
    HWND btn = GetDlgItem(g_main, 1012);
    RECT r; GetClientRect(btn, &r);
    POINT p = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
    LPARAM lp = MAKELPARAM(p.x, p.y);
    SendMessageW(btn, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    SendMessageW(btn, WM_LBUTTONUP, 0, lp);

    /* 构建期间反复检测：主窗口是否还能响应消息、进度框是否存在 */
    int notResponding = 0, sawProgress = 0;
    for (int t = 0; t < 60; ++t) {
        HWND prog = NULL;
        EnumWindows(FindProg, (LPARAM)&prog);
        if (prog) sawProgress = 1;

        /* SendMessageTimeout 超时即认为"无响应" */
        DWORD_PTR res = 0;
        LRESULT lr = SendMessageTimeoutW(g_main, WM_NULL, 0, 0,
                                         SMTO_ABORTIFHUNG | SMTO_BLOCK, 120, &res);
        if (lr == 0) ++notResponding;

        if (t == 3) printf("  t=3  进度框=%s\n", prog ? "有" : "无");
        Sleep(60);
    }
    printf("构建期间检测 60 次：无响应 %d 次，见过进度框 = %s\n",
           notResponding, sawProgress ? "是" : "否");

    /* 结束后应该弹出"生成成功"或"生成失败"提示框 */
    HWND tip = NULL;
    for (int t = 0; t < 50 && !tip; ++t) {
        tip = FindWindowW(NULL, L"生成成功");
        if (!tip) tip = FindWindowW(NULL, L"生成失败");
        if (!tip) tip = FindWindowW(NULL, L"无法生成");
        if (!tip) tip = FindWindowW(NULL, L"错误");
        if (!tip) Sleep(200);
    }
    if (tip) {
        wchar_t tt[128]; GetWindowTextW(tip, tt, 128);
        printf("完成提示框: 「%ls」 ✔\n", tt);
        SendMessageW(tip, WM_CLOSE, 0, 0);
    } else {
        printf("没有找到完成提示框（可能还在构建）\n");
    }

    /* 关键：打包器自己有没有崩掉？崩溃时会话里会留下"程序已停止工作"的窗口，
       进程也已经退出。这正是之前用户遇到的现象，必须显式检测。 */
    DWORD ec = 0;
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &ec);
        printf("!!! 打包器进程已退出，退出码 = 0x%08lX%s\n", (unsigned long)ec,
               (ec == 0xC0000005UL) ? "  <== 访问冲突（0xc0000005，就是那个崩溃）" :
               (ec == 0) ? "  （正常？但没弹提示框）" : "");
    } else {
        printf("打包器进程仍在运行（未崩溃）✔\n");
    }

    TerminateProcess(OpenProcess(PROCESS_TERMINATE, FALSE, pi.dwProcessId), 0);
    return 0;
}
