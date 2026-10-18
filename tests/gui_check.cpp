// tests/gui_check.cpp —— 枚举 WuSi_Attacher 主窗口的所有子控件，检查是否有重叠
// 只用于开发期布局检查。
#include <windows.h>
#include <stdio.h>
#include <vector>
#include <string>

struct Rect { HWND h; std::wstring cls; std::wstring text; RECT r; };

static std::vector<Rect> g_items;
static HWND g_main = NULL;

static BOOL CALLBACK EnumChildProc(HWND h, LPARAM)
{
    Rect it;
    it.h = h;
    wchar_t buf[512];
    GetClassNameW(h, buf, 512);
    it.cls = buf;
    GetWindowTextW(h, buf, 512);
    it.text = buf;
    GetWindowRect(h, &it.r);
    g_items.push_back(it);
    return TRUE;
}

static BOOL CALLBACK EnumTopProc(HWND h, LPARAM)
{
    wchar_t buf[512];
    GetWindowTextW(h, buf, 512);
    std::wstring t = buf;
    if (t.find(L"WuSi Attacher") != std::wstring::npos) { g_main = h; return FALSE; }
    return TRUE;
}

int main(void)
{
    /* 启动打包器：优先用当前工作目录，否则按 tests\out 相对位置回退 */
    std::wstring exe = L"WuSi_Attacher.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        std::wstring dir(self);
        size_t p = dir.find_last_of(L"\\/");
        dir = dir.substr(0, p);
        p = dir.find_last_of(L"\\/");
        dir = dir.substr(0, p);
        exe = dir + L"\\WuSi_Attacher.exe";
    }
    std::wstring workDir = exe;
    {
        size_t q = workDir.find_last_of(L"\\/");
        workDir = (q == std::wstring::npos) ? L"." : workDir.substr(0, q);
    }

    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    std::wstring cmd = L"\"" + exe + L"\"";
    std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);

    printf("启动: %ls\n", exe.c_str());
    if (!CreateProcessW(exe.c_str(), &c[0], NULL, NULL, FALSE, 0, NULL, workDir.c_str(), &si, &pi)) {
        printf("启动失败 err=%lu\n", GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    for (int i = 0; i < 40 && !g_main; ++i) {
        EnumWindows(EnumTopProc, 0);
        if (!g_main) Sleep(250);
    }
    if (!g_main) { printf("没找到主窗口\n"); return 1; }

    RECT mr;
    GetWindowRect(g_main, &mr);
    RECT cr;
    GetClientRect(g_main, &cr);
    printf("主窗口 外框=%dx%d 客户区=%dx%d\n",
           mr.right - mr.left, mr.bottom - mr.top, cr.right, cr.bottom);
    printf("是否可见=%d\n\n", IsWindowVisible(g_main) ? 1 : 0);

    EnumChildWindows(g_main, EnumChildProc, 0);
    printf("控件数 = %zu\n\n", g_items.size());

    /* 转成客户区坐标打印 */
    POINT origin = { mr.left, mr.top };
    printf("%-4s %-14s %6s %6s %6s %6s  %s\n", "#", "class", "x", "y", "w", "h", "text");
    for (size_t i = 0; i < g_items.size(); ++i) {
        RECT r = g_items[i].r;
        int x = r.left - mr.left, y = r.top - mr.top;
        int w = r.right - r.left, h = r.bottom - r.top;
        std::wstring t = g_items[i].text;
        if (t.size() > 44) t = t.substr(0, 44) + L"…";
        printf("%-4zu %-14ls %6d %6d %6d %6d  %ls\n", i + 1, g_items[i].cls.c_str(), x, y, w, h, t.c_str());
    }

    /* 找重叠。
       注意要排除两类"合法的包含关系"：
         - Static：标签、分组框标题
         - BS_GROUPBOX：分组框本身就是容器，它包住子控件是设计意图
       否则会把所有分组框都报成重叠。 */
    printf("\n---- 重叠检查 ----\n");
    int overlaps = 0;
    for (size_t i = 0; i < g_items.size(); ++i) {
        if (g_items[i].cls == L"Static") continue;
        LONG sti = GetWindowLongW(g_items[i].h, GWL_STYLE);
        if ((sti & BS_TYPEMASK) == BS_GROUPBOX) continue;
        for (size_t j = i + 1; j < g_items.size(); ++j) {
            if (g_items[j].cls == L"Static") continue;
            LONG stj = GetWindowLongW(g_items[j].h, GWL_STYLE);
            if ((stj & BS_TYPEMASK) == BS_GROUPBOX) continue;
            RECT a = g_items[i].r, b = g_items[j].r;
            RECT in;
            if (IntersectRect(&in, &a, &b)) {
                printf("重叠: [%zu]%ls 与 [%zu]%ls\n", i + 1,
                       g_items[i].text.substr(0, 24).c_str(), j + 1,
                       g_items[j].text.substr(0, 24).c_str());
                ++overlaps;
            }
        }
    }
    if (!overlaps) printf("没有控件重叠\n");

    /* 找超出客户区的控件 */
    printf("\n---- 越界检查 ----\n");
    int outside = 0;
    for (size_t i = 0; i < g_items.size(); ++i) {
        RECT r = g_items[i].r;
        int x = r.left - mr.left, y = r.top - mr.top;
        int w = r.right - r.left, h = r.bottom - r.top;
        if (x < 0 || y < 0 || x + w > cr.right || y + h > cr.bottom) {
            printf("越界: [%zu]%ls  x=%d y=%d w=%d h=%d (客户区 %dx%d)\n",
                   i + 1, g_items[i].text.substr(0, 24).c_str(), x, y, w, h, cr.right, cr.bottom);
            ++outside;
        }
    }
    if (!outside) printf("没有控件越界\n");

    TerminateProcess(OpenProcess(PROCESS_TERMINATE, FALSE, pi.dwProcessId), 0);
    return (overlaps || outside) ? 2 : 0;
}
