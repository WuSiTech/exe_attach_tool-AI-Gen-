// tests/diag_res.cpp —— 独立检查生成的 exe：遍历资源树 + 解析 RT_VERSION 格式
#include <windows.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <string.h>

static std::vector<unsigned char> g;
static size_t gRsrcOff = 0;

static const char* TypeName(unsigned t)
{
    switch (t) {
    case 1:  return "CURSOR";
    case 2:  return "BITMAP";
    case 3:  return "ICON";
    case 4:  return "MENU";
    case 14: return "GROUP_ICON";
    case 16: return "VERSION";
    case 24: return "MANIFEST";
    default: return "?";
    }
}

static unsigned long long RvaToOff(unsigned rva)
{
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)&g[0];
    size_t nt = dos->e_lfanew;
    IMAGE_FILE_HEADER* fh = (IMAGE_FILE_HEADER*)(&g[0] + nt + 4);
    size_t oh = nt + 4 + sizeof(IMAGE_FILE_HEADER);
    IMAGE_SECTION_HEADER* sec =
        (IMAGE_SECTION_HEADER*)(&g[0] + oh + fh->SizeOfOptionalHeader);
    for (int i = 0; i < fh->NumberOfSections; ++i) {
        if (rva >= sec[i].VirtualAddress &&
            rva < sec[i].VirtualAddress + sec[i].Misc.VirtualSize)
            return rva - sec[i].VirtualAddress + sec[i].PointerToRawData;
    }
    return 0;
}

