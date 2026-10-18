/* ============================================================================
 * packer.cpp —— 打包器（GUI）
 *
 * 用途：选定 A.exe、B.exe 和若干选项，生成一个内嵌两者的独立 exe。
 *
 * 工作方式：
 *   - 躯干模板 stub.exe 由 build.ps1 追加在打包器文件尾部，
 *     用 STUB_TEMPLATE_MAGIC 定位（不用资源段，原因见 LoadStubTemplate）
 *   - 点「生成」时：读模板 -> 原样写出 -> 回填 StubConfig -> 写 magic
 *                   -> 追加 B 字节 -> 追加 A 字节
 *   - 不做 PE 结构手术，改配置也**不需要**重新编译躯干
 *
 * 生成的 exe 由 asInvoker 清单启动，「是否需要管理员」是运行时自提权开关，
 * 这样同一个躯干能同时支持需要/不需要管理员两种场景。
 * ==========================================================================*/
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <objbase.h>
#include <windowsx.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <map>
#include <commctrl.h>
#include <tlhelp32.h>
#include "progress.h"
#include "shared_config.h"
/* ------------------------------------------------------------------------- */
/* 控件 ID                                                                    */
/* ------------------------------------------------------------------------- */
#define IDC_EDIT_A          1001
#define IDC_BTN_A           1002
#define IDC_EDIT_B          1003
#define IDC_BTN_B           1004
#define IDC_CHK_ADMIN       1005
#define IDC_CHK_NO_WINDOW   1006
#define IDC_CHK_CLEANUP     1007
#define IDC_CHK_LOG         1008
#define IDC_EDIT_NAME       1009
#define IDC_BTN_OUTDIR      1010
#define IDC_EDIT_OUTDIR     1011
#define IDC_BTN_BUILD       1012
#define IDC_BTN_QUIT        1013
#define IDC_STATUS          1014
#define IDC_LOG             1015
#define IDC_CHK_SELFDEL     1016
#define IDC_CHK_EXTRACT_A   1027
/* 图标 + 版本属性 */
#define IDC_EDIT_ICON       1017
#define IDC_BTN_ICON        1018
#define IDC_EDIT_COMPANY    1019
#define IDC_EDIT_DESC       1020
#define IDC_EDIT_COPYRIGHT  1021
#define IDC_EDIT_PRODUCT    1022
#define IDC_EDIT_FILEVER    1023
#define IDC_EDIT_PRODVER    1024
/* 分隔线由父窗口 WM_PAINT 画，不需要分组框控件（见 AddSeparator） */
#define IDC_GRP_OPTIONS     1025
#define IDC_GRP_INFO        1026
#define IDC_CMB_DEST        1028
#define IDC_ST_DEST         1029
/* ------------------------------------------------------------------------- */
/* 全局                                                                       */
/* ------------------------------------------------------------------------- */
static HINSTANCE g_hInst = NULL;
static HWND      g_hMain = NULL;
static HFONT     g_hFont = NULL;
static HFONT     g_hFontBold = NULL;
static std::wstring g_exeDir;
static std::wstring g_outDir;
static int g_clientWidth  = 700;
static int g_clientHeight = 800;
static int g_fontHeight   = 16;   /* 由实际字体算出，用于让布局跟随 DPI */
/* ------------------------------------------------------------------------- */
/* 小工具                                                                     */
/* ------------------------------------------------------------------------- */
static void SetStatus(const wchar_t* text, bool error = false)
{
    HWND h = GetDlgItem(g_hMain, IDC_STATUS);
    if (h) {
        SetWindowTextW(h, text);
        InvalidateRect(h, NULL, TRUE);
    }
    if (error) MessageBeep(MB_ICONWARNING);
}
static void LogToBox(const std::wstring& text)
{
    HWND lg = GetDlgItem(g_hMain, IDC_LOG);
    if (!lg) return;
    int len = GetWindowTextLengthW(lg);
    SendMessageW(lg, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::wstring line = text + L"\r\n";
    SendMessageW(lg, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    SendMessageW(lg, EM_LINESCROLL, 0, 1);
}
static std::wstring GetText(HWND hDlg, int id)
{
    HWND h = GetDlgItem(hDlg, id);
    if (!h) return std::wstring();
    int len = GetWindowTextLengthW(h);
    std::vector<wchar_t> buf(len + 2);
    GetWindowTextW(h, &buf[0], len + 1);
    return std::wstring(&buf[0]);
}
static void SetText(HWND hDlg, int id, const std::wstring& s)
{
    SetWindowTextW(GetDlgItem(hDlg, id), s.c_str());
}
/* 前置声明：下面这几个工具互相引用，先声明再定义，省得来回搬代码 */
static void SetCtlState(HWND h, DWORD bits, bool on);
static DWORD GetCtlState(HWND h);
static void AddSeparator(int x, int y, int w);
static LRESULT CALLBACK CheckboxProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static bool GetChecked(HWND hDlg, int id);
static void UpdateDestControls(HWND hDlg);
#define CSTAT_HOT     0x1
#define CSTAT_PRESSED 0x2
#define CSTAT_FOCUS   0x4
#define CSTAT_CHECKED 0x8
/* "解压到"下拉框只在勾选「解压并运行 A」时才有意义，其余时候隐藏 */
static void UpdateDestControls(HWND hDlg)
{
    bool on = GetChecked(hDlg, IDC_CHK_EXTRACT_A);
    ShowWindow(GetDlgItem(hDlg, IDC_CMB_DEST), on ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hDlg, IDC_ST_DEST),  on ? SW_SHOW : SW_HIDE);
}

/* 自绘之后按钮的类型位全是 BS_OWNERDRAW，靠控件 ID 区分复选框 */
static bool IsCheckboxId(int id)
{
    return id == IDC_CHK_ADMIN || id == IDC_CHK_NO_WINDOW || id == IDC_CHK_CLEANUP ||
           id == IDC_CHK_SELFDEL || id == IDC_CHK_LOG || id == IDC_CHK_EXTRACT_A;
}
/* 勾选状态自己维护（存在 GWLP_USERDATA 的 CSTAT_CHECKED 位）：
   BS_OWNERDRAW 一旦生效，系统不再维护勾选状态 ——
   BM_GETCHECK 永远返回 0，BM_SETCHECK 也无效。 */
static bool IsChecked(HWND h) { return (GetCtlState(h) & CSTAT_CHECKED) != 0; }
static void SetCheckedCtl(HWND h, bool on)
{
    SetCtlState(h, CSTAT_CHECKED, on);
    InvalidateRect(h, NULL, TRUE);
}
/* ---------------------------------------------------------------------------
   复选框的子类化窗口过程

   为什么必须子类化：复选框是**子控件**，鼠标按在它上面时系统把消息发给它
   自己，父窗口根本收不到 WM_LBUTTONDOWN。之前试图在父窗口里处理点击，
   结果"给父窗口发消息"的测试能过、**真实点击却毫无反应**。

   另外 BS_OWNERDRAW 生效后系统不再维护勾选状态，所以切换也要自己来。
   --------------------------------------------------------------------------- */
static std::map<HWND, WNDPROC> g_chkOrigProc;

static LRESULT CALLBACK CheckboxProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_LBUTTONDOWN:
        SetFocus(h);
        SetCtlState(h, CSTAT_PRESSED, true);
        InvalidateRect(h, NULL, TRUE);
        return 0;

    case WM_LBUTTONUP: {
        bool wasPressed = (GetCtlState(h) & CSTAT_PRESSED) != 0;
        SetCtlState(h, CSTAT_PRESSED, false);
        if (wasPressed) {
            SetCheckedCtl(h, !IsChecked(h));
            HWND parent = GetParent(h);
            if (parent)
                SendMessageW(parent, WM_COMMAND,
                             MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h);
        }
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (!(GetCtlState(h) & CSTAT_HOT)) {
            SetCtlState(h, CSTAT_HOT, true);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(h, NULL, TRUE);
        }
        return 0;

    case WM_MOUSELEAVE:
        SetCtlState(h, CSTAT_HOT, false);
        SetCtlState(h, CSTAT_PRESSED, false);
        InvalidateRect(h, NULL, TRUE);
        return 0;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        SetCtlState(h, CSTAT_FOCUS, msg == WM_SETFOCUS);
        InvalidateRect(h, NULL, TRUE);
        break;

    case WM_KEYDOWN:
        if (wp == VK_SPACE) {          /* 空格切换，符合标准复选框行为 */
            SetCheckedCtl(h, !IsChecked(h));
            HWND parent = GetParent(h);
            if (parent)
                SendMessageW(parent, WM_COMMAND,
                             MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h);
            return 0;
        }
        break;

    default:
        break;
    }

    std::map<HWND, WNDPROC>::const_iterator it = g_chkOrigProc.find(h);
    if (it != g_chkOrigProc.end() && it->second)
        return CallWindowProcW(it->second, h, msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}
static void SetCtlState(HWND h, DWORD bits, bool on)
{
    DWORD st = (DWORD)(ULONG_PTR)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (on) st |= bits; else st &= ~bits;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
}
static DWORD GetCtlState(HWND h)
{
    return (DWORD)(ULONG_PTR)GetWindowLongPtrW(h, GWLP_USERDATA);
}
/* 复选框是自绘的，系统不再维护勾选状态，所以走自己存的那份（见 IsChecked） */
static bool GetChecked(HWND hDlg, int id)
{
    HWND c = GetDlgItem(hDlg, id);
    return c ? IsChecked(c) : false;
}
static void SetChecked(HWND hDlg, int id, bool on)
{
    HWND c = GetDlgItem(hDlg, id);
    if (c) SetCheckedCtl(c, on);
}
static std::wstring DirNameOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return std::wstring();
    return path.substr(0, p);
}
static std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty()) return name;
    std::wstring r = dir;
    if (r[r.size() - 1] != L'\\' && r[r.size() - 1] != L'/') r += L'\\';
    r += name;
    return r;
}
static bool FileExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static unsigned long long FileSizeOf(const std::wstring& p)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}
/* ------------------------------------------------------------------------- */
/* 文件读写                                                                   */
/* ------------------------------------------------------------------------- */
static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
static bool WriteBlock(HANDLE h, const void* data, size_t size)
{
    const unsigned char* p = (const unsigned char*)data;
    size_t done = 0;
    while (done < size) {
        DWORD wr = 0;
        DWORD chunk = (size - done > 0x10000000) ? 0x10000000 : (DWORD)(size - done);
        if (!WriteFile(h, p + done, chunk, &wr, NULL) || wr == 0) return false;
        done += wr;
    }
    return true;
}
/* 把 src 文件的全部字节流式追加到 out */
static bool AppendFileTo(HANDLE out, const std::wstring& srcPath,
                         unsigned long long expectedSize, std::wstring& err)
{
    HANDLE in = CreateFileW(srcPath.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (in == INVALID_HANDLE_VALUE) {
        err = L"无法打开: " + srcPath;
        return false;
    }
    std::vector<unsigned char> buf(256 * 1024);
    unsigned long long remaining = expectedSize;
    bool ok = true;
    while (remaining > 0) {
        DWORD rd = 0;
        DWORD chunk = (remaining > buf.size()) ? (DWORD)buf.size() : (DWORD)remaining;
        if (!ReadFile(in, &buf[0], chunk, &rd, NULL) || rd == 0) {
            err = L"读取失败: " + srcPath;
            ok = false; break;
        }
        if (!WriteBlock(out, &buf[0], rd)) {
            err = L"写入失败（磁盘空间不足？）";
            ok = false; break;
        }
        remaining -= rd;
    }
    CloseHandle(in);
    return ok;
}
/* ------------------------------------------------------------------------- */
/* ICO 解析 —— 用于给生成的 exe 注入图标资源                                  */
/* ------------------------------------------------------------------------- */
/* 图标不是运行时装上去的：资源管理器里显示的文件图标、以及文件属性的图标，
   都只读 exe 自己的 PE 资源。所以必须在打包时把 RT_ICON / RT_GROUP_ICON /
   RT_VERSION 写进去（见 InjectResourcesToImage）。 */
struct IcoFrame {
    unsigned char w, h;          /* 原始宽高字节（0 表示 256） */
    unsigned char colors;
    unsigned short planes;
    unsigned short bitCount;
    unsigned int  size;          /* 图像数据字节数 */
    unsigned int  imgOff;        /* 在 ico 文件内的偏移 */
};
struct IcoFile {
    std::vector<IcoFrame> frames;
    std::vector<unsigned char> bytes;
};
static bool ParseIco(const std::wstring& path, IcoFile& out, std::wstring& err)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { err = L"无法打开图标文件"; return false; }
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart < 6 + 16) {
        CloseHandle(h); err = L"图标文件太小"; return false;
    }
    out.bytes.resize((size_t)li.QuadPart);
    DWORD rd = 0, total = 0;
    while (total < out.bytes.size()) {
        if (!ReadFile(h, &out.bytes[total], (DWORD)(out.bytes.size() - total), &rd, NULL) || rd == 0) {
            CloseHandle(h); err = L"读取图标文件失败"; return false;
        }
        total += rd;
    }
    CloseHandle(h);
    const unsigned char* b = &out.bytes[0];
    unsigned short reserved = *(const unsigned short*)(b + 0);
    unsigned short type     = *(const unsigned short*)(b + 2);
    unsigned short count    = *(const unsigned short*)(b + 4);
    if (reserved != 0 || type != 1) { err = L"不是有效的 .ico 文件"; return false; }
    if (count == 0 || count > 64)   { err = L"图标帧数异常"; return false; }
    if ((size_t)(6 + 16 * count) > out.bytes.size()) { err = L"图标目录越界"; return false; }
    for (int i = 0; i < count; ++i) {
        const unsigned char* e = b + 6 + i * 16;
        IcoFrame f;
        f.w        = e[0];
        f.h        = e[1];
        f.colors   = e[2];
        f.planes   = *(const unsigned short*)(e + 4);
        f.bitCount = *(const unsigned short*)(e + 6);
        f.size     = *(const unsigned int*)(e + 8);
        f.imgOff   = *(const unsigned int*)(e + 12);
        if (f.size == 0) continue;
        if ((unsigned long long)f.imgOff + f.size > out.bytes.size()) {
            err = L"图标帧数据越界"; return false;
        }
        out.frames.push_back(f);
    }
    if (out.frames.empty()) { err = L"图标里没有可用帧"; return false; }
    return true;
}
/* 把 ICO 里的帧组装成内存中的 GRPICONDIR（RT_GROUP_ICON 的原始数据） */
static void BuildGroupIcon(const std::vector<IcoFrame>& frames,
                           unsigned short firstId,
                           std::vector<unsigned char>& out)
{
    out.clear();
    const size_t n = frames.size();
    out.resize(6 + 14 * n, 0);
    unsigned char* p = &out[0];
    *(unsigned short*)(p + 0) = 0;                      /* reserved */
    *(unsigned short*)(p + 2) = 1;                      /* type = icon */
    *(unsigned short*)(p + 4) = (unsigned short)n;
    for (size_t i = 0; i < n; ++i) {
        unsigned char* e = p + 6 + i * 14;
        e[0] = frames[i].w;
        e[1] = frames[i].h;
        e[2] = frames[i].colors;
        e[3] = 0;
        *(unsigned short*)(e + 4)  = frames[i].planes;
        *(unsigned short*)(e + 6)  = frames[i].bitCount;
        *(unsigned int*)(e + 8)    = frames[i].size;
        *(unsigned short*)(e + 12) = (unsigned short)(firstId + i);
    }
}
/* ------------------------------------------------------------------------- */
/* 版本信息（RT_VERSION）构建                                                 */
/* ------------------------------------------------------------------------- */
static void PushAligned(std::vector<unsigned char>& v,
                        const void* data, size_t len, size_t align)
{
    while (v.size() % align) v.push_back(0);
    const unsigned char* p = (const unsigned char*)data;
    v.insert(v.end(), p, p + len);
}
static void PushW(std::vector<unsigned char>& v, const wchar_t* s)
{
    PushAligned(v, s, (wcslen(s) + 1) * sizeof(wchar_t), 4);
}
/* 单个 VS_VERSIONINFO 块。
   规范要求：有子块时 valueLength 必须为 0；没有子块时 valueLength 是值字节数。
   这里由调用方显式传入 valueLen，避免依赖对子块内容的猜测。 */
