/*
 * RelightAE.h
 * Relight Anime: 深度パスから光を注入する SmartFX エフェクト。
 * 照明モデルは TypeGPU の作例 "Monocular Light Injection" (MIT, Software Mansion) を移植したもの。
 *   - 深度の勾配から面の向きを作る (ノイズを抑える勾配制限つき)
 *   - 深度の高さ場から凹みの暗さ (アンビエントオクルージョン) を出す
 *   - 深度をレイマーチして落ち影を付ける
 *   - 点光源 (最大 3 個) を当てる。光の玉そのものとにじみも描く
 * アニメ向けに足したもの:
 *   - Look = Anime: 元の絵を暗くせず、影の部分に影色を掛け、光を足す (Cinematic は原典どおり)
 *   - セル調の陰影 (段数とぼかし)、リムライト、線画の保護、深度の膨らみ (平たい深度に丸みを付ける)
 *   - プリセット
 * 深度の生成と結線は RelightAuto (relight_depth.exe を起動して自動で読み込む)。
 */
#pragma once

#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AEGP_SuiteHandler.h"
#include "AEFX_SuiteHelper.h"
#include "Smart_Utils.h"
#include "RelightAE_Flags.h"
#include "RelightCore.h"   /* 描画の設定 (RenderParams) と照明の計算 (CPU と GPU で共通) */

#define RELIGHT_DESCRIPTION "Depth-based light injection for AI anime footage (TypeGPU light model)."


/* パラメータ index。順序を変えたら ParamsSetup も同じ順序で追加すること */
enum {
	RELIGHT_INPUT = 0,
	RELIGHT_PRESET,              /* プリセット。選ぶと各値を書き換え、表示は「Select...」に戻る */

	/* --- Auto (パス自動生成) --- */
	RELIGHT_TOPIC_AUTO,
	RELIGHT_AUTO_ENABLE,         /* 適用時に自動でパス生成・結線する */
	RELIGHT_AUTO_ENGINE,         /* 深度を作る場所 1: Local GPU (この PC)  2・3: Remote (別の PC の深度サーバー。Fast / Quality) */
	RELIGHT_AUTO_GENERATE,       /* ボタン: 再生成 */
	RELIGHT_AUTO_CLEAN,          /* ボタン: 使われていない深度キャッシュを消す */
	RELIGHT_TOPIC_AUTO_END,

	/* --- Light (1 個目。常に有効)。リムライトの設定は光源ごと (Rim の中) --- */
	RELIGHT_TOPIC_LIGHT,
	RELIGHT_LIGHT_POSITION,      /* 2D ポイント。コンポビューでドラッグできる */
	RELIGHT_LIGHT_HEIGHT,        /* 手前方向の位置 (画面の高さ = 1)。0 が最も手前の面、負なら物の後ろ */
	RELIGHT_LIGHT_COLOR,
	RELIGHT_LIGHT_INTENSITY,
	RELIGHT_LIGHT_RANGE,         /* 光の届く範囲の倍率 */
	RELIGHT_SHOW_LIGHT,          /* 光の玉とにじみを描く */
	RELIGHT_TOPIC_LIGHT_RIM,
	RELIGHT_LIGHT_RIM,           /* この光源でリムライトを付けるか */
	RELIGHT_RIM,                 /* リムライトの強さ */
	RELIGHT_RIM_WIDTH,           /* リムライトの幅 (1080p でのピクセル数) */
	RELIGHT_RIM_SOFTNESS,        /* リムの内側の境目のぼかし (0 = くっきりした帯) */
	RELIGHT_RIM_SMOOTH,          /* リムの輪郭のならし (細かいでこぼこを無視したまっすぐな線にする) */
	RELIGHT_RIM_PLACEMENT,       /* 1: Auto  2: Light Side  3: All Around */
	RELIGHT_RIM_COLOR,           /* リムの色 (光源の色に掛ける) */
	RELIGHT_RIM_SPREAD,          /* リムの回り込みの広さ */
	RELIGHT_RIM_REACH,           /* リムが届く距離 */
	RELIGHT_TOPIC_LIGHT_RIM_END,
	RELIGHT_TOPIC_LIGHT_END,

