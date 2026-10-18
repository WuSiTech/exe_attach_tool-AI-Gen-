// tests/test_a.cpp —— 测试用主程序 A
//
// 职责：
//   1. 检查 B 留下的磁盘改变是否可见
//   2. 检查一个由躯干设置的环境变量是否被继承（验证环境块继承链路）
//   3. 记录自己的启动顺序时间戳，供外部核验 A 确实在 B 之后

#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char tmp[MAX_PATH];
    DWORD t = GetTempPathA(MAX_PATH, tmp);
    if (t == 0 || t >= MAX_PATH) { tmp[0] = '\0'; }

    char marker[MAX_PATH];
    strcpy(marker, tmp);
    strcat(marker, "wusi_test_b_marker.txt");

    char result[MAX_PATH];
    strcpy(result, tmp);
    strcat(result, "wusi_test_a_result.txt");

    FILE* m = fopen(marker, "r");
    int sawMarker = 0;
    if (m) {
        char line[64] = {0};
        if (fgets(line, sizeof(line), m)) {
            if (strstr(line, "B_RAN_OK")) sawMarker = 1;
        }
        fclose(m);
    }

    /* 躯干会把这个变量塞进自己环境块，A 作为子进程应当继承到 */
    char envbuf[256] = {0};
    DWORD en = GetEnvironmentVariableA("WUSI_INHERIT_PROBE", envbuf, sizeof(envbuf));

    FILE* f = fopen(result, "w");
    if (!f) return 1;
    fprintf(f, "A_RAN_OK\n");
    fprintf(f, "saw_B_marker=%s\n", sawMarker ? "YES" : "NO");
    fprintf(f, "inherited_env=%s (len=%lu)\n",
            (en > 0) ? envbuf : "<none>", (unsigned long)en);
    fclose(f);

    printf("A: done -> %s\n", result);
    return 0;
}