static void PushVerBlock(std::vector<unsigned char>& v,
                         const wchar_t* key,
                         unsigned short type,
                         unsigned short valueLen,
                         const std::vector<unsigned char>& children)
{
    while (v.size() % 4) v.push_back(0);
    const size_t base = v.size();
    unsigned char hdr[6] = {0, 0, 0, 0, 0, 0};
    v.insert(v.end(), hdr, hdr + 6);
    PushAligned(v, key, (wcslen(key) + 1) * sizeof(wchar_t), 4);
    if (!children.empty()) v.insert(v.end(), children.begin(), children.end());
    while (v.size() % 4) v.push_back(0);
    *(unsigned short*)(&v[base + 0]) = (unsigned short)(v.size() - base);
    *(unsigned short*)(&v[base + 2]) = valueLen;
    *(unsigned short*)(&v[base + 4]) = type;
}
static void PushStringEntry(std::vector<unsigned char>& v,
                            unsigned short langCp,
                            const wchar_t* key,
                            const wchar_t* value)
{
    while (v.size() % 4) v.push_back(0);
    const size_t base = v.size();
    unsigned char hdr[6] = {0, 0, 0, 0, 1, 0};   /* type = 1 (text) */
    v.insert(v.end(), hdr, hdr + 6);
    PushAligned(v, key, (wcslen(key) + 1) * sizeof(wchar_t), 4);
    PushW(v, value);
    while (v.size() % 4) v.push_back(0);
    *(unsigned short*)(&v[base + 0]) = (unsigned short)(v.size() - base);
}
struct VerStrings {
    const wchar_t* company;
    const wchar_t* description;
    const wchar_t* copyright;
    const wchar_t* product;
    const wchar_t* fileVersion;
    const wchar_t* productVersion;
    const wchar_t* originalName;
    const wchar_t* internalName;
};
/* 用 Windows 官方 API 写资源。版本块的二进制格式**不在本程序里手写**：
   改用资源编译器（windres）现场生成，格式由它保证。
   手写 VS_VERSIONINFO 我错了五轮（wLength/wValueLength 语义、键名后的对齐、
   固定部分必须在块起始偏移 4 处…），每次都能算出"看起来对"的字节，
   实际却被 VerQueryValue 拒收。交给 rc 工具是唯一可靠的做法。 */
static const int VER_STR_COUNT = 8;
/* 在 PATH 或自身同目录里找 windres */
static std::wstring FindWindres()
{
    wchar_t found[MAX_PATH];
    DWORD n = SearchPathW(NULL, L"windres.exe", NULL, MAX_PATH, found, NULL);
    if (n > 0 && n < MAX_PATH) return found;
    std::wstring local = JoinPath(g_exeDir, L"windres.exe");
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) return local;
    return std::wstring();
}
/* 执行命令并等待结束（输出丢弃），返回退出码；失败返回 -1 */
static int RunHidden(const std::wstring& cmdLine)
{
    std::vector<wchar_t> cmd(cmdLine.begin(), cmdLine.end());
    cmd.push_back(L'\0');
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
static bool WriteTextFile(const std::wstring& path, const std::string& text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &wr, NULL);
    CloseHandle(h);
    return ok && wr == text.size();
}
static std::string AcpFromWide(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
/* 把 RC 里需要转义的反斜杠和引号处理掉 */
static std::string RcEscape(const std::wstring& w)
{
    std::string s = AcpFromWide(w);
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\') o += "\\\\";
        else if (s[i] == '"') o += "\\\"";
        else o += s[i];
    }
    return o;
}
/* 生成版本资源：写临时 .rc -> windres 编成 .res -> 从 .res 里抽出 RT_VERSION 数据。
   .res 条目格式：DWORD DataSize, DWORD HeaderSize, 然后 HeaderSize 字节的头
   （头里 +8 是 2 字节资源类型，+10 是 2 字节资源名），数据紧跟其后。 */
static bool BuildVersionBlock(const wchar_t* keys[VER_STR_COUNT],
                              const wchar_t* vals[VER_STR_COUNT],
                              unsigned short langId,
                              unsigned short cp,
                              std::vector<unsigned char>& out,
                              std::wstring& err)
{
    std::wstring windres = FindWindres();
    if (windres.empty()) {
        err = L"找不到 windres.exe（版本信息需要资源编译器生成）。\n"
              L"请把 MinGW 的 bin 目录加入 PATH，或把 windres.exe 放到打包器同目录。";
        return false;
    }
    /* 版本号拆成 4 段 */
    unsigned int fv[4] = {0, 0, 0, 0};
    {
        const wchar_t* f = NULL;
        for (int i = 0; i < VER_STR_COUNT; ++i)
            if (wcscmp(keys[i], L"FileVersion") == 0) { f = vals[i]; break; }
        int idx = 0;
        while (f && *f && idx < 4) {
            unsigned int v = 0;
            while (*f >= L'0' && *f <= L'9') { v = v * 10 + (unsigned)(*f - L'0'); ++f; }
            fv[idx++] = v;
            if (*f == L'.') ++f; else break;
        }
    }
    char buf[64];
    std::string rc = "#include <winver.h>\r\n1 VERSIONINFO\r\n";
    wsprintfA(buf, "  FILEVERSION %u,%u,%u,%u\r\n", fv[0], fv[1], fv[2], fv[3]);
    rc += buf;
    wsprintfA(buf, "  PRODUCTVERSION %u,%u,%u,%u\r\n", fv[0], fv[1], fv[2], fv[3]);
    rc += buf;
    rc += "  FILEFLAGSMASK 0x3fL\r\n  FILEFLAGS 0x0L\r\n"
          "  FILEOS 0x40004L\r\n  FILETYPE 0x1L\r\n  FILESUBTYPE 0x0L\r\n"
          "BEGIN\r\n  BLOCK \"StringFileInfo\"\r\n  BEGIN\r\n";
    wsprintfA(buf, "    BLOCK \"%04X%04X\"\r\n", (unsigned)langId, (unsigned)cp);
    rc += buf;
    rc += "    BEGIN\r\n";
    for (int i = 0; i < VER_STR_COUNT; ++i) {
        rc += "      VALUE \"";
        rc += RcEscape(keys[i]);
        rc += "\", \"";
        rc += RcEscape(vals[i]);
        rc += "\"\r\n";
    }
    rc += "    END\r\n  END\r\n  BLOCK \"VarFileInfo\"\r\n  BEGIN\r\n";
    wsprintfA(buf, "    VALUE \"Translation\", 0x%X, %u\r\n", (unsigned)langId, (unsigned)cp);
    rc += buf;
    rc += "  END\r\nEND\r\n";
    wchar_t tmp[MAX_PATH];
    DWORD tn = GetTempPathW(MAX_PATH, tmp);
    std::wstring base = (tn > 0 && tn < MAX_PATH) ? std::wstring(tmp) : g_exeDir;
    wchar_t tag[64];
    wsprintfW(tag, L"wusi_ver_%lu", (unsigned long)GetCurrentProcessId());
    std::wstring rcPath  = JoinPath(base, std::wstring(tag) + L".rc");
    std::wstring resPath = JoinPath(base, std::wstring(tag) + L".res");
    if (!WriteTextFile(rcPath, rc)) { err = L"无法写临时 .rc 文件"; return false; }
    std::wstring cmd = L"\"" + windres + L"\" --input-format=rc --output-format=res -o \""
                     + resPath + L"\" \"" + rcPath + L"\"";
    printf("[ver] 调用: %s\n", WideToUtf8(cmd).c_str());
    int rc2 = RunHidden(cmd);
    printf("[ver] windres 退出码 = %d, .res 存在 = %d, 大小 = %llu\n",
           rc2, (int)(GetFileAttributesW(resPath.c_str()) != INVALID_FILE_ATTRIBUTES),
           FileSizeOf(resPath));
    DeleteFileW(rcPath.c_str());
    if (rc2 != 0) {
        DeleteFileW(resPath.c_str());
        wchar_t m[512];
        wsprintfW(m, L"windres 编译版本资源失败（退出码 %d）", rc2);
        err = m;
        return false;
    }
    /* 读 .res，抽出 RT_VERSION(16) 的数据 */
    HANDLE h = CreateFileW(resPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { err = L"打不开 windres 输出"; return false; }
    LARGE_INTEGER li;
    GetFileSizeEx(h, &li);
    std::vector<unsigned char> res((size_t)li.QuadPart);
    DWORD rd = 0, tot = 0;
    while (tot < res.size()) {
        if (!ReadFile(h, &res[tot], (DWORD)(res.size() - tot), &rd, NULL) || !rd) break;
        tot += rd;
    }
    CloseHandle(h);
    DeleteFileW(resPath.c_str());
    size_t i = 0;
    while (i + 8 <= res.size()) {
        DWORD dataSize = *(DWORD*)&res[i];
        DWORD hdrSize  = *(DWORD*)&res[i + 4];
        if (hdrSize < 8 || i + hdrSize > res.size()) break;
        /* .res 条目头：DWORD DataSize, DWORD HeaderSize, 然后
           2 字节保留、2 字节 Ordinal/Type、2 字节 Name、2 字节对齐。
           资源类型在 +10（不是 +8，+8 是保留字段）。 */
        WORD type = *(WORD*)&res[i + 10];
        printf("[ver] .res 条目 @%zu: dataSize=%u hdrSize=%u type=%u\n",
               i, (unsigned)dataSize, (unsigned)hdrSize, (unsigned)type);
        if (dataSize && type == 16 && i + hdrSize + dataSize <= res.size()) {
            out.assign(res.begin() + i + hdrSize,
                       res.begin() + i + hdrSize + dataSize);
            printf("[ver] windres 生成的 RT_VERSION = %u 字节\n", (unsigned)dataSize);
            return true;
        }
        i += hdrSize + dataSize;
        while (i % 4) ++i;
    }
    err = L"windres 输出里没有 RT_VERSION";
    return false;
}
/* 用 UpdateResource 把图标和版本信息写进已经落盘的生成产物。
   必须在文件完全写好之后调用（资源之外的内容不受影响）。 */
static bool InjectResourcesToFile(const std::wstring& path,
                                  const IcoFile& ico,
                                  bool wantIcon,
                                  const std::vector<unsigned char>& verRes,
                                  unsigned short langId,
                                  std::wstring& note)
{
    if (!wantIcon && verRes.empty()) return true;
    HANDLE hUpd = BeginUpdateResourceW(path.c_str(), FALSE);
    if (!hUpd) {
        wchar_t m[256];
        wsprintfW(m, L"BeginUpdateResource 失败 (err=%lu)", (unsigned long)GetLastError());
        note = m;
        return false;
    }
    bool anyOk = false;
    if (wantIcon && !ico.frames.empty()) {
        /* RT_ICON：每帧一个资源，ID 从 1 开始 */
        bool iconsOk = true;
        for (size_t i = 0; i < ico.frames.size(); ++i) {
            if (!UpdateResourceW(hUpd, RT_ICON,
                                 MAKEINTRESOURCEW(1 + (WORD)i), langId,
                                 (LPVOID)&ico.bytes[ico.frames[i].imgOff],
                                 ico.frames[i].size)) {
                iconsOk = false;
                break;
            }
        }
        /* RT_GROUP_ICON：把各帧串起来 */
        std::vector<unsigned char> grp;
        BuildGroupIcon(ico.frames, 1, grp);
        bool grpOk = iconsOk && !grp.empty() &&
                     UpdateResourceW(hUpd, RT_GROUP_ICON, MAKEINTRESOURCEW(1), langId,
                                     &grp[0], (DWORD)grp.size());
        if (grpOk) {
            anyOk = true;
        } else {
            wchar_t m[256];
            wsprintfW(m, L"写入图标资源失败 (err=%lu)", (unsigned long)GetLastError());
            note += m;
        }
    }
    if (!verRes.empty()) {
        if (UpdateResourceW(hUpd, RT_VERSION, MAKEINTRESOURCEW(1), langId,
                            (LPVOID)&verRes[0], (DWORD)verRes.size())) {
            anyOk = true;
        } else {
            wchar_t m[256];
            wsprintfW(m, L"写入版本资源失败 (err=%lu)", (unsigned long)GetLastError());
            note += m;
        }
    }
    if (!EndUpdateResourceW(hUpd, FALSE)) {
        wchar_t m[256];
        wsprintfW(m, L"EndUpdateResource 失败 (err=%lu)", (unsigned long)GetLastError());
        note += m;
        return false;
    }
    return anyOk;
}
/* ------------------------------------------------------------------------- */
/* 读取躯干模板                                                               */
/* ------------------------------------------------------------------------- */
/* 模板追加在打包器文件尾部：
      [ PE 镜像 ][ STUB_TEMPLATE_MAGIC ][ stub.exe 字节 ]
   不用 Windows 资源段，因为 MinGW 的 windres/FindResource 在 RCDATA 上不可靠
   （资源枚举能看到，FindResource 返回 1813）。 */
static bool LoadStubTemplate(std::vector<unsigned char>& out)
{
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    std::wstring selfPath(self, n);
    HANDLE h = CreateFileW(selfPath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("错误：无法打开自身读取躯干模板 (err=%lu)\n", GetLastError());
        return false;
    }
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0) { CloseHandle(h); return false; }
    unsigned long long fileSize = (unsigned long long)li.QuadPart;
    const size_t magicLen = sizeof(STUB_TEMPLATE_MAGIC) - 1;
    const size_t kChunk = 1u << 20;
    std::vector<unsigned char> buf(kChunk);
    unsigned long long windowStart = fileSize;
    unsigned long long magicAt = 0;
    bool found = false;
    while (!found && windowStart > 0) {
        unsigned long long newStart =
            (windowStart >= (unsigned long long)kChunk) ? (windowStart - kChunk) : 0;
        size_t len = (size_t)(windowStart - newStart);
        LARGE_INTEGER p; p.QuadPart = (LONGLONG)newStart;
        if (!SetFilePointerEx(h, p, NULL, FILE_BEGIN)) break;
        DWORD rd = 0;
        if (!ReadFile(h, &buf[0], (DWORD)len, &rd, NULL) || rd == 0) break;
        len = rd;
        if (len >= magicLen) {
            for (size_t i = len - magicLen + 1; i-- > 0; ) {
                if (memcmp(&buf[i], STUB_TEMPLATE_MAGIC, magicLen) == 0) {
                    magicAt = newStart + i;
                    found = true;
                    break;
                }
            }
        }
        windowStart = newStart;
    }
    if (!found) {
        CloseHandle(h);
        printf("错误：打包器尾部没有躯干模板。\n"
               "      构建不完整，请重新运行 build.ps1。\n");
        return false;
    }
    unsigned long long tplOff = magicAt + magicLen;
    if (tplOff >= fileSize) { CloseHandle(h); printf("错误：躯干模板长度为 0\n"); return false; }
    unsigned long long tplSize = fileSize - tplOff;
    LARGE_INTEGER p; p.QuadPart = (LONGLONG)tplOff;
    if (!SetFilePointerEx(h, p, NULL, FILE_BEGIN)) { CloseHandle(h); return false; }
    out.resize((size_t)tplSize);
    DWORD total = 0;
    while (total < tplSize) {
        DWORD rd = 0;
        if (!ReadFile(h, &out[total], (DWORD)(tplSize - total), &rd, NULL) || rd == 0) {
            CloseHandle(h); printf("错误：读取躯干模板失败\n"); return false;
        }
        total += rd;
    }
    CloseHandle(h);
    if (out.size() < 0x40 || out[0] != 'M' || out[1] != 'Z') {
        printf("错误：躯干模板不是有效的 PE 镜像\n");
        return false;
    }
    printf("[tpl] 躯干模板: %llu 字节, 尾部8字节=", tplSize);
    for (size_t i = out.size() - 8; i < out.size(); ++i) printf("%02X ", out[i]);
    printf("\n");
    return true;
}
/* ------------------------------------------------------------------------- */
/* 核心：生成                                                                 */
/* ------------------------------------------------------------------------- */
struct PackOptions {
    std::wstring aPath;
    std::wstring bPath;
    std::wstring outPath;
    std::wstring iconPath;          /* 空 = 不改图标 */
    std::wstring aRelName;          /* 不解压 A 时，相对躯干目录的文件名 */
    unsigned int destKind = STUB_DEST_EXE_DIR;  /* 解压目的地类别 */
    StubConfig   cfg;
    unsigned long long aSize;       /* 0 = 由本函数自己量 */
    unsigned long long bSize;
    /* 版本信息（写进生成程序的软件属性） */
    std::wstring company, description, copyright, product;
    std::wstring fileVersion, productVersion, originalName, internalName;
    bool wantVersion() const {
        return !fileVersion.empty() || !company.empty() || !description.empty() ||
               !copyright.empty() || !product.empty();
    }
    bool wantIcon() const { return !iconPath.empty(); }
};
static bool BuildPackage(const PackOptions& opt, std::wstring& err);

