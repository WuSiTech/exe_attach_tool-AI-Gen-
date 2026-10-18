/* ============================================================================
 * progress.h —— 生成进度对话框接口
 * ==========================================================================*/
#ifndef WUSI_PROGRESS_H
#define WUSI_PROGRESS_H

#include <windows.h>

/* 阶段编号（与 progress.cpp 的 StageText 对应） */
#define PSTAGE_PREPARE  0
#define PSTAGE_RESOURCE 1
#define PSTAGE_WRITE_B  2
#define PSTAGE_WRITE_A  3
#define PSTAGE_VERIFY   4
#define PSTAGE_DONE     5

HWND ProgressCreate(HWND parent);
void ProgressSet(HWND dlg, int stage, int pct);   /* pct < 0 = 不确定进度 */
void ProgressPump(HWND dlg);                     /* 让进度条动起来 */
void ProgressDestroy(HWND dlg);

#endif /* WUSI_PROGRESS_H */