static void DumpTree()
{
    const IMAGE_RESOURCE_DIRECTORY* root = (const IMAGE_RESOURCE_DIRECTORY*)(&g[gRsrcOff]);
    unsigned n = root->NumberOfNamedEntries + root->NumberOfIdEntries;
    printf("根目录: named=%u id=%u\n", root->NumberOfNamedEntries, root->NumberOfIdEntries);
    const IMAGE_RESOURCE_DIRECTORY_ENTRY* e =
        (const IMAGE_RESOURCE_DIRECTORY_ENTRY*)(&g[gRsrcOff] + sizeof(*root));

    for (unsigned i = 0; i < n; ++i) {
        unsigned type = e[i].Name;
        printf("  类型 %u (%s) dirOff=0x%X\n", type, TypeName(type), e[i].OffsetToDirectory);
        size_t tdir = gRsrcOff + e[i].OffsetToDirectory;
        const IMAGE_RESOURCE_DIRECTORY* td = (const IMAGE_RESOURCE_DIRECTORY*)(&g[tdir]);
        unsigned tn = td->NumberOfNamedEntries + td->NumberOfIdEntries;
        const IMAGE_RESOURCE_DIRECTORY_ENTRY* te =
            (const IMAGE_RESOURCE_DIRECTORY_ENTRY*)(&g[tdir] + sizeof(*td));
        for (unsigned j = 0; j < tn; ++j) {
            printf("      名字 %u\n", te[j].Name);
            size_t ldir = gRsrcOff + te[j].OffsetToDirectory;
            const IMAGE_RESOURCE_DIRECTORY* ld = (const IMAGE_RESOURCE_DIRECTORY*)(&g[ldir]);
            unsigned ln = ld->NumberOfNamedEntries + ld->NumberOfIdEntries;
            const IMAGE_RESOURCE_DIRECTORY_ENTRY* le =
                (const IMAGE_RESOURCE_DIRECTORY_ENTRY*)(&g[ldir] + sizeof(*ld));
            for (unsigned k = 0; k < ln; ++k) {
                size_t deo = gRsrcOff + le[k].OffsetToData;
                const IMAGE_RESOURCE_DATA_ENTRY* de =
                    (const IMAGE_RESOURCE_DATA_ENTRY*)(&g[deo]);
                unsigned long long fo = RvaToOff(de->OffsetToData);
                printf("          语言 0x%04X: RVA=0x%X size=%u -> 文件偏移 0x%llX\n",
                       le[k].Name, de->OffsetToData, de->Size, fo);
                if (type == 16 && fo && fo + de->Size <= g.size()) {
                    /* 解析 VS_VERSIONINFO */
                    const unsigned char* p = &g[fo];
                    struct B { unsigned short len, valLen, type; std::wstring key; size_t valOff; };
                    auto parse = [](const unsigned char* q) -> B {
                        B b;
                        b.len    = *(const unsigned short*)(q + 0);
                        b.valLen = *(const unsigned short*)(q + 2);
                        b.type   = *(const unsigned short*)(q + 4);
                        b.key    = (const wchar_t*)(q + 6);
                        size_t kb = (b.key.size() + 1) * 2;
                        b.valOff = (6 + kb + 3) & ~(size_t)3;
                        return b;
                    };
                    B rb = parse(p);
                    printf("          ==> VS_VERSIONINFO: wLength=%u (资源 size=%u) %s\n",
                           rb.len, de->Size, rb.len == de->Size ? "OK" : "*** 不符 ***");
                    printf("              wValueLength=%u wType=%u key=[%ls]\n",
                           rb.valLen, rb.type, rb.key.c_str());
                    size_t co = rb.valOff + rb.valLen;
                    while (co % 4) ++co;
                    int guard = 0;
                    while (co + 6 <= de->Size && guard++ < 10) {
                        B b = parse(p + co);
                        if (b.len == 0 || co + b.len > de->Size) {
                            printf("              *** 子块异常: off=%zu len=%u\n", co, b.len);
                            break;
                        }
                        printf("              子块 off=%zu len=%u valLen=%u type=%u key=[%ls]\n",
                               co, b.len, b.valLen, b.type, b.key.c_str());
                        if (b.key == L"StringFileInfo") {
                            size_t so = co + b.valOff;
                            while (so % 4) ++so;
                            int g2 = 0;
                            while (so + 6 <= de->Size && g2++ < 4) {
                                B s = parse(p + so);
                                if (s.len == 0 || so + s.len > de->Size) break;
                                printf("                语言块 len=%u key=[%ls]\n", s.len, s.key.c_str());
                                size_t eo = so + s.valOff;
                                while (eo % 4) ++eo;
                                int g3 = 0;
                                while (eo + 6 <= so + s.len && g3++ < 24) {
                                    B ent = parse(p + eo);
                                    if (ent.len == 0 || eo + ent.len > so + s.len) break;
                                    std::wstring val;
                                    if (eo + ent.valOff + 2 <= de->Size + fo) {
                                        const wchar_t* vp = (const wchar_t*)(p + eo + ent.valOff);
                                        val = vp;
                                    }
                                    printf("                  [%ls] = [%ls]\n",
                                           ent.key.c_str(), val.c_str());
                                    eo += ent.len;
                                    while (eo % 4) ++eo;
                                }
                                so += s.len;
                                while (so % 4) ++so;
                            }
                        } else if (b.key == L"VarFileInfo") {
                            size_t vo = co + b.valOff;
                            while (vo % 4) ++vo;
                            B v = parse(p + vo);
                            if (v.len && vo + v.len <= de->Size) {
                                unsigned short lang = *(const unsigned short*)(p + vo + v.valOff);
                                unsigned short cp   = *(const unsigned short*)(p + vo + v.valOff + 2);
                                printf("                Translation = 0x%04X, %u\n", lang, cp);
                            }
                        }
                        co += b.len;
                        while (co % 4) ++co;
                    }
                }
            }
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: diag_res <exe>\n"); return 1; }
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wpath, MAX_PATH);
    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("打开失败 %lu\n", GetLastError()); return 1; }
    LARGE_INTEGER li; GetFileSizeEx(h, &li);
    g.resize((size_t)li.QuadPart);
    DWORD rd = 0, tot = 0;
    while (tot < g.size()) {
        if (!ReadFile(h, &g[tot], (DWORD)(g.size() - tot), &rd, NULL) || !rd) break;
        tot += rd;
    }
    CloseHandle(h);
    printf("文件 %zu 字节\n\n", g.size());

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)&g[0];
    size_t nt = dos->e_lfanew;
    IMAGE_FILE_HEADER* fh = (IMAGE_FILE_HEADER*)(&g[0] + nt + 4);
    size_t oh = nt + 4 + sizeof(IMAGE_FILE_HEADER);
    IMAGE_OPTIONAL_HEADER64* o64 = (IMAGE_OPTIONAL_HEADER64*)(&g[0] + oh);
    printf("节数=%u SizeOfImage=0x%X\n", fh->NumberOfSections, o64->SizeOfImage);
    unsigned rsrcRva = o64->DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress;
    unsigned rsrcSize = o64->DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].Size;
    printf("资源目录 RVA=0x%X size=%u\n\n", rsrcRva, rsrcSize);
    if (!rsrcRva) { printf("没有资源目录\n"); return 1; }
    gRsrcOff = (size_t)RvaToOff(rsrcRva);
    if (!gRsrcOff) { printf("RVA 转换失败\n"); return 1; }
    printf("资源目录文件偏移 = 0x%zX\n\n", gRsrcOff);
    DumpTree();
    return 0;
}
