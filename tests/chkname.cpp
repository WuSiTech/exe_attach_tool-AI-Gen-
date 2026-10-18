#include <windows.h>
#include <stdio.h>
#include <string>
int main(void)
{
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    std::wstring low(self, n);
    printf("self=%ls len=%zu\n", self, low.size());
    for (size_t i = 0; i < low.size(); ++i) low[i] = towlower(low[i]);
    printf("low =%ls\n", low.c_str());
    const wchar_t* want = L"packer-selftest.exe";
    printf("want len=%zu\n", wcslen(want));
    if (low.size() >= 20) {
        std::wstring tail = low.substr(low.size() - 20);
        printf("tail=[%ls]  match=%d\n", tail.c_str(), (int)(tail == want));
    } else {
        printf("too short\n");
    }
    return 0;
}
