/* ============================================================================
 * stub.cpp —— 躯干模板
 *
 * 这个程序不是给用户双击的，它是被 WuSi_Attacher.exe（打包器）当模板使用：
 * 打包器把本 exe 的完整镜像 + 配置块 + A/B 字节拼成最终的单文件 exe。
 *
 * 运行逻辑：
 *   1. 从自身文件尾向前扫描，读出配置块
 *   2. 防重入（命名互斥体）
 *   3. 按配置自提权（ShellExecuteEx runas + --wusi-elevated 标记）
 *   4. 注册表环境快照（HKCU\Environment 与 HKLM\...\Session Manager\Environment）
 *   5. 释放 B -> 运行 B -> 等退出 -> 校验退出码（非 0 则中止，绝不运行 A）
 *   6. 重读注册表 -> diff -> SetEnvironmentVariableW 合并进当前进程环境
 *   7. 释放 A -> 运行 A -> 等退出
 *   8. 按配置清理释放出来的 A/B，以及可选地删除自身
 *
 * 日志一律用 Win32 API 写 UTF-8，不依赖 CRT 的 ccs= 扩展（MinGW 下不可靠）。
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
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <vector>
#include <map>

#include "shared_config.h"

/* ------------------------------------------------------------------------- */
/* 全局状态                                                                   */
/* ------------------------------------------------------------------------- */
static std::wstring g_exeDir;
static std::wstring g_selfPath;
static std::wstring g_logPath;
static bool         g_logEnabled = false;

/* ------------------------------------------------------------------------- */
/* 基础工具                                                                   */
/* ------------------------------------------------------------------------- */

static void AppendErr(std::wstring& s, const wchar_t* what)
{
    wchar_t buf[64];
    wsprintfW(buf, L" [%s=%lu]", what, (unsigned long)GetLastError());
    s += buf;
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
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

/* ------------------------------------------------------------------------- */
/* 日志                                                                       */
/* ------------------------------------------------------------------------- */

static void RawWriteUtf8File(const std::wstring& path, const std::string& text)
{
    if (path.empty() || text.empty()) return;
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(path.c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return;
        SetFilePointer(h, 0, NULL, FILE_END);
    }
    DWORD wr = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &wr, NULL);
    CloseHandle(h);
}