	/* --- Light 2 / Light 3 (並びは Light と同じで、先頭に Enable) --- */
	RELIGHT_TOPIC_LIGHT2,
	RELIGHT_LIGHT2_ENABLE,
	RELIGHT_LIGHT2_POSITION,
	RELIGHT_LIGHT2_HEIGHT,
	RELIGHT_LIGHT2_COLOR,
	RELIGHT_LIGHT2_INTENSITY,
	RELIGHT_LIGHT2_RANGE,
	RELIGHT_LIGHT2_SHOW,
	RELIGHT_TOPIC_LIGHT2_RIM,
	RELIGHT_LIGHT2_RIM,
	RELIGHT_LIGHT2_RIM_AMOUNT,
	RELIGHT_LIGHT2_RIM_WIDTH,
	RELIGHT_LIGHT2_RIM_SOFTNESS,
	RELIGHT_LIGHT2_RIM_SMOOTH,
	RELIGHT_LIGHT2_RIM_PLACEMENT,
	RELIGHT_LIGHT2_RIM_COLOR,
	RELIGHT_LIGHT2_RIM_SPREAD,
	RELIGHT_LIGHT2_RIM_REACH,
	RELIGHT_TOPIC_LIGHT2_RIM_END,
	RELIGHT_TOPIC_LIGHT2_END,

	RELIGHT_TOPIC_LIGHT3,
	RELIGHT_LIGHT3_ENABLE,
	RELIGHT_LIGHT3_POSITION,
	RELIGHT_LIGHT3_HEIGHT,
	RELIGHT_LIGHT3_COLOR,
	RELIGHT_LIGHT3_INTENSITY,
	RELIGHT_LIGHT3_RANGE,
	RELIGHT_LIGHT3_SHOW,
	RELIGHT_TOPIC_LIGHT3_RIM,
	RELIGHT_LIGHT3_RIM,
	RELIGHT_LIGHT3_RIM_AMOUNT,
	RELIGHT_LIGHT3_RIM_WIDTH,
	RELIGHT_LIGHT3_RIM_SOFTNESS,
	RELIGHT_LIGHT3_RIM_SMOOTH,
	RELIGHT_LIGHT3_RIM_PLACEMENT,
	RELIGHT_LIGHT3_RIM_COLOR,
	RELIGHT_LIGHT3_RIM_SPREAD,
	RELIGHT_LIGHT3_RIM_REACH,
	RELIGHT_TOPIC_LIGHT3_RIM_END,
	RELIGHT_TOPIC_LIGHT3_END,

	/* --- Scene --- */
	RELIGHT_TOPIC_SCENE,
	RELIGHT_LOOK,                /* 1: Anime  2: Cinematic (原典) */
	RELIGHT_EXPOSURE,            /* 元映像の明るさ (環境光)。下げるほど光が際立つ */
	RELIGHT_AMBIENT_COLOR,
	RELIGHT_RELIEF,              /* 深度から作る凹凸の強さ */
	RELIGHT_SPECULAR,
	RELIGHT_SHADOW,              /* 落ち影の濃さ */
	RELIGHT_OCCLUSION,           /* 凹みの暗さ */
	RELIGHT_TOPIC_SCENE_END,