/* 后台构建用的上下文 */
/* 构建重入闸门。
   构建期间 ProgressPump 会派发**本线程所有**消息，也就是用户仍能点到
   「生成」按钮。第二次构建会和第一次并发写同一个输出文件 —— 轻则产物损坏，
   重则崩溃退出。这个标志就是那道闸门。 */
static bool g_building = false;

struct BuildJob {
    const PackOptions* opt;
    std::wstring*      err;
    bool*              ok;
    HWND               dlg;
};

static DWORD WINAPI BuildThreadProc(LPVOID param)
{
    BuildJob* j = (BuildJob*)param;
    ProgressSet(j->dlg, PSTAGE_PREPARE, 10);
    *j->ok = BuildPackage(*j->opt, *j->err);
    ProgressSet(j->dlg, *j->ok ? PSTAGE_DONE : PSTAGE_PREPARE, 100);
    return 0;
}

static bool BuildPackage(const PackOptions& opt, std::wstring& err)
{
    const std::wstring& aPath   = opt.aPath;
    const std::wstring& bPath   = opt.bPath;
    const std::wstring& outPath = opt.outPath;
    std::vector<unsigned char> img;
    if (!LoadStubTemplate(img)) { err = L"读取躯干模板失败（构建不完整）"; return false; }
    if (aPath == outPath || bPath == outPath) {
        err = L"输出文件不能和 A.exe / B.exe 是同一个文件";
        return false;
    }
    {
        wchar_t self[MAX_PATH];
        DWORD sn = GetModuleFileNameW(NULL, self, MAX_PATH);
        if (_wcsicmp(std::wstring(self, sn).c_str(), outPath.c_str()) == 0) {
            err = L"输出文件不能是打包器自己";
            return false;
        }
    }
    /* 不解压 A 时就不必把 A 的字节嵌进产物 —— 能省下 A 的体积。
       此时 cfg.aSize 记 0，躯干据此判断"去磁盘上找 A"。 */
    const bool embedA = (opt.cfg.flags & STUB_FLAG_EXTRACT_A) != 0;

    const unsigned long long bSize = opt.bSize ? opt.bSize : FileSizeOf(bPath);
    unsigned long long aSize = embedA ? (opt.aSize ? opt.aSize : FileSizeOf(aPath)) : 0;
    if (bSize == 0) { err = L"无法读取 B 的大小"; return false; }
    if (embedA && aSize == 0) { err = L"无法读取 A 的大小"; return false; }
    /* ---- 版本信息块 ---- */
    std::vector<unsigned char> meta;
    {
        unsigned int offs[META_COUNT];
        ZeroMemory(offs, sizeof(offs));
        const std::wstring* vals[META_COUNT] = {
            &opt.company, &opt.description, &opt.copyright, &opt.product,
            &opt.fileVersion, &opt.productVersion, &opt.originalName, &opt.internalName
        };
        bool any = false;
        for (int i = 0; i < META_COUNT; ++i) if (!vals[i]->empty()) any = true;
        if (any) {
            struct Fixed { unsigned int magic; unsigned int size; unsigned int count; unsigned int offs[META_COUNT]; };
            Fixed h;
            ZeroMemory(&h, sizeof(h));
            h.magic = 0x4154454Du;                  /* 'META' */
            h.size  = (unsigned int)sizeof(Fixed);
            h.count = META_COUNT;
            std::vector<unsigned char> body;
            for (int i = 0; i < META_COUNT; ++i) {
                offs[i] = (unsigned int)(sizeof(Fixed) + body.size());
                const std::wstring& s = *vals[i];
                const unsigned char* p = (const unsigned char*)s.c_str();
                body.insert(body.end(), p, p + (s.size() + 1) * sizeof(wchar_t));
            }
            memcpy(h.offs, offs, sizeof(offs));
            meta.resize(sizeof(Fixed));
            memcpy(&meta[0], &h, sizeof(Fixed));
            meta.insert(meta.end(), body.begin(), body.end());
        }
    }
    /* ---- 准备图标与版本数据（真正写盘在文件组装完成之后） ---- */
    const unsigned short kLangId = 1033;
    IcoFile ico;
    std::vector<unsigned char> verRes;
    bool wantIconRes = false, wantVerRes = false;
    std::wstring resNote;
    if (opt.wantIcon()) {
        std::wstring ierr;
        if (ParseIco(opt.iconPath, ico, ierr)) {
            wantIconRes = true;
        } else {
            resNote += L"图标未注入（" + ierr + L"）；";
        }
    }
    if (opt.wantVersion()) {
        std::wstring autoVer;
        std::wstring fv = opt.fileVersion;
        if (fv.empty()) fv = L"1.0.0.0";
        std::wstring pv = opt.productVersion.empty() ? fv : opt.productVersion;
        const wchar_t* keys[VER_STR_COUNT] = {
            L"CompanyName", L"FileDescription", L"FileVersion", L"InternalName",
            L"LegalCopyright", L"OriginalFilename", L"ProductName", L"ProductVersion"
        };
        const wchar_t* vals[VER_STR_COUNT] = {
            opt.company.c_str(),
            opt.description.c_str(),
            fv.c_str(),
            opt.internalName.c_str(),
            opt.copyright.c_str(),
            opt.originalName.c_str(),
            opt.product.c_str(),
            pv.c_str()
        };
        std::wstring verErr;
        BuildVersionBlock(keys, vals, kLangId, 1200, verRes, verErr);
        if (verRes.empty()) resNote += L"版本信息未写入（" + verErr + L"）；";
        wantVerRes = !verRes.empty();
    }
    const std::wstring& resNoteOut = resNote;
    /* ---- 阶段 1：先只写基底（躯干镜像）----
       为什么分两阶段：EndUpdateResource 会把"不属于任何区段"的尾部数据统统
       截掉（实测文件从 213980 被砍回 163328），所以 payload 和配置块必须等
       资源注入完成之后再追加。 */
    {
        HANDLE out = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, NULL,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) {
            err = L"无法创建输出文件（路径不可写或被占用？）";
            return false;
        }
        bool ok = WriteBlock(out, &img[0], img.size());
        if (ok) ok = FlushFileBuffers(out);
        CloseHandle(out);
        if (!ok) {
            DeleteFileW(outPath.c_str());
            err = L"写入基底失败";
            return false;
        }
    }
    /* ---- 阶段 2：资源注入（图标 + 版本属性）---- */
    if (wantIconRes || wantVerRes) {
        std::wstring note;
        if (InjectResourcesToFile(outPath, ico, wantIconRes, verRes, kLangId, note)) {
            if (wantIconRes) printf("[build] 图标已写入资源（%u 帧）\n", (unsigned)ico.frames.size());
            if (wantVerRes)  printf("[build] 版本信息已写入资源（%u 字节）\n", (unsigned)verRes.size());
        } else {
            printf("[build] 资源注入失败：%s\n", WideToUtf8(note).c_str());
        }
    }
    if (!resNoteOut.empty())
        printf("[build] 注意：%s\n", WideToUtf8(resNoteOut).c_str());
    /* 注入会改变文件大小，所以基底长度必须此刻重新量 */
    const unsigned long long baseSize = FileSizeOf(outPath);
    if (baseSize < 0x1000) {
        DeleteFileW(outPath.c_str());
        err = L"基底写入异常";
        return false;
    }
    /* ---- 阶段 3：追加 payload + 元数据 + 尾部配置块 ---- */
    StubTrailer tr;
    ZeroMemory(&tr, sizeof(tr));
    tr.config = opt.cfg;
    tr.config.bOffset = baseSize;
    tr.config.bSize   = bSize;
    tr.config.aOffset = baseSize + bSize;
    tr.config.aSize   = aSize;          /* 0 = 不解压 A，躯干去磁盘上找 */
    /* aName 是 UTF-8 字节数组，aRelName 是宽字符，需要转换 */
    ZeroMemory(tr.config.aName, sizeof(tr.config.aName));
    if (!opt.aRelName.empty()) {
        WideCharToMultiByte(CP_UTF8, 0, opt.aRelName.c_str(), (int)opt.aRelName.size(),
                            tr.config.aName, STUB_A_NAME_MAX - 1, NULL, NULL);
        tr.config.aName[STUB_A_NAME_MAX - 1] = 0;
    }
    tr.metaOffset     = meta.empty() ? 0 : (unsigned int)(baseSize + bSize + aSize);
    tr.metaSize       = (unsigned int)meta.size();
    tr.resInfoOffset  = 0;
    tr.resInfoSize    = 0;
    memcpy(tr.trailerMagic, STUB_TRAILER_MAGIC, STUB_TRAILER_MAGIC_LEN);
    {
        HANDLE out = CreateFileW(outPath.c_str(), FILE_APPEND_DATA, 0, NULL,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) {
            DeleteFileW(outPath.c_str());
            err = L"无法追加 payload";
            return false;
        }
        bool ok = true;
        if (ok) ok = AppendFileTo(out, bPath, bSize, err);
        if (ok && embedA) ok = AppendFileTo(out, aPath, aSize, err);
        if (ok && !meta.empty()) ok = WriteBlock(out, &meta[0], meta.size());
        if (ok) ok = WriteBlock(out, &tr, sizeof(tr));
        if (ok) FlushFileBuffers(out);
        CloseHandle(out);
        if (!ok) {
            DeleteFileW(outPath.c_str());
            if (err.empty()) err = L"写入输出文件失败";
            return false;
        }
    }
    unsigned long long expect = baseSize + bSize + aSize + meta.size()
                              + (unsigned long long)sizeof(tr);
    unsigned long long actual = FileSizeOf(outPath);
    if (actual != expect) {
        wchar_t buf[256];
        wsprintfW(buf, L"输出文件大小异常：期望 %llu，实际 %llu", expect, actual);
        err = buf;
        DeleteFileW(outPath.c_str());
        return false;
    }
    printf("[build] 基底=%llu B@%llu A@%llu(%s) meta=%u trailer@%llu 总长=%llu\n",
           baseSize, tr.config.bOffset, tr.config.aOffset,
           embedA ? "内嵌" : "不内嵌，直跑磁盘上的 A",
           tr.metaSize, expect - sizeof(tr), expect);
    return true;
}
/* ---------------------------------------------------------------------------
   设置持久化

   不用 WritePrivateProfileString / GetPrivateProfileString：
   那套 API 在内存里缓存整个 INI，配合"进程很快退出"的场景会丢写入
   （实测保存后文件时间戳都不变，勾选状态因此永远存不下来）。
   这里直接自己读写文件，行为完全可控。
   --------------------------------------------------------------------------- */
static std::wstring SettingsPath()
{
    /* 配置就放在打包器同目录，名字是 packer.ini。
       不用 %LOCALAPPDATA%：一来便携（拷走整个目录设置也跟着走），
       二来某些受限环境会拒绝写 AppData。 */
    return JoinPath(g_exeDir, L"packer.ini");
}

/* 把整个设置文件读成 "键=值" 的 map（UTF-8，忽略 [节] 行） */
static void ReadSettingsFile(std::map<std::wstring, std::wstring>& out)
{
    out.clear();
    std::wstring path = SettingsPath();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0 || li.QuadPart > 1 << 20) {
        CloseHandle(h); return;
    }
    std::string raw((size_t)li.QuadPart, '\0');
    DWORD rd = 0;
    ReadFile(h, &raw[0], (DWORD)raw.size(), &rd, NULL);
    CloseHandle(h);
    raw.resize(rd);

    int wn = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), NULL, 0);
    if (wn <= 0) return;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), &w[0], wn);

    size_t pos = 0;
    while (pos < w.size()) {
        size_t eol = w.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = w.size();
        std::wstring line = w.substr(pos, eol - pos);
        while (!line.empty() && (line[line.size() - 1] == L'\r' || line[line.size() - 1] == L' '))
            line.erase(line.size() - 1);
        if (!line.empty() && line[0] != L'[' && line[0] != L';') {
            size_t eq = line.find(L'=');
            if (eq != std::wstring::npos)
                out[line.substr(0, eq)] = line.substr(eq + 1);
        }
        pos = eol + 1;
    }
}

static std::wstring SettingGet(const std::map<std::wstring, std::wstring>& m,
                               const wchar_t* key, const wchar_t* def)
{
    std::map<std::wstring, std::wstring>::const_iterator it = m.find(key);
    return (it == m.end()) ? std::wstring(def) : it->second;
}

static bool SettingGetBool(const std::map<std::wstring, std::wstring>& m,
                           const wchar_t* key, bool def)
{
    std::map<std::wstring, std::wstring>::const_iterator it = m.find(key);
    if (it == m.end() || it->second.empty()) return def;
    return it->second[0] == L'1';
}

