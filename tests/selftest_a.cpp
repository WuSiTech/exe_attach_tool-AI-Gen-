/* ============================================================================
 * tests/selftest_a.cpp —— 自检用主程序 A
 *
 * 干三件事：
 *   1. 检查 B 留下的标记（验证「B 先跑、A 后跑」的顺序与可见性）
 *   2. 记录自己的所在目录（验证"就地运行、不在压缩包里"）
 *   3. 找到宿主（生成的那个单文件）并把它结束掉
 *
 * 第 3 点很关键：自检如果靠外部超时硬杀宿主，容易留下孤儿进程，
 * 让"打包器有没有崩溃"这个判断变脏。让 A 自己收尾，宿主就是正常退出。
 *
 * 只在自检链路里使用；正式生成的程序里 A 是用户自己的程序，不带这段逻辑。
 * ==========================================================================*/
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <string>

/* 找名字里含 wusi_selftest 的其它进程（那就是我们的宿主），结束它。
   排除自己，避免自杀。 */
static void KillHost(void)
{
    DWORD me = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    DWORD parent = 0;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == me) { parent = pe.th32ParentProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }

    if (parent) {
        HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, parent);
        if (p) {
            /* 稍等一下再结束宿主：让宿主有机会收尾（清理解压目录等）。
               宿主是"运行 A 并等它退出"的状态，正常应该自己退出；
               这里只是兜底，防止某些情况下 A 先退出而宿主还在等。 */
            Sleep(1500);
            TerminateProcess(p, 0);
            CloseHandle(p);
        }
    }
    CloseHandle(snap);
}

int main(void)
{
    wchar_t tmp[MAX_PATH] = {0};
    DWORD t = GetTempPathW(MAX_PATH, tmp);
    if (t == 0 || t >= MAX_PATH) tmp[0] = 0;

    wchar_t bPath[MAX_PATH];
    wsprintfW(bPath, L"%sselftest_b.txt", tmp);

    int sawB = 0;
    FILE* bf = _wfopen(bPath, L"rb");
    if (bf) {
        char line[64] = {0};
        if (fgets(line, sizeof(line), bf) && strstr(line, "B_RAN_OK")) sawB = 1;
        fclose(bf);
    }

    /* 自己的目录 —— 结果文件就写在这里，外层能直接检查 */
    wchar_t self[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    wchar_t* slash = wcsrchr(self, L'\\');
    std::wstring dir;
    if (slash) { *slash = 0; dir = self; } else { dir = L"."; }

    wchar_t aOut[MAX_PATH];
    wsprintfW(aOut, L"%s\\selftest_a.txt", dir.c_str());
    FILE* af = _wfopen(aOut, L"wb");
    if (af) {
        fprintf(af, "A_RAN_OK\r\n");
        fprintf(af, "saw_B=%s\r\n", sawB ? "YES" : "NO");
        fprintf(af, "self_dir=%ls\r\n", dir.c_str());
        fclose(af);
    }

    KillHost();
    return 0;
}