	/* --- Anime Style --- */
	RELIGHT_TOPIC_ANIME,
	RELIGHT_FORM,                /* 面の向きによる立体的な陰影の割合 */
	RELIGHT_LIGHT_SPREAD,        /* 撮影寄りのグラデーションの広がり */
	RELIGHT_SHADOW_COLOR,        /* 光が当たらない側に掛ける色 */
	RELIGHT_SHADOW_TINT,         /* 影色の強さ */
	RELIGHT_SHADE_SMOOTH,        /* 陰影をならす (細かい凹凸を拾わず、大きな面で効かせる。輪郭はまたがない) */
	RELIGHT_EDGE_DITHER,         /* 輪郭に沿って効果をならし、ディザで段を散らす */
	RELIGHT_CEL,                 /* セル調の強さ (0 = なめらか) */
	RELIGHT_CEL_STEPS,           /* 影の段数 */
	RELIGHT_CEL_SOFTNESS,        /* 段の境目のぼかし */
	RELIGHT_LINE_PROTECT,       /* 線画を光と影から守る強さ */
	RELIGHT_LINE_THRESHOLD,      /* 線とみなす暗さ */
	RELIGHT_VOLUME,              /* 深度の膨らみ (平たく推定された人物に丸みを付ける) */
	RELIGHT_TOPIC_ANIME_END,

	/* --- Passes --- */
	RELIGHT_TOPIC_PASSES,
	RELIGHT_DEPTH_LAYER,
	RELIGHT_DEPTH_NEAR,          /* 1: near = white   2: near = black */
	RELIGHT_DEPTH_STABILIZE,     /* 1: Off  2: 前後 1 フレーム  3: 前後 2 フレーム */
	RELIGHT_DEPTH_SNAP,          /* 深度の境目を素材の色の境目 (線画) に合わせる */
	RELIGHT_NORMAL_SOURCE,       /* 1: 深度から作る   2: Normal Layer を使う */
	RELIGHT_NORMAL_LAYER,
	RELIGHT_PASS_ENCODING,       /* 1: Linear (PNG / Preserve RGB)  2: sRGB (AE が EXR を変換した場合に逆変換) */
	RELIGHT_MASK_LAYER,
	RELIGHT_MASK_SOURCE,         /* 1: luminance  2: alpha */
	RELIGHT_INPUT_LINEAR,        /* 入力がリニア (リニア作業空間) */
	RELIGHT_USE_GPU,             /* GPU (CUDA) で描く */
	RELIGHT_TOPIC_PASSES_END,

	/* --- Output --- */
	RELIGHT_OUTPUT_MODE,         /* 1: Relit 2: Depth 3: Normals 4: Shadow & Occlusion 5: Line Mask */
	RELIGHT_MIX,

	RELIGHT_NUM_PARAMS
};

/* パラメータ ID (保存互換用。index と別に固定。既存の値は変えないこと) */
enum {
	ID_PRESET = 30,

	ID_TOPIC_AUTO = 40,
	ID_AUTO_ENABLE,
	ID_AUTO_GENERATE,
	ID_TOPIC_AUTO_END,
	ID_AUTO_CLEAN,

	ID_TOPIC_LIGHT = 300,
	ID_LIGHT_POSITION,
	ID_LIGHT_HEIGHT,
	ID_LIGHT_COLOR,
	ID_LIGHT_INTENSITY,
	ID_SHOW_LIGHT,
	ID_TOPIC_LIGHT_END,

	ID_TOPIC_SCENE = 320,
	ID_EXPOSURE,
	ID_AMBIENT_COLOR,
	ID_RELIEF,
	ID_SPECULAR,
	ID_SHADOW,
	ID_OCCLUSION,
	ID_TOPIC_SCENE_END,
	ID_LOOK = 329,

	ID_TOPIC_PASSES = 340,
	ID_DEPTH_LAYER,
	ID_DEPTH_NEAR,
	ID_NORMAL_SOURCE,
	ID_NORMAL_LAYER,
	ID_PASS_ENCODING,
	ID_MASK_LAYER,
	ID_MASK_SOURCE,
	ID_INPUT_LINEAR,
	ID_TOPIC_PASSES_END,

	ID_OUTPUT_MODE = 360,
	ID_MIX,

	ID_TOPIC_LIGHT2 = 380,
	ID_LIGHT2_ENABLE,
	ID_LIGHT2_POSITION,
	ID_LIGHT2_HEIGHT,
	ID_LIGHT2_COLOR,
	ID_LIGHT2_INTENSITY,
	ID_TOPIC_LIGHT2_END,