static bool WriteSettingsFile(const std::wstring& text)
{
    std::wstring path = SettingsPath();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                NULL, 0, NULL, NULL);
    std::string raw((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                   &raw[0], n, NULL, NULL);
    DWORD wr = 0;
    BOOL ok = WriteFile(h, raw.data(), (DWORD)raw.size(), &wr, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok && wr == raw.size();
}

static void SaveSettings(HWND hDlg)
{
    std::wstring t;
    t += L"[packer]\r\n";
    struct { const wchar_t* key; std::wstring val; } kv[] = {
        { L"A",          GetText(hDlg, IDC_EDIT_A) },
        { L"B",          GetText(hDlg, IDC_EDIT_B) },
        { L"Out",        GetText(hDlg, IDC_EDIT_OUTDIR) },
        { L"Name",       GetText(hDlg, IDC_EDIT_NAME) },
        { L"Icon",       GetText(hDlg, IDC_EDIT_ICON) },
        { L"Company",    GetText(hDlg, IDC_EDIT_COMPANY) },
        { L"Desc",       GetText(hDlg, IDC_EDIT_DESC) },
        { L"Copyright",  GetText(hDlg, IDC_EDIT_COPYRIGHT) },
        { L"Product",    GetText(hDlg, IDC_EDIT_PRODUCT) },
        { L"FileVer",    GetText(hDlg, IDC_EDIT_FILEVER) },
        { L"ProdVer",    GetText(hDlg, IDC_EDIT_PRODVER) },
    };
    for (int i = 0; i < 11; ++i) { t += kv[i].key; t += L"="; t += kv[i].val; t += L"\r\n"; }

    struct { int id; const wchar_t* key; } chk[] = {
        { IDC_CHK_ADMIN,     L"Admin"    },
        { IDC_CHK_NO_WINDOW, L"NoWindow" },
        { IDC_CHK_CLEANUP,   L"Cleanup"  },
        { IDC_CHK_SELFDEL,   L"SelfDel"  },
        { IDC_CHK_LOG,       L"Log"      },
        { IDC_CHK_EXTRACT_A, L"ExtractA" },
    };
    for (int i = 0; i < 6; ++i) {
        t += chk[i].key;
        t += (GetChecked(hDlg, chk[i].id) ? L"=1\r\n" : L"=0\r\n");
    }
    {
        wchar_t dv[16];
        wsprintfW(dv, L"%u", (unsigned)SendMessageW(GetDlgItem(hDlg, IDC_CMB_DEST), CB_GETCURSEL, 0, 0));
        t += L"DestKind="; t += dv; t += L"\r\n";
    }
    WriteSettingsFile(t);
}

static void LoadSettings(HWND hDlg)
{
    std::map<std::wstring, std::wstring> m;
    ReadSettingsFile(m);

    std::wstring a = SettingGet(m, L"A", L"");
    if (!a.empty()) SetText(hDlg, IDC_EDIT_A, a);
    std::wstring b = SettingGet(m, L"B", L"");
    if (!b.empty()) SetText(hDlg, IDC_EDIT_B, b);
    SetText(hDlg, IDC_EDIT_OUTDIR, SettingGet(m, L"Out", g_outDir.c_str()));
    SetText(hDlg, IDC_EDIT_NAME,   SettingGet(m, L"Name", L"WuSi_App.exe"));
    SetText(hDlg, IDC_EDIT_ICON,      SettingGet(m, L"Icon", L""));
    SetText(hDlg, IDC_EDIT_COMPANY,   SettingGet(m, L"Company", L""));
    SetText(hDlg, IDC_EDIT_DESC,      SettingGet(m, L"Desc", L""));
    SetText(hDlg, IDC_EDIT_COPYRIGHT, SettingGet(m, L"Copyright", L""));
    SetText(hDlg, IDC_EDIT_PRODUCT,   SettingGet(m, L"Product", L""));
    SetText(hDlg, IDC_EDIT_FILEVER,   SettingGet(m, L"FileVer", L"1.0.0.0"));
    SetText(hDlg, IDC_EDIT_PRODVER,   SettingGet(m, L"ProdVer", L"1.0.0.0"));

    struct { int id; const wchar_t* key; bool def; } chk[] = {
        { IDC_CHK_ADMIN,     L"Admin",    true  },
        { IDC_CHK_NO_WINDOW, L"NoWindow", true  },
        { IDC_CHK_CLEANUP,   L"Cleanup",  true  },
        { IDC_CHK_SELFDEL,   L"SelfDel",  false },
        { IDC_CHK_LOG,       L"Log",      false },
        { IDC_CHK_EXTRACT_A, L"ExtractA", true  },
    };
    for (int i = 0; i < 6; ++i)
        SetChecked(hDlg, chk[i].id, SettingGetBool(m, chk[i].key, chk[i].def));
    {
        std::wstring dv = SettingGet(m, L"DestKind", L"0");
        int idx = _wtoi(dv.c_str());
        if (idx < 0 || idx > 4) idx = 0;
        SendMessageW(GetDlgItem(hDlg, IDC_CMB_DEST), CB_SETCURSEL, (WPARAM)idx, 0);
    }
}
/* ------------------------------------------------------------------------- */
/* 文件选择对话框                                                             */
/* ------------------------------------------------------------------------- */
static bool BrowseForExe(HWND hDlg, std::wstring& out)
{
    wchar_t buf[MAX_PATH * 2];
    buf[0] = L'\0';
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hDlg;
    ofn.lpstrFilter = L"可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH * 2;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle  = L"选择程序";
    if (!GetOpenFileNameW(&ofn)) return false;
    out = buf;
    return true;
}
static bool BrowseForIcon(HWND hDlg, std::wstring& out)
{
    wchar_t buf[MAX_PATH * 2];
    buf[0] = L'\0';
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hDlg;
    ofn.lpstrFilter = L"图标文件 (*.ico)\0*.ico\0可执行文件 (*.exe;*.dll)\0*.exe;*.dll\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH * 2;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle  = L"选择图标";
    if (!GetOpenFileNameW(&ofn)) return false;
    out = buf;
    return true;
}
static bool BrowseForFolder(HWND hDlg, std::wstring& out)
{
    wchar_t display[MAX_PATH] = L"";
    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = hDlg;
    bi.lpszTitle = L"选择输出目录";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    bool ok = SHGetPathFromIDListW(pidl, display) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok) return false;
    out = display;
    return true;
}
/* ------------------------------------------------------------------------- */
/* 「生成」按钮                                                               */
/* ------------------------------------------------------------------------- */
static void OnBuild(HWND hDlg)
{
    std::wstring aPath   = GetText(hDlg, IDC_EDIT_A);
    std::wstring bPath   = GetText(hDlg, IDC_EDIT_B);
    std::wstring outDir  = GetText(hDlg, IDC_EDIT_OUTDIR);
    std::wstring outName = GetText(hDlg, IDC_EDIT_NAME);
    if (aPath.empty() || !FileExists(aPath)) {
        SetStatus(L"请先选择存在的主程序 A.exe", true);
        MessageBoxW(hDlg, L"主程序 A 的路径无效或文件不存在。", L"无法生成", MB_OK | MB_ICONWARNING);
        return;
    }
    if (bPath.empty() || !FileExists(bPath)) {
        SetStatus(L"请先选择存在的辅助程序 B.exe", true);
        MessageBoxW(hDlg, L"辅助程序 B 的路径无效或文件不存在。", L"无法生成", MB_OK | MB_ICONWARNING);
        return;
    }
    if (_wcsicmp(aPath.c_str(), bPath.c_str()) == 0) {
        SetStatus(L"A 和 B 不能是同一个文件", true);
        MessageBoxW(hDlg, L"主程序 A 和辅助程序 B 指向了同一个文件，请重新选择。",
                    L"无法生成", MB_OK | MB_ICONWARNING);
        return;
    }
    if (outDir.empty() || GetFileAttributesW(outDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SetStatus(L"输出目录无效", true);
        MessageBoxW(hDlg, L"输出目录不存在，请重新选择。", L"无法生成", MB_OK | MB_ICONWARNING);
        return;
    }
    if (outName.empty()) outName = L"WuSi_App.exe";
    if (outName.find(L'.') == std::wstring::npos) outName += L".exe";
    std::wstring outPath = JoinPath(outDir, outName);
    unsigned long long aSize = FileSizeOf(aPath);
    unsigned long long bSize = FileSizeOf(bPath);
    if (aSize == 0 || bSize == 0) {
        SetStatus(L"读取 A/B 大小失败", true);
        MessageBoxW(hDlg, L"无法读取 A 或 B 的文件大小。", L"无法生成", MB_OK | MB_ICONWARNING);
        return;
    }
    StubConfig cfg;
    ZeroMemory(&cfg, sizeof(cfg));
    cfg.configVersion  = STUB_CONFIG_VERSION;
    cfg.bWaitTimeoutMs = STUB_DEFAULT_B_TIMEOUT_MS;
    if (GetChecked(hDlg, IDC_CHK_ADMIN))     cfg.flags |= STUB_FLAG_REQUIRE_ADMIN;
    if (GetChecked(hDlg, IDC_CHK_NO_WINDOW)) cfg.flags |= STUB_FLAG_CREATE_NO_WINDOW;
    if (GetChecked(hDlg, IDC_CHK_CLEANUP))   cfg.flags |= STUB_FLAG_CLEANUP_AFTER;
    if (GetChecked(hDlg, IDC_CHK_LOG))       cfg.flags |= STUB_FLAG_ENABLE_LOG;
    if (GetChecked(hDlg, IDC_CHK_SELFDEL))   cfg.flags |= STUB_FLAG_SELF_DELETE;
    if (GetChecked(hDlg, IDC_CHK_EXTRACT_A)) cfg.flags |= STUB_FLAG_EXTRACT_A;
    {
        int di = (int)SendMessageW(GetDlgItem(hDlg, IDC_CMB_DEST), CB_GETCURSEL, 0, 0);
        cfg.destKind = (di >= 0 && di <= 4) ? (unsigned)di : STUB_DEST_EXE_DIR;
    }
    SetStatus(L"正在生成……");
    LogToBox(L"开始生成：" + outName);
    /* 闸门：构建期间再点「生成」直接忽略，避免两次构建并发写同一个输出文件 */
    if (g_building) {
        LogToBox(L"已有生成任务在进行中，请稍候……");
        return;
    }
    g_building = true;
    HWND btnBuild = GetDlgItem(hDlg, IDC_BTN_BUILD);
    if (btnBuild) EnableWindow(btnBuild, FALSE);
    PackOptions opt;
    opt.aPath = aPath;
    opt.bPath = bPath;
    opt.outPath = outPath;
    opt.cfg = cfg;
    opt.aSize = aSize;
    opt.bSize = bSize;
    opt.iconPath = GetText(hDlg, IDC_EDIT_ICON);
    opt.company        = GetText(hDlg, IDC_EDIT_COMPANY);
    opt.description    = GetText(hDlg, IDC_EDIT_DESC);
    opt.copyright      = GetText(hDlg, IDC_EDIT_COPYRIGHT);
    opt.product        = GetText(hDlg, IDC_EDIT_PRODUCT);
    opt.fileVersion    = GetText(hDlg, IDC_EDIT_FILEVER);
    opt.productVersion = GetText(hDlg, IDC_EDIT_PRODVER);
    /* 原始文件名默认用输出文件名，内部名默认同主程序 */
    opt.originalName = outName;
    opt.internalName = outName;
    /* 不解压 A 时，躯干要在同目录找它，就用用户选的 A 的文件名 */
    {
        size_t q = aPath.find_last_of(L"\\/");
        opt.aRelName = (q == std::wstring::npos) ? aPath : aPath.substr(q + 1);
    }
    if (!opt.iconPath.empty()) {
        if (!FileExists(opt.iconPath)) {
            opt.iconPath.clear();
            LogToBox(L"警告：图标文件不存在，本次不注入图标");
        } else {
            LogToBox(L"图标：" + opt.iconPath);
        }
    }
    if (opt.wantVersion()) LogToBox(L"将写入版本信息");

    /* ---- 在后台线程里构建，主线程只跑进度条 ----
       构建要拷贝 A/B 上百 MB，直接在窗口线程里做会让窗口"未响应"。 */
    std::wstring err;
    bool built = false;
    {
        HWND dlg = ProgressCreate(hDlg);
        if (dlg) {
            ProgressSet(dlg, PSTAGE_PREPARE, 0);
            ProgressPump(dlg);
        }

        BuildJob job;
        job.opt  = &opt;
        job.err  = &err;
        job.ok   = &built;
        job.dlg  = dlg;

        HANDLE th = CreateThread(NULL, 0, BuildThreadProc, &job, 0, NULL);
        if (!th) {
            built = BuildPackage(opt, err);          /* 起不了线程就退回同步 */
            if (dlg) ProgressDestroy(dlg);
        } else {
            /* 等线程结束期间持续泵消息，进度条才会动、窗口才不会假死 */
            DWORD wr = WAIT_TIMEOUT;
            while (wr == WAIT_TIMEOUT) {
                wr = WaitForSingleObject(th, 40);
                ProgressPump(dlg);
            }
            CloseHandle(th);
            ProgressPump(dlg);
            if (dlg) ProgressDestroy(dlg);
        }
    }

    if (!built) {
        g_building = false;
        if (btnBuild) EnableWindow(btnBuild, TRUE);
        SetStatus((L"生成失败：" + err).c_str(), true);
        LogToBox(L"失败：" + err);
        MessageBoxW(hDlg, (L"生成失败：\n\n" + err).c_str(), L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    g_building = false;
    SaveSettings(hDlg);

    /* 生成成功提示 ------------------------------------------------------------
       这里**不要**用 user32 的 wsprintfW：它对手写的混合格式串（十几个 %s 加
       几个 %.2f）非常脆弱，参数一多就会在内部把某个 %s 当成指针解引用，
       直接崩在 wvsprintfW 里（访问 0xFFFFFFFFFFFFFFFF，用户看到的就是
       "应用程序错误"然后退出）。改成 swprintf + 显式指定缓冲区大小，
       参数全部是明确的类型，不会再出现这种对齐问题。 */
    wchar_t info[2048];
    const wchar_t* ynAdmin   = (cfg.flags & STUB_FLAG_REQUIRE_ADMIN)    ? L"是" : L"否";
    const wchar_t* ynNoWin   = (cfg.flags & STUB_FLAG_CREATE_NO_WINDOW) ? L"是" : L"否";
    const wchar_t* ynClean   = (cfg.flags & STUB_FLAG_CLEANUP_AFTER)    ? L"是" : L"否";
    const wchar_t* ynSelfDel = (cfg.flags & STUB_FLAG_SELF_DELETE)      ? L"是" : L"否";
    const wchar_t* ynLog     = (cfg.flags & STUB_FLAG_ENABLE_LOG)       ? L"是" : L"否";
    swprintf(info, 2048,
             L"生成成功\n\n"
             L"输出文件 : %ls\n"
             L"文件大小 : %.2f MB\n\n"
             L"主程序 A : %ls\n"
             L"            %.2f MB\n"
             L"辅助程序 B : %ls\n"
             L"            %.2f MB\n\n"
             L"选项：\n"
             L"    需要管理员权限 : %ls\n"
             L"    隐藏 B 的控制台 : %ls\n"
             L"    运行后清理 A/B : %ls\n"
             L"    运行后删除自己 : %ls\n"
             L"    记录运行日志   : %ls\n",
             outPath.c_str(), (double)(aSize + bSize) / 1048576.0,
             aPath.c_str(), (double)aSize / 1048576.0,
             bPath.c_str(), (double)bSize / 1048576.0,
             ynAdmin, ynNoWin, ynClean, ynSelfDel, ynLog);
    SetStatus(L"生成成功");
    LogToBox(L"成功：" + outPath);
    /* 弹完成功框再放开按钮：否则用户先看到按钮亮了、成功框还没出来 */
    MessageBoxW(hDlg, info, L"生成成功", MB_OK | MB_ICONINFORMATION);
    if (btnBuild) EnableWindow(btnBuild, TRUE);
}
/* 由字体行高推出需要的客户区大小。
   必须**在创建窗口之前**调用：控件是在 WM_CREATE 里建的，那时窗口尺寸已经定死，
   再改 g_clientWidth/Height 就来不及了（这正是早期版本窗口尺寸不对的原因）。 */
/* 布局全部由"字体行高"驱动，而不是写死像素。
   原因：系统 UI 字体在不同 DPI 下高度不同（96 DPI 约 16px，150% 缩放约 24px）。
   如果控件高度写死成 20px，在 150% 缩放下文字（24px）比控件还高，就会被裁掉、
   行与行挤在一起。用行高做基数，布局会自动跟随 DPI。
   尺寸计算和控件摆放共用下面这份 Layout，避免两处算出不同的高度
   （早期版本就因为这个不一致，导致窗口尺寸和内容对不上）。 */
struct Layout {
    int FH, PAD, MARGIN, EDIT_H, BTN_W, CHK_H, CHK_STEP, BLOCK, W, GRP_PAD, ROW_H;
};
static Layout ComputeLayout()
{
    Layout L;
    L.FH       = g_fontHeight;
    L.PAD      = g_fontHeight / 3;
    L.MARGIN   = L.FH + L.PAD * 2;
    L.EDIT_H   = L.FH + L.PAD * 2;
    L.BTN_W    = L.FH * 5;
    L.CHK_H    = L.FH + L.PAD;
    L.CHK_STEP = L.FH + L.PAD * 2;
    L.BLOCK    = L.FH;
    /* 内容宽度：标题栏那行最长的文案大约 50 个字符宽。
       实际渲染还会被 DPI 放大，按行高的倍数给足余量。 */
    L.W        = L.FH * 34;
    L.GRP_PAD  = L.FH + L.PAD * 2;   /* 分组框标题占的高度 */
    /* 分组框内每行：必须 >= 输入框高度，否则相邻两行会差 1px 互相压住 */
    L.ROW_H    = L.EDIT_H + L.PAD;
    return L;
}
/* 信息分组框里的一行：标签宽度固定。
   "程序图标" 是 4 个汉字，6 个单位放不下（会只剩一个方块），所以给 7。 */
static const int INFO_LABEL_UNITS = 7;    /* 标签宽度 = FH * 该值 */
/* 双列列宽按**内容**算，而不是等分。
   等分会导致左列复选框文字被裁、右列图标输入框被裁。
   左列最宽的是复选框文案，右列最宽的是 标签 + 输入框 + 浏览按钮。
   复选框文案已刻意精简（详见 README 里的完整说明），所以左列只要 FH*17。 */
static void ColumnWidths(const Layout& L, int& leftW, int& rightW)
{
    leftW  = L.FH * 17;
    /* 右列 = 标签 + 间隙 + 图标输入框 + 间隙 + 浏览按钮。
       输入框要放得下一段可读的路径，用 FH*7 给它 */
    rightW = L.FH * INFO_LABEL_UNITS + L.PAD * 2 + L.FH * 7 + L.PAD + L.BTN_W;
}
/* 一列里"运行选项"分组框的高度（5 个复选框） */
static int OptionsGroupHeight(const Layout& L)
{
    return L.GRP_PAD + L.CHK_STEP * 7 + L.PAD;
}
/* 一列里"程序信息"分组框的高度（图标 + 6 个版本字段） */
static int InfoGroupHeight(const Layout& L)
{
    return L.GRP_PAD + L.ROW_H * 7 + L.PAD;
}
static void ConfigureWindowSize()
{
    const Layout L = ComputeLayout();
    int COL_LW = 0, COL_RW = 0;
    ColumnWidths(L, COL_LW, COL_RW);
    const int W  = L.MARGIN * 2 + COL_LW + L.PAD * 3 + COL_RW;
    const int LW = COL_LW - L.BTN_W - L.PAD;   /* 左列里输入框可用宽 */
    /* 左列：程序 A + 程序 B + 运行选项 + 输出两行
       右列：程序信息（图标与版本）
       两列刻意配平，避免一侧留大片空白。 */
    const int EDIT_ROW = L.FH + 2 + L.PAD + L.EDIT_H;
    const int LEFT_H  = EDIT_ROW + (L.BLOCK + L.PAD)
                      + EDIT_ROW + (L.BLOCK + L.PAD)
                      + OptionsGroupHeight(L) + (L.BLOCK + L.PAD)
                      + L.EDIT_H + L.PAD * 2
                      + L.EDIT_H;
    const int RIGHT_H = InfoGroupHeight(L);
    (void)LW;
    int h = L.MARGIN;
    h += L.FH + 6 + L.PAD + 2;                          /* 标题 */
    h += (L.FH + 2) * 2 + L.BLOCK + L.PAD;              /* 副标题（两行） */
    h += (LEFT_H > RIGHT_H ? LEFT_H : RIGHT_H);         /* 双列主体 */
    h += L.BLOCK + L.PAD;
    h += (L.FH + L.PAD * 3) + L.BLOCK;                  /* 按钮行 */
    h += L.FH + 2 + L.PAD * 2;                          /* 状态行 */
    h += L.FH * 4 + L.MARGIN;                           /* 日志框 + 下边距 */
    g_clientHeight = h;
    g_clientWidth  = W;
}
/* ============================================================================
 * Win11 风格主题
 *
 * 三部分：
 *   1) 窗口层：DWM 圆角 + 沉浸式暗色 + 背景色（SetWindowCompositionAttribute）
 *   2) 配色：从注册表读系统深浅色偏好，两套调色板
 *   3) 控件层：按钮/复选框/输入框全部自绘（圆角、悬停、按下、焦点环）
 *
 * 为什么自绘而不是设视觉样式：Win11 的按钮是 4px 圆角 + 悬停/按下三态，
 * 标准 BUTTON 类做不出来；只有自绘才能拿到一致的观感。
 * ==========================================================================*/
/* ---- DWM 属性（MinGW 头里没有这些常量，自己定义） ---- */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DEFAULT
#define DWMWCP_DEFAULT 0
#define DWMWCP_DONOTROUND 1
#define DWMWCP_ROUND 2
#define DWMWCP_ROUNDSMALL 3
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
typedef HRESULT (WINAPI *DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
/* 窗口背景用 SetWindowCompositionAttribute 设 ACCENT_ENABLE_HOSTBACKDROP。
   这是 Mica/云母 的常用退化做法；系统不支持时静默忽略，退回纯色背景。 */
typedef struct _WCA_DATA {
    int  nAttribute;
    PVOID pvData;
    SIZE_T cbData;
} WCA_DATA;
typedef BOOL (WINAPI *SetWindowCompositionAttributeFn)(HWND, WCA_DATA*);
#define WCA_ACCENT_POLICY 19
typedef struct _ACCENT_POLICY {
    int   nAccentState;
    int   nFlags;
    DWORD nColor;
    int   nAnimationId;
} ACCENT_POLICY;
#define ACCENT_ENABLE_HOSTBACKDROP 5
/* ------------------------------------------------------------------------- */
/* 配色                                                                       */
/* ------------------------------------------------------------------------- */
struct Theme {
    bool dark;
    COLORREF winBg;      /* 窗口底 */
    COLORREF surface;    /* 卡片/输入框底 */
    COLORREF surfaceAlt; /* 悬停底 */
    COLORREF border;     /* 常规描边 */
    COLORREF borderStrong;
    COLORREF text;       /* 主文字 */
    COLORREF textDim;    /* 次要文字 */
    COLORREF accent;     /* 强调色（复选框选中、焦点环） */
    COLORREF accentText; /* 强调色上的文字 */
    COLORREF pressed;    /* 按下态 */
    HBRUSH   brWin;
    HBRUSH   brSurface;
    HBRUSH   brSurfaceAlt;
};
static Theme g_theme;
static bool  g_fontVariable = false;   /* 是否用上了 Segoe UI Variable */
static HWND  g_hHotCtl = NULL;         /* 当前鼠标悬停的控件 */
static bool SystemUsesLightTheme()
{
    HKEY hk = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hk) != ERROR_SUCCESS)
        return true;
    DWORD v = 1, sz = sizeof(v), type = 0;
    RegQueryValueExW(hk, L"AppsUseLightTheme", NULL, &type, (LPBYTE)&v, &sz);
    RegCloseKey(hk);
    return v != 0;
}
static void InitTheme()
{
    Theme& T = g_theme;
    ZeroMemory(&T, sizeof(T));
    T.dark = !SystemUsesLightTheme();
    if (!T.dark) {
        T.winBg        = RGB(0xF3, 0xF3, 0xF3);
        T.surface      = RGB(0xFF, 0xFF, 0xFF);
        T.surfaceAlt   = RGB(0xFA, 0xFA, 0xFA);
        T.border       = RGB(0xC8, 0xC8, 0xC8);   /* 再浅就看不出输入框边界了 */
        T.borderStrong = RGB(0x8A, 0x8A, 0x8A);
        T.text         = RGB(0x1A, 0x1A, 0x1A);
        T.textDim      = RGB(0x5A, 0x5A, 0x5A);
        T.accent       = RGB(0x00, 0x67, 0xC0);   /* Win11 默认蓝 */
        T.accentText   = RGB(0xFF, 0xFF, 0xFF);
        T.pressed      = RGB(0xE5, 0xE5, 0xE5);
    } else {
        T.winBg        = RGB(0x20, 0x20, 0x20);
        T.surface      = RGB(0x2B, 0x2B, 0x2B);
        T.surfaceAlt   = RGB(0x32, 0x32, 0x32);
        T.border       = RGB(0x42, 0x42, 0x42);
        T.borderStrong = RGB(0x5A, 0x5A, 0x5A);
        T.text         = RGB(0xF2, 0xF2, 0xF2);
        T.textDim      = RGB(0xA8, 0xA8, 0xA8);
        T.accent       = RGB(0x60, 0xCD, 0xFF);   /* 深色下用亮蓝更清楚 */
        T.accentText   = RGB(0x10, 0x10, 0x10);
        T.pressed      = RGB(0x26, 0x26, 0x26);
    }
    T.brWin        = CreateSolidBrush(T.winBg);
    T.brSurface    = CreateSolidBrush(T.surface);
    T.brSurfaceAlt = CreateSolidBrush(T.surfaceAlt);
}
/* ------------------------------------------------------------------------- */
/* 绘图小工具                                                                 */
/* ------------------------------------------------------------------------- */
static void FillRoundRect(HDC dc, const RECT& r, int radius, COLORREF fill)
{
    HBRUSH br = CreateSolidBrush(fill);
    HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1,
                                  radius * 2, radius * 2);
    FillRgn(dc, rgn, br);
    DeleteObject(rgn);
    DeleteObject(br);
}
static void StrokeRoundRect(HDC dc, const RECT& r, int radius, COLORREF color, int width)
{
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBr  = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}
static void DrawTextIn(HDC dc, const RECT& r, const wchar_t* s, HFONT font,
                       COLORREF color, UINT flags)
{
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT rr = r;
    DrawTextW(dc, s, -1, &rr, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}
/* 抗锯齿圆角：先用路径再描边，边缘比 RoundRect 干净 */
static void FillRoundRectAA(HDC dc, const RECT& r, int radius, COLORREF fill)
{
    HBRUSH br = CreateSolidBrush(fill);
    HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1,
                                  radius * 2, radius * 2);
    FillRgn(dc, rgn, br);
    DeleteObject(rgn);
    DeleteObject(br);
}
/* ------------------------------------------------------------------------- */
/* 控件自绘                                                                   */
/* ------------------------------------------------------------------------- */
/* 分隔线：由父窗口在 WM_PAINT 里直接画，**不创建任何控件**。
   原因：曾经用 BS_GROUPBOX 当容器，它是个大矩形窗口，会盖住里面的
   复选框和输入框，把点击全吃掉 —— 症状就是"除了浏览按钮其他都点不了"。
   现在标题是普通标签、线由父窗口画，没有任何东西挡在控件前面。 */
struct SepLine { int x, y, w; };
static std::vector<SepLine> g_seps;
static void AddSeparator(int x, int y, int w)
{
    SepLine s = { x, y, w };
    g_seps.push_back(s);
}
/* 输入的 EDIT：自绘边框，文字交给系统画（这样光标、选区、滚动都正常） */
static void DrawButtonWin11(const DRAWITEMSTRUCT* dis)
{
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;
    const Theme& T = g_theme;
    DWORD st = GetCtlState(dis->hwndItem);
    bool primary = (dis->CtlID == IDC_BTN_BUILD);
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed  = (dis->itemState & ODS_SELECTED) != 0 || (st & CSTAT_PRESSED);
    bool hot      = (st & CSTAT_HOT) != 0;
    bool focus    = (st & CSTAT_FOCUS) != 0 || (dis->itemState & ODS_FOCUS) != 0;
    COLORREF fill, border, fg;
    if (primary) {
        fill   = disabled ? T.border
               : pressed  ? (T.dark ? RGB(0x4A, 0xB8, 0xE8) : RGB(0x00, 0x54, 0x9E))
               : hot      ? (T.dark ? RGB(0x74, 0xD6, 0xFF) : RGB(0x1A, 0x76, 0xD0))
                          : T.accent;
        border = fill;
        fg     = T.accentText;
    } else {
        fill   = disabled ? T.surfaceAlt
               : pressed  ? T.pressed
               : hot      ? T.surfaceAlt
                          : T.surface;
        border = (hot || focus) ? T.borderStrong : T.border;
        fg     = disabled ? T.textDim : T.text;
    }
    FillRoundRectAA(dc, r, 4, fill);
    if (!primary) StrokeRoundRect(dc, r, 4, border, 1);
    /* 焦点环：Win11 是在按钮内侧再描一圈 */
    if (focus && !disabled) {
        RECT fr = r;
        InflateRect(&fr, -2, -2);
        StrokeRoundRect(dc, fr, 3, primary ? T.accentText : T.text, 1);
    }
    wchar_t text[128];
    GetWindowTextW(dis->hwndItem, text, 128);
    DrawTextIn(dc, r, text, g_hFont,
               disabled ? fg : fg,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void DrawCheckboxWin11(const DRAWITEMSTRUCT* dis)
{
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;
    const Theme& T = g_theme;
    DWORD st = GetCtlState(dis->hwndItem);
    bool checked  = IsChecked(dis->hwndItem);   /* 自绘后 BM_GETCHECK 失效 */
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool hot      = (st & CSTAT_HOT) != 0;
    bool focus    = (st & CSTAT_FOCUS) != 0;
    /* 方块尺寸跟着字高走，保证高 DPI 下比例不变 */
    int box = g_fontHeight;
    if (box > r.bottom - r.top - 2) box = r.bottom - r.top - 2;
    int by = r.top + ((r.bottom - r.top) - box) / 2;
    RECT br = { r.left, by, r.left + box, by + box };
    COLORREF boxFill, boxBorder;
    if (checked) {
        boxFill   = disabled ? T.border : (hot ? (T.dark ? RGB(0x74,0xD6,0xFF) : RGB(0x1A,0x76,0xD0)) : T.accent);
        boxBorder = boxFill;
    } else {
        boxFill   = disabled ? T.surfaceAlt : (hot ? T.surfaceAlt : T.surface);
        boxBorder = (hot || focus) ? T.borderStrong : T.border;
    }
    FillRoundRectAA(dc, br, 4, boxFill);
    StrokeRoundRect(dc, br, 4, boxBorder, 1);
    /* 勾：用两段线画，比字体勾更可控 */
    if (checked) {
        HPEN pen = CreatePen(PS_SOLID, (box >= 18 ? 2 : 1),
                             disabled ? T.textDim : T.accentText);
        HGDIOBJ op = SelectObject(dc, pen);
        int x0 = br.left + box * 22 / 100;
        int y0 = br.top  + box * 52 / 100;
        int x1 = br.left + box * 42 / 100;
        int y1 = br.top  + box * 72 / 100;
        int x2 = br.left + box * 78 / 100;
        int y2 = br.top  + box * 28 / 100;
        MoveToEx(dc, x0, y0, NULL);
        LineTo(dc, x1, y1);
        LineTo(dc, x2, y2);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    /* 文字排在方块右侧 */
    RECT tr = r;
    tr.left = br.right + g_fontHeight / 2;
    wchar_t text[256];
    GetWindowTextW(dis->hwndItem, text, 256);
    DrawTextIn(dc, tr, text, g_hFont,
               disabled ? T.textDim : T.text,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (focus && !disabled) {
        RECT fr = r;
        fr.right = r.left + box + 4;
        fr.left  = r.left - 1;
        StrokeRoundRect(dc, fr, 5, T.text, 1);
    }
}
static void DrawEditFrame(const DRAWITEMSTRUCT* dis)
{
    /* EDIT 不支持 owner-draw，这里不会被调用；保留给以后扩展 */
    (void)dis;
}
/* ------------------------------------------------------------------------- */
/* 窗口级 Win11 效果                                                          */
/* ------------------------------------------------------------------------- */
static void ApplyWin11Window(HWND hWnd)
{
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        DwmSetWindowAttributeFn fn =
            (DwmSetWindowAttributeFn)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (fn) {
            int corner = DWMWCP_ROUND;
            fn(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
            BOOL dark = g_theme.dark ? TRUE : FALSE;
            fn(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
            /* 标题栏跟随主题色，避免暗色下顶上一块白 */
            COLORREF cap = g_theme.dark ? RGB(0x20,0x20,0x20) : RGB(0xF3,0xF3,0xF3);
            fn(hWnd, DWMWA_CAPTION_COLOR, &cap, sizeof(cap));
            COLORREF capText = g_theme.text;
            fn(hWnd, DWMWA_TEXT_COLOR, &capText, sizeof(capText));
        }
        FreeLibrary(dwm);
    }
    /* 背景：优先 Mica（云母），不支持时退回纯色 */
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        SetWindowCompositionAttributeFn fn =
            (SetWindowCompositionAttributeFn)(void*)GetProcAddress(u32, "SetWindowCompositionAttribute");
        if (fn) {
            ACCENT_POLICY ap;
            ZeroMemory(&ap, sizeof(ap));
            ap.nAccentState = ACCENT_ENABLE_HOSTBACKDROP;
            WCA_DATA wd;
            wd.nAttribute = WCA_ACCENT_POLICY;
            wd.pvData     = &ap;
            wd.cbData     = sizeof(ap);
            fn(hWnd, &wd);
        }
    }
}
/* ------------------------------------------------------------------------- */
/* 控件创建                                                                   */
/* ------------------------------------------------------------------------- */
static HWND Mk(HWND parent, const wchar_t* cls, const wchar_t* text,
               DWORD style, int x, int y, int w, int h, int id, bool bold = false)
{
    /* 按钮一律改成自绘：Win11 的圆角 + 三态只有自绘才做得出来。
       注意 BS_TYPEMASK 里取出原有类型（复选框/单选/分组框），再叠加 OWNERDRAW。 */
    DWORD exStyle = 0;
    bool isCheckbox = false;
    if (_wcsicmp(cls, L"BUTTON") == 0) {
        DWORD type = style & BS_TYPEMASK;
        if (type == BS_GROUPBOX) {
            /* 分组框已经不用了（见 AddSeparator），这里只是兜底 */
            style &= ~BS_TYPEMASK;
            style |= BS_OWNERDRAW;
            exStyle |= WS_EX_TRANSPARENT;
        } else if (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX) {
            isCheckbox = true;
            style &= ~BS_TYPEMASK;
            style |= BS_OWNERDRAW;
        } else {
            /* BS_OWNERDRAW 和 BS_DEFPUSHBUTTON 共用位段，不能共存；
               默认按钮退化成普通自绘按钮（重点按钮靠 ID 判断画成蓝色） */
            style &= ~BS_TYPEMASK;
            style |= BS_PUSHBUTTON | BS_OWNERDRAW;
        }
    }
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, NULL);
    if (c) {
        HFONT f = (bold && g_hFontBold) ? g_hFontBold : g_hFont;
        if (f) SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        /* 复选框必须子类化，否则点不到（见 CheckboxProc 的说明） */
        if (isCheckbox) {
            WNDPROC orig = (WNDPROC)SetWindowLongPtrW(c, GWLP_WNDPROC,
                                                      (LONG_PTR)CheckboxProc);
            if (orig) g_chkOrigProc[c] = orig;
        }
    }
    return c;
}
/* 布局全部由"字体行高"驱动，而不是写死像素。
   原因：系统 UI 字体在不同 DPI 下高度不同（96 DPI 约 16px，150% 缩放约 24px）。
   如果控件高度写死成 20px，在 150% 缩放下文字（24px）比控件还高，就会被裁掉、
   行与行挤在一起。用行高做基数，布局会自动跟随 DPI。 */
static void CreateControls(HWND hDlg)
{
    const Layout Lay = ComputeLayout();
    g_seps.clear();
    const int FH       = Lay.FH;
    const int PAD      = Lay.PAD;
    const int MARGIN   = Lay.MARGIN;
    const int EDIT_H   = Lay.EDIT_H;
    const int BTN_W    = Lay.BTN_W;
    const int CHK_H    = Lay.CHK_H;
    const int CHK_STEP = Lay.CHK_STEP;
    const int BLOCK    = Lay.BLOCK;
    const int W        = Lay.W;
    const int X  = MARGIN;             /* 左边距（原来叫 X，改名避免和 Layout 撞名） */
    const int EW = W - BTN_W - 8;      /* 输入框宽度 */
    int y = MARGIN;
    /* ---- 标题 ---- */
    Mk(hDlg, L"STATIC", L"WuSi Attacher —— 把 A/B 打包成单个 exe",
       SS_LEFT, X, y, W, FH + 6, -1, true);
    y += FH + 6 + PAD + 2;
    Mk(hDlg, L"STATIC",
       L"生成的程序会按顺序运行：先释放并运行 B，等它退出并合并环境变化，再释放并运行 A。\r\n"
       L"B 非正常退出时会中止，不会启动 A，避免主程序在错误的环境下运行。",
       SS_LEFT, X, y, W, (FH + 2) * 2, -1, false);
    y += (FH + 2) * 2 + BLOCK + PAD;
    /* ================= 双列主体 =================
       左列：程序 A 路径 / 程序 B 路径 / 运行选项 / 输出两行
       右列：程序信息（图标与版本属性）
       列宽按内容算（见 ColumnWidths），两列高度刻意配平。 */
    int COL_LW = 0, COL_RW = 0;
    ColumnWidths(Lay, COL_LW, COL_RW);
    const int COL_LX = X;
    const int COL_RX = X + COL_LW + PAD * 3;
    const int bodyTop = y;
    const int leftEditW = COL_LW - BTN_W - PAD;   /* 左列输入框宽度（右侧留给浏览按钮） */
    int ly = bodyTop;
    /* ---------- 左列 ---------- */
    /* 程序 A */
    Mk(hDlg, L"STATIC", L"主程序 A（最后启动的那个）",
       SS_LEFT, COL_LX, ly, COL_LW, FH + 2, -1, true);
    ly += FH + 2 + PAD;
    Mk(hDlg, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | ES_LEFT,
       COL_LX, ly, leftEditW, EDIT_H, IDC_EDIT_A);
    Mk(hDlg, L"BUTTON", L"浏览…", BS_PUSHBUTTON | WS_TABSTOP,
       COL_LX + COL_LW - BTN_W, ly, BTN_W, EDIT_H, IDC_BTN_A);
    ly += EDIT_H + BLOCK + PAD;
    /* 程序 B */
    Mk(hDlg, L"STATIC", L"辅助程序 B（先启动、改完环境就退出）",
       SS_LEFT, COL_LX, ly, COL_LW, FH + 2, -1, true);
    ly += FH + 2 + PAD;
    Mk(hDlg, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | ES_LEFT,
       COL_LX, ly, leftEditW, EDIT_H, IDC_EDIT_B);
    Mk(hDlg, L"BUTTON", L"浏览…", BS_PUSHBUTTON | WS_TABSTOP,
       COL_LX + COL_LW - BTN_W, ly, BTN_W, EDIT_H, IDC_BTN_B);
    ly += EDIT_H + BLOCK + PAD;
    /* 运行选项 */
    {
        const int GRP_H = OptionsGroupHeight(Lay);
        /* 分组标题用普通标签，分隔线由父窗口 WM_PAINT 画。
           **不要用 BS_GROUPBOX 控件** —— 它是个大矩形窗口，会盖住里面的
           复选框和输入框，把点击全吃掉（这个 bug 真出现过，症状就是
           "除了不被覆盖的浏览按钮，其他都点不了"）。 */
        Mk(hDlg, L"STATIC", L"运行选项", SS_LEFT,
           COL_LX + PAD, ly, COL_LW, Lay.GRP_PAD, -1, true);
        AddSeparator(COL_LX, ly + Lay.GRP_PAD - PAD, COL_LW);
        const int gx = COL_LX + PAD * 2;
        const int gw = COL_LW - PAD * 4;
        int gy = ly + Lay.GRP_PAD;
        /* 文案刻意精简：这一列是半宽，太长的句子会被右边缘裁掉。
           完整含义见 README。 */
        Mk(hDlg, L"BUTTON", L"需要管理员权限（弹 UAC）",
           BS_AUTOCHECKBOX | WS_TABSTOP, gx, gy, gw, CHK_H, IDC_CHK_ADMIN);
        gy += CHK_STEP;
        Mk(hDlg, L"BUTTON", L"以无窗口方式启动 B",
           BS_AUTOCHECKBOX | WS_TABSTOP, gx, gy, gw, CHK_H, IDC_CHK_NO_WINDOW);
        gy += CHK_STEP;
        Mk(hDlg, L"BUTTON", L"运行后清理临时文件（B 即删）",
           BS_AUTOCHECKBOX | WS_TABSTOP, gx, gy, gw, CHK_H, IDC_CHK_CLEANUP);
        gy += CHK_STEP;
        Mk(hDlg, L"BUTTON", L"运行后删除生成的程序自身",
           BS_AUTOCHECKBOX | WS_TABSTOP, gx, gy, gw, CHK_H, IDC_CHK_SELFDEL);
        gy += CHK_STEP;
        Mk(hDlg, L"BUTTON", L"记录运行日志到 %TEMP%",
           BS_AUTOCHECKBOX | WS_TABSTOP, gx, gy, gw, CHK_H, IDC_CHK_LOG);
        gy += CHK_STEP;
        Mk(hDlg, L"BUTTON", L"解压并运行 A（不勾选就直跑磁盘上的它）",
           BS_AUTOCHECKBOX | WS_TABSTOP, gx, gy, gw, CHK_H, IDC_CHK_EXTRACT_A);
        gy += CHK_STEP;
        /* 解压目的地：选了就写到配置里，生成的程序按它释放 A。
           默认"程序所在目录" —— %TEMP% 会让 A 以为自己在压缩包里。 */
        Mk(hDlg, L"STATIC", L"解压到:", SS_LEFT, gx, gy + PAD / 2, FH * 4, FH + 2, IDC_ST_DEST);
        Mk(hDlg, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
           gx + FH * 4 + PAD, gy, gw - FH * 4 - PAD, CHK_H * 6, IDC_CMB_DEST);
        {
            HWND cb = GetDlgItem(hDlg, IDC_CMB_DEST);
            const wchar_t* items[] = {
                L"程序所在目录（推荐）", L"系统临时目录 %TEMP%",
                L"下载 Downloads", L"文档 Documents", L"桌面 Desktop" };
            for (int i = 0; i < 5; ++i)
                SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)items[i]);
            SendMessageW(cb, CB_SETCURSEL, 0, 0);
        }
        gy += CHK_STEP + PAD;
        ly += GRP_H + BLOCK + PAD;
    }
    /* 输出不放在列里：路径很长，跨整个窗口宽度才不会一进来就被截断。
       左列剩的高度会小于右列（运行选项比版本属性矮），补齐到等高即可。 */
    (void)ly;
    /* ---------- 右列：程序信息（图标 + 版本属性） ---------- */
    {
        const int GRP_H = InfoGroupHeight(Lay);
        Mk(hDlg, L"STATIC", L"程序信息（图标与版本属性）", SS_LEFT,
           COL_RX + PAD, bodyTop, COL_RW, Lay.GRP_PAD, -1, true);
        AddSeparator(COL_RX, bodyTop + Lay.GRP_PAD - PAD, COL_RW);
        const int gx      = COL_RX + PAD * 2;
        const int gw      = COL_RW - PAD * 4;
        const int LABEL_W = FH * INFO_LABEL_UNITS;
        const int FX      = gx + LABEL_W + PAD;
        const int FW      = gw - LABEL_W - PAD;
        int gy = bodyTop + Lay.GRP_PAD;
        /* 图标：输入框 + 浏览按钮 */
        Mk(hDlg, L"STATIC", L"程序图标", SS_LEFT, gx, gy, LABEL_W, EDIT_H, -1, true);
        Mk(hDlg, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | ES_LEFT,
           FX, gy, FW - BTN_W - PAD, EDIT_H, IDC_EDIT_ICON);
        Mk(hDlg, L"BUTTON", L"浏览…", BS_PUSHBUTTON | WS_TABSTOP,
           gx + gw - BTN_W, gy, BTN_W, EDIT_H, IDC_BTN_ICON);
        gy += Lay.ROW_H;
        struct { const wchar_t* label; int id; const wchar_t* def; } fields[] = {
            { L"公司名",   IDC_EDIT_COMPANY,   L"" },
            { L"文件说明", IDC_EDIT_DESC,      L"" },
            { L"版权",     IDC_EDIT_COPYRIGHT, L"" },
            { L"产品名",   IDC_EDIT_PRODUCT,   L"" },
            { L"文件版本", IDC_EDIT_FILEVER,   L"1.0.0.0" },
            { L"产品版本", IDC_EDIT_PRODVER,   L"1.0.0.0" },
        };
        for (int i = 0; i < 6; ++i) {
            Mk(hDlg, L"STATIC", fields[i].label, SS_LEFT,
               gx, gy, LABEL_W, EDIT_H, -1, true);
            Mk(hDlg, L"EDIT", fields[i].def,
               ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | ES_LEFT,
               FX, gy, FW, EDIT_H, fields[i].id);
            gy += Lay.ROW_H;
        }
    }
    /* 主体结束：落到两列中较高的一列底部 */
    {
        const int EDIT_ROW = FH + 2 + PAD + EDIT_H;
        const int LEFT_H  = EDIT_ROW + (BLOCK + PAD)
                          + EDIT_ROW + (BLOCK + PAD)
                          + OptionsGroupHeight(Lay);
        const int RIGHT_H = InfoGroupHeight(Lay);
        y = bodyTop + (LEFT_H > RIGHT_H ? LEFT_H : RIGHT_H);
    }
    y += BLOCK + PAD;
    /* ---- 输出：跨整个窗口宽度，长路径不会一进来就被截断 ---- */
    {
        const int LABEL_W = FH * 5;
        Mk(hDlg, L"STATIC", L"输出目录", SS_LEFT, X, y + PAD / 2, LABEL_W, FH + 2, -1, true);
        Mk(hDlg, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | ES_LEFT,
           X + LABEL_W + PAD, y, W - LABEL_W - PAD - BTN_W - 8, EDIT_H, IDC_EDIT_OUTDIR);
        Mk(hDlg, L"BUTTON", L"更改…", BS_PUSHBUTTON | WS_TABSTOP,
           X + W - BTN_W, y, BTN_W, EDIT_H, IDC_BTN_OUTDIR);
        y += EDIT_H + PAD * 2;
    }
    {
        const int LABEL_W = FH * 5;
        Mk(hDlg, L"STATIC", L"输出文件名", SS_LEFT, X, y + PAD / 2, LABEL_W, FH + 2, -1, true);
        Mk(hDlg, L"EDIT", L"WuSi_App.exe", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | ES_LEFT,
           X + LABEL_W + PAD, y, FH * 11, EDIT_H, IDC_EDIT_NAME);
        y += EDIT_H + BLOCK + PAD;
    }
    /* ---- 按钮 ---- */
    {
        const int BH = FH + PAD * 3;
        Mk(hDlg, L"BUTTON", L"生成", BS_DEFPUSHBUTTON | WS_TABSTOP,
           X, y, FH * 6, BH, IDC_BTN_BUILD, true);
        Mk(hDlg, L"BUTTON", L"退出", BS_PUSHBUTTON | WS_TABSTOP,
           X + FH * 6 + PAD * 2, y, FH * 4, BH, IDC_BTN_QUIT);
        y += BH + BLOCK;
    }
    /* ---- 状态行 ---- */
    Mk(hDlg, L"STATIC", L"就绪。选择 A 和 B，点「生成」。",
       SS_LEFT, X, y, W, FH + 2, IDC_STATUS);
    y += FH + 2 + PAD * 2;
    /* ---- 日志框 ---- */
    {
        const int LOG_H = FH * 5;
        Mk(hDlg, L"EDIT", L"",
           ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_BORDER | WS_VSCROLL,
           X, y, W, LOG_H, IDC_LOG);
        y += LOG_H + MARGIN;
    }
    /* 客户端宽度按实际使用的列宽算：MARGIN + 左列 + 间隙 + 右列 + MARGIN。
       注意不能再用 L.W —— 双列布局里列宽是按内容算的，和 L.W 无关。 */
    g_clientHeight = y;
    g_clientWidth  = MARGIN * 2 + COL_LW + PAD * 3 + COL_RW;
}
/* ------------------------------------------------------------------------- */
/* 窗口过程                                                                   */
/* ------------------------------------------------------------------------- */
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hMain = hWnd;
        CreateControls(hWnd);
        LoadSettings(hWnd);
        return 0;
    /* ---- 背景：整窗铺主题底色，分组框标题和分隔线也在这里画 ---- */
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect((HDC)wp, &rc, g_theme.brWin);
        return 1;
    }
    /* ---- 分组标题下的分隔线：父窗口直接画，不占控件、不挡点击 ---- */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hWnd, &ps);
        if (!g_seps.empty()) {
            /* 分隔线要比常规描边明显一点，否则在白底上看不见 */
            HPEN pen = CreatePen(PS_SOLID, 1, g_theme.dark ? g_theme.borderStrong : RGB(0xD0,0xD0,0xD0));
            HGDIOBJ op = SelectObject(dc, pen);
            for (size_t i = 0; i < g_seps.size(); ++i) {
                MoveToEx(dc, g_seps[i].x, g_seps[i].y, NULL);
                LineTo(dc, g_seps[i].x + g_seps[i].w, g_seps[i].y);
            }
            SelectObject(dc, op);
            DeleteObject(pen);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }
    /* ---- 自绘控件 ---- */
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lp;
        if (dis->CtlType != ODT_BUTTON) return FALSE;
        if (IsCheckboxId((int)dis->CtlID)) {
            DrawCheckboxWin11(dis);
            return TRUE;
        }
        DrawButtonWin11(dis);
        return TRUE;
    }
    /* ---- 悬停跟踪：驱动自绘控件的三态 ---- */
    case WM_MOUSEMOVE: {
        HWND child = ChildWindowFromPointEx(hWnd, *(POINT*)&lp,
                                           CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
        if (child != g_hHotCtl) {
            if (g_hHotCtl) {
                SetCtlState(g_hHotCtl, CSTAT_HOT, false);
                InvalidateRect(g_hHotCtl, NULL, FALSE);
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, g_hHotCtl, 0 };
                TrackMouseEvent(&tme);
            }
            g_hHotCtl = child;
            if (child) {
                SetCtlState(child, CSTAT_HOT, true);
                InvalidateRect(child, NULL, FALSE);
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, child, 0 };
                TrackMouseEvent(&tme);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        if (g_hHotCtl) {
            SetCtlState(g_hHotCtl, CSTAT_HOT, false);
            InvalidateRect(g_hHotCtl, NULL, FALSE);
            g_hHotCtl = NULL;
        }
        return 0;
    }
    /* ---- 焦点变化：重画出/去掉焦点环 ---- */
    case WM_PARENTNOTIFY:
        return DefWindowProcW(hWnd, msg, wp, lp);
    /* 静态文本必须给一个和窗口底一致的画刷。
       返回 NULL_BRUSH 会让控件重绘时不擦除自己的背景，
       于是"就绪…"和后面的错误提示会叠在一起（这个 bug 真出现过）。 */
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, g_theme.winBg);
        SetTextColor(dc, g_theme.text);
        return (LRESULT)g_theme.brWin;
    }
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, g_theme.winBg);
        SetTextColor(dc, g_theme.text);
        return (LRESULT)g_theme.brWin;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, g_theme.surface);
        SetTextColor(dc, g_theme.text);
        return (LRESULT)g_theme.brSurface;
    }
    /* 复选框的点击已经在 CheckboxProc 里处理（子类化），
       父窗口这里不再插手 —— 否则会双重切换，等于点了没反应。 */
    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case IDC_BTN_A: {
            std::wstring p = GetText(hWnd, IDC_EDIT_A);
            if (BrowseForExe(hWnd, p)) SetText(hWnd, IDC_EDIT_A, p);
            return 0;
        }
        case IDC_BTN_B: {
            std::wstring p = GetText(hWnd, IDC_EDIT_B);
            if (BrowseForExe(hWnd, p)) SetText(hWnd, IDC_EDIT_B, p);
            return 0;
        }
        case IDC_BTN_OUTDIR: {
            std::wstring p = GetText(hWnd, IDC_EDIT_OUTDIR);
            if (BrowseForFolder(hWnd, p)) SetText(hWnd, IDC_EDIT_OUTDIR, p);
            return 0;
        }
        case IDC_BTN_ICON: {
            std::wstring p = GetText(hWnd, IDC_EDIT_ICON);
            if (BrowseForIcon(hWnd, p)) SetText(hWnd, IDC_EDIT_ICON, p);
            return 0;
        }
        case IDC_BTN_BUILD:
            OnBuild(hWnd);
            return 0;
        case IDC_BTN_QUIT:
            /* 直接在这里保存，不要指望 WM_CLOSE：
               这条路径上 WM_CLOSE 并不可靠地送达，而设置丢了用户是察觉不到的。 */
            SaveSettings(hWnd);
            DestroyWindow(hWnd);
            return 0;
        }
        /* 勾选状态变了：同步"解压到"那行的显示（只在解压模式下有意义）。
           这个 if 必须放在 switch **外面** —— 之前紧跟在 case 后面，
           被上面的 return 0 挡成了死代码，下拉框永远不跟着显示/隐藏。 */
        if (HIWORD(wp) == BN_CLICKED && IsCheckboxId(id)) {
            UpdateDestControls(hWnd);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        SaveSettings(hWnd);
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}
/* ------------------------------------------------------------------------- */
/* 命令行模式                                                                 */
/* ------------------------------------------------------------------------- */
static void PrintUsage()
{
    const wchar_t* msg =
        L"WuSi Attacher 打包器\n\n"
        L"图形界面（双击直接运行，不带参数）：\n"
        L"    选择 A、B 和选项后点「生成」。\n\n"
        L"命令行模式：\n"
        L"    WuSi_Attacher.exe --build --a <A.exe> --b <B.exe> --out <输出.exe>\n"
        L"                      [--admin 0|1] [--nowindow 0|1]\n"
        L"                      [--cleanup 0|1] [--log 0|1] [--selfdel 0|1]\n"
        L"                      [--timeout <毫秒>]\n"
        L"                      [--icon <图标.ico>]\n"
        L"                      [--company <公司名>] [--description <文件说明>]\n"
        L"                      [--copyright <版权>] [--product <产品名>]\n"
        L"                      [--filever <1.0.0.0>] [--prodver <1.0.0.0>]\n\n"
        L"自带自检（验证尾部躯干模板可用）：\n"
        L"    WuSi_Attacher.exe --selftest\n\n"
        L"退出码：0 成功；1 参数错误；2 生成失败。\n";
    MessageBoxW(NULL, msg, L"WuSi Attacher 用法", MB_OK | MB_ICONINFORMATION);
}
static bool ArgFlag(int argc, wchar_t** argv, const wchar_t* name, bool defVal)
{
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) {
            if (i + 1 < argc) {
                const wchar_t* v = argv[i + 1];
                if (!_wcsicmp(v, L"1") || !_wcsicmp(v, L"true") ||
                    !_wcsicmp(v, L"yes") || !_wcsicmp(v, L"on"))  return true;
                if (!_wcsicmp(v, L"0") || !_wcsicmp(v, L"false") ||
                    !_wcsicmp(v, L"no")  || !_wcsicmp(v, L"off")) return false;
            }
            return true;
        }
    }
    return defVal;
}
static std::wstring ArgValue(int argc, wchar_t** argv, const wchar_t* name)
{
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], name) == 0 && i + 1 < argc) return argv[i + 1];
    return std::wstring();
}
static int RunCli(int argc, wchar_t** argv)
{
    std::wstring aPath   = ArgValue(argc, argv, L"--a");
    std::wstring bPath   = ArgValue(argc, argv, L"--b");
    std::wstring outPath = ArgValue(argc, argv, L"--out");
    if (aPath.empty() || bPath.empty() || outPath.empty()) {
        printf("错误: --build 需要 --a、--b、--out\n");
        return 1;
    }
    if (!FileExists(aPath)) { printf("错误: A 不存在: %ls\n", aPath.c_str()); return 1; }
    if (!FileExists(bPath)) { printf("错误: B 不存在: %ls\n", bPath.c_str()); return 1; }
    unsigned long long aSize = FileSizeOf(aPath);
    unsigned long long bSize = FileSizeOf(bPath);
    if (aSize == 0 || bSize == 0) { printf("错误: 无法读取文件大小\n"); return 1; }
    StubConfig cfg;
    ZeroMemory(&cfg, sizeof(cfg));
    cfg.configVersion  = STUB_CONFIG_VERSION;
    cfg.bWaitTimeoutMs = STUB_DEFAULT_B_TIMEOUT_MS;
    if (ArgFlag(argc, argv, L"--admin",    true))  cfg.flags |= STUB_FLAG_REQUIRE_ADMIN;
    if (ArgFlag(argc, argv, L"--nowindow", true))  cfg.flags |= STUB_FLAG_CREATE_NO_WINDOW;
    if (ArgFlag(argc, argv, L"--cleanup",  true))  cfg.flags |= STUB_FLAG_CLEANUP_AFTER;
    if (ArgFlag(argc, argv, L"--log",      false)) cfg.flags |= STUB_FLAG_ENABLE_LOG;
    if (ArgFlag(argc, argv, L"--selfdel",  false)) cfg.flags |= STUB_FLAG_SELF_DELETE;
    if (ArgFlag(argc, argv, L"--extracta", true))  cfg.flags |= STUB_FLAG_EXTRACT_A;
    {
        std::wstring ds = ArgValue(argc, argv, L"--dest");
        if (!ds.empty()) { int d = _wtoi(ds.c_str()); if (d >= 0 && d <= 4) cfg.destKind = (unsigned)d; }
    }
    std::wstring to = ArgValue(argc, argv, L"--timeout");
    if (!to.empty()) cfg.bWaitTimeoutMs = (unsigned int)_wtoi(to.c_str());
    PackOptions opt;
    opt.aPath = aPath;
    opt.bPath = bPath;
    opt.outPath = outPath;
    opt.cfg = cfg;
    opt.aSize = aSize;
    opt.bSize = bSize;
    opt.iconPath = ArgValue(argc, argv, L"--icon");
    if (!opt.iconPath.empty() && !FileExists(opt.iconPath)) {
        printf("错误: 图标不存在: %ls\n", opt.iconPath.c_str());
        return 1;
    }
    opt.company        = ArgValue(argc, argv, L"--company");
    opt.description    = ArgValue(argc, argv, L"--description");
    opt.copyright      = ArgValue(argc, argv, L"--copyright");
    opt.product        = ArgValue(argc, argv, L"--product");
    opt.fileVersion    = ArgValue(argc, argv, L"--filever");
    opt.productVersion = ArgValue(argc, argv, L"--prodver");
    {
        size_t q = outPath.find_last_of(L"\\/");
        opt.originalName = (q == std::wstring::npos) ? outPath : outPath.substr(q + 1);
        opt.internalName = opt.originalName;
    }
    /* 不解压 A 时躯干要找的文件名：默认用 --a 的文件名，可用 --aname 覆盖 */
    {
        std::wstring an = ArgValue(argc, argv, L"--aname");
        if (an.empty()) {
            size_t q2 = aPath.find_last_of(L"\\/");
            an = (q2 == std::wstring::npos) ? aPath : aPath.substr(q2 + 1);
        }
        opt.aRelName = an;
    }
    std::wstring err;
    if (!BuildPackage(opt, err)) {
        printf("生成失败: %ls\n", err.c_str());
        return 2;
    }
    printf("生成成功\n");
    printf("  输出   : %ls (%llu 字节)\n", outPath.c_str(), FileSizeOf(outPath));
    printf("  A      : %ls (%llu 字节)\n", aPath.c_str(), aSize);
    printf("  B      : %ls (%llu 字节)\n", bPath.c_str(), bSize);
    printf("  图标   : %ls\n", opt.iconPath.empty() ? L"（未设置）" : opt.iconPath.c_str());
    printf("  版本   : %ls\n", opt.wantVersion() ? L"已写入" : L"（未设置）");
    printf("  配置块 : %u 字节, flags=0x%08X, B 超时=%u ms\n",
           (unsigned)sizeof(StubConfig), cfg.flags, cfg.bWaitTimeoutMs);
    return 0;
}
static int RunSelfTest(const std::wstring& outDir)
{
    std::vector<unsigned char> stub;
    printf("WuSi Attacher Packer 自检\n\n");
    if (!LoadStubTemplate(stub)) { printf("失败: 读不到尾部躯干模板\n"); return 1; }
    printf("[OK] 躯干模板: %u 字节\n", (unsigned)stub.size());
    if (stub.size() < 0x100 || stub[0] != 'M' || stub[1] != 'Z') {
        printf("失败: 不是有效的 PE 镜像（缺 MZ）\n"); return 1;
    }
    printf("[OK] MZ 头存在\n");
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)&stub[0];
    if ((size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > stub.size()) {
        printf("失败: e_lfanew 越界\n"); return 1;
    }
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)&stub[dos->e_lfanew];
    if (nt->Signature != IMAGE_NT_SIGNATURE) { printf("失败: PE 签名错误\n"); return 1; }
    printf("[OK] PE 签名有效, Machine=0x%04X, 节数=%u\n",
           nt->FileHeader.Machine, nt->FileHeader.NumberOfSections);
    {
        std::string blob((const char*)&stub[0], stub.size());
        printf("[%s] 清单包含 asInvoker\n",
               blob.find("asInvoker") != std::string::npos ? "OK" : "!!");
    }
    printf("[OK] sizeof(StubConfig) = %u\n", (unsigned)sizeof(StubConfig));
    printf("[OK] STUB_CONFIG_VERSION = %u\n", STUB_CONFIG_VERSION);
    std::wstring dump = JoinPath(outDir, L"stub_template_dump.exe");
    HANDLE h = CreateFileW(dump.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, &stub[0], (DWORD)stub.size(), &wr, NULL);
        CloseHandle(h);
        printf("\n[OK] 已导出模板: %ls\n", dump.c_str());
    }
    printf("\n自检通过\n");
    return 0;
}
/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */
static void EnableDpiAwareness()
{
    typedef BOOL (WINAPI *SetFn)(void);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;
    SetFn fn = (SetFn)(void*)GetProcAddress(u32, "SetProcessDPIAware");
    if (fn) fn();
}