static void LogLine(const wchar_t* fmt, ...)
{
    if (!g_logEnabled || g_logPath.empty()) return;

    wchar_t msg[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(msg, 4095, fmt, ap);
    msg[4095] = L'\0';
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[4300];
    wsprintfW(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u  %s\r\n",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
              st.wMilliseconds, msg);

    static bool bomWritten = false;
    std::string out;
    if (!bomWritten) { out.append("\xEF\xBB\xBF"); bomWritten = true; }
    out += WideToUtf8(line);
    RawWriteUtf8File(g_logPath, out);
}

static void FailMsg(const std::wstring& msg)
{
    LogLine(L"[FAIL] %s", msg.c_str());
    MessageBoxW(NULL, msg.c_str(), L"启动失败", MB_OK | MB_ICONERROR | MB_TOPMOST);
}

/* ------------------------------------------------------------------------- */
/* 读取文件最末尾的配置块                                                     */
/* ------------------------------------------------------------------------- */

/* 配置块固定在文件最后 STUB_TRAILER_SIZE 字节里，所以读取是 O(1)：
   直接 seek 到末尾往回读定长结构，再用 trailerMagic 校验一次。
   不扫描、不需要把整个文件读进来、不会误命中躯干自身代码里的字符串常量。 */
static bool LoadConfigFromSelf(StubConfig* cfg)
{
    HANDLE h = CreateFileW(g_selfPath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { LogLine(L"打开自身失败"); return false; }

    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li)) { CloseHandle(h); return false; }
    const unsigned long long fileSize = (unsigned long long)li.QuadPart;

    const unsigned int trailerSize = STUB_TRAILER_SIZE;
    if (fileSize < (unsigned long long)trailerSize + 2) {
        CloseHandle(h);
        LogLine(L"文件太小（%llu 字节），不是打包产物", fileSize);
        return false;
    }

    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)(fileSize - trailerSize);
    DWORD rd = 0;
    StubTrailer tr;
    ZeroMemory(&tr, sizeof(tr));

    if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN) ||
        !ReadFile(h, &tr, trailerSize, &rd, NULL) || rd != trailerSize) {
        CloseHandle(h);
        LogLine(L"读取尾部配置块失败");
        return false;
    }
    CloseHandle(h);

    /* 用固定长度比较，避免 magic 后面的 '\0' 造成误判 */
    if (memcmp(tr.trailerMagic, STUB_TRAILER_MAGIC, STUB_TRAILER_MAGIC_LEN) != 0) {
        LogLine(L"尾部识别码不匹配，不是打包器生成的程序");
        return false;
    }

    *cfg = tr.config;

    LogLine(L"配置块位于文件尾 %u 字节处（文件共 %llu 字节）", trailerSize, fileSize);
    LogLine(L"configVersion=%u flags=0x%08X", cfg->configVersion, cfg->flags);

    if (cfg->configVersion != STUB_CONFIG_VERSION) {
        LogLine(L"配置版本不匹配：%u != %u", cfg->configVersion, STUB_CONFIG_VERSION);
        return false;
    }
    if (cfg->bSize == 0) {
        LogLine(L"B 为空"); return false;
    }
    /* aSize 为 0 是合法的：表示"不解压 A、直接运行磁盘上的它"。
       此时配置里的 aOffset 无意义。 */
    if (cfg->bOffset + cfg->bSize > fileSize ||
        (cfg->aSize > 0 && cfg->aOffset + cfg->aSize > fileSize)) {
        LogLine(L"payload 偏移越界"); return false;
    }
    return true;
}

static bool ExtractPayload(unsigned long long offset, unsigned long long size,
                           const std::wstring& outPath)
{
    HANDLE in = CreateFileW(g_selfPath.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (in == INVALID_HANDLE_VALUE) { LogLine(L"释放时打开自身失败"); return false; }

    HANDLE out = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        CloseHandle(in);
        std::wstring m = L"无法创建释放文件: " + outPath;
        AppendErr(m, L"GetLastError");
        LogLine(L"%s", m.c_str());
        return false;
    }

    LARGE_INTEGER p; p.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(in, p, NULL, FILE_BEGIN)) {
        CloseHandle(in); CloseHandle(out); return false;
    }

    std::vector<unsigned char> buf(64 * 1024);
    unsigned long long remaining = size;
    bool ok = true;

    while (remaining > 0) {
        DWORD chunk = (remaining > buf.size()) ? (DWORD)buf.size() : (DWORD)remaining;
        DWORD rd = 0, wr = 0;
        if (!ReadFile(in, &buf[0], chunk, &rd, NULL) || rd == 0) { ok = false; break; }
        if (!WriteFile(out, &buf[0], rd, &wr, NULL) || wr != rd) { ok = false; break; }
        remaining -= rd;
    }

    /* 先 flush 再关：否则紧接着 CreateProcess 可能读到不完整文件 */
    if (ok) FlushFileBuffers(out);
    CloseHandle(in);
    CloseHandle(out);

    if (!ok) {
        LogLine(L"写入释放文件失败（磁盘满？）: %s", outPath.c_str());
        DeleteFileW(outPath.c_str());
        return false;
    }

    HANDLE chk = CreateFileW(outPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (chk == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool sizeOk = GetFileSizeEx(chk, &sz) && ((unsigned long long)sz.QuadPart == size);
    CloseHandle(chk);
    if (!sizeOk) {
        LogLine(L"释放后大小不符: %s", outPath.c_str());
        DeleteFileW(outPath.c_str());
        return false;
    }
    LogLine(L"已释放 %llu 字节 -> %s", size, outPath.c_str());
    return true;
}

/* ------------------------------------------------------------------------- */
/* 进程启动与等待                                                             */
/* ------------------------------------------------------------------------- */

static bool RunAndWait(const std::wstring& exePath, const std::wstring& workDir,
                       unsigned int waitTimeoutMs, bool createNoWindow,
                       DWORD* outExitCode)
{
    std::wstring cmdLine = L"\"" + exePath + L"\"";
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    DWORD flags = createNoWindow ? CREATE_NO_WINDOW : 0;

    /* 不传 lpEnvironment：子进程继承当前进程环境块，而它已经被注册表 diff 更新过 */
    BOOL ok = CreateProcessW(exePath.c_str(), &mutableCmd[0], NULL, NULL, FALSE,
                             flags, NULL, workDir.empty() ? NULL : workDir.c_str(),
                             &si, &pi);
    if (!ok) {
        std::wstring m = L"CreateProcess 失败: " + exePath;
        AppendErr(m, L"GetLastError");
        LogLine(L"%s", m.c_str());
        return false;
    }

    DWORD wait = (waitTimeoutMs == 0) ? INFINITE : waitTimeoutMs;
    DWORD wr = WaitForSingleObject(pi.hProcess, wait);
    bool timedOut = (wr == WAIT_TIMEOUT);

    DWORD code = 0;
    if (!timedOut) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (timedOut) {
        LogLine(L"等待超时(%u ms): %s", waitTimeoutMs, exePath.c_str());
        return false;
    }
    if (outExitCode) *outExitCode = code;
    LogLine(L"进程退出，退出码 = %lu : %s", (unsigned long)code, exePath.c_str());
    return true;
}

/* ------------------------------------------------------------------------- */
/* 注册表环境快照与 diff 合并                                                 */
/* ------------------------------------------------------------------------- */

typedef std::map<std::wstring, std::wstring> EnvMap;

static const wchar_t* kEnvKeys[] = {
    L"Environment",
    L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"
};
static const HKEY kEnvRoots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };

static void SnapshotOneKey(HKEY root, const wchar_t* sub, EnvMap& out)
{
    HKEY hk = NULL;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &hk) != ERROR_SUCCESS) return;

    DWORD idx = 0;
    for (;;) {
        wchar_t name[512];
        DWORD nameLen = 512;
        DWORD type = 0;
        BYTE data[8192];
        DWORD dataLen = sizeof(data);

        LSTATUS st = RegEnumValueW(hk, idx++, name, &nameLen, NULL, &type, data, &dataLen);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) break;
        if (nameLen == 0) continue;

        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            const wchar_t* s = (const wchar_t*)data;
            size_t chars = dataLen / sizeof(wchar_t);
            while (chars > 0 && s[chars - 1] == L'\0') chars--;
            out[std::wstring(name, nameLen)] = std::wstring(s, chars);
        } else if (type == REG_DWORD && dataLen >= 4) {
            wchar_t num[32];
            wsprintfW(num, L"%lu", (unsigned long)*(const DWORD*)data);
            out[std::wstring(name, nameLen)] = num;
        }
    }
    RegCloseKey(hk);
}

static EnvMap SnapshotEnv()
{
    EnvMap m;
    for (int i = 0; i < 2; ++i) SnapshotOneKey(kEnvRoots[i], kEnvKeys[i], m);
    return m;
}

static int MergeEnvDiff(const EnvMap& before, const EnvMap& afterMap)
{
    int applied = 0;
    for (EnvMap::const_iterator it = afterMap.begin(); it != afterMap.end(); ++it) {
        EnvMap::const_iterator old = before.find(it->first);
        if (old != before.end() && old->second == it->second) continue;
        if (SetEnvironmentVariableW(it->first.c_str(), it->second.c_str())) {
            ++applied;
            LogLine(L"环境已合并: %s", it->first.c_str());
        }
    }
    return applied;
}

/* ------------------------------------------------------------------------- */
/* 提权                                                                       */
/* ------------------------------------------------------------------------- */

static bool IsElevated()
{
    bool elevated = false;
    HANDLE token = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION te;
        DWORD ret = 0;
        if (GetTokenInformation(token, TokenElevation, &te, sizeof(te), &ret))
            elevated = (te.TokenIsElevated != 0);
        CloseHandle(token);
    }
    return elevated;
}