	ID_TOPIC_LIGHT3 = 390,
	ID_LIGHT3_ENABLE,
	ID_LIGHT3_POSITION,
	ID_LIGHT3_HEIGHT,
	ID_LIGHT3_COLOR,
	ID_LIGHT3_INTENSITY,
	ID_TOPIC_LIGHT3_END,

	ID_TOPIC_ANIME = 400,
	ID_SHADOW_COLOR,
	ID_SHADOW_TINT,
	ID_CEL,
	ID_CEL_STEPS,
	ID_CEL_SOFTNESS,
	ID_RIM,
	ID_RIM_WIDTH,
	ID_LINE_PROTECT,
	ID_LINE_THRESHOLD,
	ID_VOLUME,
	ID_TOPIC_ANIME_END,
	ID_RIM_SOFTNESS,
	ID_RIM_PLACEMENT,
	ID_RIM_COLOR,
	ID_SHADE_SMOOTH,
	ID_DEPTH_SNAP,
	ID_EDGE_DITHER,
	ID_USE_GPU,
	ID_RIM_SPREAD,
	ID_RIM_REACH,
	ID_FORM,
	ID_LIGHT_SPREAD,
	ID_DEPTH_STABILIZE,
	ID_LIGHT_RIM,
	ID_LIGHT2_RIM,
	ID_LIGHT3_RIM,

	/* 光源ごとの範囲・光の玉・リム (Light 1 のリムは上の ID_RIM 〜 ID_RIM_REACH をそのまま使い、以前の値を引き継ぐ) */
	ID_LIGHT_RANGE = 430,
	ID_RIM_SMOOTH,
	ID_TOPIC_LIGHT_RIM,
	ID_TOPIC_LIGHT_RIM_END,

	ID_LIGHT2_RANGE = 440,
	ID_LIGHT2_SHOW,
	ID_TOPIC_LIGHT2_RIM,
	ID_LIGHT2_RIM_AMOUNT,
	ID_LIGHT2_RIM_WIDTH,
	ID_LIGHT2_RIM_SOFTNESS,
	ID_LIGHT2_RIM_SMOOTH,
	ID_LIGHT2_RIM_PLACEMENT,
	ID_LIGHT2_RIM_COLOR,
	ID_LIGHT2_RIM_SPREAD,
	ID_LIGHT2_RIM_REACH,
	ID_TOPIC_LIGHT2_RIM_END,

	ID_LIGHT3_RANGE = 460,
	ID_LIGHT3_SHOW,
	ID_TOPIC_LIGHT3_RIM,
	ID_LIGHT3_RIM_AMOUNT,
	ID_LIGHT3_RIM_WIDTH,
	ID_LIGHT3_RIM_SOFTNESS,
	ID_LIGHT3_RIM_SMOOTH,
	ID_LIGHT3_RIM_PLACEMENT,
	ID_LIGHT3_RIM_COLOR,
	ID_LIGHT3_RIM_SPREAD,
	ID_LIGHT3_RIM_REACH,
	ID_TOPIC_LIGHT3_RIM_END,

	ID_AUTO_ENGINE = 480
};

/* PreRender → SmartRender に渡す情報 */
struct PreRenderData {
	A_long originX;   /* 出力ワールド左上のレイヤー座標 (ダウンサンプル後) */
	A_long originY;
	/* 補助レイヤー (深度・法線・マスク) は全体を取り出す。その範囲 (補助レイヤー自身の座標) */
	PF_LRect auxRect[3];
	bool     auxChecked[3];   /* 予約できた (SmartRender で取り出してよい) */
	/* 深度を時間方向にならすための前後のフレーム */
	int      nbCount;
	int      nbOff[4];        /* 時刻の差 (フレーム) */
	bool     nbChecked[4];
};

extern "C" {
	DllExport PF_Err EffectMain(
		PF_Cmd        cmd,
		PF_InData    *in_data,
		PF_OutData   *out_data,
		PF_ParamDef  *params[],
		PF_LayerDef  *output,
		void         *extra);
}