/* ---------------------------------------------------------------------------
   开发自检：完整跑一遍「构建 -> 启动生成的程序 -> 等它结束 -> 检查结果」

   需要两个小载荷同目录存在：
       selftest_a.exe —— 主程序 A，跑完把结果写到 selfdir\selftest_a.txt
       selftest_b.exe —— 辅助程序 B，跑完把结果写到 %TEMP%\selftest_b.txt
   A 的代码里会**主动结束**生成的程序（按名字找父级 wusi_selftest*.exe），
   所以这里不用超时硬杀，也就不会留下孤儿进程。
   --------------------------------------------------------------------------- */
static void SelfTestKillLeftovers()
{
    /* 清掉上一次可能残留的生成程序（按名字精确匹配，避免误伤别的进程） */
    for (int round = 0; round < 2; ++round) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        PROCESSENTRY32W pe; ZeroMemory(&pe, sizeof(pe)); pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                wchar_t low[MAX_PATH];
                wcsncpy(low, pe.szExeFile, MAX_PATH - 1); low[MAX_PATH - 1] = 0;
                for (int i = 0; low[i]; ++i) low[i] = towlower(low[i]);
                if (wcsstr(low, L"wusi_selftest")) {
                    HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (p) { TerminateProcess(p, 0); CloseHandle(p); }
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        if (round == 0) Sleep(300);
    }
}