static bool RelaunchElevated()
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);

    std::wstring params = L"\"";
    params += self;
    params += L"\" ";
    params += STUB_ELEVATED_FLAG_W;

    std::vector<wchar_t> fileBuf(params.begin(), params.end());
    fileBuf.push_back(L'\0');

    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb       = L"runas";
    sei.lpFile       = self;
    sei.lpParameters = &fileBuf[0];
    sei.lpDirectory  = g_exeDir.c_str();
    sei.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        LogLine(L"提权失败，错误码 = %lu", (unsigned long)err);
        if (err == ERROR_CANCELLED) {
            FailMsg(L"用户取消了管理员权限请求。\n\n"
                    L"本程序需要管理员权限才能运行。\n"
                    L"如需在无管理员权限下运行，请在打包时取消勾选「需要管理员权限」。");
        } else {
            std::wstring m = L"提权失败。";
            AppendErr(m, L"GetLastError");
            FailMsg(m);
        }
        return false;
    }

    /* 等提权实例干完活，然后原进程静默退出 */
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        CloseHandle(sei.hProcess);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* 删除相关                                                                   */
/* ------------------------------------------------------------------------- */

static bool DeleteWithRetry(const std::wstring& path, int attempts, DWORD delayMs)
{
    for (int i = 0; i < attempts; ++i) {
        if (DeleteFileW(path.c_str())) return true;
        /* 文件可能处于"删除挂起"状态，先改名能绕开 */
        std::wstring tmp = path + L".del";
        if (MoveFileExW(path.c_str(), tmp.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp.c_str());
            return true;
        }
        Sleep(delayMs);
    }
    return false;
}

/* 跑完删掉自己。
   Windows 不允许运行中的 exe 删除自己，而且此时它的映像仍被"运行中映像"锁
   占用，任何外部进程也删不掉。所以这里走两步：

     1) 把自己改名成 .tmp —— 改名只需要目录项权限，不要求文件独占，
        因此即使文件正在运行也能成功。
     2) 登记 MOVEFILE_DELAY_UNTIL_REBOOT，交给系统在下次重启时删除。
        这是微软文档保证的路径：注册表项 PendingFileRenameOperations，
        SMSS 在下次启动早期清理，不需要任何写权限、不需要常驻帮手。

   刻意不做"改名 + cmd 延时删除"那套：cmd 的 for/引号/重定向组合在不同
   环境下行为不一致，实测经常空转 30 秒仍删不掉，可靠性反而不如官方机制。
   代价是文件要到下次重启才真正消失，但用户双击的那个 exe 名字立刻就不在了。 */
