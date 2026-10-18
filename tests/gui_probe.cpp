/* ============================================================================
 * tests/gui_probe.cpp —— GUI 端到端探针（第二轮修正版）
 *
 * 教训（都踩过）：
 *   1. GetWindowTextW **跨进程读不到**文本，必须 SendMessageTimeout + WM_GETTEXT
 *   2. 无 BOM 的 UTF-8 源文件会被 MinGW 按 GBK 解析，宽字面量变乱码 ——
 *      编译时必须加 -finput-charset=UTF-8
 *   3. 控制台默认 GBK，中文打不出来 —— 要 SetConsoleOutputCP(65001)
 *   4. stdout 重定向时是全缓冲，中途崩溃就丢日志 —— 要 setvbuf(_IONBF)
 *
 * 用法: gui_probe.exe <A路径> <B路径> <输出目录> [输出文件名]
 * ==========================================================================*/

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>

#define ID_A       1001
#define ID_B       1003
#define ID_NAME    1009
#define ID_OUTDIR  1011
#define ID_BUILD   1012
#define ID_QUIT    1013
#define ID_LOG     1015      /* 「生成日志」多行框，如果存在 */

static HWND g_main = NULL;

/* 控制台按 UTF-8 直写。之前用 printf("%ls") 打中文全是空白 ——
   控制台代码页和宽字符转换在重定向场景下不可靠，直接自己转 UTF-8 最稳。 */
static void PutsU8(const wchar_t* w)
{
    if (!w) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 1) return;
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
    fwrite(s.data(), 1, s.size(), stdout);
}

/* 打印码点，用来确认中文到底解出来没有（绕过控制台渲染） */
static void DumpCodes(const wchar_t* w)
{
    printf("[U+");
    for (const wchar_t* p = w; *p; ++p) printf("%04X ", (unsigned)*p);
    printf("]");
}

/* 跨进程读文本（GetWindowTextW 不行） */
static void GetTextX(HWND h, wchar_t* out, int cap)
{
    out[0] = 0;
    if (!h) return;
    DWORD_PTR res = 0;
    if (SendMessageTimeoutW(h, WM_GETTEXT, (WPARAM)cap, (LPARAM)out,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &res) == 0)
        out[0] = 0;
}

static BOOL CALLBACK EnumMain(HWND h, LPARAM)
{
    wchar_t cls[128] = {0};
    GetClassNameW(h, cls, 128);
    if (!wcsstr(cls, L"WuSiPackerWnd")) return TRUE;
    g_main = h;
    return FALSE;
}

struct WinScan {
    DWORD pid;
    HWND  progress;
    HWND  tip;
    int   count;
};

static BOOL CALLBACK ScanProc(HWND h, LPARAM lp)
{
    WinScan* s = (WinScan*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != s->pid) return TRUE;
    ++s->count;

    wchar_t t[256] = {0};
    GetTextX(h, t, 256);
    if (!s->progress && wcsstr(t, L"正在生成")) s->progress = h;
    if (!s->tip && (wcsstr(t, L"生成成功") || wcsstr(t, L"生成失败") ||
                    wcsstr(t, L"无法生成") || wcsstr(t, L"错误"))) s->tip = h;
    return TRUE;
}

static bool Alive(HANDLE proc, DWORD* exitCode)
{
    if (WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) {
        if (exitCode) GetExitCodeProcess(proc, exitCode);
        return false;
    }
    return true;
}

