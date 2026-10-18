/* ============================================================================
 * shared_config.h
 *
 * 打包器 (WuSi_Attacher.exe) 与躯干模板 (build\stub.exe) 之间共享的数据契约。
 *
 * 生成的 exe 的磁盘布局：
 *
 *   [ 完整的 stub.exe 镜像 ]          <-- 含 PE 头、asInvoker 清单，原样拷贝
 *   [ B.exe 的原始字节 ]
 *   [ A.exe 的原始字节 ]
 *   [ StubMeta（可选，版本信息字符串） ]
 *   [ StubTrailer 定长结构（在文件最末尾） ]
 *
 * 配置块刻意放在**最末尾**，躯干只需从文件尾往回读定长结构就能拿到全部信息：
 * 不扫描、无歧义、O(1)。早期版本把配置块放在躯干之后靠扫描 magic 定位，
 * 结果躯干自身的 .rdata 里也含有同样的 magic 常量，极易误命中，已废弃。
 * ==========================================================================*/

#ifndef SHARED_CONFIG_H
#define SHARED_CONFIG_H

#include <windows.h>

/* ---- 版本控制：改 StubConfig 布局必须 +1 -------------------------------- */
#define STUB_CONFIG_VERSION 3u

/* A 文件名槽位长度（含结尾 '\0'）。不填则默认找 "A.exe"。 */
#define STUB_A_NAME_MAX 64

/* 解压 A 的目的地类别。
   为什么需要这个：A 常常会检查自己"是不是从压缩包/临时目录里跑起来的"，
   而 %TEMP% 正好会触发这种检测（表现就是提示"不要在压缩包内运行"）。
   所以默认释放到**生成程序自己的目录**，那是一个正常路径。 */
#define STUB_DEST_EXE_DIR   0   /* 生成程序所在目录（默认） */
#define STUB_DEST_TEMP      1   /* 系统临时目录             */
#define STUB_DEST_DOWNLOADS 2
#define STUB_DEST_DOCUMENTS 3
#define STUB_DEST_DESKTOP   4

/* 解压目标下再建一层子目录名（含结尾 '\0'）。留空则直接放在目的地里。 */
#define STUB_SUBDIR_MAX 64

/* ---- 尾部配置块的识别串：确认确实读到了配置块，而不是把垃圾数据当配置 --- */
#define STUB_TRAILER_MAGIC "WuSiStubTrailer.v1.9f3a1c7d"

/* ---- 打包器尾部用于定位躯干模板的标记（与上面的 trailer 是两回事） ------ */
#define STUB_TEMPLATE_MAGIC "WuSiPackerTemplate.v1.Slot"

/* ---- 命令行开关：躯干被提权重启时给自己加的标记，防无限提权循环 --------- */
#define STUB_ELEVATED_FLAG_W L"--wusi-elevated"

/* ---- 配置结构体（必须与 tests\pack_test.ps1 里的 CFG_SIZE 一致 = 136） -- */
#pragma pack(push, 1)
typedef struct StubConfig {
    unsigned int  configVersion;        /* 必须 == STUB_CONFIG_VERSION        */
    unsigned int  flags;                /* STUB_FLAG_* 位或                   */

    unsigned long long bOffset;         /* B 在生成文件中的绝对偏移           */
    unsigned long long bSize;           /* B 的字节数                         */
    unsigned long long aOffset;         /* A 在生成文件中的绝对偏移           */
    unsigned long long aSize;           /* A 的字节数                         */

    unsigned int  bWaitTimeoutMs;       /* B 的等待超时，0 = 无限等待         */
    unsigned int  destKind;             /* STUB_DEST_* 解压目的地类别         */
    unsigned int  reserved[6];          /* 留白，便于扩展而不改版本号         */

    /* 不解压 A 时，躯干要去磁盘上找它。这是文件名（相对躯干所在目录），
       留空则默认 "A.exe"。允许带子目录，例如 "app\\main.exe"。 */
    char          aName[STUB_A_NAME_MAX];

    /* 解压目的地下的子目录名。留空 = 直接放在目的地（例如就放在程序同目录）。 */
    char          subDir[STUB_SUBDIR_MAX];
} StubConfig;

/* 文件最末尾的定长结构：躯干读它就能拿到全部配置。
   trailerMagic 放在最后，任何截断或错位都会导致校验失败。 */
typedef struct StubTrailer {
    StubConfig   config;                /* 72 字节                            */
    unsigned int metaOffset;            /* StubMeta 的绝对偏移，0 = 无        */
    unsigned int metaSize;              /* StubMeta 的字节数                  */
    unsigned int resInfoOffset;         /* ResInfo 的绝对偏移，0 = 无         */
    unsigned int resInfoSize;           /* ResInfo 的字节数                   */
    char         trailerMagic[32];      /* 32 字节，末尾是 '\0'               */
} StubTrailer;                          /* 共 120 字节                        */
#pragma pack(pop)

#define STUB_TRAILER_SIZE       ((unsigned int)sizeof(StubTrailer))
#define STUB_TRAILER_MAGIC_LEN  ((unsigned int)(sizeof(STUB_TRAILER_MAGIC) - 1))

/* ---- 版本信息字段 -------------------------------------------------------- */
enum StubMetaField {
    META_COMPANY = 0,
    META_DESCRIPTION,
    META_COPYRIGHT,
    META_PRODUCT,
    META_FILEVERSION,
    META_PRODUCTVERSION,
    META_ORIGINALNAME,
    META_INTERNALNAME,
    META_COUNT
};

/* 软性上限：版本信息块的预留空间。实际按内容自动收缩，填满则截断。 */
#define STUB_META_MAX_BYTES 16384u

/* ---- 资源注入结果：躯干靠它挑图标 --------------------------------------- */
#pragma pack(push, 1)
typedef struct ResInfo {
    unsigned short langId;              /* 资源语言 ID，例如 1033            */
    unsigned short groupIconId;         /* RT_GROUP_ICON 的 ID               */
} ResInfo;
#pragma pack(pop)

/* ---- flags 位定义 -------------------------------------------------------- */
#define STUB_FLAG_REQUIRE_ADMIN     0x00000001u /* 启动时自提权              */
#define STUB_FLAG_CREATE_NO_WINDOW  0x00000002u /* 子进程用 CREATE_NO_WINDOW */
#define STUB_FLAG_CLEANUP_AFTER     0x00000004u /* 跑完清理释放出来的文件    */
#define STUB_FLAG_ENABLE_LOG        0x00000008u /* 写运行日志，便于排查      */
#define STUB_FLAG_SELF_DELETE       0x00000010u /* 跑完把自己也删掉          */
/* A 的处理方式：
   置位 = 从自身解压 A 到临时目录再运行（适合 A 需要"落地"才能跑的场景）；
   清零 = 直接在磁盘上运行 A（不占临时目录、启动更快）。
   两种模式下 A 都必须被嵌进生成的 exe；清零时改为在躯干同目录找它。 */
#define STUB_FLAG_EXTRACT_A         0x00000020u

/* 默认 B 等待超时：60 秒。B 正常是毫秒级，超时说明它挂住了 */
#define STUB_DEFAULT_B_TIMEOUT_MS 60000u

#endif /* SHARED_CONFIG_H */
