/* ============================================================================
 * tests/selftest_b.cpp —— 自检用辅助程序 B
 *
 * 模拟"先运行、改变环境、然后退出"的辅助程序：
 * 在 %TEMP% 下写一个标记文件，证明自己确实跑过。
 * 落点刻意选 %TEMP%，这样即使躯干最后把解压目录删了，标记也还在。
 * ==========================================================================*/
#include <windows.h>
#include <stdio.h>

int main(void)
{
    wchar_t tmp[MAX_PATH] = {0};
    DWORD t = GetTempPathW(MAX_PATH, tmp);
    if (t == 0 || t >= MAX_PATH) return 1;

    wchar_t marker[MAX_PATH];
    wsprintfW(marker, L"%sselftest_b.txt", tmp);

    FILE* f = _wfopen(marker, L"wb");
    if (!f) return 2;
    fputs("B_RAN_OK\r\n", f);
    fclose(f);
    return 0;
}