/* 读多行 EDIT 的全部内容（WM_GETTEXT 一次拿全，需要按字符数分配） */
static void DumpLogBox(HWND h)
{
    if (!h) { printf("  (没有日志框)\n"); return; }
    int n = (int)SendMessageTimeoutW(h, WM_GETTEXTLENGTH, 0, 0,
                                     SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, NULL);
    if (n <= 0) { printf("  (日志框是空的)\n"); return; }
    std::vector<wchar_t> buf(n + 2, 0);
    GetTextX(h, &buf[0], n + 1);
    printf("  ---- 生成日志框内容 ----\n");
    /* 逐行打，避免一坨 */
    std::wstring all(&buf[0]);
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t e = all.find(L'\r', pos);
        std::wstring line = (e == std::wstring::npos) ? all.substr(pos) : all.substr(pos, e - pos);
        if (!line.empty()) { printf("    | "); PutsU8(line.c_str()); printf("\n"); }
        if (e == std::wstring::npos) break;
        pos = e + 1;
        if (pos < all.size() && all[pos] == L'\n') ++pos;
    }
    printf("  ------------------------\n");
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    SetConsoleOutputCP(65001);

    if (argc < 4) { printf("用法: gui_probe.exe <A> <B> <输出目录> [输出文件名]\n"); return 2; }

    { typedef BOOL (WINAPI *F)(void);
      HMODULE u = GetModuleHandleW(L"user32.dll");
      F f = u ? (F)(void*)GetProcAddress(u, "SetProcessDPIAware") : NULL;
      if (f) f(); }

    std::wstring cmd = L"\"WuSi_Attacher.exe\"";
    std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(NULL, &c[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("启动打包器失败 (err=%lu)\n", GetLastError());
        return 1;
    }
    printf("打包器 pid=%lu\n", (unsigned long)pi.dwProcessId);

    HWND hA = NULL, hB = NULL, hName = NULL, hOut = NULL, hBtn = NULL;
    for (int i = 0; i < 80; ++i) {
        if (!g_main) EnumWindows(EnumMain, 0);
        if (g_main) {
            hA    = GetDlgItem(g_main, ID_A);
            hB    = GetDlgItem(g_main, ID_B);
            hName = GetDlgItem(g_main, ID_NAME);
            hOut  = GetDlgItem(g_main, ID_OUTDIR);
            hBtn  = GetDlgItem(g_main, ID_BUILD);
            if (hA && hB && hOut && hBtn) break;
        }
        DWORD ec = 0;
        if (!Alive(pi.hProcess, &ec)) {
            printf("!! 打包器在窗口就绪前就退出了，退出码=0x%08lX\n", (unsigned long)ec);
            return 1;
        }
        Sleep(100);
    }
    if (!hA || !hB || !hOut || !hBtn) { printf("!! 控件没找齐\n"); return 1; }
    printf("主窗口=%p A=%p B=%p 名称=%p 输出=%p 生成=%p 日志=%p\n",
           (void*)g_main, (void*)hA, (void*)hB, (void*)hName, (void*)hOut,
           (void*)hBtn, (void*)GetDlgItem(g_main, ID_LOG));

    wchar_t wa[512], wb[512], wd[512];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wa, 512);
    MultiByteToWideChar(CP_ACP, 0, argv[2], -1, wb, 512);
    MultiByteToWideChar(CP_ACP, 0, argv[3], -1, wd, 512);
    SetWindowTextW(hA, wa);
    SetWindowTextW(hB, wb);
    SetWindowTextW(hOut, wd);
    if (argc >= 5) { wchar_t wn[260]; MultiByteToWideChar(CP_ACP, 0, argv[4], -1, wn, 260);
                     SetWindowTextW(hName, wn); }

    wchar_t chk[512] = {0};
    GetTextX(hA, chk, 512);   printf("A    = %ls\n", chk);
    GetTextX(hB, chk, 512);   printf("B    = %ls\n", chk);
    GetTextX(hOut, chk, 512); printf("输出 = %ls\n", chk);

    Sleep(500);

    /* 点击前再回读一次：确认参数没有被重新载入覆盖掉 */
    GetTextX(hOut, chk, 512); printf("点击前 输出 = "); PutsU8(chk); printf("\n");
    GetTextX(hName, chk, 512); printf("点击前 名称 = "); PutsU8(chk); printf("\n");

    printf("点击「生成」...\n");
    RECT r; GetClientRect(hBtn, &r);
    LPARAM lp = MAKELPARAM((r.right - r.left) / 2, (r.bottom - r.top) / 2);
    PostMessageW(hBtn, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    PostMessageW(hBtn, WM_LBUTTONUP, 0, lp);

    int notResp = 0, sawProgress = 0, died = 0;
    DWORD exitCode = 0;
    HWND progHwnd = NULL, tipHwnd = NULL;
    DWORD t0 = GetTickCount();

    for (int t = 0; t < 1200; ++t) {   /* 最多 ~120 秒 */
        WinScan scan; ZeroMemory(&scan, sizeof(scan));
        scan.pid = pi.dwProcessId;
        EnumWindows(ScanProc, (LPARAM)&scan);

        if (!progHwnd && scan.progress) { progHwnd = scan.progress; sawProgress = 1; }
        if (!tipHwnd && scan.tip) tipHwnd = scan.tip;

        DWORD_PTR res = 0;
        LRESULT lr = SendMessageTimeoutW(g_main, WM_NULL, 0, 0,
                                         SMTO_ABORTIFHUNG | SMTO_BLOCK, 150, &res);
        if (lr == 0) ++notResp;

        if (tipHwnd) {
            wchar_t tt[128] = {0};
            GetTextX(tipHwnd, tt, 128);
            printf("\n完成提示框 (t=%.1fs): ", (GetTickCount() - t0) / 1000.0);
            PutsU8(tt); DumpCodes(tt); printf("\n");

            /* MessageBox 正文控件 id 是 0xFFFF；兜底取最长文本 */
            wchar_t body[2048] = {0};
            GetTextX(GetDlgItem(tipHwnd, 0xFFFF), body, 2048);
            if (!body[0]) {
                HWND best = NULL; int bestN = 0;
                for (HWND c = GetWindow(tipHwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
                    wchar_t cb[2048] = {0};
                    GetTextX(c, cb, 2048);
                    int n = (int)wcslen(cb);
                    if (n > bestN) { bestN = n; best = c; }
                }
                GetTextX(best, body, 2048);
            }
            printf("提示框正文: ");
            if (body[0]) PutsU8(body); else printf("(空)");
            printf("\n");
            DumpLogBox(GetDlgItem(g_main, ID_LOG));

            HWND ok = GetDlgItem(tipHwnd, IDOK);
            if (ok) PostMessageW(ok, BM_CLICK, 0, 0);
            else    PostMessageW(tipHwnd, WM_CLOSE, 0, 0);
            Sleep(400);
            break;
        }

        if (t < 40 && (t % 10) == 0)
            printf("  t=%4.1fs 窗口=%d 进度框=%s\n", t / 10.0, scan.count,
                   scan.progress ? "有" : "无");

        if (!Alive(pi.hProcess, &exitCode)) { died = 1; break; }
        Sleep(100);
    }

    DWORD elapsed = GetTickCount() - t0;
    printf("\n构建观测 %.1f 秒：无响应 %d 次，进度框=%s\n",
           elapsed / 1000.0, notResp, sawProgress ? "有" : "无");
    if (died) {
        printf("!!! 打包器进程中途退出，退出码=0x%08lX%s\n", (unsigned long)exitCode,
               (exitCode == 0xC0000005UL) ? "  <== 访问冲突 0xc0000005（那个崩溃）" : "");
    } else {
        printf("打包器进程全程存活 ✔\n");
        DumpLogBox(GetDlgItem(g_main, ID_LOG));   /* 没弹框也要看日志 */
    }
    return 0;
}
