// tests/subsystem.cpp —— 报告 PE 的子系统（2=GUI, 3=Console），用于确认黑框问题
#include <windows.h>
#include <stdio.h>
#include <vector>

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        HANDLE h = CreateFileA(argv[i], GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) { printf("%s: 打不开\n", argv[i]); continue; }
        LARGE_INTEGER li; GetFileSizeEx(h, &li);
        std::vector<unsigned char> b((size_t)li.QuadPart);
        DWORD rd = 0; ReadFile(h, &b[0], (DWORD)b.size(), &rd, NULL);
        CloseHandle(h);

        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)&b[0];
        IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)&b[dos->e_lfanew];
        WORD sub = nt->OptionalHeader.Subsystem;
        const char* name = (sub == 2) ? "WINDOWS (GUI，无控制台)"
                         : (sub == 3) ? "CONSOLE (会开黑框)"
                         : "其它";
        printf("%-16s 子系统 = %u  %s\n", argv[i], sub, name);
    }
    return 0;
}