static int PackerSelfTest()
{
    std::wstring dir = g_exeDir;
    std::wstring outExe = JoinPath(dir, L"wusi_selftest_out.exe");
    std::wstring logPath = JoinPath(dir, L"selftest.log");
    std::wstring aTxt = JoinPath(dir, L"selftest_a.txt");
    std::wstring bTxt = JoinPath(dir, L"selftest_b.txt");
    std::wstring selfA = JoinPath(dir, L"selftest_a.exe");
    std::wstring selfB = JoinPath(dir, L"selftest_b.exe");

    /* 日志边跑边落盘：中途崩了也能看到停在哪一步 */
    static FILE* g_stLog = NULL;
    g_stLog = _wfopen(logPath.c_str(), L"wb");
    #define LOGE(s) do { if (g_stLog) { fputs((s), g_stLog); fputs("\r\n", g_stLog); fflush(g_stLog); } } while (0)
    LOGE("SELFTEST_ENTERED");

    SelfTestKillLeftovers();
    DeleteFileW(outExe.c_str());
    DeleteFileW(aTxt.c_str());
    DeleteFileW(bTxt.c_str());

    PackOptions opt;
    opt.aPath = selfA;
    opt.bPath = selfB;
    opt.outPath = outExe;
    opt.internalName = L"WuSiSelfTest.exe";
    opt.originalName = L"WuSiSelfTest.exe";
    /* 注意两点，都踩过坑：
       1) 必须带 STUB_FLAG_EXTRACT_A，否则 A 不会被嵌进产物（aSize 记 0），
          躯干会去磁盘上找 A；
       2) 尾部配置块总是用 opt.aRelName 覆盖 cfg.aName —— aRelName 留空就会把
          aName 清零，躯干只能回退到默认的 "A.exe"。 */
    opt.aRelName = L"selftest_a.exe";
    opt.cfg.configVersion  = STUB_CONFIG_VERSION;
    opt.cfg.bWaitTimeoutMs = STUB_DEFAULT_B_TIMEOUT_MS;
    opt.cfg.flags = STUB_FLAG_EXTRACT_A | STUB_FLAG_CLEANUP_AFTER | STUB_FLAG_ENABLE_LOG;
    opt.cfg.destKind = STUB_DEST_EXE_DIR;
    opt.aSize = FileSizeOf(selfA);
    opt.bSize = FileSizeOf(selfB);

    char buf[512];
    sprintf(buf, "A=%llu B=%llu", opt.aSize, opt.bSize); LOGE(buf);

    std::wstring err;
    bool built = BuildPackage(opt, err);
    std::string e8;
    { int n = WideCharToMultiByte(CP_UTF8, 0, err.c_str(), -1, NULL, 0, NULL, NULL);
      if (n > 1) { e8.resize(n - 1); WideCharToMultiByte(CP_UTF8, 0, err.c_str(), -1, &e8[0], n, NULL, NULL); } }
    LOGE(built ? "BUILD_OK" : ("BUILD_FAIL: " + e8).c_str());
    if (!built) {
        if (g_stLog) { fclose(g_stLog); g_stLog = NULL; }
        MessageBoxW(NULL, L"构建失败，详见 selftest.log", L"packer-selftest", MB_OK | MB_ICONERROR);
        return 1;
    }
    sprintf(buf, "OUT_SIZE=%llu", FileSizeOf(outExe)); LOGE(buf);

    /* 启动生成的单文件。它自己会跑 B、跑 A，A 结束后会主动把宿主结束掉。 */
    std::wstring cmd = L"\"" + outExe + L"\"";
    std::vector<wchar_t> cbuf(cmd.begin(), cmd.end()); cbuf.push_back(0);
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessW(NULL, &cbuf[0], NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi);
    if (!ok) {
        sprintf(buf, "RUN_FAIL err=%lu", GetLastError()); LOGE(buf);
    } else {
        LOGE("RUN_STARTED");
        DWORD wr = WaitForSingleObject(pi.hProcess, 60000);
        if (wr == WAIT_TIMEOUT) {
            LOGE("RUN_TIMEOUT(60s)");
            SelfTestKillLeftovers();
        } else {
            DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
            sprintf(buf, "RUN_EXITED code=%lu (0x%08lX)", (unsigned long)ec, (unsigned long)ec); LOGE(buf);
        }
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }

    /* 读 A / B 各自留下的结果文件 */
    struct { const char* label; std::wstring path; } res[2] = {
        { "A_RESULT", aTxt }, { "B_RESULT", bTxt }
    };
    for (int i = 0; i < 2; ++i) {
        HANDLE h = CreateFileW(res[i].path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            std::string miss = std::string(res[i].label) + "=(缺失)";
            LOGE(miss.c_str());
            continue;
        }
        char rb[4096] = {0};
        DWORD rd = 0;
        ReadFile(h, rb, sizeof(rb) - 1, &rd, NULL);
        CloseHandle(h);
        std::string s = std::string(res[i].label) + "=" + std::string(rb, rd);
        while (!s.empty() && (s[s.size() - 1] == '\r' || s[s.size() - 1] == '\n')) s.erase(s.size() - 1);
        LOGE(s.c_str());
    }

    /* A 是不是留在自己目录里（解压模式才是这样；这里验证"不内嵌、就地运行"）
       顺手确认清理解压目录那次记录 */
    {
        std::wstring aRunDir = JoinPath(dir, L"selftest_a.exe");
        sprintf(buf, "A_IN_PLACE=%d", FileExists(aRunDir) ? 1 : 0); LOGE(buf);
    }

    FILE* f = _wfopen(logPath.c_str(), L"rb");
    std::string log;
    if (f) {
        char rb[8192] = {0};
        size_t rd = fread(rb, 1, sizeof(rb) - 1, f);
        log.assign(rb, rd);
        fclose(f);
    }
    if (g_stLog) { fclose(g_stLog); g_stLog = NULL; }

    /* 结果转成宽字符弹个框，方便肉眼确认 */
    int wn = MultiByteToWideChar(CP_UTF8, 0, log.c_str(), (int)log.size(), NULL, 0);
    std::wstring wlog((size_t)(wn > 0 ? wn : 0), L'\0');
    if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, log.c_str(), (int)log.size(), &wlog[0], wn);
    bool allOk = (wlog.find(L"BUILD_OK") != std::wstring::npos) &&
                 (wlog.find(L"A_RESULT=A_RAN_OK") != std::wstring::npos) &&
                 (wlog.find(L"B_RESULT=B_RAN_OK") != std::wstring::npos);
    MessageBoxW(NULL, wlog.c_str(), allOk ? L"packer-selftest: 全部通过" : L"packer-selftest: 有失败项",
                MB_OK | (allOk ? MB_ICONINFORMATION : MB_ICONERROR));
    #undef LOGE
    return allOk ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    g_hInst = hInst;
    {
        wchar_t self[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
        g_exeDir = DirNameOf(std::wstring(self, n));
        g_outDir = g_exeDir;
    }

    /* ------------------------------------------------------------------
       开发自检开关：只有当可执行文件**恰好**叫 packer-selftest.exe 时才生效。
       它把整个真实链路跑一遍（构建 -> 启动产物 -> 结束产物 -> 收结果），
       结果写日志文件和消息框。
       之所以用文件名而不是命令行开关：GUI 子系统程序从 PowerShell 拿不到
       控制台输出，而改成 CONSOLE 子系统又会让正常使用时多出一个黑框。
       用文件名可以完全复制一份、互不干扰，正式使用绝不会误触发。
       ------------------------------------------------------------------ */
    {
        const wchar_t* kSelfTestName = L"packer-selftest.exe";   /* 19 个字符，别写死数字 */
        wchar_t self[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
        std::wstring low(self, n);
        for (size_t i = 0; i < low.size(); ++i) low[i] = towlower(low[i]);
        size_t nl = wcslen(kSelfTestName);
        if (low.size() >= nl && low.compare(low.size() - nl, nl, kSelfTestName) == 0)
            return PackerSelfTest();
    }

    EnableDpiAwareness();
    /* 有命令行参数就不开 GUI */
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            int cliRc = -1;
            bool cliMode = false;
            for (int i = 1; i < argc; ++i) {
                if (_wcsicmp(argv[i], L"--selftest") == 0) {
                    cliMode = true;
                    AttachConsole(ATTACH_PARENT_PROCESS);
                    cliRc = RunSelfTest(g_exeDir);
                    break;
                }
                if (_wcsicmp(argv[i], L"--build") == 0) {
                    cliMode = true;
                    AttachConsole(ATTACH_PARENT_PROCESS);
                    cliRc = RunCli(argc, argv);
                    break;
                }
                if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0 ||
                    _wcsicmp(argv[i], L"/?") == 0) {
                    cliMode = true;
                    PrintUsage();
                    cliRc = 0;
                    break;
                }
            }
            LocalFree(argv);
            if (cliMode) return (cliRc < 0) ? 0 : cliRc;
        }
    }
    {
        NONCLIENTMETRICSW ncm;
        ZeroMemory(&ncm, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            /* 字体：优先 Segoe UI Variable（Win11 的界面字体），
               没有就退回系统 UI 字体（Win10 上是 Segoe UI）。
               注意必须用 DPI 感知后的字体高度：lfMessageFont.lfHeight 在
               高 DPI 下已经是缩放过的值，直接用即可。 */
            LOGFONTW lf = ncm.lfMessageFont;
            lf.lfQuality = CLEARTYPE_QUALITY;
            LOGFONTW var = lf;
            wcscpy_s(var.lfFaceName, LF_FACESIZE, L"Segoe UI Variable Text");
            HFONT hVar = CreateFontIndirectW(&var);
            if (hVar) {
                /* CreateFontIndirect 对不存在的字体也会返回句柄，
                   所以要实际取一次度量确认字体真的被选中了 */
                HDC dc = GetDC(NULL);
                if (dc) {
                    HGDIOBJ old = SelectObject(dc, hVar);
                    wchar_t face[LF_FACESIZE] = {0};
                    GetTextFaceW(dc, LF_FACESIZE, face);
                    SelectObject(dc, old);
                    ReleaseDC(NULL, dc);
                    if (_wcsicmp(face, L"Segoe UI Variable Text") == 0) {
                        g_hFont = hVar;
                        g_fontVariable = true;
                    } else {
                        DeleteObject(hVar);
                    }
                }
            }
            if (!g_hFont) g_hFont = CreateFontIndirectW(&lf);
            LOGFONTW lbf = lf;
            lbf.lfWeight = FW_SEMIBOLD;   /* Win11 的小标题用 Semibold */
            if (g_fontVariable)
                wcscpy_s(lbf.lfFaceName, LF_FACESIZE, L"Segoe UI Variable Text");
            g_hFontBold = CreateFontIndirectW(&lbf);
            /* 用实际字体高度推算行高，这样高 DPI 下布局会跟着放大，
               不会出现“字体放大了、间距没放大”导致的挤成一团 */
            HDC dc = GetDC(NULL);
            if (dc) {
                HFONT old = (HFONT)SelectObject(dc, g_hFont);
                TEXTMETRICW tm;
                if (GetTextMetricsW(dc, &tm)) g_fontHeight = (int)tm.tmHeight;
                SelectObject(dc, old);
                ReleaseDC(NULL, dc);
            }
        }
        if (!g_hFont) g_hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        if (!g_hFontBold) g_hFontBold = g_hFont;
    }
    /* 主题：从系统偏好决定深浅色，再据此调整布局尺寸 */
    InitTheme();
    /* 关键：必须在创建窗口之前定好客户区大小 */
    ConfigureWindowSize();
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    /* 背景交给 WM_ERASEBKGND 用主题色刷，这里给 NULL 避免先闪一下系统色 */
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"WuSiPackerWnd";
    /* 用打包器自己的图标（PE 资源里的第一个图标） */
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm       = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIcon)   wc.hIcon   = LoadIconW(NULL, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"窗口类注册失败。", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, g_clientWidth, g_clientHeight };
    AdjustWindowRect(&rc, style, FALSE);
    HWND hWnd = CreateWindowExW(0, L"WuSiPackerWnd",
                                L"WuSi Attacher —— A/B 单文件打包器",
                                style, CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                NULL, NULL, hInst, NULL);
    if (!hWnd) {
        MessageBoxW(NULL, L"创建窗口失败。", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }
    /* Win11 观感：标题栏跟随深浅色、窗口圆角、Mica 背景 */
    ApplyWin11Window(hWnd);
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_hFont) DeleteObject(g_hFont);
    if (g_hFontBold && g_hFontBold != g_hFont) DeleteObject(g_hFontBold);
    return (int)msg.wParam;
}
