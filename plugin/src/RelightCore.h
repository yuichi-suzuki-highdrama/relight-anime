/*
 * RelightCore.h
 * 照明の計算そのもの (CPU と GPU で共通)。
 *
 * C++ (CPU 版、確認用ツール) と CUDA (GPU 版、RelightGPU.cu) の両方から取り込む。
 * AE の SDK には依存しない (GPU 側で取り込めるように)。関数は RL_HD で CPU・GPU どちらでも動く。
 * 1 画素・1 マスぶんの処理だけをここに置き、全体の手順 (並列の回し方) は
 *   CPU: RelightAE.cpp の RenderFrameCPU
 *   GPU: RelightGPU.cu の RelightGPU_Render
 * がそれぞれ持つ。同じ関数を呼ぶので、CPU と GPU の結果は (浮動小数の誤差を除いて) 一致する。
 *
 * 照明モデルは TypeGPU の作例 "Monocular Light Injection"
 *   apps/typegpu-docs/src/examples/image-processing/monocular-light-injection/shaders.ts
 *   (MIT License, Copyright (c) 2025 Software Mansion)
 * を移植したもの。定数と式は原典に合わせてある。そのうえでアニメ向けの処理を足している。
 *
 * 座標 (原典の uv 空間を一般のアスペクト比に拡張):
 *   u = 0.5 + (px - W/2) / H,  v = py / H   (px, py はレイヤー座標、H = レイヤーの高さ)
 *   縦は 0..1、横は高さ比で伸びる。z はカメラ側が正。最も手前の面が z = 0。
 *   原典の深度テクスチャは一辺 448 texel なので、勾配・凹みのサンプリング距離は H/448 を 1 texel とする。
 */
#pragma once

#include <math.h>

#ifdef __CUDACC__
#define RL_HD  __host__ __device__
#define RL_INL __forceinline__
#else
#define RL_HD
#define RL_INL inline
#endif

/* ------------------------------------------------------------------ */
/* 描画の設定                                                           */
/* ------------------------------------------------------------------ */

#define RELIGHT_MAX_LIGHTS 3

enum {
	OUTPUT_RELIT = 1,
	OUTPUT_DEPTH,
	OUTPUT_NORMALS,
	OUTPUT_SHADOW,
	OUTPUT_LINES
};

enum {
	LOOK_ANIME = 1,
	LOOK_CINEMATIC
};

enum {
	RIM_AUTO = 1,        /* 光源が手前なら光源の側だけ、物の後ろなら輪郭全体 */
	RIM_LIGHT_SIDE,
	RIM_ALL_AROUND
};

struct LightParams {
	bool  on;
	/* uv 座標: 画面の高さ = 1、横は中央 0.5 を基準に高さ比で伸びる */
	float u, v, z;
	float tint[3];
	float intensity;
	float range;         /* 光の届く範囲の倍率 (1 = 既定。減衰と撮影寄りのグラデーションの広がりに掛ける) */
	bool  show;          /* 光の玉とにじみを描く */
	int   look;          /* この光源の当て方 0: Scene の Look と同じ  LOOK_ANIME / LOOK_CINEMATIC */
	/* リムライト (光源ごと) */
	bool  rim;           /* この光源でリムライトを付けるか */
	float rimAmount;     /* 強さ */
	float rimWidth;      /* 幅 (1080p でのピクセル数) */
	float rimSoft;       /* 内側の境目のぼかし (0 = くっきりした帯) */
	float rimSmooth;     /* 輪郭のならし (0 = 深度の輪郭そのまま。上げるほど細かいでこぼこを無視したまっすぐな線) */
	int   rimPlacement;
	float rimColor[3];   /* 光源の色に掛ける */
	float rimSpread;     /* 光源側の輪郭からどこまで回り込んで光らせるか (0 = 光源を向いた側だけ、1 = 輪郭全体) */
	float rimReach;      /* 画面上で光源からどこまで光らせるか (画面の高さが 1。3 以上なら制限なし) */
};

struct RenderParams {
	LightParams light[RELIGHT_MAX_LIGHTS];
	/* scene */
	int   look;
	float exposure;
	float ambient[3];
	float relief, specular, shadow, occlusion;
	/* anime */
	float shadowColor[3];
	float shadowTint;
	float shadeSmooth;
	float edgeDither;
	float cel;
	int   celSteps;
	float celSoft;
	float form;         /* 面の向きによる立体的な陰影の割合 (0 = 撮影寄りのグラデーションだけ、1 = 立体的な陰影だけ) */
	float lightSpread;   /* 撮影寄りのグラデーションの広がり (画面の高さが 1) */
	int   depthStabilize;/* 深度を前後何フレームで時間方向にならすか (0 = しない) */
	float lineProtect, lineThreshold;
	float volume;
	/* passes */
	int   depthNear;
	bool  depthSnap;
	int   normalSource;
	int   passEncoding;
	int   maskSource;
	bool  inputLinear;
	/* output */
	int   outputMode;
	float mix;
};

/* ------------------------------------------------------------------ */
/* 原典の定数                                                           */
/* ------------------------------------------------------------------ */

static constexpr float REF_TEXELS            = 448.f;   /* 原典の深度テクスチャの一辺 */

static constexpr float GRADIENT_RADIUS       = 7.f;
static constexpr float GRADIENT_LIMIT        = 0.009f;
static constexpr float GRADIENT_NOISE        = 0.0003f;
static constexpr float GRADIENT_NOISE_ENERGY = GRADIENT_NOISE * GRADIENT_NOISE;
static constexpr int   OCCLUSION_RADIUS_A    = 3;
static constexpr int   OCCLUSION_RADIUS_B    = 9;
static constexpr float OCCLUSION_TAPS        = 16.f;
static constexpr float OCCLUSION_SCALE       = 0.07f;
static constexpr float OCCLUSION_RANGE       = 0.25f;
static constexpr float OCCLUSION_FLOOR       = 0.012f;

static constexpr float NEAR_Z                = 0.f;
static constexpr float SURFACE_FAR_Z         = -0.7f;
static constexpr float LIGHT_RADIUS          = 0.85f;
static constexpr float LIGHT_WRAP            = 0.25f;
static constexpr float RELIEF_SCALE          = 200.f;
static constexpr float SLOPE_COMPRESSION     = 0.55f;
static constexpr float SPECULAR_F0           = 0.06f;
static constexpr float WHITE_POINT           = 2.6f;
static constexpr float DITHER_STEP           = 1.f / 255.f;

static constexpr float BULB_WORLD_RADIUS     = 0.05f;
static constexpr float BULB_CAMERA_Z         = 2.f;
static constexpr float BULB_REFERENCE_Z      = 0.42f;
static constexpr float BULB_CORE             = 8.f;
static constexpr float BULB_LIMB             = 0.28f;
static constexpr float BULB_EDGE             = 0.75f;
static constexpr float BULB_EDGE_FLOOR       = 0.004f;
static constexpr float BULB_EDGE_LIMIT       = 0.3f;
static constexpr float BULB_HALO             = 1.6f;
static constexpr float BULB_HALO_SPAN        = 1.2f;
static constexpr float BULB_VEIL             = 0.12f;
static constexpr float BULB_VEIL_SPAN        = 4.f;
static constexpr float BULB_ONSET            = 0.6f;
static constexpr float BULB_OCCLUSION_SOFTNESS = 0.02f;
static constexpr float BULB_SOURCE_SOFTNESS  = 0.08f;
static constexpr float BULB_SAMPLE_SPREAD    = 0.6f;

static constexpr float SHADOW_FAR_Z          = -1.25f;
static constexpr int   SHADOW_STEPS          = 32;
static constexpr float SHADOW_SPAN           = 0.3f;
static constexpr float SHADOW_BASELINE       = 0.005f;
static constexpr float SHADOW_BIAS           = 0.014f;
static constexpr float SHADOW_SLOPE_BIAS     = 0.02f;
static constexpr float SHADOW_THICKNESS      = 0.7f;
static constexpr float SHADOW_THICKNESS_GROWTH = 2.6f;
static constexpr float SHADOW_SOFTNESS       = 0.089f;
static constexpr float SHADOW_GAIN           = 2.5f;
static constexpr float SHADOW_FRONT_FADE     = 0.2f;

static constexpr float LIGHT_Z_MIN           = -0.6f;
static constexpr float LIGHT_Z_MAX           = 1.65f;

/* ------------------------------------------------------------------ */
/* アニメ向けの定数                                                     */
/* ------------------------------------------------------------------ */

static constexpr float ANIME_LIGHT_GAIN      = 0.4f;    /* Anime で光を足す割合 (Intensity 3 で明るさ約 2 倍) */
static constexpr float ANIME_KNEE            = 0.85f;   /* これより明るい所だけ丸める (元の色はそのまま) */
static constexpr float DARK_SIDE_LO          = 0.25f;   /* 光の当たり具合がこれ以下なら影色 */
static constexpr float DARK_SIDE_HI          = 0.6f;    /* これ以上なら影色なし */
static constexpr float DARK_SIDE_REACH       = 1.2f;    /* 減衰がこの逆数 (約 0.83、光源から約 0.4) を下回る距離から影色が乗り始める */
static constexpr float VOLUME_EDGE           = 0.02f;   /* 隣のマスとの深度差がこれ以上なら輪郭 */
static constexpr float VOLUME_RADIUS         = 40.f;    /* 輪郭からこのマス数で膨らみきる */
static constexpr float VOLUME_SCALE          = 0.08f;   /* Depth Volume 1 での最大の盛り上がり (深度 0..1 に対して) */
static constexpr float RIM_EDGE              = 0.04f;   /* 後ろとの深度差がこれ以上なら輪郭 */
static constexpr int   STABILIZE_MAX         = 2;       /* 前後何フレームまで */
static constexpr float BACK_DARK             = 0.8f;    /* 撮影寄りのグラデーションで、逆光の手前の物を暗くする割合 */
static constexpr float RIM_SILHOUETTE        = 0.18f;   /* 後ろとの深度差がこれ以上なら外側の輪郭 (リムを最大に)。小さい重なり (フリルの段など) は弱く */
static constexpr int   RIM_AROUND_DIRS       = 8;       /* All Around で輪郭を探す方向の数 */
static constexpr int   RIM_SAMPLES           = 6;       /* リムの帯を作るとき、1 方向にずらして見る点の数 */
static constexpr float RIM_GAIN              = 0.6f;
static constexpr float RIM_BACK_SPAN         = 0.25f;   /* 光源が面よりこれだけ奥なら完全な逆光 */
static constexpr float RIM_FRONT             = 0.6f;    /* Auto で光源が手前のときのリムの割合 */
static constexpr float RIM_SMOOTH_MAX        = 12.f;    /* Rim Smoothing 1 で輪郭をならす半径 (1080p でのピクセル数) */
static constexpr float LINE_CONTRAST         = 0.10f;   /* 周りよりこれだけ暗ければ線 */
static constexpr float SNAP_COLOR_SIGMA      = 0.08f;   /* 深度を線画に合わせるとき、色の近さの幅 */

/* ------------------------------------------------------------------ */
/* 数学ユーティリティ                                                  */
/* ------------------------------------------------------------------ */

