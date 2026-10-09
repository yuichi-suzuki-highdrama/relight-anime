/*
 * RelightGPU.h
 * GPU 版 (CUDA) の入口。中身は RelightGPU.cu。
 * 計算は RelightCore.h の関数をそのまま GPU で動かすので、CPU 版と同じ絵になる。
 * このヘッダは CUDA のヘッダを取り込まない (CPU 側のコンパイラからも使えるように)。
 */
#pragma once

#include "RelightCore.h"

struct RLGpuFrame {
	/* 呼ぶ側が入れるもの: rp, outW, outH, originX, originY, layerW, layerH, normalA, maskA (GPU のワールド)。
	   ほか (格子・光源・表) は RelightGPU_Render が埋める */
	ShadeContext ctx;
	RLImage      src;          /* 入力 (GPU のワールド、BGRA float) */
	AuxMap       depthMap;     /* 深度ワールドと位置合わせ (img.data が NULL なら深度なし) */
	void        *dst;          /* 出力 (GPU のワールド、BGRA float) */
	int          dstRowbytes;
	/* 前後のフレームの深度ワールド (深度を時間方向にならす。深度ワールドと同じ大きさ) */
	RLImage      depthNb[4];
	int          depthNbOff[4];
	int          depthNbCount;
};

/* 1 フレームを描く。0 なら成功。失敗したら err に理由を書く */
int  RelightGPU_Render(RLGpuFrame &frame, char *err, int errLen);

/* GPU のメモリ (使い回しの作業領域と表) を手放す (PF_Cmd_GPU_DEVICE_SETDOWN で呼ぶ) */
void RelightGPU_Release();
