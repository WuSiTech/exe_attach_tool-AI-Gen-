// tests/test_b.cpp —— 测试用辅助程序 B
//
// 职责：制造一个「磁盘上的持久改变」，然后退出。
// 真实场景里这个改变通常是写注册表环境变量；但 DSH 沙箱禁止一切注册表写入，
// 所以验证时改用文件承载这个改变 —— 对「释放 B -> 跑 B -> 等退出 -> 清理 B -> 跑 A」
// 这条链路的验证是等价的。

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

int main(void)
{
    char path[MAX_PATH];
    DWORD t = GetTempPathA(MAX_PATH, path);
    if (t == 0 || t >= MAX_PATH) { printf("B: GetTempPath failed\n"); return 1; }
    strcat(path, "wusi_test_b_marker.txt");

    FILE* f = fopen(path, "w");
    if (!f) {
        printf("B: cannot create marker at %s (errno=%d)\n", path, errno);
        return 1;
    }
    fprintf(f, "B_RAN_OK\n");
    fclose(f);

    printf("B: wrote marker -> %s\n", path);
    return 0;
}