struct V3 { float x, y, z; };
RL_HD RL_INL V3 v3(float x, float y, float z) { V3 r; r.x = x; r.y = y; r.z = z; return r; }
RL_HD RL_INL V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
RL_HD RL_INL V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
RL_HD RL_INL V3 operator*(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
RL_HD RL_INL V3 mul(V3 a, V3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
RL_HD RL_INL float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
RL_HD RL_INL float length(V3 a) { return sqrtf(dot(a, a)); }
RL_HD RL_INL V3 normalize(V3 a) { float l = length(a); return l > 1e-12f ? a * (1.f / l) : v3(0.f, 0.f, 1.f); }

template <typename T> RL_HD RL_INL T rlMin(T a, T b) { return a < b ? a : b; }
template <typename T> RL_HD RL_INL T rlMax(T a, T b) { return a > b ? a : b; }
template <typename T> RL_HD RL_INL T rlClamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

RL_HD RL_INL float saturate(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
RL_HD RL_INL float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
RL_HD RL_INL float mixf(float a, float b, float t) { return a + (b - a) * t; }
RL_HD RL_INL V3 mix3(V3 a, V3 b, float t) { return a + (b - a) * t; }
RL_HD RL_INL float smoothstepf(float e0, float e1, float x)
{
	float t = saturate((x - e0) / (e1 - e0));
	return t * t * (3.f - 2.f * t);
}
RL_HD RL_INL float fract(float x) { return x - floorf(x); }
RL_HD RL_INL int rlRound(float x) { return (int)floorf(x + 0.5f); }

RL_HD RL_INL float surfaceZ(float depth) { return mixf(SURFACE_FAR_Z, NEAR_Z, depth); }
RL_HD RL_INL float shadowZ(float depth)  { return mixf(SHADOW_FAR_Z, NEAR_Z, depth); }

RL_HD RL_INL float srgbToLinear(float c)
{
	if (c <= 0.f) return c;
	return (c <= 0.04045f) ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* 画面上の位置ごとに決まる 0..1 の揺らぎ (原典と同じ式) */
RL_HD RL_INL float rlNoise(float a, float b) { return fract(52.9829189f * fract(0.06711056f * a + 0.00583715f * b)); }

/* ガンマ 2.2 の表 (powf を避ける)。表は CPU なら RelightAE.cpp、GPU なら RelightGPU.cu が用意する。
   dec[i] = (i/1024)^2.2、enc[i] = (i/1024)^(2/2.2) (enc は sqrt を挟んで原点付近の誤差を抑える) */
#define RL_LUT_SIZE 1026
inline void rlBuildLut(float *dec, float *enc)
{
	for (int i = 0; i < RL_LUT_SIZE; ++i) {
		const double x = (i / 1024.0) < 1.0 ? i / 1024.0 : 1.0;
		dec[i] = (float)pow(x, 2.2);
		enc[i] = (float)pow(x, 2.0 / 2.2);
	}
}
RL_HD RL_INL float lutLookup(const float *t, float x)
{
	float f = saturate(x) * 1024.f;
	int i = (int)f;
	return t[i] + (t[i + 1] - t[i]) * (f - (float)i);
}

/* ------------------------------------------------------------------ */
/* 画像の読み取り (AE のワールドと GPU のワールドの両方)                  */
/* ------------------------------------------------------------------ */

struct Rgba { float a, r, g, b; };

enum {
	RL_FMT_ARGB8 = 0,    /* PF_Pixel8 */
	RL_FMT_ARGB16,       /* PF_Pixel16 (最大値 32768) */
	RL_FMT_ARGB32F,      /* PF_PixelFloat */
	RL_FMT_BGRA32F,      /* AE の GPU ワールド (PF_PixelFormat_GPU_BGRA128) */
	RL_FMT_GRAY32F       /* 1 チャンネルの float (確認用ツールの深度) */
};

struct RLImage {
	const unsigned char *data;   /* NULL なら無し */
	int width, height, rowbytes, format;
};

/* 端はクランプ。premultiplied のまま返す */
RL_HD RL_INL Rgba rlRead(const RLImage &im, int x, int y)
{
	Rgba o;
	o.a = o.r = o.g = o.b = 0.f;
	if (!im.data) return o;
	x = rlClamp(x, 0, im.width - 1);
	y = rlClamp(y, 0, im.height - 1);
	const unsigned char *row = im.data + (size_t)y * (size_t)im.rowbytes;
	switch (im.format) {
	case RL_FMT_ARGB32F: {
		const float *p = (const float *)row + (size_t)x * 4;
		o.a = p[0]; o.r = p[1]; o.g = p[2]; o.b = p[3];
		break;
	}
	case RL_FMT_BGRA32F: {
		const float *p = (const float *)row + (size_t)x * 4;
		o.b = p[0]; o.g = p[1]; o.r = p[2]; o.a = p[3];
		break;
	}
	case RL_FMT_ARGB16: {
		const unsigned short *p = (const unsigned short *)row + (size_t)x * 4;
		const float s = 1.f / 32768.f;
		o.a = p[0] * s; o.r = p[1] * s; o.g = p[2] * s; o.b = p[3] * s;
		break;
	}
	case RL_FMT_GRAY32F: {
		const float v = ((const float *)row)[x];
		o.a = 1.f; o.r = o.g = o.b = v;
		break;
	}
	default: {
		const unsigned char *p = row + (size_t)x * 4;
		const float s = 1.f / 255.f;
		o.a = p[0] * s; o.r = p[1] * s; o.g = p[2] * s; o.b = p[3] * s;
		break;
	}
	}
	return o;
}

/* 補助レイヤー (深度・法線・マスク) の位置合わせ。
   補助レイヤーは素材と大きさが違うことがある (深度は短辺 518px) ので、レイヤー全体を取り出し、
   「素材レイヤーの中での位置の割合」で対応させる。出力の画素 x → 補助ワールドの x * sx + ox */
struct AuxMap {
	RLImage img;
	float sx, sy, ox, oy;
};

/* fullW, fullH: 補助レイヤー全体の大きさ (ダウンサンプル後)。originX/Y: 出力 (0,0) のレイヤー座標 */
RL_HD RL_INL AuxMap rlMakeAuxMap(const RLImage &img, float fullW, float fullH, float originX, float originY,
                                 float layerW, float layerH)
{
	AuxMap m;
	m.img = img;
	m.sx = rlMax(1.f, fullW) / layerW;
	m.sy = rlMax(1.f, fullH) / layerH;
	/* 画素の中心どうしを合わせる: (x + originX + 0.5) * s - 0.5 */
	m.ox = (originX + 0.5f) * m.sx - 0.5f;
	m.oy = (originY + 0.5f) * m.sy - 0.5f;
	return m;
}

/* バイリニアで読む。decodeSrgb が真なら AE が掛けたリニア→sRGB 変換を打ち消す */
RL_HD RL_INL Rgba rlReadAux(const AuxMap &m, int x, int y, bool decodeSrgb)
{
	Rgba o;
	o.a = o.r = o.g = o.b = 0.f;
	if (!m.img.data) return o;
	const float fx = clampf((float)x * m.sx + m.ox, 0.f, (float)(m.img.width - 1));
	const float fy = clampf((float)y * m.sy + m.oy, 0.f, (float)(m.img.height - 1));
	const int x0 = (int)fx, y0 = (int)fy;
	const float tx = fx - (float)x0, ty = fy - (float)y0;
	const Rgba a = rlRead(m.img, x0, y0), b = rlRead(m.img, x0 + 1, y0);
	const Rgba c = rlRead(m.img, x0, y0 + 1), d = rlRead(m.img, x0 + 1, y0 + 1);
	o.a = mixf(mixf(a.a, b.a, tx), mixf(c.a, d.a, tx), ty);
	o.r = mixf(mixf(a.r, b.r, tx), mixf(c.r, d.r, tx), ty);
	o.g = mixf(mixf(a.g, b.g, tx), mixf(c.g, d.g, tx), ty);
	o.b = mixf(mixf(a.b, b.b, tx), mixf(c.b, d.b, tx), ty);
	if (decodeSrgb) { o.r = srgbToLinear(o.r); o.g = srgbToLinear(o.g); o.b = srgbToLinear(o.b); }
	return o;
}

/* ------------------------------------------------------------------ */
/* 描画コンテキスト                                                     */
/* ------------------------------------------------------------------ */

/* 有効な光源 1 個分 (フレームごとの定数) */
struct LightCtx {
	V3    pos;            /* (u-0.5, v-0.5, z) */
	float u, v, z;
	V3    tint;
	float intensity;
	float bulbRadius;
	float bulbExposure;
	float presence;
	float range;          /* 光の届く範囲の倍率 */
	bool  show;           /* 光の玉とにじみを描く */
	int   look;           /* この光源の当て方 (LOOK_ANIME / LOOK_CINEMATIC。Scene と同じなら Scene の値を入れてある) */
	bool  lookOwn;        /* Light Look を光源で選んだ (Same as Scene でない) */
	/* リムライト */
	bool  rim;            /* この光源でリムライトを付けるか (Use Rim がオンで強さが 0 より大きい) */
	float rimAmount;
	float rimW;           /* 幅 (出力のピクセル) */
	float rimSoft;
	int   rimSmoothR;     /* 輪郭をならす半径 (出力のピクセル、0 = ならさない) */
	int   rimPlacement;
	V3    rimColor;
	float rimSpread, rimReach;
	const float *rimDepth;/* 輪郭をならした深度 (outW x outH、無ければ NULL = 深度そのもの) */
	const float *rimInside;/* 実際の深度で物の内側か (0..1、outW x outH。rimDepth と組で使う) */
};

/*
 * 高速化のため、原典と同じく「画面の高さ = 448 マス」の格子で
 * 深度・勾配・凹み・落ち影・当たり具合を先に計算し、各画素では補間して使う。
 * 画素ごとに計算するのは、元の色、照明の式、リムライト (輪郭までの距離)、光の玉。
 * ポインタは CPU 版ならメインメモリ、GPU 版なら GPU のメモリを指す。
 */
struct ShadeContext {
	RenderParams  rp;
	const float  *lutDec, *lutEnc;    /* ガンマの表 */
	const float  *depth;              /* outW x outH、近い = 1 (輪郭をくっきり保つために画素単位でも持つ) */
	bool          hasDepth;
	AuxMap        normalA;            /* 法線レイヤー (img.data が NULL なら無し) */
	AuxMap        maskA;              /* マスクレイヤー */
	int           outW, outH;
	float         originX, originY;   /* 出力 (0,0) のレイヤー座標 */
	float         layerW, layerH;     /* ダウンサンプル後のレイヤー寸法 */
	const float  *lineMask;           /* outW x outH。線らしさ 0..1 (線画の保護とリムの位置合わせに使う。無ければ NULL) */
	/* 光源 */
	LightCtx      L[RELIGHT_MAX_LIGHTS];
	int           nL;
	/* 格子 */
	int           gw, gh;
	float         cell;               /* 1 マスのピクセル数 (= 原典の 1 texel) */
	const float  *gDepth;             /* gw x gh (膨らみ込み) */
	const float  *gBulge;             /* gw x gh: 膨らみ (無ければ NULL) */
	const float  *gSurf;              /* gw x gh x 3: 勾配 x, 勾配 y, 凹み */
	const float  *gShadow;            /* gw x gh x nL: 落ち影 (Shadow を掛ける前) */
	const float  *gRimMin;            /* gw x gh: 周りで最も奥の深度 (リムの下準備。無ければ NULL) */
	const float  *gKey;               /* gw x gh x nL: ならした「当たり具合」(無ければ NULL) */
	const float  *rimMask;            /* outW x outH x nL: ぼかしたリムの下地 (無ければ NULL) */
	float         rimW;               /* リムの幅の最大 (出力のピクセル。リムを付ける光源のうち) */
};

RL_HD RL_INL float gammaDecode(const ShadeContext *c, float x) { return lutLookup(c->lutDec, x); }
RL_HD RL_INL float gammaEncode(const ShadeContext *c, float x) { return lutLookup(c->lutEnc, sqrtf(saturate(x))); }

/* フレームの準備のうち、格子を使わない部分 (光源と格子の大きさ)。CPU 版と GPU 版の両方がまず呼ぶ */
inline void rlSetupFrame(ShadeContext &c)
{
	const RenderParams &rp = c.rp;
	c.nL = 0;
	for (int k = 0; k < RELIGHT_MAX_LIGHTS; ++k) {
		const LightParams &lp = rp.light[k];
		if (!lp.on || lp.intensity <= 0.f) continue;
		LightCtx &L = c.L[c.nL++];
		L.u = lp.u; L.v = lp.v; L.z = clampf(lp.z, LIGHT_Z_MIN, LIGHT_Z_MAX);
		L.pos = v3(L.u - 0.5f, L.v - 0.5f, L.z);
		L.tint = v3(lp.tint[0], lp.tint[1], lp.tint[2]);
		L.intensity = lp.intensity;
		L.range = clampf(lp.range, 0.05f, 10.f);
		L.show = lp.show;
		L.lookOwn = (lp.look == LOOK_ANIME || lp.look == LOOK_CINEMATIC);
		L.look = L.lookOwn ? lp.look : rp.look;
		L.rim = lp.rim && lp.rimAmount > 0.f;
		L.rimAmount = rlMax(0.f, lp.rimAmount);
		L.rimW = rlMax(1.f, lp.rimWidth * c.layerH / 1080.f);
		L.rimSoft = saturate(lp.rimSoft);
		L.rimSmoothR = lp.rimSmooth > 0.f ? rlClamp((int)floorf(saturate(lp.rimSmooth) * RIM_SMOOTH_MAX * c.layerH / 1080.f + 0.5f), 1, 16) : 0;
		L.rimPlacement = lp.rimPlacement;
		L.rimColor = v3(lp.rimColor[0], lp.rimColor[1], lp.rimColor[2]);
		L.rimSpread = saturate(lp.rimSpread);
		L.rimReach = lp.rimReach;
		L.rimDepth = 0;
		L.rimInside = 0;
		L.bulbRadius = BULB_WORLD_RADIUS * ((BULB_CAMERA_Z - BULB_REFERENCE_Z) / (BULB_CAMERA_Z - L.z));
		L.presence = saturate(L.intensity / BULB_ONSET);
		L.bulbExposure = 1.f;
	}
	c.cell = rlMax(1.f, c.layerH / REF_TEXELS);
	c.gw = rlMax(1, (int)ceilf((float)c.outW / c.cell));
	c.gh = rlMax(1, (int)ceilf((float)c.outH / c.cell));
	c.gDepth = c.gSurf = c.gShadow = c.gRimMin = c.gBulge = c.gKey = 0;
	c.rimMask = 0;
	c.rimW = 1.f;
	for (int k = 0; k < c.nL; ++k)
		if (c.L[k].rim) c.rimW = rlMax(c.rimW, c.L[k].rimW);
}

/* リムを付ける光源があるか */
RL_HD RL_INL bool rlAnyRim(const ShadeContext &c)
{
	for (int k = 0; k < c.nL; ++k)
		if (c.L[k].rim) return true;
	return false;
}

/* 陰影をならす半径 (マス)。Shade Smoothness が 0 なら 0 */
RL_HD RL_INL int rlSmoothRadius(const RenderParams &rp) { return rp.shadeSmooth > 0.f ? rlMax(1, rlRound(rp.shadeSmooth * 12.f)) : 0; }
RL_HD RL_INL int rlKeyRadius(const RenderParams &rp)    { return rlMax(1, rlRound(rp.shadeSmooth * 16.f)); }
/* 当たり具合を格子で先に求めるか (ならすときだけ。法線レイヤーを使うときは画素ごと) */
RL_HD RL_INL bool rlUseKeyGrid(const ShadeContext &c) { return c.nL > 0 && c.rp.shadeSmooth > 0.f && !(c.rp.normalSource == 2 && c.normalA.img.data); }
/* リムの下準備の半径 (マス) */
RL_HD RL_INL int rlRimCells(const ShadeContext &c) { return (int)ceilf(c.rimW / c.cell) + 1; }
/* 線画のマスクのぼかし半径 (px)。1080p で 4 px 相当 */
RL_HD RL_INL int rlLineRadius(float layerH) { return rlMax(2, rlRound(4.f * layerH / 1080.f)); }

/* ------------------------------------------------------------------ */
/* 座標と格子                                                           */
/* ------------------------------------------------------------------ */

/* uv → 出力バッファのピクセル座標 */
RL_HD RL_INL void uvToBuffer(const ShadeContext *c, float u, float v, float &bx, float &by)
{
	bx = (u - 0.5f) * c->layerH + c->layerW * 0.5f - 0.5f - c->originX;
	by = v * c->layerH - 0.5f - c->originY;
}

/* 出力バッファのピクセル座標 → 格子座標 */
RL_HD RL_INL void bufferToGrid(const ShadeContext *c, float bx, float by, float &gx, float &gy)
{
	gx = (bx + 0.5f) / c->cell - 0.5f;
	gy = (by + 0.5f) / c->cell - 0.5f;
}

/* 格子のバイリニアサンプル (端はクランプ)。gx, gy はマス中心を整数とする座標 */
RL_HD RL_INL float gridSample(const ShadeContext *c, const float *arr, int stride, int comp, float gx, float gy)
{
	const int W = c->gw, H = c->gh;
	gx = clampf(gx, 0.f, (float)(W - 1));
	gy = clampf(gy, 0.f, (float)(H - 1));
	const int x0 = (int)gx, y0 = (int)gy;
	const int x1 = rlMin(x0 + 1, W - 1), y1 = rlMin(y0 + 1, H - 1);
	const float fx = gx - (float)x0, fy = gy - (float)y0;
	const float a = arr[((size_t)y0 * W + x0) * stride + comp], b = arr[((size_t)y0 * W + x1) * stride + comp];
	const float e = arr[((size_t)y1 * W + x0) * stride + comp], f = arr[((size_t)y1 * W + x1) * stride + comp];
	return mixf(mixf(a, b, fx), mixf(e, f, fx), fy);
}

RL_HD RL_INL float gridDepthAt(const ShadeContext *c, int i, int j)
{
	i = rlClamp(i, 0, c->gw - 1);
	j = rlClamp(j, 0, c->gh - 1);
	return c->gDepth[(size_t)j * c->gw + i];
}

/* 格子のマス中心の uv */
RL_HD RL_INL void gridCellUv(const ShadeContext *c, int i, int j, float &u, float &v)
{
	const float bx = ((float)i + 0.5f) * c->cell - 0.5f;
	const float by = ((float)j + 0.5f) * c->cell - 0.5f;
	const float px = bx + c->originX + 0.5f, py = by + c->originY + 0.5f;
	u = 0.5f + (px - c->layerW * 0.5f) / c->layerH;
	v = py / c->layerH;
}

/* 原典 depthAt: 448 texel の深度をバイリニアで引く (影のレイマーチと光の玉の遮蔽に使う) */
RL_HD RL_INL float depthAtUv(const ShadeContext *c, float u, float v)
{
	if (!c->hasDepth) return 1.f;
	float bx, by, gx, gy;
	uvToBuffer(c, u, v, bx, by);
	bufferToGrid(c, bx, by, gx, gy);
	return gridSample(c, c->gDepth, 1, 0, gx, gy);
}

/* ------------------------------------------------------------------ */
/* 原典の関数                                                           */
/* ------------------------------------------------------------------ */

/* 原典 gentlerDelta: 前後差のうち小さい方に寄せて、輪郭での跳ねを抑える */
RL_HD RL_INL float gentlerDelta(float backward, float forward)
{
	const float back = fabsf(backward), front = fabsf(forward);
	return (backward * front + forward * back) / rlMax(back + front, 1e-9f);
}

/* 原典 surfaceSlope: ノイズ分を引き、tanh で上限を付ける */
RL_HD RL_INL void surfaceSlope(float &gx, float &gy)
{
	const float steep = rlMax(sqrtf(gx * gx + gy * gy), 1e-9f);
	const float shrunk = sqrtf(rlMax(steep * steep - GRADIENT_NOISE_ENERGY, 0.f));
	const float ceiling = GRADIENT_LIMIT * tanhf(shrunk / GRADIENT_LIMIT);
	gx *= ceiling / steep;
	gy *= ceiling / steep;
}

/* 原典 shadowFactor (光源の z を引数で受ける) */
RL_HD RL_INL float shadowFactor(const ShadeContext *c, V3 origin, V3 dir, float reach, float jitter, float lightZ)
{
	const float stride = reach / (float)SHADOW_STEPS;
	const float baselineTravel = reach * (SHADOW_BASELINE / SHADOW_SPAN);
	const V3 trail = origin - dir * baselineTravel;
	const float receiverRise = rlMax(
		origin.z - shadowZ(depthAtUv(c, trail.x + 0.5f, trail.y + 0.5f)) - baselineTravel * dir.z, 0.f);
	const float risePerTravel = receiverRise / rlMax(baselineTravel, 1e-9f);

	float occ = 0.f;
	for (int step = 0; step < SHADOW_STEPS; ++step) {
		const float travel = ((float)step + jitter) * stride;
		const V3 probe = origin + dir * travel;
		const float sampleZ = shadowZ(depthAtUv(c, probe.x + 0.5f, probe.y + 0.5f));
		const float diff = sampleZ - probe.z;
		const float bias = SHADOW_BIAS + travel * (SHADOW_SLOPE_BIAS + risePerTravel);
		const float thickness = SHADOW_THICKNESS * (1.f + (travel / SHADOW_SPAN) * SHADOW_THICKNESS_GROWTH);
		if (diff > bias && diff < thickness) {
			const float behindLight = 1.f - saturate((sampleZ - lightZ) / SHADOW_FRONT_FADE);
			occ += saturate((diff - bias) / SHADOW_SOFTNESS) * behindLight;
		}
	}
	return 1.f - saturate((occ / (float)SHADOW_STEPS) * SHADOW_GAIN);
}

/* 原典 compress / tonemap */
RL_HD RL_INL float compressTone(float v)
{
	return (v * (v / (WHITE_POINT * WHITE_POINT) + 1.f)) / (v + 1.f);
}

RL_HD RL_INL V3 tonemap(V3 c)
{
	const float lum = rlMax(0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z, 0.0001f);
	const float mapped = compressTone(lum);
	const float wp2 = WHITE_POINT * WHITE_POINT;
	const V3 per = v3(c.x * (c.x / wp2 + 1.f) / (c.x + 1.f),
	                  c.y * (c.y / wp2 + 1.f) / (c.y + 1.f),
	                  c.z * (c.z / wp2 + 1.f) / (c.z + 1.f));
	const float sm = saturate(mapped);
	const float bleach = sm * sm;   /* 原典 pow(mapped, HIGHLIGHT_BLEACH)、HIGHLIGHT_BLEACH = 2 */
	const V3 r = mix3(c * (mapped / lum), per, bleach);
	return v3(saturate(r.x), saturate(r.y), saturate(r.z));
}

/* Anime 用: ANIME_KNEE までは元の色をそのまま通し、それより明るい所だけ色相を保って丸める。
   とても明るい所 (光の玉の近く) は白に寄せる */
RL_HD RL_INL V3 animeTone(V3 c)
{
	c = v3(rlMax(c.x, 0.f), rlMax(c.y, 0.f), rlMax(c.z, 0.f));
	const float m = rlMax(c.x, rlMax(c.y, c.z));
	if (m <= ANIME_KNEE) return c;
	const float range = 1.f - ANIME_KNEE;
	const float mm = ANIME_KNEE + range * (1.f - expf(-(m - ANIME_KNEE) / range));
	V3 r = c * (mm / m);
	const float bleach = saturate((m - 1.f) / 4.f) * 0.7f;
	r = mix3(r, v3(mm, mm, mm), bleach);
	return v3(saturate(r.x), saturate(r.y), saturate(r.z));
}

/* セル調: 0..1 を steps 段に分ける。soft は段の境目のぼかし (0 で硬い境目) */
RL_HD RL_INL float celQuant(float x, int steps, float soft)
{
	const float y = saturate(x) * (float)steps;
	const float i = floorf(y);
	if (i >= (float)steps) return 1.f;
	const float f = y - i;
	const float e = (soft > 1e-3f) ? smoothstepf(0.5f - soft * 0.5f, 0.5f + soft * 0.5f, f) : (f >= 0.5f ? 1.f : 0.f);
	return (i + e) / (float)steps;
}

RL_HD RL_INL V3 depthRamp(float value)
{
	const V3 cold = v3(0.03f, 0.02f, 0.12f), middle = v3(0.11f, 0.45f, 0.94f);
	const V3 warm = v3(0.85f, 0.36f, 0.96f), hot = v3(0.97f, 0.97f, 0.87f);
	if (value < 0.4f)  return mix3(cold, middle, value / 0.4f);
	if (value < 0.75f) return mix3(middle, warm, (value - 0.4f) / 0.35f);
	return mix3(warm, hot, (value - 0.75f) / 0.25f);
}

/* 原典 bulbExposure: 光の玉が手前の物に隠れているか (光源ごと、フレームごとに 1 回) */
RL_HD RL_INL float computeBulbExposure(const ShadeContext *c, const LightCtx &L)
{
	float open = 0.f;
	for (int sy = -1; sy <= 1; ++sy)
		for (int sx = -1; sx <= 1; ++sx) {
			const float pu = L.u + (float)sx * L.bulbRadius * BULB_SAMPLE_SPREAD;
			const float pv = L.v + (float)sy * L.bulbRadius * BULB_SAMPLE_SPREAD;
			open += smoothstepf(0.f, BULB_SOURCE_SOFTNESS, L.z - surfaceZ(depthAtUv(c, pu, pv)));
		}
	return open / 9.f;
}

RL_HD RL_INL float maskAt(const ShadeContext *c, int x, int y)
{
	if (!c->maskA.img.data) return 1.f;
	const Rgba m = rlReadAux(c->maskA, x, y, c->rp.passEncoding == 2);
	if (c->rp.maskSource == 2) return saturate(m.a);
	return saturate(0.2126f * m.r + 0.7152f * m.g + 0.0722f * m.b);
}

/* ------------------------------------------------------------------ */
/* 深度の引き伸ばしと線画のマスク (1 画素ぶん)                            */
/* ------------------------------------------------------------------ */

/* 深度ワールドの 1 点 → 0..1 (近い = 1) */
RL_HD RL_INL float rlLowDepth(const RLImage &img, int i, int j, int depthNear, bool decodeSrgb)
{
	Rgba d = rlRead(img, i, j);
	if (decodeSrgb) { d.r = srgbToLinear(d.r); d.g = srgbToLinear(d.g); d.b = srgbToLinear(d.b); }
	float v = saturate(0.2126f * d.r + 0.7152f * d.g + 0.0722f * d.b);
	if (depthNear == 2) v = 1.f - v;
	return v;
}

/* 深度ワールドの 1 点が使えるか (前後のフレームがレイヤーの外だと透明になる) */
RL_HD RL_INL bool rlDepthValid(const RLImage &img, int i, int j) { return img.data && rlRead(img, i, j).a > 0.5f; }

/* 深度を時間方向にならす (1 点)。前後のフレームの同じ位置の深度との中央値を取る。
   深度推定はフレームごとに揺れ、細い物 (髪の毛先など) は 1〜2 フレームだけ背景になったりする。
   その揺れが影の判定やリムで大きくなって、動画ではちらつく。中央値なら飛び出した値は消え、
   一定の速さで動く物の輪郭は今のフレームの位置のまま (前後で単調に変わる値の中央値は真ん中 = 今) なので残像にならない。
   nb[k]: 前後のフレームの深度ワールド (k = 0..n-1)、off[k]: 時刻の差 (フレーム、ここでは使わない) */
RL_HD RL_INL float rlStabilizeDepth(float d0, const RLImage *nb, const int *off, int n, int i, int j, int depthNear, bool decodeSrgb)
{
	(void)off;
	float v[5];
	int m = 0;
	v[m++] = d0;
	for (int k = 0; k < n && m < 5; ++k)
		if (rlDepthValid(nb[k], i, j)) v[m++] = rlLowDepth(nb[k], i, j, depthNear, decodeSrgb);
	/* 小さい順に並べる (多くて 5 個) */
	for (int a = 1; a < m; ++a) {
		const float x = v[a];
		int b = a - 1;
		while (b >= 0 && v[b] > x) { v[b + 1] = v[b]; --b; }
		v[b + 1] = x;
	}
	return (m & 1) ? v[m / 2] : 0.5f * (v[m / 2 - 1] + v[m / 2]);
}

/* 素材の画素の表示の色 (0..1)。深度を線画に合わせる手がかり */
RL_HD RL_INL V3 rlGuide(const ShadeContext *c, const RLImage &src, int x, int y)
{
	const Rgba p = rlRead(src, x, y);
	const float a = p.a > 1e-6f ? p.a : 1.f;
	V3 g = v3(p.r / a, p.g / a, p.b / a);
	if (c->rp.inputLinear) g = v3(gammaEncode(c, g.x), gammaEncode(c, g.y), gammaEncode(c, g.z));
	return v3(saturate(g.x), saturate(g.y), saturate(g.z));
}

/* 深度を出力の大きさへ引き伸ばす (1 画素)。
   深度は短辺 518px で推定するので、素材より粗く、輪郭も素材の線画より少し太くぼけている。
   Snap Depth To Lines がオンなら、深度が変わる所 (輪郭の近く) だけ、周り 4x4 の深度の点のうち
   「その点の位置の素材の色が、この画素の色に近いもの」を重く見て混ぜる (joint bilateral upsampling)。
   アニメは線画と塗り分けがはっきりしているので、深度の境目が線画に沿う。
   low: dw x dh の深度、map: 出力の画素 → 深度の点 */
RL_HD RL_INL float rlUpsampleDepth(const ShadeContext *c, const float *low, int dw, int dh, const AuxMap &map,
                                   const RLImage &src, int x, int y)
{
	const float fx = clampf((float)x * map.sx + map.ox, 0.f, (float)(dw - 1));
	const float fy = clampf((float)y * map.sy + map.oy, 0.f, (float)(dh - 1));
	const int x0 = (int)fx, y0 = (int)fy;
	const float tx = fx - (float)x0, ty = fy - (float)y0;
#define RL_DAT(i, j) low[(size_t)rlClamp((j), 0, dh - 1) * dw + rlClamp((i), 0, dw - 1)]
	const float a = RL_DAT(x0, y0), b = RL_DAT(x0 + 1, y0), cc = RL_DAT(x0, y0 + 1), d = RL_DAT(x0 + 1, y0 + 1);
	const float bil = mixf(mixf(a, b, tx), mixf(cc, d, tx), ty);
	const float lo = rlMin(rlMin(a, b), rlMin(cc, d)), hi = rlMax(rlMax(a, b), rlMax(cc, d));
	if (!c->rp.depthSnap || hi - lo < 0.02f) return bil;
	const V3 g0 = rlGuide(c, src, x, y);
	float sum = 0.f, wsum = 0.f;
	for (int j = y0 - 1; j <= y0 + 2; ++j)
		for (int i = x0 - 1; i <= x0 + 2; ++i) {
			/* 深度の点の中心が出力のどの画素に当たるか */
			const int px = rlClamp(rlRound(((float)i - map.ox) / map.sx), 0, c->outW - 1);
			const int py = rlClamp(rlRound(((float)j - map.oy) / map.sy), 0, c->outH - 1);
			const V3 g = rlGuide(c, src, px, py);
			const float cd = rlMax(fabsf(g.x - g0.x), rlMax(fabsf(g.y - g0.y), fabsf(g.z - g0.z)));
			const float ds2 = ((float)i - fx) * ((float)i - fx) + ((float)j - fy) * ((float)j - fy);
			const float w = expf(-0.5f * ds2) * expf(-cd * cd * (1.f / (2.f * SNAP_COLOR_SIGMA * SNAP_COLOR_SIGMA)));
			sum += RL_DAT(i, j) * w;
			wsum += w;
		}
#undef RL_DAT
	return wsum > 1e-6f ? sum / wsum : bil;
}

/* 線画のマスク: 輝度 (表示の値) */
RL_HD RL_INL float rlLuma(const ShadeContext *c, const RLImage &src, int x, int y)
{
	const Rgba p = rlRead(src, x, y);
	const float a = p.a > 1e-6f ? p.a : 1.f;
	float l = 0.2126f * (p.r / a) + 0.7152f * (p.g / a) + 0.0722f * (p.b / a);
	if (c->rp.inputLinear) l = gammaEncode(c, l);
	return saturate(l);
}

/* 箱形のぼかし (1 方向、1 画素)。horizontal なら横、そうでなければ縦 */
RL_HD RL_INL float rlBox(const float *in, int W, int H, int rad, bool horizontal, int x, int y)
{
	float s = 0.f;
	for (int o = -rad; o <= rad; ++o) {
		const int xx = horizontal ? rlClamp(x + o, 0, W - 1) : x;
		const int yy = horizontal ? y : rlClamp(y + o, 0, H - 1);
		s += in[(size_t)yy * W + xx];
	}
	return s / (float)(2 * rad + 1);
}

/* 線画の保護の強さ: 暗く、周りより暗い細い所を線とみなす。
   黒髪のような広い暗部は周りと同じ暗さなので含まれない */
RL_HD RL_INL float rlLineMask(float luma, float blur, float threshold, float protect)
{
	const float T = rlMax(threshold, 0.01f);
	const float darkness = 1.f - smoothstepf(T * 0.5f, T, luma);
	const float contrast = saturate((blur - luma - 0.02f) / LINE_CONTRAST);
	return saturate(darkness * contrast) * protect;
}

/* ------------------------------------------------------------------ */
/* 格子の準備 (1 マスぶん)                                               */
/* ------------------------------------------------------------------ */

/* 1. 深度をマス単位に平均して縮める */
RL_HD RL_INL float rlCellDepth(const ShadeContext *c, int i, int j)
{
	const int W = c->outW, H = c->outH;
	int y0 = (int)((float)j * c->cell);
	const int y1 = rlMin(H, rlMax(y0 + 1, (int)((float)(j + 1) * c->cell)));
	y0 = rlMin(y0, H - 1);
	const int x0 = rlMin((int)((float)i * c->cell), W - 1);
	const int x1 = rlMin(W, rlMax(x0 + 1, (int)((float)(i + 1) * c->cell)));
	float sum = 0.f;
	for (int y = y0; y < y1; ++y)
		for (int x = x0; x < x1; ++x) sum += c->depth[(size_t)y * W + x];
	return sum / (float)((y1 - y0) * (x1 - x0));
}

/* 1b. 深度の膨らみ: 隣のマスと深度が大きく違えば輪郭 (距離 0)。輪郭でなければ大きな値 */
RL_HD RL_INL float rlVolumeSeed(const ShadeContext *c, int i, int j)
{
	const float cd = gridDepthAt(c, i, j);
	const float m = rlMax(rlMax(fabsf(gridDepthAt(c, i - 1, j) - cd), fabsf(gridDepthAt(c, i + 1, j) - cd)),
	                      rlMax(fabsf(gridDepthAt(c, i, j - 1) - cd), fabsf(gridDepthAt(c, i, j + 1) - cd)));
	return m > VOLUME_EDGE ? 0.f : 1e9f;
}

/* 輪郭からの距離 (マス) → 盛り上がり。実写で学習した深度モデルはアニメの人物を平たく推定しやすいので、
   輪郭からの距離に応じて手前に盛り上げ、丸みを付ける。手前の物ほど強く、奥の背景はそのまま */
RL_HD RL_INL float rlBulge(float dist, float depth, float volume)
{
	const float t = rlMin(dist / VOLUME_RADIUS, 1.f);
	const float round = 1.f - (1.f - t) * (1.f - t);
	return volume * VOLUME_SCALE * round * smoothstepf(0.25f, 0.7f, depth);
}

/* 2. 勾配と凹み (原典 surfaceKernel) */
RL_HD RL_INL void rlCellSurface(const ShadeContext *c, int i, int j, float *s)
{
	const int R = (int)GRADIENT_RADIUS;
	const float center = gridDepthAt(c, i, j);
	const float left = gridDepthAt(c, i - R, j), right = gridDepthAt(c, i + R, j);
	const float up = gridDepthAt(c, i, j - R), down = gridDepthAt(c, i, j + R);
	float gx = gentlerDelta(center - left, right - center) / GRADIENT_RADIUS;
	float gy = gentlerDelta(center - up, down - center) / GRADIENT_RADIUS;
	surfaceSlope(gx, gy);
	float occ = 0.f;
	for (int ri = 0; ri < 2; ++ri) {
		const int r = ri == 0 ? OCCLUSION_RADIUS_A : OCCLUSION_RADIUS_B;
		for (int sy = -1; sy <= 1; ++sy)
			for (int sx = -1; sx <= 1; ++sx) {
				if (sx == 0 && sy == 0) continue;
				const float diff = gridDepthAt(c, i + sx * r, j + sy * r) - center;
				const float contact = 1.f - saturate(fabsf(diff) / OCCLUSION_RANGE);
				const float cleared = rlMax(diff - OCCLUSION_FLOOR, 0.f);
				occ += saturate(cleared / OCCLUSION_SCALE) * contact;
			}
	}
	s[0] = gx; s[1] = gy; s[2] = 1.f - saturate(occ / OCCLUSION_TAPS);
}

/* 3. 落ち影 (32 歩のレイマーチ)。陰影をならすときは揺らぎを止める (しきい値で分けると、まだらの粒になるため) */
RL_HD RL_INL float rlCellShadow(const ShadeContext *c, int i, int j, int k)
{
	float u, v;
	gridCellUv(c, i, j, u, v);
	float noise = rlNoise(u * 1024.f, v * 1024.f);
	if (c->rp.shadeSmooth > 0.f) noise = 0.5f;
	const V3 origin = v3(u - 0.5f, v - 0.5f, shadowZ(gridDepthAt(c, i, j)));
	const V3 s2l = c->L[k].pos - origin;
	const float sd = rlMax(length(s2l), 0.0001f);
	const float xy = sqrtf(s2l.x * s2l.x + s2l.y * s2l.y);
	const float reach = sd * (SHADOW_SPAN / rlMax(xy, SHADOW_SPAN));
	return shadowFactor(c, origin, s2l * (1.f / sd), reach, noise, c->L[k].z);
}

/* 3b. 格子の値を、深度が近いマスどうしだけでならす (縦横に分けた 1 回ぶん。輪郭はまたがない)。
   src / dst は 1 マスあたり stride 個の値 */
RL_HD RL_INL void rlSmoothCell(const ShadeContext *c, const float *src, float *dst, int stride, int R, bool horizontal,
                               int i, int j)
{
	const int GW = c->gw, GH = c->gh;
	const float d0 = c->gDepth[(size_t)j * GW + i];
	float acc[RELIGHT_MAX_LIGHTS > 3 ? RELIGHT_MAX_LIGHTS : 3];
	for (int s = 0; s < stride; ++s) acc[s] = 0.f;
	float wsum = 0.f;
	for (int o = -R; o <= R; ++o) {
		const int ii = horizontal ? i + o : i, jj = horizontal ? j : j + o;
		if (ii < 0 || jj < 0 || ii >= GW || jj >= GH) continue;
		const size_t n = (size_t)jj * GW + ii;
		const float dd = (c->gDepth[n] - d0) * 50.f;   /* 深度差 0.02 で重み半分 */
		const float w = (1.f - fabsf((float)o) / (float)(R + 1)) / (1.f + dd * dd);
		for (int s = 0; s < stride; ++s) acc[s] += src[n * stride + s] * w;
		wsum += w;
	}
	const size_t m = ((size_t)j * GW + i) * stride;
	for (int s = 0; s < stride; ++s) dst[m + s] = acc[s] / wsum;
}

/* 格子の面の向き */
RL_HD RL_INL V3 rlCellNormal(const ShadeContext *c, size_t n)
{
	const float *s = &c->gSurf[n * 3];
	const float sx = s[0] * (c->rp.relief * RELIEF_SCALE), sy = s[1] * (c->rp.relief * RELIEF_SCALE);
	const float k = 1.f / (1.f + sqrtf(sx * sx + sy * sy) * SLOPE_COMPRESSION);
	return normalize(v3(-sx * k, -sy * k, 1.f));
}

/* 3c. 「当たり具合」(面の向き × 落ち影 × 光の届く範囲) を格子で求める。
   ならしてから画素でしきい値に掛ける。3D のセルシェーダーと同じく、なめらかな値を一本のしきい値で分けるので、
   明暗の境目がきれいな曲線になる */
RL_HD RL_INL float rlCellKey(const ShadeContext *c, int i, int j, int l)
{
	const size_t n = (size_t)j * c->gw + i;
	const V3 normal = rlCellNormal(c, n);
	float u, v;
	gridCellUv(c, i, j, u, v);
	const V3 position = v3(u - 0.5f, v - 0.5f, surfaceZ(c->gDepth[n]));
	const V3 toLight = c->L[l].pos - position;
	const float distance = rlMax(length(toLight), 0.0001f);
	const float spread = distance / (LIGHT_RADIUS * c->L[l].range);
	const float falloff = 1.f / (1.f + spread * spread);
	const float wrapped = saturate((dot(normal, toLight * (1.f / distance)) + LIGHT_WRAP) / (1.f + LIGHT_WRAP));
	const float shadow = c->gShadow ? mixf(1.f, c->gShadow[n * c->nL + l], c->rp.shadow) : 1.f;
	return wrapped * shadow * saturate(falloff * DARK_SIDE_REACH);
}

/* 4. リムの下準備: 周り (RC マス) で最も奥の深度 (膨らみを除く)。横 → 縦の 2 回 */
RL_HD RL_INL float rlRowMin(const ShadeContext *c, int RC, int i, int j)
{
	float m = 1e9f;
	for (int x = rlMax(0, i - RC); x <= rlMin(c->gw - 1, i + RC); ++x) {
		const size_t n = (size_t)j * c->gw + x;
		m = rlMin(m, c->gDepth[n] - (c->gBulge ? c->gBulge[n] : 0.f));
	}
	return m;
}

RL_HD RL_INL float rlColMin(const ShadeContext *c, const float *rowMin, int RC, int i, int j)
{
	float m = 1e9f;
	for (int y = rlMax(0, j - RC); y <= rlMin(c->gh - 1, j + RC); ++y) m = rlMin(m, rowMin[(size_t)y * c->gw + i]);
	return m;
}

/* ------------------------------------------------------------------ */
/* 画素の補間                                                           */
/* ------------------------------------------------------------------ */

/* 深度が近いマスを重く見る補間 (人物の輪郭で影や縁がにじまないように) */
struct GridTaps {
	size_t idx[4];
	float  w[4];
};

/* 周りの 4 マスが手前と奥に分かれている (輪郭をまたぐ) ときは、手前組と奥組の値を
   この画素の深度で混ぜる。画素の深度はなめらかなので、切り替わりが格子の段にならず輪郭に沿う */
RL_HD RL_INL void depthAwareTaps(const ShadeContext *c, float bx, float by, float center, GridTaps &t)
{
	float gx, gy;
	bufferToGrid(c, bx, by, gx, gy);
	gx = clampf(gx, 0.f, (float)(c->gw - 1));
	gy = clampf(gy, 0.f, (float)(c->gh - 1));
	const int x0 = (int)gx, y0 = (int)gy;
	const int x1 = rlMin(x0 + 1, c->gw - 1), y1 = rlMin(y0 + 1, c->gh - 1);
	const float fx = gx - (float)x0, fy = gy - (float)y0;
	const int xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
	const float bw[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
	float d[4], dmin = 1e9f, dmax = -1e9f;
	for (int k = 0; k < 4; ++k) {
		t.idx[k] = (size_t)ys[k] * c->gw + xs[k];
		d[k] = c->gDepth[t.idx[k]];
		dmin = rlMin(dmin, d[k]);
		dmax = rlMax(dmax, d[k]);
		t.w[k] = bw[k];
	}
	if (dmax - dmin < 0.02f) return;   /* 輪郭ではない: 普通のバイリニア */
	const float mid = 0.5f * (dmin + dmax);
	float wn = 0.f, wf = 0.f, dn = 0.f, df = 0.f;
	for (int k = 0; k < 4; ++k) {
		const float b = rlMax(bw[k], 1e-4f);
		if (d[k] >= mid) { wn += b; dn += d[k] * b; } else { wf += b; df += d[k] * b; }
	}
	dn /= wn; df /= wf;
	const float s = smoothstepf(0.3f, 0.7f, saturate((center - df) / rlMax(dn - df, 1e-4f)));
	for (int k = 0; k < 4; ++k) {
		const float b = rlMax(bw[k], 1e-4f);
		t.w[k] = (d[k] >= mid) ? s * b / wn : (1.f - s) * b / wf;
	}
}

RL_HD RL_INL float tapSample(const GridTaps &t, const float *arr, int stride, int comp)
{
	return arr[t.idx[0] * stride + comp] * t.w[0] + arr[t.idx[1] * stride + comp] * t.w[1] +
	       arr[t.idx[2] * stride + comp] * t.w[2] + arr[t.idx[3] * stride + comp] * t.w[3];
}

/* 出力の大きさの深度 (d) をバイリニアで引く (リム用。膨らみは含めない) */
RL_HD RL_INL float depthBilinearOf(const ShadeContext *c, const float *d, float x, float y)
{
	const int W = c->outW, H = c->outH;
	x = clampf(x, 0.f, (float)(W - 1));
	y = clampf(y, 0.f, (float)(H - 1));
	const int x0 = (int)x, y0 = (int)y;
	const int x1 = rlMin(x0 + 1, W - 1), y1 = rlMin(y0 + 1, H - 1);
	const float fx = x - (float)x0, fy = y - (float)y0;
	return mixf(mixf(d[(size_t)y0 * W + x0], d[(size_t)y0 * W + x1], fx),
	            mixf(d[(size_t)y1 * W + x0], d[(size_t)y1 * W + x1], fx), fy);
}
RL_HD RL_INL float depthBilinear(const ShadeContext *c, float x, float y) { return depthBilinearOf(c, c->depth, x, y); }

/* 回り込みの広さ (Rim Spread): 輪郭の外向き (dot = 光源の向きとの内積、-1..1) ごとの強さ。
   現実の逆光では、光源の側を向いた輪郭だけが光り、反対側 (例: 上に光源があるときのスカートの下の縁) は光らない */
RL_HD RL_INL float rimSpreadWeight(float dot, float spread)
{
	if (spread >= 0.999f) return 1.f;
	const float limit = 1.f - 2.f * spread;
	return smoothstepf(limit - 0.3f, rlMin(limit + 0.3f, 1.f), dot);
}

/* 届く距離 (Rim Reach): 画面上で光源から離れた輪郭ほど弱く。回り込む光は光源の近くほど強い */
RL_HD RL_INL float rimReachWeight(float dist, float reach)
{
	if (reach >= 2.999f) return 1.f;
	return 1.f - smoothstepf(reach * 0.4f, reach, dist);
}

/* リムの帯 (1 方向)。撮影でリムライトや透過光を作るときの定番「マットを光源の方へずらして引く」を深度でやる。
   この画素から (dx, dy) の向きにリムの幅の中で RIM_SAMPLES 点ずらして深度を見て、ずらした先が十分奥
   (物の外) になる割合を返す。輪郭のすぐ内側で 1、幅の奥へ向かって 0 に下がる。
   以前は 1 画素ずつ輪郭を探して距離を測っていたため、輪郭の判定が少し揺れるだけで帯の幅が段になり、
   ビーズのように切れて汚く見えた。この方法なら帯の内側の線は輪郭をずらした形そのものになるので、幅がそろってなめらか。
   後ろとの深度差が小さい重なり (物の内側) は弱く、背景に対する外側の輪郭で最大。出力の端は輪郭とみなさない */
/* 出力の画素の線らしさ (0..1、最寄りの画素) */
RL_HD RL_INL float lineAt(const ShadeContext *c, float x, float y)
{
	if (!c->lineMask) return 0.f;
	const int xi = rlClamp(rlRound(x), 0, c->outW - 1), yi = rlClamp(rlRound(y), 0, c->outH - 1);
	return c->lineMask[(size_t)yi * c->outW + xi];
}

/* この画素が、輪郭線の外側 (背景の側) にいるのに深度では手前の物と判定されている所か。
   深度の境目は線画より 1〜3px 外へずれることがあり、そのすき間にリムが乗ると人物の輪郭の外へ光がはみ出す。
   すぐ後ろ (dx, dy の逆向き、物の内側の方) 3px 以内に線があり、前 (光源の側) へ進むと線を越えずに深度が落ちるなら、
   線の外のすき間とみなす。線が無い輪郭は判定しない (これまでどおり深度の境目を使う) */
RL_HD RL_INL bool rlOutsideOutline(const ShadeContext *c, float rimW, float x, float y, float dx, float dy, float center)
{
	if (!c->lineMask || lineAt(c, x, y) > 0.5f) return false;
	bool behind = false;
	for (float k = 1.f; k <= 3.f && !behind; k += 1.f) behind = lineAt(c, x - dx * k, y - dy * k) > 0.5f;
	if (!behind) return false;
	for (float t = 1.f; t <= rimW + 3.f; t += 1.f) {
		const float sx = x + dx * t, sy = y + dy * t;
		if (sx < 0.f || sy < 0.f || sx > (float)(c->outW - 1) || sy > (float)(c->outH - 1)) return false;
		if (lineAt(c, sx, sy) > 0.5f) return false;                      /* 先に線がある = 自分は物の内側 */
		if (center - depthBilinear(c, sx, sy) > RIM_EDGE) return true;     /* 線を越えずに背景に出た = 線の外のすき間 */
	}
	return false;
}

/* Rim Smoothing のときは、この画素とずらした先の両方を輪郭をならした深度 (L.rimDepth) から引く
   (center も呼ぶ側がならした深度を渡す)。帯の内側・外側の線がどちらもなめらかになる。
   ならした輪郭が実際の輪郭より外に出た所 (くぼみ) は、呼ぶ側が L.rimInside で消す (線画の外へは出さない) */
RL_HD RL_INL float rimOffsetBand(const ShadeContext *c, const LightCtx &L, float x, float y, float dx, float dy, float center)
{
	if (rlOutsideOutline(c, L.rimW, x, y, dx, dy, center)) return 0.f;
	const float *dz = L.rimDepth ? L.rimDepth : c->depth;
	float acc = 0.f;
	for (int i = 0; i < RIM_SAMPLES; ++i) {
		const float t = L.rimW * ((float)i + 0.5f) / (float)RIM_SAMPLES;
		const float sx = x + dx * t, sy = y + dy * t;
		if (sx < 0.f || sy < 0.f || sx > (float)(c->outW - 1) || sy > (float)(c->outH - 1)) continue;
		acc += smoothstepf(RIM_EDGE, RIM_SILHOUETTE, center - depthBilinearOf(c, dz, sx, sy));
	}
	return acc / (float)RIM_SAMPLES;
}

/* リムの下地 (1 画素、光源 k、0..1)。光源の向きの帯と、逆光なら輪郭全体 (Rim Spread で光源側に絞る) の帯。
   帯の形 (Rim Softness: 0 で幅いっぱいのくっきりした帯、1 で輪郭から内側へなだらかに消える) もここで決める。
   このあと少しぼかしてから (rlBoxStrided)、画素の照明で光源の色と強さを付ける */
RL_HD RL_INL float rlRimMask(const ShadeContext *c, int x, int y, int k)
{
	if (!c->hasDepth || !c->gRimMin) return 0.f;
	const LightCtx &L = c->L[k];
	if (!L.rim) return 0.f;   /* この光源はリムなし */
	const float bx = (float)x, by = (float)y;
	const float raw = c->depth[(size_t)y * c->outW + x];
	const int gi = rlClamp((int)((bx + 0.5f) / c->cell), 0, c->gw - 1);
	const int gj = rlClamp((int)((by + 0.5f) / c->cell), 0, c->gh - 1);
	if (raw - c->gRimMin[(size_t)gj * c->gw + gi] <= RIM_EDGE * 0.5f) return 0.f;   /* 近くに輪郭が無い */
	const float u = 0.5f + (bx + c->originX + 0.5f - c->layerW * 0.5f) / c->layerH;
	const float v = (by + c->originY + 0.5f) / c->layerH;
	const float lx = L.u - u, ly = L.v - v;   /* 画面上で光源へ向かう向き (uv と出力のピクセルは縦横同じ比率) */
	const float ll = sqrtf(lx * lx + ly * ly);
	float center = raw;
	if (c->gBulge) {
		float gx, gy;
		bufferToGrid(c, bx, by, gx, gy);
		center += gridSample(c, c->gBulge, 1, 0, gx, gy);
	}
	const float back = saturate((surfaceZ(center) - L.z) / RIM_BACK_SPAN);
	const int place = L.rimPlacement;
	/* 帯を作る基準の深度 (Rim Smoothing のときは輪郭をならした深度) と、実際の輪郭の内側か */
	const size_t pn = (size_t)y * c->outW + x;
	const float inside = L.rimInside ? L.rimInside[pn] : 1.f;
	if (inside <= 0.f) return 0.f;
	const float base = L.rimDepth ? L.rimDepth[pn] : raw;
	float s = (ll > 1e-4f) ? rimOffsetBand(c, L, bx, by, lx / ll, ly / ll, base) : 0.f;
	const float aroundW = (place == RIM_ALL_AROUND) ? 1.f : (place == RIM_AUTO ? back : 0.f);
	if (aroundW > 0.f) {
		float best = 0.f;
		for (int d = 0; d < RIM_AROUND_DIRS; ++d) {
			const float a = 6.2831853f * (float)d / (float)RIM_AROUND_DIRS;
			const float ca = cosf(a), sa = sinf(a);
			const float dot = ll > 1e-4f ? (ca * lx + sa * ly) / ll : 1.f;
			const float w = rimSpreadWeight(dot, L.rimSpread);
			if (w <= 0.f) continue;
			best = rlMax(best, rimOffsetBand(c, L, bx, by, ca, sa, base) * w);
		}
		s = mixf(s, rlMax(s, best), aroundW);
	}
	return mixf(smoothstepf(0.f, 0.25f, s), s, L.rimSoft) * inside;
}

/* リムの輪郭をならす (Rim Smoothing)。深度に中央値フィルターを掛ける (横 → 縦、これを 2 回)。
   中央値は段差 (輪郭) をくっきり保ったまま、半径より小さい出っ張りやくぼみ (深度推定の細かい揺れ・粗い解像度の階段・
   フリルの細かい波形) だけを消すので、輪郭がなめらかな線になる。ぼかしと違って深度の差は薄まらず、
   奥行きが何段も重なった所でも実際にある深度の値しか出さない (差を大げさにしない)。
   この深度で帯を作ると、帯の内側・外側の線がどちらもなめらかになる。
   ならした輪郭が実際の輪郭より外へ出た所 (くぼみ) にはリムを付けない (rlRimInside)。
   同じ半径の光源どうしは同じものを使い回す。rlRimSmoothShare は、光源 k の前に同じ半径の光源があればその番号を返す (無ければ k) */
#define RL_MEDIAN_MAX 16   /* 中央値の半径の上限 (画素) */
RL_HD RL_INL float rlMedian1D(const float *in, int W, int H, int rad, bool horizontal, int x, int y)
{
	float v[2 * RL_MEDIAN_MAX + 1];
	rad = rlMin(rad, RL_MEDIAN_MAX);
	int m = 0;
	for (int o = -rad; o <= rad; ++o) {
		const int xx = horizontal ? rlClamp(x + o, 0, W - 1) : x;
		const int yy = horizontal ? y : rlClamp(y + o, 0, H - 1);
		const float a = in[(size_t)yy * W + xx];
		int b = m - 1;
		while (b >= 0 && v[b] > a) { v[b + 1] = v[b]; --b; }
		v[b + 1] = a;
		++m;
	}
	return v[m / 2];
}
/* 実際の深度 (raw) がならした深度より大きく奥なら、ならした輪郭が外へ出たくぼみ (背景) */
RL_HD RL_INL float rlRimInside(float raw, float smoothed) { return smoothstepf(-RIM_EDGE, -RIM_EDGE * 0.25f, raw - smoothed); }
RL_HD RL_INL int rlRimSmoothShare(const ShadeContext &c, int k)
{
	for (int j = 0; j < k; ++j)
		if (c.L[j].rim && c.L[j].rimSmoothR == c.L[k].rimSmoothR) return j;
	return k;
}

/* ぼかした下地を輪郭の内側に収める。ぼかすと帯が輪郭の外 (背景) へもにじむので、
   ぼかす前の下地が無い画素は消す (撮影でマットのずらしをぼかした後、元のマットで切り抜くのと同じ)。
   人物の輪郭の外へ光を 1px も出さない */
RL_HD RL_INL float rlRimClip(const float *raw, const float *blurred, int W, int H, int stride, int k, int x, int y)
{
	(void)H;
	const size_t n = ((size_t)y * W + x) * stride + k;
	return blurred[n] * smoothstepf(0.f, 0.1f, raw[n]);
}

/* リムの下地のぼかしの半径 (px)。光源ごと */
RL_HD RL_INL int rlRimBlurRadius(const LightCtx &L) { return rlMax(1, rlRound(L.rimW * 0.3f)); }

/* 箱形のぼかし (1 方向、1 画素、stride 個ずつ並んだ値の comp 番目) */
RL_HD RL_INL float rlBoxStrided(const float *in, int W, int H, int stride, int comp, int rad, bool horizontal, int x, int y)
{
	float s = 0.f;
	for (int o = -rad; o <= rad; ++o) {
		const int xx = horizontal ? rlClamp(x + o, 0, W - 1) : x;
		const int yy = horizontal ? y : rlClamp(y + o, 0, H - 1);
		s += in[((size_t)yy * W + xx) * stride + comp];
	}
	return s / (float)(2 * rad + 1);
}

/* ------------------------------------------------------------------ */
/* 画素シェーダ (原典 relightFragment + アニメ向け)                      */
/* ------------------------------------------------------------------ */

RL_HD inline void shadePixel(const ShadeContext *c, int x, int y, const Rgba &src, Rgba &out)
{
	const RenderParams *rp = &c->rp;

	/* このピクセルの uv */
	const float px = (float)x + c->originX + 0.5f;
	const float py = (float)y + c->originY + 0.5f;
	const float u = 0.5f + (px - c->layerW * 0.5f) / c->layerH;
	const float v = py / c->layerH;

	/* --- 表面: 格子から補間 (原典も 448 texel の surface を線形補間で引く) --- */
	const float bx = (float)x, by = (float)y;
	float ggx = 0.f, ggy = 0.f;
	float center = 1.f;
	if (c->hasDepth) {
		bufferToGrid(c, bx, by, ggx, ggy);
		center = c->depth[(size_t)y * c->outW + x];
		if (c->gBulge) center += gridSample(c, c->gBulge, 1, 0, ggx, ggy);
	}

	V3 normal;
	if (rp->normalSource == 2 && c->normalA.img.data) {
		const Rgba p = rlReadAux(c->normalA, x, y, rp->passEncoding == 2);
		/* OpenGL 法線 (x右 y上 zカメラ側) → 原典の空間 (x右 y下 zカメラ側)。Relief で傾きを強弱 */
		const float nx = p.r * 2.f - 1.f, ny = -(p.g * 2.f - 1.f), nz = rlMax(p.b * 2.f - 1.f, 0.05f);
		normal = normalize(v3(nx * rp->relief, ny * rp->relief, nz));
	} else if (c->hasDepth) {
		const float gx = gridSample(c, c->gSurf, 3, 0, ggx, ggy);
		const float gy = gridSample(c, c->gSurf, 3, 1, ggx, ggy);
		const float sx = gx * (rp->relief * RELIEF_SCALE), sy = gy * (rp->relief * RELIEF_SCALE);
		const float sl = sqrtf(sx * sx + sy * sy);
		const float k = 1.f / (1.f + sl * SLOPE_COMPRESSION);
		normal = normalize(v3(-sx * k, -sy * k, 1.f));
	} else {
		normal = v3(0.f, 0.f, 1.f);
	}

	const float occlusionTerm = c->hasDepth ? gridSample(c, c->gSurf, 3, 2, ggx, ggy) : 1.f;
	const float lineRaw = c->lineMask ? c->lineMask[(size_t)y * c->outW + x] : 0.f;
	const float lineM = lineRaw * rp->lineProtect;   /* 線画を元の色に戻す強さ */

	if (rp->outputMode == OUTPUT_DEPTH) {
		const V3 r = depthRamp(saturate(center));
		out.a = 1.f; out.r = r.x; out.g = r.y; out.b = r.z;
		return;
	}
	if (rp->outputMode == OUTPUT_NORMALS) {
		out.a = 1.f;
		out.r = normal.x * 0.5f + 0.5f; out.g = normal.y * 0.5f + 0.5f; out.b = normal.z * 0.5f + 0.5f;
		return;
	}
	if (rp->outputMode == OUTPUT_LINES) {
		out.a = 1.f; out.r = out.g = out.b = lineRaw;
		return;
	}

	/* 深度が近いマスを重く見る補間の重み (落ち影と当たり具合で使う) */
	GridTaps taps;
	const bool useTaps = c->hasDepth && c->nL > 0 && (rp->shadow > 0.f || c->gKey);
	if (useTaps) depthAwareTaps(c, bx, by, center, taps);

	/* --- 照明 --- */
	const float noise = rlNoise(u * 1024.f, v * 1024.f);
	const V3 position = v3(u - 0.5f, v - 0.5f, surfaceZ(center));
	/* 立体的な陰影の割合。Cinematic は原典どおり立体的な陰影だけ。
	   光源ごとの当て方 (Light Look) は光源のところで決め、ここは画面全体 (Scene の Look) の値 */
	const float sceneForm = (rp->look == LOOK_CINEMATIC) ? 1.f : rp->form;
	/* 凹みの暗さも立体的な陰影の一部 (深度の細部から作るので揺れやすい) */
	const float occlusion = mixf(1.f, occlusionTerm, rp->occlusion * sceneForm);
	const int steps = rlMax(1, rp->celSteps);

	/* 入力 (premultiplied) → 非乗算の表示色 */
	const float alpha = src.a;
	V3 cam = (alpha > 1e-6f) ? v3(src.r / alpha, src.g / alpha, src.b / alpha) : v3(0.f, 0.f, 0.f);
	V3 albedo;
	if (rp->inputLinear) {
		albedo = v3(rlMax(cam.x, 0.f), rlMax(cam.y, 0.f), rlMax(cam.z, 0.f));
	} else {
		cam = v3(saturate(cam.x), saturate(cam.y), saturate(cam.z));
		albedo = v3(gammaDecode(c, cam.x), gammaDecode(c, cam.y), gammaDecode(c, cam.z));   /* x^2.2 */
	}

	V3 diffuse = v3(0.f, 0.f, 0.f), spec = v3(0.f, 0.f, 0.f), rimAdd = v3(0.f, 0.f, 0.f);
	float keyMax = 0.f, anyLight = 0.f, shadowView = 1.f;
	float tintKeyMax = 0.f;   /* 影色を乗せる判定用 (撮影寄りの当たり具合を主にする) */
	for (int k = 0; k < c->nL; ++k) {
		const LightCtx &L = c->L[k];
		/* この光源の当て方。Cinematic: 原典どおり (面の向きによる陰影、光をそのまま足す、セル調なし)。
		   Anime: 元の色を保って光を足す (撮影寄りのグラデーションと面の向きを Form Shading で混ぜる) */
		const bool cine = L.look == LOOK_CINEMATIC;
		const float form = cine ? 1.f : rp->form;
		/* 光源で Cinematic を選んだら段は付けない (Same as Scene なら以前どおり Cel Shading に従う) */
		const float cel = (cine && L.lookOwn) ? 0.f : rp->cel;
		const V3 toLight = L.pos - position;
		const float distance = rlMax(length(toLight), 0.0001f);
		const V3 lightDir = toLight * (1.f / distance);
		const float spread = distance / (LIGHT_RADIUS * L.range);   /* Range で光の届く距離を伸び縮み */
		const float falloff = 1.f / (1.f + spread * spread);
		const float wrapped = saturate((dot(normal, lightDir) + LIGHT_WRAP) / (1.f + LIGHT_WRAP));

		float shadow = 1.f;
		if (rp->shadow > 0.f && c->hasDepth)
			shadow = mixf(1.f, tapSample(taps, c->gShadow, c->nL, k), rp->shadow);
		shadowView = rlMin(shadowView, shadow);

		/* 「当たり具合」は 2 つを Form Shading の割合で混ぜる。
		   撮影寄り (para): 画面上で光源からの距離によるなめらかなグラデーション (撮影のパラのように)。
		     光源が物の後ろ (逆光) なら、手前の物は暗い側にする。面の細かい向きを使わないので時間で揺れない。
		   立体的 (form): 面の向き × 落ち影 × 光の届く範囲 (原典の考え方)。形に沿うが、深度の揺れで揺れやすい。
		   白い服のように明るさを足しても変わらない所でも、当たり具合が低い所に影色が乗るので光の範囲が見える。
		   セル調ではこれを段に分ける */
		const float plx = L.u - u, ply = L.v - v;
		const float sp2 = sqrtf(plx * plx + ply * ply) / rlMax(rp->lightSpread * L.range, 0.05f);
		const float backSide = saturate((position.z - L.z) / RIM_BACK_SPAN);
		const float paraKey = (1.f / (1.f + sp2 * sp2)) * (1.f - BACK_DARK * backSide);
		const float formKey = c->gKey ? tapSample(taps, c->gKey, c->nL, k) : wrapped * shadow * saturate(falloff * DARK_SIDE_REACH);
		float key = mixf(paraKey, formKey, form);
		/* 光の量。原典は wrapped^2 × 落ち影 × 距離の減衰。撮影寄りはグラデーションそのもの */
		float amount = mixf(paraKey, wrapped * wrapped * shadow * falloff, form);
		if (cel > 0.f) {
			const float q = celQuant(key, steps, rp->celSoft);
			amount = mixf(amount, mixf(q, q * falloff, form), cel);
			key = mixf(key, q, cel);
		}
		/* Anime の光源は元の色を保つよう控えめに足す (以前は画面全体で掛けていた割合を光源ごとに) */
		diffuse = diffuse + L.tint * (amount * L.intensity * (cine ? 1.f : ANIME_LIGHT_GAIN));
		const float weight = saturate(L.intensity / 0.5f);
		keyMax = rlMax(keyMax, key * weight);
		/* 影色の判定は、時間で揺れない撮影寄りの当たり具合を主にする。立体的な当たり具合は Form Shading の 2 乗の割合でだけ混ぜる
		   (しきい値で影色を乗せるので、揺れやすい値が混ざると動画でちらつく) */
		{
			float pk = paraKey;
			if (cel > 0.f) pk = mixf(pk, celQuant(pk, steps, rp->celSoft), cel);
			tintKeyMax = rlMax(tintKeyMax, mixf(pk, key, form) * weight);
		}
		anyLight = rlMax(anyLight, weight);

		/* 鏡面: pow(t, 36) と pow(g, 5) を掛け算で */
		const V3 halfDir = normalize(lightDir + v3(0.f, 0.f, 1.f));
		const float t1 = saturate(dot(normal, halfDir));
		const float t2 = t1 * t1, t4 = t2 * t2, t8 = t4 * t4, t16 = t8 * t8, t32 = t16 * t16;
		const float lobe = t32 * t4;   /* SPECULAR_POWER = 36 */
		const float g1 = 1.f - saturate(normal.z), g2 = g1 * g1;
		const float grazing = g2 * g2 * g1;
		const float highlight = lobe * (SPECULAR_F0 + (1.f - SPECULAR_F0) * grazing);
		spec = spec + L.tint * (highlight * falloff * shadow * occlusion * rp->specular * L.intensity * form * (cine ? 1.f : 0.5f));

		/* リムライト: 先に作ってぼかした下地 (rlRimMask) に、光源の色・強さ・距離を付ける */
		if (c->rimMask) {
			float s = c->rimMask[((size_t)y * c->outW + x) * c->nL + k];
			if (s > 0.f) {
				const float back = saturate((position.z - L.z) / RIM_BACK_SPAN);
				/* Auto: 光源が手前なら控えめ (面の明るさと二重にならないように)、逆光なら強め */
				const float gain = (L.rimPlacement == RIM_AUTO) ? mixf(RIM_FRONT, 1.5f, back) : 1.f;
				s *= rimReachWeight(sqrtf(plx * plx + ply * ply), L.rimReach);
				rimAdd = rimAdd + mul(L.tint, L.rimColor) * (s * gain * mixf(1.f, falloff, 0.5f) * L.intensity * L.rimAmount * RIM_GAIN);
			}
		}
	}

	if (rp->outputMode == OUTPUT_SHADOW) {
		const float s = shadowView * occlusion;
		out.a = 1.f; out.r = out.g = out.b = s;
		return;
	}

	/* 影色: 光が当たらない側に掛ける。色は乗算 (表示の色で掛けたのと同じになるようリニアで) */
	const float darkSide = (1.f - smoothstepf(DARK_SIDE_LO, DARK_SIDE_HI, tintKeyMax)) * anyLight * rp->shadowTint;
	const V3 shadowCol = v3(gammaDecode(c, rp->shadowColor[0]), gammaDecode(c, rp->shadowColor[1]), gammaDecode(c, rp->shadowColor[2]));
	const V3 shadeMul = mix3(v3(1.f, 1.f, 1.f), shadowCol, darkSide);
	const V3 ambient = v3(rp->ambient[0], rp->ambient[1], rp->ambient[2]);

	V3 lit = mul(mul(albedo, ambient), shadeMul) * (rp->exposure * occlusion);
	/* 光の強さの割合 (Anime は控えめ) は光源ごとに掛けてある */
	lit = lit + mul(albedo, diffuse) + spec;
	/* リムは面の色に沿った光として足す (暗い色の上でも少し見えるように白を少しだけ混ぜる) */
	lit = lit + mul(mix3(albedo, v3(1.f, 1.f, 1.f), 0.3f), rimAdd);

	float bulbCover = 0.f;
	{
		for (int k = 0; k < c->nL; ++k) {
			const LightCtx &L = c->L[k];
			if (!L.show || L.presence <= 0.f) continue;   /* Show Light は光源ごと */
			/* 原典 bulbSurface */
			const float du = u - L.u, dv = v - L.v;
			const float dist = sqrtf(du * du + dv * dv);
			const float sp = dist / L.bulbRadius;
			const float limb = saturate(sp);
			const float dome = sqrtf(rlMax(1.f - limb * limb, 0.f));
			const float facing = dome * dome;
			const float front = L.z + BULB_WORLD_RADIUS * dome;
			const float solid = smoothstepf(0.f, BULB_OCCLUSION_SOFTNESS, front - surfaceZ(center));
			/* fwidth(spread) を解析的に: (|du/dx| + |dv/dy|) 方向成分の和 / (半径 × 高さ) */
			const float fw = (dist > 1e-6f) ? (fabsf(du) + fabsf(dv)) / (dist * L.bulbRadius * c->layerH)
			                                : 1.f / (L.bulbRadius * c->layerH);
			const float edge = clampf(fw * BULB_EDGE, BULB_EDGE_FLOOR, BULB_EDGE_LIMIT);
			const float coverage = (1.f - smoothstepf(1.f - edge, 1.f + edge, sp)) * solid;
			const V3 hue = mix3(L.tint, v3(1.f, 1.f, 1.f), facing * facing);
			const V3 bulb = hue * (BULB_CORE * mixf(BULB_LIMB, 1.f, facing));
			lit = mix3(lit, bulb * L.presence, coverage * L.presence);
			bulbCover = rlMax(bulbCover, coverage * L.presence);
			/* 原典 bulbGlow。光源が物の後ろにあるときは、画素ごとに「この面より光源が手前か」で
			   にじみを出す (人物の周りの背景だけが光る逆光になる) */
			float vis = L.bulbExposure;
			const float behind = saturate(-L.z / 0.1f);
			if (behind > 0.f) {
				const float pixelOpen = smoothstepf(-0.02f, 0.05f, L.z - surfaceZ(center));
				vis = mixf(vis, mixf(0.15f, 1.f, pixelOpen), behind);
			}
			const float halo = expf(-sp / BULB_HALO_SPAN);
			const float veil = expf(-sp / BULB_VEIL_SPAN);
			lit = lit + L.tint * ((halo * BULB_HALO + veil * BULB_VEIL) * vis * L.presence);
		}
	}

	const V3 mapped = (rp->look == LOOK_CINEMATIC) ? tonemap(lit) : animeTone(lit);
	V3 display;
	if (rp->inputLinear) {
		display = mapped;
	} else {
		display = v3(gammaEncode(c, mapped.x), gammaEncode(c, mapped.y), gammaEncode(c, mapped.z));   /* x^(1/2.2) */
		const float d = (noise - 0.5f) * DITHER_STEP;
		display = v3(display.x + d, display.y + d, display.z + d);
	}

	/* 線画は元の色のまま残す (光の玉の上は除く)。
	   リムが乗る画素では線のはっきりした芯だけを守る。輪郭線の縁 (アンチエイリアスや圧縮のにじみで線らしさが半端な画素) まで
	   守ると、輪郭に沿ったリムの帯が細かく欠けてガタガタに見えるため */
	if (lineM > 0.f) {
		const float rimHere = saturate(rlMax(rimAdd.x, rlMax(rimAdd.y, rimAdd.z)) * 10.f);
		const float lineEff = mixf(lineRaw, smoothstepf(0.6f, 0.95f, lineRaw), rimHere) * rp->lineProtect;
		display = mix3(display, cam, lineEff * (1.f - bulbCover));
	}

	/* マスクと Mix で元の色と混ぜ、premultiplied に戻す */
	const float w = maskAt(c, x, y) * rp->mix;
	const V3 res = mix3(cam, display, w);
	out.a = alpha;
	out.r = res.x * alpha;
	out.g = res.y * alpha;
	out.b = res.z * alpha;
}

/* ------------------------------------------------------------------ */
/* 仕上げ (Edge Dither、1 画素ぶん)                                      */
/* ------------------------------------------------------------------ */

/* Edge Dither を掛けるか */
RL_HD RL_INL bool rlUseEdgeDither(const ShadeContext &c)
{
	return c.hasDepth && c.rp.edgeDither > 0.f && c.rp.outputMode == OUTPUT_RELIT;
}

/* 深度の輪郭の近くだけ、効果 (照明後 − 元の絵) を輪郭に沿う向きにならす。
   深度は素材より粗いので、影やリムの縁に画素の段 (ギザギザ) が残る。輪郭を横切る向きにはぼかさず、
   沿う向きだけで平均するので、縁の切れ味は保ったまま段が消える。
   読み取る位置を画素ごとに少しずらし (ディザ)、残った段を細かい粒に散らす。
   delta: 効果の差分 (r, g, b を 3 個ずつ)。src: 元の画素。戻り値を out に (変えないなら false) */
RL_HD RL_INL bool rlEdgeDither(const ShadeContext *c, const float *delta, const Rgba &s, int x, int y, Rgba &out)
{
	const int W = c->outW, H = c->outH;
#define RL_D(xx, yy) c->depth[(size_t)rlClamp((yy), 0, H - 1) * W + rlClamp((xx), 0, W - 1)]
	const float gx = 0.5f * (RL_D(x + 1, y) - RL_D(x - 1, y));
	const float gy = 0.5f * (RL_D(x, y + 1) - RL_D(x, y - 1));
#undef RL_D
	const float g = sqrtf(gx * gx + gy * gy);
	const float e = smoothstepf(0.004f, 0.015f, g);
	if (e <= 0.f) return false;
	const float R = rlMax(1.f, c->rp.edgeDither * 4.f * c->layerH / 1080.f);   /* 片側の長さ (px) */
	const int N = rlMax(3, (int)ceilf(R * 2.f) + 1);
	/* 輪郭に沿う向き (傾きに直交) に、ずらした位置で平均する */
	const float tx = -gy / g, ty = gx / g;
	const float jitter = rlNoise((float)x, (float)y);
	float sr = 0.f, sg = 0.f, sb = 0.f, ws = 0.f;
	for (int k = 0; k < N; ++k) {
		const float o = ((float)k + jitter) / (float)N * 2.f * R - R;
		const float w = 1.f - fabsf(o) / (R + 1.f);
		const float fx = clampf((float)x + tx * o, 0.f, (float)(W - 1));
		const float fy = clampf((float)y + ty * o, 0.f, (float)(H - 1));
		const int x0 = (int)fx, y0 = (int)fy;
		const int x1 = rlMin(x0 + 1, W - 1), y1 = rlMin(y0 + 1, H - 1);
		const float ax = fx - (float)x0, ay = fy - (float)y0;
		const float *a = &delta[((size_t)y0 * W + x0) * 3], *b = &delta[((size_t)y0 * W + x1) * 3];
		const float *ee = &delta[((size_t)y1 * W + x0) * 3], *f = &delta[((size_t)y1 * W + x1) * 3];
		sr += mixf(mixf(a[0], b[0], ax), mixf(ee[0], f[0], ax), ay) * w;
		sg += mixf(mixf(a[1], b[1], ax), mixf(ee[1], f[1], ax), ay) * w;
		sb += mixf(mixf(a[2], b[2], ax), mixf(ee[2], f[2], ax), ay) * w;
		ws += w;
	}
	const float *d0 = &delta[((size_t)y * W + x) * 3];
	out.r = s.r + mixf(d0[0], sr / ws, e);
	out.g = s.g + mixf(d0[1], sg / ws, e);
	out.b = s.b + mixf(d0[2], sb / ws, e);
	return true;
}