static void SelfDelete(const std::wstring& logPath, bool logEnabled)
{
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    std::wstring selfPath(self, n);

    /* 先尽量立即删除：此时子进程都已退出，本进程的映像锁还在，
       但映像有时允许删除（例如文件属性允许），试一下没有坏处。 */
    if (DeleteFileW(selfPath.c_str())) {
        if (logEnabled)
            RawWriteUtf8File(logPath, WideToUtf8(L"自身删除：已直接删除\r\n"));
        return;
    }

    std::wstring tmpPath = selfPath + L".tmp";

    if (!MoveFileExW(selfPath.c_str(), tmpPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD e1 = GetLastError();
        BOOL ok = MoveFileExW(selfPath.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        if (logEnabled) {
            wchar_t m[512];
            wsprintfW(m,
                      L"自身删除：改名失败(err=%lu)；延迟删除登记%s\r\n",
                      (unsigned long)e1, ok ? L"成功" : L"也失败，文件会留下");
            RawWriteUtf8File(logPath, WideToUtf8(m));
        }
        return;
    }

    /* 已改名成 .tmp，再登记重启时删除 */
    BOOL ok = MoveFileExW(tmpPath.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);

    if (logEnabled) {
        wchar_t m[512];
        wsprintfW(m,
                  L"自身删除：已改名为 %s；重启时删除登记%s\r\n",
                  tmpPath.c_str(), ok ? L"成功" : L"失败（该 .tmp 会留下）");
        RawWriteUtf8File(logPath, WideToUtf8(m));
    }
}

/* ------------------------------------------------------------------------- */
/* 主流程                                                                     */
/* ------------------------------------------------------------------------- */

static std::wstring MakeLogPath()
{
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(tmp) : g_exeDir;
    return JoinPath(dir, L"wusi_attacher_stub_log.txt");
}

/* 真正的逻辑。入口是下面的 WinMain ——
   躯干必须以 **GUI 子系统** 编译：控制台子系统会被 Windows 分配一个控制台窗口，
   运行生成的程序时会弹出一个难看的黑框（躯干自己就是那个黑框）。
   GUI 子系统下不会分配控制台，错误一律用 MessageBox 呈现。 */
static int StubMain()
{
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    g_selfPath = std::wstring(self, n);
    g_exeDir   = DirNameOf(g_selfPath);

    StubConfig cfg;
    ZeroMemory(&cfg, sizeof(cfg));
    if (!LoadConfigFromSelf(&cfg)) {
        MessageBoxW(NULL,
                    L"无法读取内置配置。\n\n"
                    L"这个程序需要用 WuSi_Attacher 打包器生成，\n"
                    L"直接运行躯干模板文件是没有意义的。",
                    L"启动失败", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 2;
    }

    g_logEnabled = (cfg.flags & STUB_FLAG_ENABLE_LOG) != 0;
    if (g_logEnabled) {
        g_logPath = MakeLogPath();
        LogLine(L"===== 启动 =====");
        LogLine(L"自身: %s", g_selfPath.c_str());
        LogLine(L"flags = 0x%08X", cfg.flags);
    }

    /* 防重入：名字由自身路径哈希决定，同一个产物全局串行。
       用 Local\ 命名空间：Global\ 在普通权限下可能因权限不足失败。 */
    wchar_t mutexName[256];
    {
        unsigned long h = 5381;
        for (const wchar_t* p = g_selfPath.c_str(); *p; ++p)
            h = ((h << 5) + h) + (unsigned long)(*p);
        wsprintfW(mutexName, L"Local\\WuSiAttacher_%08lX", h);
    }

    /* GUI 子系统没有 argv，直接在原始命令行里找提权标记 */
    bool elevatedRun = (wcsstr(GetCommandLineW(), STUB_ELEVATED_FLAG_W) != NULL);

    HANDLE hMutex = CreateMutexW(NULL, TRUE, mutexName);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        LogLine(L"已有实例在运行，本次退出");
        if (!elevatedRun) { CloseHandle(hMutex); return 0; }
    }

    /* 到这里才算真正要干活，把 payload 布局记进日志 */
    LogLine(L"B: offset=%llu size=%llu", cfg.bOffset, cfg.bSize);
    LogLine(L"A: offset=%llu size=%llu", cfg.aOffset, cfg.aSize);

    if ((cfg.flags & STUB_FLAG_REQUIRE_ADMIN) && !elevatedRun && !IsElevated()) {
        LogLine(L"需要管理员权限且当前未提权，尝试自提权");
        if (RelaunchElevated()) {
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }
        if (hMutex) CloseHandle(hMutex);
        return 3;
    }
    if (cfg.flags & STUB_FLAG_REQUIRE_ADMIN)
        LogLine(L"管理员模式（已提权 = %s）", IsElevated() ? L"是" : L"否");

    std::wstring tempDir;
    {
        wchar_t tmp[MAX_PATH];
        DWORD tn = GetTempPathW(MAX_PATH, tmp);
        std::wstring base = (tn > 0 && tn < MAX_PATH) ? std::wstring(tmp) : g_exeDir;
        wchar_t sub[64];
        wsprintfW(sub, L"wusi_attacher_%lu", (unsigned long)GetCurrentProcessId());
        tempDir = JoinPath(base, sub);
    }
    CreateDirectoryW(tempDir.c_str(), NULL);

    /* 解析 A 的解压目的地。
       B 永远只放临时目录（它只是过一下，不该落进用户目录）；
       A 按配置放：默认是生成程序自己的目录 —— 因为 A 常常会检查自己
       "是不是从压缩包/临时目录里跑起来的"，%TEMP% 会触发那种检测。 */
    std::wstring aDir;
    {
        std::wstring base;
        switch (cfg.destKind) {
        case STUB_DEST_DOWNLOADS:
        case STUB_DEST_DOCUMENTS:
        case STUB_DEST_DESKTOP: {
            wchar_t up[MAX_PATH];
            DWORD n2 = GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
            const wchar_t* leaf = (cfg.destKind == STUB_DEST_DOWNLOADS) ? L"Downloads"
                                : (cfg.destKind == STUB_DEST_DOCUMENTS) ? L"Documents"
                                                                        : L"Desktop";
            base = (n2 > 0 && n2 < MAX_PATH) ? JoinPath(up, leaf) : g_exeDir;
            break;
        }
        case STUB_DEST_TEMP:
            base = tempDir;
            break;
        case STUB_DEST_EXE_DIR:
        default:
            base = g_exeDir;
            break;
        }
        if (GetFileAttributesW(base.c_str()) == INVALID_FILE_ATTRIBUTES)
            base = g_exeDir;                  /* 目的地不存在就退回程序目录 */

        char sd[STUB_SUBDIR_MAX];
        memcpy(sd, cfg.subDir, STUB_SUBDIR_MAX);
        sd[STUB_SUBDIR_MAX - 1] = '\0';
        if (sd[0]) {
            int wn = MultiByteToWideChar(CP_UTF8, 0, sd, -1, NULL, 0);
            if (wn > 0) {
                std::vector<wchar_t> w((size_t)wn);
                MultiByteToWideChar(CP_UTF8, 0, sd, -1, &w[0], wn);
                base = JoinPath(base, &w[0]);
            }
        }
        aDir = base;
    }
    CreateDirectoryW(aDir.c_str(), NULL);
    LogLine(L"A 解压目的地: %s", aDir.c_str());

    std::wstring bPath = JoinPath(tempDir, L"B.exe");

    /* A 的路径按处理方式决定：
         解压模式 -> 释放到临时目录（cfg.aOffset/aSize）
         直跑模式 -> 直接在躯干同目录找它（cfg.aName，留空则 A.exe） */
    const bool extractA = (cfg.flags & STUB_FLAG_EXTRACT_A) != 0;
    std::wstring aPath;
    if (extractA) {
        aPath = JoinPath(aDir, L"A.exe");   /* 目的地由 cfg.destKind 决定 */
    } else {
        char nameA[STUB_A_NAME_MAX];
        memcpy(nameA, cfg.aName, STUB_A_NAME_MAX);
        nameA[STUB_A_NAME_MAX - 1] = '\0';
        std::wstring rel;
        if (nameA[0]) {
            int n = MultiByteToWideChar(CP_UTF8, 0, nameA, -1, NULL, 0);
            if (n > 0) {
                std::vector<wchar_t> tmp((size_t)n);
                MultiByteToWideChar(CP_UTF8, 0, nameA, -1, &tmp[0], n);
                rel = &tmp[0];
            }
        }
        if (rel.empty()) rel = L"A.exe";
        aPath = JoinPath(g_exeDir, rel);
        LogLine(L"A 直跑模式，目标: %s", aPath.c_str());
    }

    bool bExtracted = false, aExtracted = false;
    int rc = 0;
    /* A 的工作目录。必须在这里声明：后面有多处 goto cleanup，
       在 goto 之后声明会因"跨过初始化"而编译不过。 */
    std::wstring aWorkDir = aDir;

    /* --- 1. 释放并运行 B --- */
    {
        EnvMap envBefore = SnapshotEnv();
        LogLine(L"环境快照: %u 个变量", (unsigned)envBefore.size());

        if (!ExtractPayload(cfg.bOffset, cfg.bSize, bPath)) {
            FailMsg(L"释放辅助程序 B 失败。\n\n目标目录: " + tempDir);
            rc = 4; goto cleanup;
        }
        bExtracted = true;

        DWORD bExit = 0;
        if (!RunAndWait(bPath, tempDir, cfg.bWaitTimeoutMs,
                        (cfg.flags & STUB_FLAG_CREATE_NO_WINDOW) != 0, &bExit)) {
            FailMsg(L"辅助程序 B 启动失败或等待超时。\n\n路径: " + bPath);
            rc = 5; goto cleanup;
        }
        if (bExit != 0) {
            wchar_t msg[768];
            wsprintfW(msg,
                      L"辅助程序 B 异常退出，退出码 = %lu。\n\n"
                      L"为避免主程序在错误的环境下运行，已中止启动。\n\n"
                      L"如需排查，请在打包时勾选「记录运行日志」。",
                      (unsigned long)bExit);
            FailMsg(msg);
            rc = 6; goto cleanup;
        }

        if (cfg.flags & STUB_FLAG_CLEANUP_AFTER) {
            if (DeleteWithRetry(bPath, 5, 20)) { bExtracted = false; LogLine(L"B 已删除"); }
            else LogLine(L"B 删除失败，留待退出时再试");
        }

        /* --- 2. 合并 B 造成的环境变化 --- */
        EnvMap envAfter = SnapshotEnv();
        int merged = MergeEnvDiff(envBefore, envAfter);
        LogLine(L"环境 diff 合并完成，变更 %d 项", merged);
    }

    /* --- 3. 运行 A --- */
    aWorkDir = aDir;
    if (extractA) {
        if (!ExtractPayload(cfg.aOffset, cfg.aSize, aPath)) {
            FailMsg(L"释放主程序 A 失败。\n\n目标目录: " + aDir);
            rc = 7; goto cleanup;
        }
        aExtracted = true;
    } else {
        aWorkDir = DirNameOf(aPath);
        if (GetFileAttributesW(aPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            FailMsg(L"找不到主程序 A。\n\n应该在: " + aPath +
                    L"\n\n请在打包时勾选「解压并运行 A」，"
                    L"或把 A 放在生成程序的同目录。");
            rc = 9; goto cleanup;
        }
    }

    {
        DWORD aExit = 0;
        /* A 走正常启动：它自己的控制台窗口是它自己的事。
           RUN 历史：这里曾跟着 B 一起加 CREATE_NO_WINDOW，但 A 若是需要交互的
           控制台程序就会被整个藏起来，所以去掉了。
           躯干本身已是 GUI 子系统，不会再多出一个控制台。（见 WinMain） */
        if (!RunAndWait(aPath, aWorkDir, 0, false, &aExit)) {
            FailMsg(L"主程序 A 启动失败。\n\n路径: " + aPath);
            rc = 8; goto cleanup;
        }
        rc = (int)aExit;
        LogLine(L"A 退出，退出码 = %d", rc);
    }

cleanup:
    if (cfg.flags & STUB_FLAG_CLEANUP_AFTER) {
        /* 直跑模式下 A 是用户自己的文件，绝不能删 */
        if (aExtracted && DeleteWithRetry(aPath, 5, 20)) LogLine(L"A 已删除");
        if (bExtracted && DeleteWithRetry(bPath, 5, 20)) LogLine(L"B 已删除（补删）");
    }
    RemoveDirectoryW(tempDir.c_str());

    LogLine(L"===== 结束，返回码 %d =====", rc);

    /* 自身删除放在最后：它会改名当前 exe，之后不能再依赖 g_selfPath */
    if (cfg.flags & STUB_FLAG_SELF_DELETE) {
        SelfDelete(g_logPath, g_logEnabled);
    }

    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return rc;
}

/* GUI 子系统的入口。改子系统前这里是 wmain —— 控制台子系统会被系统分配
   一个控制台窗口，运行生成的程序时会弹出黑框，所以换成了 WinMain。 */
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    return StubMain();
}
