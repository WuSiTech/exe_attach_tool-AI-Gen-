// tests/gui_input.cpp —— 逐个验证控件的交互路径
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

/* 用屏幕坐标真实点击：算出控件中心 -> 转屏幕 -> 发 WM_LBUTTONDOWN/UP 到那个控件 */
static void ClickChild(int id, const char* label)
{
    HWND c = GetDlgItem(g_main, id);
    if (!c) { printf("  [%s] 控件 %d 不存在\n", label, id); return; }
    RECT r; GetClientRect(c, &r);
    POINT pt = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
    LPARAM lp = MAKELPARAM(pt.x, pt.y);
    printf("  [%s] 控件 %d 存活=%d 可见=%d 可用=%d 样式=0x%08X\n",
           label, id, IsWindow(c) ? 1 : 0, IsWindowVisible(c) ? 1 : 0,
           IsWindowEnabled(c) ? 1 : 0, (unsigned)GetWindowLongW(c, GWL_STYLE));
    SendMessageW(c, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    SendMessageW(c, WM_LBUTTONUP, 0, lp);
    Sleep(180);
}

int main(void)
{
    /* 必须和打包器一样 DPI 感知，否则算出来的屏幕坐标会差一个缩放比 */
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

    printf("=== 主窗口信息 ===\n");
    printf("  存活=%d 可见=%d 可用=%d\n", IsWindow(g_main) ? 1 : 0,
           IsWindowVisible(g_main) ? 1 : 0, IsWindowEnabled(g_main) ? 1 : 0);

    HWND st = GetDlgItem(g_main, 1014);

    printf("\n=== 1. 直接给按钮发 BM_CLICK ===\n");
    /* 不点退出，避免窗口被关掉 */
    printf("\n=== 2. 给「浏览」按钮(1002=A的浏览)发 BM_CLICK —— 会弹文件对话框，跳过 ===\n");
    printf("  （跳过，避免弹模态框卡住）\n");

    printf("\n=== 3. 跳过「生成」按钮（会弹模态框卡住测试） ===\n");



    printf("\n=== 5. 点击复选框(1005) 看是否切换（走父窗口 WM_LBUTTONDOWN） ===\n");
    {
        HWND c2 = GetDlgItem(g_main, 1005);
        if (!c2) { printf("  控件 1005 不存在\n"); }
        else {
            RECT r; GetClientRect(c2, &r);
            POINT cp = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
            ClientToScreen(c2, &cp);
            ScreenToClient(g_main, &cp);
            printf("  控件客户区中心=%ld,%ld -> 主窗口客户区=%ld,%ld\n",
                   (r.right - r.left) / 2, (r.bottom - r.top) / 2, cp.x, cp.y);
            printf("  ChildWindowFromPointEx 返回的控件 ID = %d (期望 1005)\n",
                   GetDlgCtrlID(ChildWindowFromPointEx(g_main, cp,
                       CWP_SKIPINVISIBLE | CWP_SKIPDISABLED)));
            ULONG_PTR before = GetWindowLongPtrW(c2, GWLP_USERDATA);

            /* 方式 A：把消息直接发给主窗口（模拟父窗口收到点击） */
            SendMessageW(g_main, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cp.x, cp.y));
            Sleep(200);
            ULONG_PTR afterA = GetWindowLongPtrW(c2, GWLP_USERDATA);
            printf("  A) 发 WM_LBUTTONDOWN 给主窗口: 0x%llX -> 0x%llX  %s\n",
                   (unsigned long long)before, (unsigned long long)afterA,
                   (before != afterA) ? "切换 ✔" : "没切换 ✘");

            /* 方式 B：真实鼠标（用户实际走的就是这条） */
            POINT scr = cp;
            ClientToScreen(g_main, &scr);
            SetCursorPos(scr.x, scr.y);
            Sleep(80);
            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            Sleep(40);
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            Sleep(250);
            ULONG_PTR afterB = GetWindowLongPtrW(c2, GWLP_USERDATA);
            printf("  B) 真实鼠标点击: 0x%llX -> 0x%llX  %s\n",
                   (unsigned long long)afterA, (unsigned long long)afterB,
                   (afterA != afterB) ? "切换 ✔" : "没切换 ✘");

            /* 方式 C：逐个控件验证"点自己身上能命中自己" */
            printf("  --- 所有控件的命中测试（WindowFromPoint 是否等于自己）---\n");
            {
                int ids[] = { 1001,1002,1003,1004,1005,1006,1007,1008,
                              1009,1010,1011,1012,1013,1017,1018,
                              1019,1020,1021,1022,1023,1024 };
                const char* names[] = { "A路径","A浏览","B路径","B浏览","管理员","无窗口",
                                        "清理","日志","输出名","改目录","目录","生成",
                                        "退出","图标","图标浏览","公司名","说明","版权",
                                        "产品名","文件版本","产品版本" };
                int ok = 0, total = 0;
                for (int k = 0; k < 21; ++k) {
                    HWND cc = GetDlgItem(g_main, ids[k]);
                    if (!cc) continue;
                    ++total;
                    RECT rr; GetClientRect(cc, &rr);
                    POINT pp = { (rr.right-rr.left)/2, (rr.bottom-rr.top)/2 };
                    ClientToScreen(cc, &pp);
                    /* 不能用 WindowFromPoint：那受 z-order 影响，
                       打包器窗口被浏览器挡住时会返回浏览器的窗口。
                       用 ChildWindowFromPointEx 在父窗口内部做命中测试。 */
                    POINT cp = pp;
                    ScreenToClient(g_main, &cp);
                    HWND hit = ChildWindowFromPointEx(g_main, cp,
                                CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
                    BOOL same = (hit == cc);
                    if (same) ++ok;
                    else printf("    ✘ %-8s 命中到 id=%d (期望 %d)\n",
                                names[k], hit ? GetDlgCtrlID(hit) : -1, ids[k]);
                }
                printf("  命中通过 %d/%d\n", ok, total);
            }

            /* 方式 D：鼠标落在「生成」按钮上，看按钮是否收到按下 */
            HWND bb = GetDlgItem(g_main, 1012);
            if (bb) {
                RECT br; GetClientRect(bb, &br);
                POINT bp = { (br.right-br.left)/2, (br.bottom-br.top)/2 };
                ClientToScreen(bb, &bp);
                printf("  生成按钮屏幕坐标=%ld,%ld 该点窗口=%p(按钮=%p) 同否=%d\n",
                       bp.x, bp.y, (void*)WindowFromPoint(bp), (void*)bb,
                       WindowFromPoint(bp) == bb ? 1 : 0);
            }
        }
    }

    printf("\n=== 6. 最后用 BM_CLICK 点「退出」，验证 WM_COMMAND 通路 ===\n");
    {
        HWND c2 = GetDlgItem(g_main, 1013);
        printf("  点击前窗口存活=%d\n", IsWindow(g_main) ? 1 : 0);
        if (c2) { SendMessageW(c2, BM_CLICK, 0, 0); Sleep(300); }
        printf("  点击后窗口存活=%d  (0 = WM_COMMAND 正常处理了退出)\n", IsWindow(g_main) ? 1 : 0);
    }

    TerminateProcess(OpenProcess(PROCESS_TERMINATE, FALSE, pi.dwProcessId), 0);
    return 0;
}
