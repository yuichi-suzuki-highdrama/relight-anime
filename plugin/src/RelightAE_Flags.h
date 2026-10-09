/*
 * RelightAE_Flags.h
 * PiPL リソース (.r) と C++ の両方から読む共有定義。
 * PiPLtool は列挙子や式を解釈できないため数値を直書きし、
 * RelightAE.cpp 側の static_assert で SDK の列挙値と一致することを保証する。
 */
#ifndef RELIGHTAE_FLAGS_H
#define RELIGHTAE_FLAGS_H

#define RELIGHT_NAME        "Relight Anime"
#define RELIGHT_MATCH_NAME  "SRLM Relight Anime"
#define RELIGHT_CATEGORY    "SRLM"
#define RELIGHT_SUPPORT_URL "https://github.com/"

#define RELIGHT_MAJOR_VERSION 2
#define RELIGHT_MINOR_VERSION 2
#define RELIGHT_BUG_VERSION   0
#define RELIGHT_STAGE_VERSION 0   /* PF_Stage_DEVELOP */
#define RELIGHT_BUILD_VERSION 1

/* PF_VERSION(2,2,0,PF_Stage_DEVELOP,1) = (2<<19) | (2<<15) | 1 */
#define RELIGHT_PIPL_VERSION  1114113

/* PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_CUSTOM_UI (コンポビューの光源ギズモ)
   | PF_OutFlag_SEND_UPDATE_PARAMS_UI (無効な光源の項目を灰色にする) */
#define RELIGHT_OUTFLAGS      0x06008000
/* PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_SUPPORTS_THREADED_RENDERING
   | PF_OutFlag2_SUPPORTS_GPU_RENDER_F32 (GPU 版。実際に使うかは PF_Cmd_GPU_DEVICE_SETUP で CUDA のときだけ返す) */
#define RELIGHT_OUTFLAGS2     0x0A001400

#endif
