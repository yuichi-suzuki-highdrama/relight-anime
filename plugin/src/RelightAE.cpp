/*
 * RelightAE.cpp
 * Relight Anime - 深度パスから光を注入する SmartFX エフェクト (32bpc / MFR 対応)
 *
 * 照明モデルは TypeGPU の作例 "Monocular Light Injection"
 *   apps/typegpu-docs/src/examples/image-processing/monocular-light-injection/shaders.ts
 *   (MIT License, Copyright (c) 2025 Software Mansion)
 * を C++ に移植したもの。定数と式は原典に合わせてある。
 * そのうえでアニメ向けの処理 (Look = Anime、影色、セル調、リムライト、線画の保護、深度の膨らみ、
 * 複数光源、プリセット) を足している。Look = Cinematic・アニメ向けの値がすべて 0 なら原典と同じ絵になる。
 *
 * 座標 (原典の uv 空間を一般のアスペクト比に拡張):
 *   u = 0.5 + (px - W/2) / H,  v = py / H   (px, py はレイヤー座標、H = レイヤーの高さ)
 *   縦は 0..1、横は高さ比で伸びる。z はカメラ側が正。最も手前の面が z = 0。
 *   原典の深度テクスチャは一辺 448 texel なので、勾配・凹みのサンプリング距離は H/448 を 1 texel とする。
 */

#include "RelightAE.h"
#include "RelightAuto.h"
#include "RelightGPU.h"
#include "AE_EffectGPUSuites.h"
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <string>
#include <functional>
#include <memory>
#include <condition_variable>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static_assert(RELIGHT_OUTFLAGS == (PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_CUSTOM_UI | PF_OutFlag_SEND_UPDATE_PARAMS_UI),
	"RelightAE_Flags.h の RELIGHT_OUTFLAGS を GlobalSetup のフラグと一致させること");
static_assert(RELIGHT_OUTFLAGS2 == (PF_OutFlag2_FLOAT_COLOR_AWARE |
                                    PF_OutFlag2_SUPPORTS_SMART_RENDER |
                                    PF_OutFlag2_SUPPORTS_THREADED_RENDERING |
                                    PF_OutFlag2_SUPPORTS_GPU_RENDER_F32),
	"RelightAE_Flags.h の RELIGHT_OUTFLAGS2 を GlobalSetup のフラグと一致させること");

/* ------------------------------------------------------------------ */
/* プリセット                                                           */
/* ------------------------------------------------------------------ */

/* 値は UI の単位 (位置はレイヤーの幅・高さに対する %、色は 0..255)。
   0 番が新しく適用したときの既定値 */
struct PresetLight {
	bool          on;
	float         px, py, z;
	unsigned char rgb[3];
	float         intensity;
	bool          rim = true;   /* この光源でリムライトを付けるか */
	float         range = 1.f;  /* 光の届く範囲の倍率 */
};

/* Rim Smoothing の既定 (全プリセット共通) */
static const float PRESET_RIM_SMOOTH = 0.5f;

struct Preset {
	const char   *name;
	int           look;
	PresetLight   l[RELIGHT_MAX_LIGHTS];
	bool          show;
	float         ambient;
	unsigned char ambientRgb[3];
	float         relief, specular, shadow, occlusion;
	unsigned char shadowRgb[3];
	float         shadowTint, cel, celSteps, celSoft, rim, rimWidth, lineProtect, lineThreshold, volume;
	float         rimSoft;
	int           rimPlacement;
	unsigned char rimRgb[3];
	float         shadeSmooth;
	float         edgeDither;
	/* 逆光のリムを光源側・光源の近くに絞る (全プリセット共通の既定値) */
	float         rimSpread = 0.35f;
	float         rimReach  = 0.9f;
	/* 撮影寄りの陰影 (全プリセット共通の既定値。Cinematic は Form を使わない) */
	float         form        = 0.25f;
	float         lightSpread = 0.7f;
};

/* 2 個目・3 個目を使わないプリセットでも、有効にしたときに妥当な位置・色になるようにしておく */
#define PRESET_L2_OFF {false, 78.f, 35.f, 0.35f, {130, 170, 255}, 1.5f}
#define PRESET_L3_OFF {false, 55.f, 18.f, -0.25f, {255, 238, 215}, 3.0f}

static const Preset PRESETS[] = {
	{ "Anime Standard", LOOK_ANIME,
	  { {true, 34.f, 30.f, 0.45f, {255, 200, 150}, 3.0f}, PRESET_L2_OFF, PRESET_L3_OFF },
	  true, 0.85f, {228, 234, 255}, 0.7f, 0.15f, 0.7f, 0.25f,
	  {120, 105, 190}, 0.7f, 0.6f, 1.f, 0.5f, 0.6f, 6.f, 0.8f, 0.35f, 0.5f,
	  0.35f, RIM_AUTO, {255, 255, 255}, 0.6f, 0.5f },
	{ "Soft Daylight", LOOK_ANIME,
	  { {true, 30.f, 12.f, 0.7f, {255, 244, 228}, 2.2f}, PRESET_L2_OFF, PRESET_L3_OFF },
	  false, 1.0f, {255, 255, 255}, 0.8f, 0.08f, 0.5f, 0.35f,
	  {150, 140, 205}, 0.45f, 0.25f, 2.f, 0.35f, 0.3f, 6.f, 0.8f, 0.35f, 0.5f,
	  0.6f, RIM_AUTO, {255, 255, 255}, 0.8f, 0.5f },
	{ "Sunset Backlight", LOOK_ANIME,
	  { {true, 62.f, 28.f, -0.25f, {255, 150, 80}, 3.5f}, {true, -15.f, 60.f, 0.8f, {130, 150, 255}, 0.9f}, PRESET_L3_OFF },
	  /* 逆光では人物の影が後ろの壁に大きく落ちて荒れやすいので、落ち影は弱め */
	  true, 0.9f, {235, 215, 255}, 0.9f, 0.1f, 0.4f, 0.5f,
	  {115, 90, 170}, 0.75f, 0.6f, 2.f, 0.15f, 1.2f, 9.f, 0.8f, 0.35f, 0.6f,
	  0.4f, RIM_AUTO, {255, 240, 220}, 0.6f, 0.5f },
	{ "Night Neon", LOOK_ANIME,
	  { {true, 18.f, 45.f, 0.3f, {255, 60, 180}, 3.0f}, {true, 82.f, 45.f, 0.3f, {60, 200, 255}, 3.0f}, PRESET_L3_OFF },
	  true, 0.55f, {150, 160, 255}, 0.9f, 0.3f, 0.7f, 0.55f,
	  {60, 50, 130}, 0.8f, 0.5f, 2.f, 0.2f, 1.0f, 6.f, 0.8f, 0.3f, 0.5f,
	  0.25f, RIM_LIGHT_SIDE, {255, 255, 255}, 0.5f, 0.5f },
	{ "Cel Hard Light", LOOK_ANIME,
	  { {true, 35.f, 22.f, 0.5f, {255, 232, 205}, 2.6f}, PRESET_L2_OFF, PRESET_L3_OFF },
	  false, 1.0f, {255, 255, 255}, 1.0f, 0.0f, 0.85f, 0.3f,
	  {135, 115, 200}, 0.85f, 1.0f, 1.f, 0.06f, 0.6f, 5.f, 0.9f, 0.35f, 0.7f,
	  /* セル調の形を見せるプリセットなので、立体的な陰影を多めにする (撮影寄りだけだと光源を中心にした丸になる) */
	  0.05f, RIM_LIGHT_SIDE, {255, 255, 255}, 0.35f, 0.3f, 0.35f, 0.9f, 0.6f, 0.7f },
	{ "Cinematic (TypeGPU)", LOOK_CINEMATIC,
	  { {true, 34.f, 34.f, 0.42f, {255, 184, 117}, 3.0f}, PRESET_L2_OFF, PRESET_L3_OFF },
	  true, 0.5f, {199, 219, 255}, 0.85f, 0.22f, 0.7f, 0.55f,
	  {120, 105, 190}, 0.f, 0.f, 2.f, 0.2f, 0.f, 6.f, 0.f, 0.35f, 0.f,
	  0.35f, RIM_AUTO, {255, 255, 255}, 0.f, 0.f },
};
static const int NUM_PRESETS = (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));
#define PRESET_POPUP "Select...|Anime Standard|Soft Daylight|Sunset Backlight|Night Neon|Cel Hard Light|Cinematic (TypeGPU)"

/* プリセットの値を RenderParams に入れる (確認用ツール relight_cli 用。AE ではパラメータ経由) */
static void PresetToRenderParams(const Preset &p, float layerW, float layerH, RenderParams &rp)
{
	for (int k = 0; k < RELIGHT_MAX_LIGHTS; ++k) {
		const PresetLight &s = p.l[k];
		LightParams &d = rp.light[k];
		d.on = s.on;
		d.u = 0.5f + (layerW * s.px / 100.f - layerW * 0.5f) / layerH;
		d.v = s.py / 100.f;
		d.z = s.z;
		for (int c = 0; c < 3; ++c) d.tint[c] = s.rgb[c] / 255.f;
		d.intensity = s.intensity;
		d.range = s.range;
		d.show = p.show;
		/* リムはプリセットでは全部の光源が同じ値 */
		d.rim = s.rim;
		d.rimAmount = p.rim; d.rimWidth = p.rimWidth; d.rimSoft = p.rimSoft; d.rimSmooth = PRESET_RIM_SMOOTH;
		d.rimPlacement = p.rimPlacement; d.rimSpread = p.rimSpread; d.rimReach = p.rimReach;
		for (int c = 0; c < 3; ++c) d.rimColor[c] = p.rimRgb[c] / 255.f;
	}
	rp.look = p.look;
	rp.exposure = p.ambient;
	for (int c = 0; c < 3; ++c) { rp.ambient[c] = p.ambientRgb[c] / 255.f; rp.shadowColor[c] = p.shadowRgb[c] / 255.f; }
	rp.relief = p.relief; rp.specular = p.specular; rp.shadow = p.shadow; rp.occlusion = p.occlusion;
	rp.shadowTint = p.shadowTint; rp.cel = p.cel; rp.celSteps = (int)p.celSteps; rp.celSoft = p.celSoft;
	rp.form = p.form; rp.lightSpread = p.lightSpread; rp.depthStabilize = 2;
	rp.shadeSmooth = p.shadeSmooth;
	rp.edgeDither = p.edgeDither;
	rp.lineProtect = p.lineProtect; rp.lineThreshold = p.lineThreshold; rp.volume = p.volume;
}

/*
 * 行を分けて並列に処理する。
 * 以前は呼ぶたびにスレッドを 16 本作っていたが、AE は複数のフレームを同時に描く (マルチフレームレンダリング) ので、
 * フレームの数 × 16 本が奪い合い、確認用ツールの 5〜8 倍遅かった。
 * いまは使い回しの作業スレッド (CPU の数 − 1 本) を全フレームで共有し、呼んだスレッドも一緒に働く。
 */
struct RowJob {
	std::function<void(A_long)> fn;
	A_long rows;
	std::atomic<A_long> next;
	std::atomic<A_long> finished;
	RowJob(std::function<void(A_long)> f, A_long r) : fn(std::move(f)), rows(r), next(0), finished(0) {}
	void work()
	{
		for (A_long y; (y = next.fetch_add(1)) < rows;) { fn(y); finished.fetch_add(1); }
	}
};

struct RowPool {
	std::mutex m;
	std::condition_variable cv;
	std::vector<std::shared_ptr<RowJob>> jobs;
	unsigned workers = 0;

	static RowPool &get()
	{
		/* 作業スレッドは AE が終わるまで残る。DLL が先に外されないよう固定し、後片付けもしない */
		static RowPool *p = []() {
			HMODULE self = NULL;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			                   (LPCWSTR)&RowPool::get, &self);
			RowPool *pool = new RowPool;
			unsigned n = std::thread::hardware_concurrency();
			pool->workers = std::max(1u, std::min(n, 32u)) - 1;
			for (unsigned i = 0; i < pool->workers; ++i) std::thread([pool]() { pool->loop(); }).detach();
			return pool;
		}();
		return *p;
	}

	void loop()
	{
		for (;;) {
			std::shared_ptr<RowJob> job;
			{
				std::unique_lock<std::mutex> lk(m);
				cv.wait(lk, [&]() { return !jobs.empty(); });
				job = jobs.front();
				/* 次の作業スレッドが別のジョブを拾えるよう、先頭を後ろへ回す */
				std::rotate(jobs.begin(), jobs.begin() + 1, jobs.end());
			}
			job->work();
			std::lock_guard<std::mutex> lk(m);
			auto it = std::find(jobs.begin(), jobs.end(), job);
			if (it != jobs.end()) jobs.erase(it);
		}
	}

	void run(A_long rows, std::function<void(A_long)> fn)
	{
		auto job = std::make_shared<RowJob>(std::move(fn), rows);
		{
			std::lock_guard<std::mutex> lk(m);
			jobs.push_back(job);
		}
		cv.notify_all();
		job->work();
		while (job->finished.load() < rows) std::this_thread::yield();
		std::lock_guard<std::mutex> lk(m);
		auto it = std::find(jobs.begin(), jobs.end(), job);
		if (it != jobs.end()) jobs.erase(it);
	}
};

template <typename F>
static void parallelRows(A_long rows, F fn)
{
	if (rows < 16) { for (A_long y = 0; y < rows; ++y) fn(y); return; }
	RowPool::get().run(rows, std::function<void(A_long)>(fn));
}

/* 2 パスの面取り距離変換 (縦横 1、斜め √2)。0 のマスからの距離を入れる */
static void chamferDistance(std::vector<float> &d, A_long W, A_long H)
{
	const float D1 = 1.f, D2 = 1.41421356f;
	for (A_long j = 0; j < H; ++j)
		for (A_long i = 0; i < W; ++i) {
			float v = d[(size_t)j * W + i];
			if (i > 0) v = std::min(v, d[(size_t)j * W + i - 1] + D1);
			if (j > 0) {
				v = std::min(v, d[(size_t)(j - 1) * W + i] + D1);
				if (i > 0)     v = std::min(v, d[(size_t)(j - 1) * W + i - 1] + D2);
				if (i < W - 1) v = std::min(v, d[(size_t)(j - 1) * W + i + 1] + D2);
			}
			d[(size_t)j * W + i] = v;
		}
	for (A_long j = H - 1; j >= 0; --j)
		for (A_long i = W - 1; i >= 0; --i) {
			float v = d[(size_t)j * W + i];
			if (i < W - 1) v = std::min(v, d[(size_t)j * W + i + 1] + D1);
			if (j < H - 1) {
				v = std::min(v, d[(size_t)(j + 1) * W + i] + D1);
				if (i < W - 1) v = std::min(v, d[(size_t)(j + 1) * W + i + 1] + D2);
				if (i > 0)     v = std::min(v, d[(size_t)(j + 1) * W + i - 1] + D2);
			}
			d[(size_t)j * W + i] = v;
		}
}

/* ------------------------------------------------------------------ */
/* CPU 版の全体の手順 (GPU 版は RelightGPU.cu の RelightGPU_Render)       */
/* ------------------------------------------------------------------ */

/* ガンマの表 (CPU 用) */
static const float *HostLut()
{
	static std::vector<float> lut = []() {
		std::vector<float> l(RL_LUT_SIZE * 2);
		rlBuildLut(l.data(), l.data() + RL_LUT_SIZE);
		return l;
	}();
	return lut.data();
}

/* 格子の値をならす (横 → 縦) */
static void SmoothGrid(const ShadeContext &c, std::vector<float> &arr, int stride, int R)
{
	std::vector<float> tmp(arr.size());
	parallelRows(c.gh, [&](A_long j) { for (int i = 0; i < c.gw; ++i) rlSmoothCell(&c, arr.data(), tmp.data(), stride, R, true, i, (int)j); });
	parallelRows(c.gh, [&](A_long j) { for (int i = 0; i < c.gw; ++i) rlSmoothCell(&c, tmp.data(), arr.data(), stride, R, false, i, (int)j); });
}

/*
 * 1 フレームを描く。c には rp, outW, outH, originX, originY, layerW, layerH, normalA, maskA を入れておく。
 * src: 入力 (premultiplied)。depthMap: 深度ワールドと位置合わせ (img.data が NULL なら深度なし)。
 * nb / nbOff / nbCount: 前後のフレームの深度ワールドと時刻の差 (深度を時間方向にならす。無ければ 0 個)。
 * out: 結果 (premultiplied、outW x outH)
 */
static void RenderFrameCPU(ShadeContext &c, const RLImage &src, const AuxMap &depthMap,
                           const RLImage *nb, const int *nbOff, int nbCount, std::vector<Rgba> &out)
{
	const RenderParams &rp = c.rp;
	const int W = c.outW, H = c.outH;
	const size_t NP = (size_t)W * H;
	c.lutDec = HostLut();
	c.lutEnc = c.lutDec + RL_LUT_SIZE;
	c.depth = NULL;
	c.hasDepth = false;
	c.lineMask = NULL;
	rlSetupFrame(c);

	/* 1. 深度: 深度の点を浮動小数にし、出力の大きさへ引き伸ばす (線画に合わせる) */
	std::vector<float> low, depth;
	if (depthMap.img.data) {
		const int dw = depthMap.img.width, dh = depthMap.img.height;
		low.resize((size_t)dw * dh);
		parallelRows(dh, [&](A_long j) {
			for (int i = 0; i < dw; ++i) {
				float d = rlLowDepth(depthMap.img, i, (int)j, rp.depthNear, rp.passEncoding == 2);
				if (nbCount > 0) d = rlStabilizeDepth(d, nb, nbOff, nbCount, i, (int)j, rp.depthNear, rp.passEncoding == 2);
				low[(size_t)j * dw + i] = d;
			}
		});
		depth.resize(NP);
		parallelRows(H, [&](A_long y) {
			for (int x = 0; x < W; ++x) depth[(size_t)y * W + x] = rlUpsampleDepth(&c, low.data(), dw, dh, depthMap, src, x, (int)y);
		});
		c.depth = depth.data();
		c.hasDepth = true;
	}

	/* 2. 線画のマスク */
	std::vector<float> luma, tmp, blur, mask;
	/* 線らしさ (線画の保護と、リムを輪郭線の外へ出さないのに使う) */
	if (rp.lineProtect > 0.f || rp.outputMode == OUTPUT_LINES || rlAnyRim(c)) {
		luma.resize(NP); tmp.resize(NP); blur.resize(NP); mask.resize(NP);
		const int rad = rlLineRadius(c.layerH);
		const float protect = 1.f;
		parallelRows(H, [&](A_long y) { for (int x = 0; x < W; ++x) luma[(size_t)y * W + x] = rlLuma(&c, src, x, (int)y); });
		parallelRows(H, [&](A_long y) { for (int x = 0; x < W; ++x) tmp[(size_t)y * W + x] = rlBox(luma.data(), W, H, rad, true, x, (int)y); });
		parallelRows(H, [&](A_long y) { for (int x = 0; x < W; ++x) blur[(size_t)y * W + x] = rlBox(tmp.data(), W, H, rad, false, x, (int)y); });
		parallelRows(H, [&](A_long y) {
			for (int x = 0; x < W; ++x) {
				const size_t n = (size_t)y * W + x;
				mask[n] = rlLineMask(luma[n], blur[n], rp.lineThreshold, protect);
			}
		});
		c.lineMask = mask.data();
	}

	/* 3. 格子 */
	std::vector<float> gDepth, gBulge, gSurf, gShadow, gKey, rowMin, rimMin;
	if (c.hasDepth) {
		const int GW = c.gw, GH = c.gh, nL = c.nL;
		const size_t GN = (size_t)GW * GH;
		gDepth.resize(GN);
		parallelRows(GH, [&](A_long j) { for (int i = 0; i < GW; ++i) gDepth[(size_t)j * GW + i] = rlCellDepth(&c, i, (int)j); });
		c.gDepth = gDepth.data();

		/* 深度の膨らみ: 輪郭からの距離に応じて手前に盛り上げる */
		if (rp.volume > 0.f) {
			std::vector<float> dist(GN);
			parallelRows(GH, [&](A_long j) { for (int i = 0; i < GW; ++i) dist[(size_t)j * GW + i] = rlVolumeSeed(&c, i, (int)j); });
			chamferDistance(dist, GW, GH);
			gBulge.resize(GN);
			for (size_t n = 0; n < GN; ++n) {
				gBulge[n] = rlBulge(dist[n], gDepth[n], rp.volume);
				gDepth[n] += gBulge[n];
			}
			c.gBulge = gBulge.data();
		}

		/* 勾配と凹み */
		gSurf.resize(GN * 3);
		parallelRows(GH, [&](A_long j) { for (int i = 0; i < GW; ++i) rlCellSurface(&c, i, (int)j, &gSurf[((size_t)j * GW + i) * 3]); });
		c.gSurf = gSurf.data();

		/* 落ち影 (光源ごと) */
		gShadow.assign(GN * std::max(1, nL), 1.f);
		if (rp.shadow > 0.f && nL > 0)
			parallelRows(GH, [&](A_long j) {
				for (int i = 0; i < GW; ++i)
					for (int k = 0; k < nL; ++k) gShadow[((size_t)j * GW + i) * nL + k] = rlCellShadow(&c, i, (int)j, k);
			});

		/* 陰影をならす (Shade Smoothness) */
		const int R = rlSmoothRadius(rp);
		if (R > 0) {
			SmoothGrid(c, gSurf, 3, R);
			if (rp.shadow > 0.f && nL > 0) SmoothGrid(c, gShadow, nL, R);
		}
		c.gShadow = gShadow.data();

		/* 当たり具合を格子で求めてならす (セルシェーダー式) */
		if (rlUseKeyGrid(c)) {
			gKey.resize(GN * nL);
			parallelRows(GH, [&](A_long j) {
				for (int i = 0; i < GW; ++i)
					for (int l = 0; l < nL; ++l) gKey[((size_t)j * GW + i) * nL + l] = rlCellKey(&c, i, (int)j, l);
			});
			SmoothGrid(c, gKey, nL, rlKeyRadius(rp));
			c.gKey = gKey.data();
		}

		/* リムの下準備 */
		if (rlAnyRim(c)) {
			const int RC = rlRimCells(c);
			rowMin.resize(GN); rimMin.resize(GN);
			parallelRows(GH, [&](A_long j) { for (int i = 0; i < GW; ++i) rowMin[(size_t)j * GW + i] = rlRowMin(&c, RC, i, (int)j); });
			parallelRows(GH, [&](A_long j) { for (int i = 0; i < GW; ++i) rimMin[(size_t)j * GW + i] = rlColMin(&c, rowMin.data(), RC, i, (int)j); });
			c.gRimMin = rimMin.data();
		}
	}
	for (int k = 0; k < c.nL; ++k) c.L[k].bulbExposure = computeBulbExposure(&c, c.L[k]);

	/* 3b. リムの下地: 光源の方へずらした深度との差で帯を作り、少しぼかす */
	std::vector<float> rimRaw, rimTmp, rimMask;
	std::vector<float> rimDepth[RELIGHT_MAX_LIGHTS], rimInside[RELIGHT_MAX_LIGHTS];
	if (c.gRimMin && c.nL > 0) {
		const int nL = c.nL;
		/* Rim Smoothing: 輪郭をならした深度 (光源ごと。同じ半径なら使い回す) */
		for (int k = 0; k < nL; ++k) {
			LightCtx &L = c.L[k];
			if (!L.rim || L.rimSmoothR <= 0) continue;
			const int share = rlRimSmoothShare(c, k);
			if (share != k) { L.rimDepth = c.L[share].rimDepth; L.rimInside = c.L[share].rimInside; continue; }
			std::vector<float> &med = rimDepth[k], t(NP);
			med = depth;
			const int r = L.rimSmoothR;
			for (int pass = 0; pass < 2; ++pass) {
				parallelRows(H, [&](A_long y) { for (int x = 0; x < W; ++x) t[(size_t)y * W + x] = rlMedian1D(med.data(), W, H, r, true, x, (int)y); });
				parallelRows(H, [&](A_long y) { for (int x = 0; x < W; ++x) med[(size_t)y * W + x] = rlMedian1D(t.data(), W, H, r, false, x, (int)y); });
			}
			rimInside[k].resize(NP);
			parallelRows(H, [&](A_long y) {
				for (int x = 0; x < W; ++x) {
					const size_t n = (size_t)y * W + x;
					rimInside[k][n] = rlRimInside(depth[n], med[n]);
				}
			});
			L.rimDepth = rimDepth[k].data();
			L.rimInside = rimInside[k].data();
		}
		rimRaw.resize(NP * nL); rimTmp.resize(NP * nL); rimMask.resize(NP * nL);
		parallelRows(H, [&](A_long y) {
			for (int x = 0; x < W; ++x)
				for (int k = 0; k < nL; ++k) rimRaw[((size_t)y * W + x) * nL + k] = rlRimMask(&c, x, (int)y, k);
		});
		int rr[RELIGHT_MAX_LIGHTS];
		for (int k = 0; k < nL; ++k) rr[k] = rlRimBlurRadius(c.L[k]);
		parallelRows(H, [&](A_long y) {
			for (int x = 0; x < W; ++x)
				for (int k = 0; k < nL; ++k) rimTmp[((size_t)y * W + x) * nL + k] = rlBoxStrided(rimRaw.data(), W, H, nL, k, rr[k], true, x, (int)y);
		});
		parallelRows(H, [&](A_long y) {
			for (int x = 0; x < W; ++x)
				for (int k = 0; k < nL; ++k) rimMask[((size_t)y * W + x) * nL + k] = rlBoxStrided(rimTmp.data(), W, H, nL, k, rr[k], false, x, (int)y);
		});
		/* 輪郭の外へにじんだ分を消す (rimTmp を作業に使い回す) */
		parallelRows(H, [&](A_long y) {
			for (int x = 0; x < W; ++x)
				for (int k = 0; k < nL; ++k) rimTmp[((size_t)y * W + x) * nL + k] = rlRimClip(rimRaw.data(), rimMask.data(), W, H, nL, k, x, (int)y);
		});
		rimMask.swap(rimTmp);
		c.rimMask = rimMask.data();
	}

	/* 4. 照明 */
	out.resize(NP);
	parallelRows(H, [&](A_long y) {
		for (int x = 0; x < W; ++x) shadePixel(&c, x, (int)y, rlRead(src, x, (int)y), out[(size_t)y * W + x]);
	});

	/* 5. 仕上げ (Edge Dither) */
	if (rlUseEdgeDither(c)) {
		std::vector<float> delta(NP * 3);
		parallelRows(H, [&](A_long y) {
			for (int x = 0; x < W; ++x) {
				const size_t n = (size_t)y * W + x;
				const Rgba s = rlRead(src, x, (int)y);
				delta[n * 3 + 0] = out[n].r - s.r; delta[n * 3 + 1] = out[n].g - s.g; delta[n * 3 + 2] = out[n].b - s.b;
			}
		});
		parallelRows(H, [&](A_long y) {
			for (int x = 0; x < W; ++x) rlEdgeDither(&c, delta.data(), rlRead(src, x, (int)y), x, (int)y, out[(size_t)y * W + x]);
		});
	}
}

#ifndef RELIGHT_HAS_CUDA
/* CUDA なしでビルドしたとき (GPU 版は使われない) */
int  RelightGPU_Render(RLGpuFrame &, char *err, int errLen) { if (err && errLen > 0) err[0] = 0; return -1; }
void RelightGPU_Release() {}
#endif

/* 浮動小数の結果 (premultiplied) を出力ワールドへ書く */
static void WriteWorld(PF_EffectWorld *w, PF_PixelFormat fmt, const std::vector<Rgba> &buf)
{
	const A_long W = w->width, H = w->height;
	parallelRows(H, [&](A_long y) {
		char *row = (char *)w->data + (size_t)y * (size_t)w->rowbytes;
		for (A_long x = 0; x < W; ++x) {
			const Rgba &o = buf[(size_t)y * W + x];
			switch (fmt) {
			case PF_PixelFormat_ARGB128: {
				PF_PixelFloat *p = (PF_PixelFloat *)row + x;
				p->alpha = o.a; p->red = o.r; p->green = o.g; p->blue = o.b;
				break;
			}
			case PF_PixelFormat_ARGB64: {
				PF_Pixel16 *p = (PF_Pixel16 *)row + x;
				p->alpha = (A_u_short)(saturate(o.a) * PF_MAX_CHAN16 + 0.5f);
				p->red   = (A_u_short)(saturate(o.r) * PF_MAX_CHAN16 + 0.5f);
				p->green = (A_u_short)(saturate(o.g) * PF_MAX_CHAN16 + 0.5f);
				p->blue  = (A_u_short)(saturate(o.b) * PF_MAX_CHAN16 + 0.5f);
				break;
			}
			default: {
				PF_Pixel8 *p = (PF_Pixel8 *)row + x;
				p->alpha = (A_u_char)(saturate(o.a) * PF_MAX_CHAN8 + 0.5f);
				p->red   = (A_u_char)(saturate(o.r) * PF_MAX_CHAN8 + 0.5f);
				p->green = (A_u_char)(saturate(o.g) * PF_MAX_CHAN8 + 0.5f);
				p->blue  = (A_u_char)(saturate(o.b) * PF_MAX_CHAN8 + 0.5f);
				break;
			}
			}
		}
	});
}

/* ------------------------------------------------------------------ */
/* AE コマンド処理                                                     */
/* ------------------------------------------------------------------ */

static PF_Err About(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_LayerDef *output)
{
	AEGP_SuiteHandler suites(in_data->pica_basicP);
	suites.ANSICallbacksSuite1()->sprintf(out_data->return_msg,
		"%s v%d.%d\r%s\rLight model ported from TypeGPU (MIT, Software Mansion).",
		RELIGHT_NAME, RELIGHT_MAJOR_VERSION, RELIGHT_MINOR_VERSION, RELIGHT_DESCRIPTION);
	return PF_Err_NONE;
}

static PF_Err GlobalSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_LayerDef *output)
{
	out_data->my_version = PF_VERSION(RELIGHT_MAJOR_VERSION, RELIGHT_MINOR_VERSION,
	                                  RELIGHT_BUG_VERSION, RELIGHT_STAGE_VERSION, RELIGHT_BUILD_VERSION);
	out_data->out_flags  = RELIGHT_OUTFLAGS;
	out_data->out_flags2 = RELIGHT_OUTFLAGS2;
	Auto_Init(in_data);   /* AEGP 登録とタイマー (自動パス生成用) */
	return PF_Err_NONE;
}

/* 光源ごとのパラメータの index と ID (並びは 3 個とも同じ。Light 1 だけ Enable が無い) */
struct LightSlots {
	A_long enable, position, height, color, intensity, range, show, look;
	A_long rimTopic, rim, rimAmount, rimWidth, rimSoft, rimSmooth, rimPlacement, rimColor, rimSpread, rimReach, rimTopicEnd;
};
static const LightSlots LIGHT_IDX[RELIGHT_MAX_LIGHTS] = {
	{-1, RELIGHT_LIGHT_POSITION, RELIGHT_LIGHT_HEIGHT, RELIGHT_LIGHT_COLOR, RELIGHT_LIGHT_INTENSITY, RELIGHT_LIGHT_RANGE, RELIGHT_SHOW_LIGHT, RELIGHT_LIGHT_LOOK,
	 RELIGHT_TOPIC_LIGHT_RIM, RELIGHT_LIGHT_RIM, RELIGHT_RIM, RELIGHT_RIM_WIDTH, RELIGHT_RIM_SOFTNESS, RELIGHT_RIM_SMOOTH,
	 RELIGHT_RIM_PLACEMENT, RELIGHT_RIM_COLOR, RELIGHT_RIM_SPREAD, RELIGHT_RIM_REACH, RELIGHT_TOPIC_LIGHT_RIM_END},
	{RELIGHT_LIGHT2_ENABLE, RELIGHT_LIGHT2_POSITION, RELIGHT_LIGHT2_HEIGHT, RELIGHT_LIGHT2_COLOR, RELIGHT_LIGHT2_INTENSITY, RELIGHT_LIGHT2_RANGE, RELIGHT_LIGHT2_SHOW, RELIGHT_LIGHT2_LOOK,
	 RELIGHT_TOPIC_LIGHT2_RIM, RELIGHT_LIGHT2_RIM, RELIGHT_LIGHT2_RIM_AMOUNT, RELIGHT_LIGHT2_RIM_WIDTH, RELIGHT_LIGHT2_RIM_SOFTNESS, RELIGHT_LIGHT2_RIM_SMOOTH,
	 RELIGHT_LIGHT2_RIM_PLACEMENT, RELIGHT_LIGHT2_RIM_COLOR, RELIGHT_LIGHT2_RIM_SPREAD, RELIGHT_LIGHT2_RIM_REACH, RELIGHT_TOPIC_LIGHT2_RIM_END},
	{RELIGHT_LIGHT3_ENABLE, RELIGHT_LIGHT3_POSITION, RELIGHT_LIGHT3_HEIGHT, RELIGHT_LIGHT3_COLOR, RELIGHT_LIGHT3_INTENSITY, RELIGHT_LIGHT3_RANGE, RELIGHT_LIGHT3_SHOW, RELIGHT_LIGHT3_LOOK,
	 RELIGHT_TOPIC_LIGHT3_RIM, RELIGHT_LIGHT3_RIM, RELIGHT_LIGHT3_RIM_AMOUNT, RELIGHT_LIGHT3_RIM_WIDTH, RELIGHT_LIGHT3_RIM_SOFTNESS, RELIGHT_LIGHT3_RIM_SMOOTH,
	 RELIGHT_LIGHT3_RIM_PLACEMENT, RELIGHT_LIGHT3_RIM_COLOR, RELIGHT_LIGHT3_RIM_SPREAD, RELIGHT_LIGHT3_RIM_REACH, RELIGHT_TOPIC_LIGHT3_RIM_END}};
/* Light 1 のリムは以前の Anime Style のリムの ID をそのまま使う (保存したプロジェクトの値を引き継ぐ) */
static const LightSlots LIGHT_ID[RELIGHT_MAX_LIGHTS] = {
	{0, ID_LIGHT_POSITION, ID_LIGHT_HEIGHT, ID_LIGHT_COLOR, ID_LIGHT_INTENSITY, ID_LIGHT_RANGE, ID_SHOW_LIGHT, ID_LIGHT_LOOK,
	 ID_TOPIC_LIGHT_RIM, ID_LIGHT_RIM, ID_RIM, ID_RIM_WIDTH, ID_RIM_SOFTNESS, ID_RIM_SMOOTH,
	 ID_RIM_PLACEMENT, ID_RIM_COLOR, ID_RIM_SPREAD, ID_RIM_REACH, ID_TOPIC_LIGHT_RIM_END},
	{ID_LIGHT2_ENABLE, ID_LIGHT2_POSITION, ID_LIGHT2_HEIGHT, ID_LIGHT2_COLOR, ID_LIGHT2_INTENSITY, ID_LIGHT2_RANGE, ID_LIGHT2_SHOW, ID_LIGHT2_LOOK,
	 ID_TOPIC_LIGHT2_RIM, ID_LIGHT2_RIM, ID_LIGHT2_RIM_AMOUNT, ID_LIGHT2_RIM_WIDTH, ID_LIGHT2_RIM_SOFTNESS, ID_LIGHT2_RIM_SMOOTH,
	 ID_LIGHT2_RIM_PLACEMENT, ID_LIGHT2_RIM_COLOR, ID_LIGHT2_RIM_SPREAD, ID_LIGHT2_RIM_REACH, ID_TOPIC_LIGHT2_RIM_END},
	{ID_LIGHT3_ENABLE, ID_LIGHT3_POSITION, ID_LIGHT3_HEIGHT, ID_LIGHT3_COLOR, ID_LIGHT3_INTENSITY, ID_LIGHT3_RANGE, ID_LIGHT3_SHOW, ID_LIGHT3_LOOK,
	 ID_TOPIC_LIGHT3_RIM, ID_LIGHT3_RIM, ID_LIGHT3_RIM_AMOUNT, ID_LIGHT3_RIM_WIDTH, ID_LIGHT3_RIM_SOFTNESS, ID_LIGHT3_RIM_SMOOTH,
	 ID_LIGHT3_RIM_PLACEMENT, ID_LIGHT3_RIM_COLOR, ID_LIGHT3_RIM_SPREAD, ID_LIGHT3_RIM_REACH, ID_TOPIC_LIGHT3_RIM_END}};

/* 光源 1 個分のパラメータ (2 個目・3 個目は有効のチェックが付く)。リムの設定は Rim の中 */
static PF_Err AddLightParams(PF_InData *in_data, int k, const Preset &p)
{
	PF_Err      err = PF_Err_NONE;
	PF_ParamDef def;
	const PresetLight &d = p.l[k];
	const LightSlots &id = LIGHT_ID[k];
	if (k > 0) {
		/* 切り替えたら無効な光源の項目を灰色にするため SUPERVISE */
		AEFX_CLR_STRUCT(def);
		PF_ADD_CHECKBOXX("Enable", FALSE, PF_ParamFlag_SUPERVISE, id.enable);
	}
	AEFX_CLR_STRUCT(def);
	/* 既定値はレイヤー幅・高さに対する %。コンポビューに十字ハンドルが出る */
	PF_ADD_POINT(k == 0 ? "Light Position" : "Position", (A_long)d.px, (A_long)d.py, FALSE, id.position);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX(k == 0 ? "Light Height" : "Height", LIGHT_Z_MIN, LIGHT_Z_MAX, LIGHT_Z_MIN, LIGHT_Z_MAX, d.z,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, id.height);

	AEFX_CLR_STRUCT(def);
	PF_ADD_COLOR(k == 0 ? "Light Color" : "Color", d.rgb[0], d.rgb[1], d.rgb[2], id.color);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Intensity", 0.f, 20.f, 0.f, 6.f, d.intensity,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, id.intensity);

	/* 光の届く範囲 (1 = 既定。上げると遠くまで明るく、下げると光源の近くだけ) */
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Range", 0.05f, 10.f, 0.2f, 3.f, d.range,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, id.range);

	/* 2 個目・3 個目の既定はオフ (以前は Light の Show Light が全部の光源に効いていた。
	   以前のプロジェクトで急に光の玉が出ないように) */
	AEFX_CLR_STRUCT(def);
	PF_ADD_CHECKBOXX("Show Light", k == 0 ? p.show : FALSE, 0, id.show);

	/* この光源の当て方。Same as Scene は Scene の Look に従う (以前のプロジェクトはこれ)。
	   Cinematic: 面の向きで陰影を作り、光をそのまま足す (原典どおり)。Anime: 元の色を保って控えめに足す。
	   画面全体の暗さ (Ambient)・影色・最後の色の整え方は Scene の Look のまま */
	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Light Look", 3, 1, "Same as Scene|Anime|Cinematic", id.look);

	/* --- Rim (この光源のリムライト) --- */
	AEFX_CLR_STRUCT(def);
	def.flags = PF_ParamFlag_START_COLLAPSED;
	PF_ADD_TOPIC("Rim", id.rimTopic);

	/* この光源でリムライトを付けるか (切ると面を照らすだけの光源になる) */
	AEFX_CLR_STRUCT(def);
	PF_ADD_CHECKBOXX("Use Rim", d.rim, 0, id.rim);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Rim Light", 0.f, 5.f, 0.f, 3.f, p.rim,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, id.rimAmount);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Rim Width", 1.f, 60.f, 1.f, 40.f, p.rimWidth,
		PF_Precision_TENTHS, PF_ValueDisplayFlag_PIXEL, 0, id.rimWidth);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Rim Softness", 0.f, 1.f, 0.f, 1.f, p.rimSoft,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, id.rimSoft);

	/* 輪郭をならしてから帯を作る (細かいでこぼこを無視して、アニメのようなまっすぐな線に) */
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Rim Smoothing", 0.f, 1.f, 0.f, 1.f, PRESET_RIM_SMOOTH,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, id.rimSmooth);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Rim Placement", 3, p.rimPlacement, "Auto|Light Side|All Around", id.rimPlacement);

	AEFX_CLR_STRUCT(def);
	PF_ADD_COLOR("Rim Color", p.rimRgb[0], p.rimRgb[1], p.rimRgb[2], id.rimColor);

	/* 逆光のリムが光る範囲: 光源側の輪郭からの回り込み (0 = 光源を向いた側だけ、1 = 輪郭全体) と、光源からの距離 (画面の高さ = 1、3 = 制限なし) */
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Rim Spread", 0.f, 1.f, 0.f, 1.f, p.rimSpread,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, id.rimSpread);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Rim Reach", 0.1f, 3.f, 0.1f, 3.f, p.rimReach,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, id.rimReach);

	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(id.rimTopicEnd);
	return err;
}

static PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_LayerDef *output)
{
	PF_Err        err = PF_Err_NONE;
	PF_ParamDef   def;
	const Preset &d = PRESETS[0];   /* 既定値 = Anime Standard */

	/* --- Preset --- */
	AEFX_CLR_STRUCT(def);
	def.flags = PF_ParamFlag_SUPERVISE;
	PF_ADD_POPUP("Preset", NUM_PRESETS + 1, 1, PRESET_POPUP, ID_PRESET);

	/* --- Auto --- */
	AEFX_CLR_STRUCT(def);
	PF_ADD_TOPIC("Auto", ID_TOPIC_AUTO);

	AEFX_CLR_STRUCT(def);
	PF_ADD_CHECKBOXX("Auto Passes", TRUE, 0, ID_AUTO_ENABLE);

	/* 深度を作る場所。Generate Passes を押したときに使う。
	   Local GPU: この PC (小さいモデル vits、速い)。
	   Remote Fast / Quality: 別の PC の深度サーバー (tools/depth_server、依頼が来たときだけ起動) に動画を送る。この PC の GPU を使わない。
	     Fast は小さいモデル (vits、8 秒の動画で約 45 秒)、Quality は大きいモデル (vitl、約 2 分 20 秒。揺らぎが約 3 分の 1)。
	   つながらなければ Local GPU で作り直す */
	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Depth Engine", 3, 1, "Local GPU|Remote Fast|Remote Quality", ID_AUTO_ENGINE);

	/* ボタン押下で PF_Cmd_USER_CHANGED_PARAM が来るよう SUPERVISE を付ける */
	PF_ADD_BUTTON("Regenerate", "Generate Passes", 0, PF_ParamFlag_SUPERVISE, ID_AUTO_GENERATE);
	PF_ADD_BUTTON("Depth Cache", "Clean Cache...", 0, PF_ParamFlag_SUPERVISE, ID_AUTO_CLEAN);

	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(ID_TOPIC_AUTO_END);

	/* --- Light --- */
	AEFX_CLR_STRUCT(def);
	PF_ADD_TOPIC("Light", ID_TOPIC_LIGHT);
	ERR(AddLightParams(in_data, 0, d));
	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(ID_TOPIC_LIGHT_END);

	/* --- Light 2 / 3 (既定は無効) --- */
	AEFX_CLR_STRUCT(def);
	def.flags = PF_ParamFlag_START_COLLAPSED;
	PF_ADD_TOPIC("Light 2", ID_TOPIC_LIGHT2);
	ERR(AddLightParams(in_data, 1, d));
	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(ID_TOPIC_LIGHT2_END);

	AEFX_CLR_STRUCT(def);
	def.flags = PF_ParamFlag_START_COLLAPSED;
	PF_ADD_TOPIC("Light 3", ID_TOPIC_LIGHT3);
	ERR(AddLightParams(in_data, 2, d));
	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(ID_TOPIC_LIGHT3_END);

	/* --- Scene --- */
	AEFX_CLR_STRUCT(def);
	PF_ADD_TOPIC("Scene", ID_TOPIC_SCENE);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Look", 2, d.look, "Anime|Cinematic", ID_LOOK);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Ambient", 0.f, 4.f, 0.f, 1.2f, d.ambient,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_EXPOSURE);

	AEFX_CLR_STRUCT(def);
	PF_ADD_COLOR("Ambient Color", d.ambientRgb[0], d.ambientRgb[1], d.ambientRgb[2], ID_AMBIENT_COLOR);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Relief", 0.f, 10.f, 0.f, 2.5f, d.relief,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_RELIEF);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Specular", 0.f, 4.f, 0.f, 1.f, d.specular,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_SPECULAR);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Shadow", 0.f, 1.f, 0.f, 1.f, d.shadow,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_SHADOW);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Occlusion", 0.f, 1.f, 0.f, 1.f, d.occlusion,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_OCCLUSION);

	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(ID_TOPIC_SCENE_END);

	/* --- Anime Style --- */
	AEFX_CLR_STRUCT(def);
	PF_ADD_TOPIC("Anime Style", ID_TOPIC_ANIME);

	/* 陰影の作り方: 撮影寄りのグラデーション (光源からの画面上の距離) と、面の向きによる立体的な陰影の割合 */
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Form Shading", 0.f, 1.f, 0.f, 1.f, d.form,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_FORM);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Light Spread", 0.05f, 3.f, 0.1f, 2.f, d.lightSpread,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_LIGHT_SPREAD);

	AEFX_CLR_STRUCT(def);
	PF_ADD_COLOR("Shadow Color", d.shadowRgb[0], d.shadowRgb[1], d.shadowRgb[2], ID_SHADOW_COLOR);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Shadow Tint", 0.f, 1.f, 0.f, 1.f, d.shadowTint,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_SHADOW_TINT);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Shade Smoothness", 0.f, 1.f, 0.f, 1.f, d.shadeSmooth,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_SHADE_SMOOTH);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Edge Dither", 0.f, 1.f, 0.f, 1.f, d.edgeDither,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_EDGE_DITHER);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Cel Shading", 0.f, 1.f, 0.f, 1.f, d.cel,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_CEL);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Cel Steps", 1.f, 4.f, 1.f, 4.f, d.celSteps,
		PF_Precision_INTEGER, PF_ValueDisplayFlag_NONE, 0, ID_CEL_STEPS);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Cel Softness", 0.f, 1.f, 0.f, 1.f, d.celSoft,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_CEL_SOFTNESS);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Line Protect", 0.f, 1.f, 0.f, 1.f, d.lineProtect,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_LINE_PROTECT);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Line Threshold", 0.f, 1.f, 0.f, 1.f, d.lineThreshold,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_LINE_THRESHOLD);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Depth Volume", 0.f, 2.f, 0.f, 2.f, d.volume,
		PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_VOLUME);

	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(ID_TOPIC_ANIME_END);

	/* --- Passes --- */
	AEFX_CLR_STRUCT(def);
	def.flags = PF_ParamFlag_START_COLLAPSED;
	PF_ADD_TOPIC("Passes", ID_TOPIC_PASSES);

	AEFX_CLR_STRUCT(def);
	PF_ADD_LAYER("Depth Layer", PF_LayerDefault_NONE, ID_DEPTH_LAYER);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Depth Near Is", 2, 1, "White|Black", ID_DEPTH_NEAR);

	/* 深度を前後のフレームで時間方向にならす (動画で影がちらつくのを抑える) */
	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Depth Stabilize", 3, 3, "Off|1 Frame|2 Frames", ID_DEPTH_STABILIZE);

	/* 深度の境目を素材の線画に合わせる (影やリムの縁が線画に沿う) */
	AEFX_CLR_STRUCT(def);
	PF_ADD_CHECKBOXX("Snap Depth To Lines", TRUE, 0, ID_DEPTH_SNAP);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Normal Source", 2, 1, "From Depth|Normal Layer", ID_NORMAL_SOURCE);

	AEFX_CLR_STRUCT(def);
	PF_ADD_LAYER("Normal Layer", PF_LayerDefault_NONE, ID_NORMAL_LAYER);

	/* AE は Preserve RGB を付けないと EXR をリニア→sRGB 変換して渡してくる。その場合は逆変換で戻す */
	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Pass Encoding", 2, 1, "Linear (Preserve RGB)|sRGB (decode)", ID_PASS_ENCODING);

	AEFX_CLR_STRUCT(def);
	PF_ADD_LAYER("Mask Layer", PF_LayerDefault_NONE, ID_MASK_LAYER);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Mask Source", 2, 1, "Luminance|Alpha", ID_MASK_SOURCE);

	AEFX_CLR_STRUCT(def);
	PF_ADD_CHECKBOXX("Input Is Linear", FALSE, 0, ID_INPUT_LINEAR);

	/* GPU (CUDA) で描く。AE のプロジェクト設定が Mercury GPU アクセラレーション (CUDA) のときだけ効く */
	AEFX_CLR_STRUCT(def);
	PF_ADD_CHECKBOXX("Use GPU", TRUE, 0, ID_USE_GPU);

	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(ID_TOPIC_PASSES_END);

	/* --- Output --- */
	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Output", 5, 1, "Relit|Depth|Normals|Shadow & Occlusion|Line Mask", ID_OUTPUT_MODE);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Mix", 0.f, 100.f, 0.f, 100.f, 100.f,
		PF_Precision_INTEGER, PF_ValueDisplayFlag_PERCENT, 0, ID_MIX);

	out_data->num_params = RELIGHT_NUM_PARAMS;

	/* コンポビューに光源ギズモを描くため、コンポ窓のイベントを受け取る */
	if (!err) {
		PF_CustomUIInfo ci;
		AEFX_CLR_STRUCT(ci);
		ci.events = PF_CustomEFlag_COMP;
		ci.comp_ui_alignment = PF_UIAlignment_NONE;
		ci.layer_ui_alignment = PF_UIAlignment_NONE;
		ci.preview_ui_alignment = PF_UIAlignment_NONE;
		err = (*(in_data->inter.register_ui))(in_data->effect_ref, &ci);
	}
	return err;
}

/* 2 個目・3 個目の光源が無効なら、その項目を灰色にする (見た目だけ。値は変えない) */
static PF_Err UpdateLightUI(PF_InData *in_data, PF_ParamDef *params[])
{
	PF_Err err = PF_Err_NONE;
	AEGP_SuiteHandler suites(in_data->pica_basicP);
	for (int k = 1; k < RELIGHT_MAX_LIGHTS && !err; ++k) {
		const LightSlots &s = LIGHT_IDX[k];
		const bool on = params[s.enable]->u.bd.value != 0;
		const A_long idx[] = {s.position, s.height, s.color, s.intensity, s.range, s.show, s.look, s.rim, s.rimAmount, s.rimWidth,
		                      s.rimSoft, s.rimSmooth, s.rimPlacement, s.rimColor, s.rimSpread, s.rimReach};
		for (A_long i : idx) {
			const bool disabled = (params[i]->ui_flags & PF_PUI_DISABLED) != 0;
			if (disabled == !on) continue;
			PF_ParamDef copy = *params[i];   /* PF_UpdateParamUI には写しを渡す */
			if (on) copy.ui_flags &= ~PF_PUI_DISABLED; else copy.ui_flags |= PF_PUI_DISABLED;
			ERR(suites.ParamUtilsSuite3()->PF_UpdateParamUI(in_data->effect_ref, i, &copy));
		}
	}
	return err;
}

/* ------------------------------------------------------------------ */
/* プリセットの適用 (PF_Cmd_USER_CHANGED_PARAM)                        */
/* ------------------------------------------------------------------ */

static void ApplyPreset(PF_InData *in_data, PF_ParamDef *params[], const Preset &p)
{
	auto changed = [&](A_long i) { params[i]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE; };
	auto setF = [&](A_long i, float v) { params[i]->u.fs_d.value = v; changed(i); };
	auto setB = [&](A_long i, bool v) { params[i]->u.bd.value = v ? 1 : 0; changed(i); };
	auto setC = [&](A_long i, const unsigned char *rgb) {
		params[i]->u.cd.value.alpha = PF_MAX_CHAN8;
		params[i]->u.cd.value.red = rgb[0]; params[i]->u.cd.value.green = rgb[1]; params[i]->u.cd.value.blue = rgb[2];
		changed(i);
	};
	/* 2D ポイントはレイヤー座標。ダウンサンプル中ならその倍率で渡される */
	const float dsx = (float)in_data->downsample_x.num / (float)std::max<A_long>(1, in_data->downsample_x.den);
	const float dsy = (float)in_data->downsample_y.num / (float)std::max<A_long>(1, in_data->downsample_y.den);
	for (int k = 0; k < RELIGHT_MAX_LIGHTS; ++k) {
		const PresetLight &l = p.l[k];
		const LightSlots &s = LIGHT_IDX[k];
		if (s.enable >= 0) setB(s.enable, l.on);
		params[s.position]->u.td.x_value = FLOAT2FIX(in_data->width * dsx * l.px / 100.f);
		params[s.position]->u.td.y_value = FLOAT2FIX(in_data->height * dsy * l.py / 100.f);
		changed(s.position);
		setF(s.height, l.z);
		setC(s.color, l.rgb);
		setF(s.intensity, l.intensity);
		setF(s.range, l.range);
		setB(s.show, p.show);
		params[s.look]->u.pd.value = 1; changed(s.look);   /* Same as Scene */
		/* リムはプリセットでは全部の光源が同じ値 */
		setB(s.rim, l.rim);
		setF(s.rimAmount, p.rim);
		setF(s.rimWidth, p.rimWidth);
		setF(s.rimSoft, p.rimSoft);
		setF(s.rimSmooth, PRESET_RIM_SMOOTH);
		params[s.rimPlacement]->u.pd.value = p.rimPlacement; changed(s.rimPlacement);
		setC(s.rimColor, p.rimRgb);
		setF(s.rimSpread, p.rimSpread);
		setF(s.rimReach, p.rimReach);
	}
	params[RELIGHT_LOOK]->u.pd.value = p.look; changed(RELIGHT_LOOK);
	setF(RELIGHT_EXPOSURE, p.ambient);
	setC(RELIGHT_AMBIENT_COLOR, p.ambientRgb);
	setF(RELIGHT_RELIEF, p.relief);
	setF(RELIGHT_SPECULAR, p.specular);
	setF(RELIGHT_SHADOW, p.shadow);
	setF(RELIGHT_OCCLUSION, p.occlusion);
	setC(RELIGHT_SHADOW_COLOR, p.shadowRgb);
	setF(RELIGHT_SHADOW_TINT, p.shadowTint);
	setF(RELIGHT_SHADE_SMOOTH, p.shadeSmooth);
	setF(RELIGHT_EDGE_DITHER, p.edgeDither);
	setF(RELIGHT_CEL, p.cel);
	setF(RELIGHT_CEL_STEPS, p.celSteps);
	setF(RELIGHT_CEL_SOFTNESS, p.celSoft);
	setF(RELIGHT_FORM, p.form);
	setF(RELIGHT_LIGHT_SPREAD, p.lightSpread);
	setF(RELIGHT_LINE_PROTECT, p.lineProtect);
	setF(RELIGHT_LINE_THRESHOLD, p.lineThreshold);
	setF(RELIGHT_VOLUME, p.volume);
}

/* ------------------------------------------------------------------ */
/* パラメータ読み出し (SmartRender 時に checkout)                      */
/* ------------------------------------------------------------------ */

struct ParamReader {
	PF_InData *in_data;
	PF_Err     err;

	explicit ParamReader(PF_InData *d) : in_data(d), err(PF_Err_NONE) {}

	bool checkout(A_long index, PF_ParamDef &def)
	{
		AEFX_CLR_STRUCT(def);
		PF_Err e = PF_CHECKOUT_PARAM(in_data, index, in_data->current_time,
		                             in_data->time_step, in_data->time_scale, &def);
		if (e) { if (!err) err = e; return false; }
		return true;
	}
	void checkin(PF_ParamDef &def)
	{
		PF_Err e = PF_CHECKIN_PARAM(in_data, &def);
		if (e && !err) err = e;
	}
	float fs(A_long index, float dflt = 0.f)
	{
		PF_ParamDef def; float v = dflt;
		if (checkout(index, def)) { v = (float)def.u.fs_d.value; checkin(def); }
		return v;
	}
	A_long popup(A_long index, A_long dflt = 1)
	{
		PF_ParamDef def; A_long v = dflt;
		if (checkout(index, def)) { v = def.u.pd.value; checkin(def); }
		return v;
	}
	bool checkbox(A_long index, bool dflt = false)
	{
		PF_ParamDef def; bool v = dflt;
		if (checkout(index, def)) { v = def.u.bd.value != 0; checkin(def); }
		return v;
	}
	void color(A_long index, float *rgb)
	{
		PF_ParamDef def;
		rgb[0] = rgb[1] = rgb[2] = 1.f;
		if (checkout(index, def)) {
			rgb[0] = def.u.cd.value.red   / (float)PF_MAX_CHAN8;
			rgb[1] = def.u.cd.value.green / (float)PF_MAX_CHAN8;
			rgb[2] = def.u.cd.value.blue  / (float)PF_MAX_CHAN8;
			checkin(def);
		}
	}
	/* 2D ポイント。値はレンダー解像度 (ダウンサンプル後) のレイヤー座標で渡される */
	void point2d(A_long index, float *xy)
	{
		PF_ParamDef def;
		xy[0] = xy[1] = 0.f;
		if (checkout(index, def)) {
			xy[0] = (float)FIX_2_FLOAT(def.u.td.x_value);
			xy[1] = (float)FIX_2_FLOAT(def.u.td.y_value);
			checkin(def);
		}
	}
};

static PF_Err ReadRenderParams(PF_InData *in_data, float layerW, float layerH, RenderParams &rp)
{
	ParamReader r(in_data);

	for (int k = 0; k < RELIGHT_MAX_LIGHTS; ++k) {
		LightParams &L = rp.light[k];
		const LightSlots &s = LIGHT_IDX[k];
		L.on = (s.enable < 0) ? true : r.checkbox(s.enable, false);
		if (!L.on) continue;
		float p[2];
		r.point2d(s.position, p);
		L.u = 0.5f + (p[0] - layerW * 0.5f) / layerH;
		L.v = p[1] / layerH;
		L.z = clampf(r.fs(s.height, 0.42f), LIGHT_Z_MIN, LIGHT_Z_MAX);
		r.color(s.color, L.tint);
		L.intensity    = r.fs(s.intensity, 3.f);
		L.range        = std::max(0.05f, r.fs(s.range, 1.f));
		L.show         = r.checkbox(s.show, k == 0);
		L.look         = (int)r.popup(s.look, 1) - 1;   /* 0: Scene と同じ  1: Anime  2: Cinematic */
		L.rim          = r.checkbox(s.rim, true);
		L.rimAmount    = std::max(0.f, r.fs(s.rimAmount, 0.f));
		L.rimWidth     = std::max(1.f, r.fs(s.rimWidth, 6.f));
		L.rimSoft      = saturate(r.fs(s.rimSoft, 0.35f));
		L.rimSmooth    = saturate(r.fs(s.rimSmooth, 0.f));
		L.rimPlacement = (int)r.popup(s.rimPlacement, RIM_AUTO);
		r.color(s.rimColor, L.rimColor);
		L.rimSpread    = saturate(r.fs(s.rimSpread, 1.f));
		L.rimReach     = r.fs(s.rimReach, 3.f);
	}

	rp.look      = (int)r.popup(RELIGHT_LOOK, LOOK_ANIME);
	rp.exposure  = r.fs(RELIGHT_EXPOSURE, 0.5f);
	r.color(RELIGHT_AMBIENT_COLOR, rp.ambient);
	rp.relief    = r.fs(RELIGHT_RELIEF, 0.85f);
	rp.specular  = r.fs(RELIGHT_SPECULAR, 0.22f);
	rp.shadow    = saturate(r.fs(RELIGHT_SHADOW, 0.7f));
	rp.occlusion = saturate(r.fs(RELIGHT_OCCLUSION, 0.55f));

	r.color(RELIGHT_SHADOW_COLOR, rp.shadowColor);
	rp.shadowTint    = saturate(r.fs(RELIGHT_SHADOW_TINT, 0.f));
	rp.shadeSmooth   = saturate(r.fs(RELIGHT_SHADE_SMOOTH, 0.f));
	rp.edgeDither    = saturate(r.fs(RELIGHT_EDGE_DITHER, 0.f));
	rp.cel           = saturate(r.fs(RELIGHT_CEL, 0.f));
	rp.celSteps      = (int)clampf(std::round(r.fs(RELIGHT_CEL_STEPS, 2.f)), 1.f, 4.f);
	rp.celSoft       = saturate(r.fs(RELIGHT_CEL_SOFTNESS, 0.2f));
	rp.form         = saturate(r.fs(RELIGHT_FORM, 1.f));
	rp.lightSpread   = std::max(0.05f, r.fs(RELIGHT_LIGHT_SPREAD, 0.7f));
	rp.depthStabilize = (int)r.popup(RELIGHT_DEPTH_STABILIZE, 3) - 1;
	rp.lineProtect   = saturate(r.fs(RELIGHT_LINE_PROTECT, 0.f));
	rp.lineThreshold = saturate(r.fs(RELIGHT_LINE_THRESHOLD, 0.35f));
	rp.volume        = std::max(0.f, r.fs(RELIGHT_VOLUME, 0.f));

	rp.depthNear    = (int)r.popup(RELIGHT_DEPTH_NEAR, 1);
	rp.depthSnap    = r.checkbox(RELIGHT_DEPTH_SNAP, true);
	rp.normalSource = (int)r.popup(RELIGHT_NORMAL_SOURCE, 1);
	rp.passEncoding = (int)r.popup(RELIGHT_PASS_ENCODING, 1);
	rp.maskSource   = (int)r.popup(RELIGHT_MASK_SOURCE, 1);
	rp.inputLinear  = r.checkbox(RELIGHT_INPUT_LINEAR, false);

	rp.outputMode = (int)r.popup(RELIGHT_OUTPUT_MODE, OUTPUT_RELIT);
	rp.mix        = saturate(r.fs(RELIGHT_MIX, 100.f) / 100.f);
	return r.err;
}

/* ------------------------------------------------------------------ */
/* SmartFX                                                             */
/* ------------------------------------------------------------------ */

static void RenderLog(const std::wstring &msg) { Relight_Log(msg); }

/* 前後のフレームの深度を取り出すときの番号 (他の取り出しと重ならない値) */
static const A_long RELIGHT_NB_ID = 400;

static void DeletePreRenderData(void *pre_render_dataPV)
{
	delete static_cast<PreRenderData *>(pre_render_dataPV);
}

static PF_Err PreRender(PF_InData *in_data, PF_OutData *out_data, PF_PreRenderExtra *extra)
{
	PF_Err err = PF_Err_NONE;
	PF_RenderRequest req = extra->input->output_request;
	req.preserve_rgb_of_zero_alpha = TRUE;

	PF_CheckoutResult in_result;
	AEFX_CLR_STRUCT(in_result);
	ERR(extra->cb->checkout_layer(in_data->effect_ref, RELIGHT_INPUT, RELIGHT_INPUT, &req,
	                              in_data->current_time, in_data->time_step, in_data->time_scale, &in_result));
	if (err) return err;

	extra->output->result_rect     = in_result.result_rect;
	extra->output->max_result_rect = in_result.max_result_rect;
	extra->output->solid           = FALSE;

#ifdef RELIGHT_HAS_CUDA
	if (extra->input->what_gpu == PF_GPU_Framework_CUDA) {
		PF_ParamDef def;
		AEFX_CLR_STRUCT(def);
		bool useGpu = true;
		if (PF_CHECKOUT_PARAM(in_data, RELIGHT_USE_GPU, in_data->current_time, in_data->time_step, in_data->time_scale, &def) == PF_Err_NONE) {
			useGpu = def.u.bd.value != 0;
			PF_CHECKIN_PARAM(in_data, &def);
		}
		if (useGpu) extra->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;
	}
#endif

	PreRenderData *prd = new PreRenderData;
	prd->originX = in_result.result_rect.left;
	prd->originY = in_result.result_rect.top;
	extra->output->pre_render_data = prd;
	extra->output->delete_pre_render_data_func = DeletePreRenderData;

	/* 補助レイヤー。未指定 (None) の場合は失敗しても無視する。
	   補助レイヤーは素材と大きさが違う (深度は短辺 518px) ので、素材の描く範囲ではなくレイヤー全体を取り出す。
	   (以前は素材の範囲をそのまま渡していたため、拡大表示で一部だけ描くときに深度の位置がずれていた。
	    次に巨大な範囲を渡したところ、しばらくすると取り出しに失敗して深度が消え、ただの光源のようになった)
	   まず 1 画素だけ要求してレイヤー全体の範囲 (max_result_rect) を知り、その範囲で取り出す */
	/* SmartRender で取り出すもの (auxIdx の番号) は、レイヤーが未指定でも必ずここで予約しておく。
	   予約していない番号を取り出すと AE が「Unknown checkout id」のエラーを出す */
	static const A_long auxIdx[3] = {RELIGHT_DEPTH_LAYER, RELIGHT_NORMAL_LAYER, RELIGHT_MASK_LAYER};
	for (int i = 0; i < 3; ++i) {
		AEFX_CLR_STRUCT(prd->auxRect[i]);
		prd->auxChecked[i] = false;
		PF_CheckoutResult probe;
		AEFX_CLR_STRUCT(probe);
		PF_RenderRequest probeReq = req;
		probeReq.rect.left = 0; probeReq.rect.top = 0; probeReq.rect.right = 1; probeReq.rect.bottom = 1;
		PF_LRect full;
		AEFX_CLR_STRUCT(full);
		if (extra->cb->checkout_layer(in_data->effect_ref, auxIdx[i], auxIdx[i] + 100, &probeReq,
		                              in_data->current_time, in_data->time_step, in_data->time_scale, &probe) == PF_Err_NONE)
			full = probe.max_result_rect;
		PF_CheckoutResult aux;
		AEFX_CLR_STRUCT(aux);
		PF_RenderRequest auxReq = req;
		/* 全体の範囲が分からないとき (未指定など) は、以前と同じく素材の描く範囲で予約する */
		auxReq.rect = (full.right > full.left && full.bottom > full.top) ? full : in_result.result_rect;
		if (extra->cb->checkout_layer(in_data->effect_ref, auxIdx[i], auxIdx[i], &auxReq,
		                              in_data->current_time, in_data->time_step, in_data->time_scale, &aux) == PF_Err_NONE) {
			prd->auxChecked[i] = true;
			if (full.right > full.left && full.bottom > full.top) prd->auxRect[i] = aux.result_rect;
		}
	}

	/* 深度を時間方向にならすため、前後のフレームの深度も予約する (SmartRender で RELIGHT_NB_ID + k として取り出す) */
	prd->nbCount = 0;
	if (prd->auxChecked[0] && prd->auxRect[0].right > prd->auxRect[0].left) {
		int stab = 2;
		PF_ParamDef def;
		AEFX_CLR_STRUCT(def);
		if (PF_CHECKOUT_PARAM(in_data, RELIGHT_DEPTH_STABILIZE, in_data->current_time, in_data->time_step, in_data->time_scale, &def) == PF_Err_NONE) {
			stab = def.u.pd.value - 1;
			PF_CHECKIN_PARAM(in_data, &def);
		}
		stab = std::max(0, std::min(stab, STABILIZE_MAX));
		static const int offs[4] = {-1, 1, -2, 2};
		for (int k = 0; k < stab * 2; ++k) {
			const int slot = prd->nbCount;
			PF_CheckoutResult r;
			AEFX_CLR_STRUCT(r);
			PF_RenderRequest q = req;
			q.rect = prd->auxRect[0];
			const A_long when = in_data->current_time + offs[k] * in_data->time_step;
			if (extra->cb->checkout_layer(in_data->effect_ref, RELIGHT_DEPTH_LAYER, RELIGHT_NB_ID + slot, &q,
			                              when, in_data->time_step, in_data->time_scale, &r) == PF_Err_NONE) {
				prd->nbOff[slot] = offs[k];
				prd->nbChecked[slot] = true;
				prd->nbCount++;
			}
		}
	}
	return err;
}

/* 描画のログ。設定が前回と同じなら 3 秒に 1 回まで (描画のたびに書くと多すぎる) */

static void RenderLogThrottled(const wchar_t *line)
{
	static std::mutex mtx;
	static std::wstring last;
	static std::chrono::steady_clock::time_point lastTime;
	/* 時刻と処理時間を除いた部分が同じなら「同じ設定」 */
	std::wstring key = line;
	size_t a = key.find(L" t="), b = key.find(L" ", a + 3);
	if (a != std::wstring::npos && b != std::wstring::npos) key.erase(a, b - a);
	size_t m = key.rfind(L" err=");
	if (m != std::wstring::npos) key.erase(m);
	std::lock_guard<std::mutex> lk(mtx);
	const auto now = std::chrono::steady_clock::now();
	if (key == last && now - lastTime < std::chrono::seconds(3)) return;
	last = key;
	lastTime = now;
	Relight_Log(line);
}

/* AE のワールド → RLImage。data を渡せば (GPU のワールド) それを使う */
static RLImage ImageOf(const PF_EffectWorld *w, int format, const void *data = NULL)
{
	RLImage im;
	im.data = NULL;
	im.width = im.height = im.rowbytes = 0;
	im.format = format;
	if (!w) return im;
	im.data = (const unsigned char *)(data ? data : w->data);
	im.width = w->width;
	im.height = w->height;
	im.rowbytes = w->rowbytes;
	return im;
}

/*
 * 描画。isGPU なら PF_Cmd_SMART_RENDER_GPU (ワールドは GPU のメモリ、BGRA float) で、RelightGPU_Render が描く。
 * そうでなければ CPU で RenderFrameCPU が描く。どちらも RelightCore.h の同じ計算。
 */
static PF_Err SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra, bool isGPU)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

	PF_EffectWorld *inputP = NULL, *outputP = NULL;
	PF_EffectWorld *depthP = NULL, *normalP = NULL, *maskP = NULL;

	ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, RELIGHT_INPUT, &inputP));
	ERR(extra->cb->checkout_output(in_data->effect_ref, &outputP));

	const PreRenderData *prd = (const PreRenderData *)extra->input->pre_render_data;
	if (!err) {
		/* PreRender で予約できたものだけ取り出す */
		auto checked = [&](int i) { return prd && prd->auxChecked[i]; };
		if (!checked(0) || extra->cb->checkout_layer_pixels(in_data->effect_ref, RELIGHT_DEPTH_LAYER,  &depthP))  depthP  = NULL;
		if (!checked(1) || extra->cb->checkout_layer_pixels(in_data->effect_ref, RELIGHT_NORMAL_LAYER, &normalP)) normalP = NULL;
		if (!checked(2) || extra->cb->checkout_layer_pixels(in_data->effect_ref, RELIGHT_MASK_LAYER,   &maskP))   maskP   = NULL;
		if (!isGPU) {
			/* CPU のワールドは data が無ければ「未指定」 (GPU のワールドは data を使わない) */
			if (depthP  && !depthP->data)  depthP  = NULL;
			if (normalP && !normalP->data) normalP = NULL;
			if (maskP   && !maskP->data)   maskP   = NULL;
		}
		if (depthP  && depthP->width  <= 0) depthP  = NULL;
		if (normalP && normalP->width <= 0) normalP = NULL;
		if (maskP   && maskP->width   <= 0) maskP   = NULL;
		if (!depthP && prd && prd->auxRect[0].right > prd->auxRect[0].left)
			RenderLog(L"render: depth layer is set but pixels are missing");
	}

	/* 前後のフレームの深度 (深度を時間方向にならす)。大きさが違うものは使わない */
	PF_EffectWorld *nbW[4] = {NULL, NULL, NULL, NULL};
	int nbOff[4] = {0, 0, 0, 0}, nbN = 0;
	std::vector<A_long> nbCheckedOut;
	if (!err && depthP && prd) {
		for (int k = 0; k < prd->nbCount && k < 4; ++k) {
			if (!prd->nbChecked[k]) continue;
			PF_EffectWorld *w = NULL;
			if (extra->cb->checkout_layer_pixels(in_data->effect_ref, RELIGHT_NB_ID + k, &w) != PF_Err_NONE) continue;
			nbCheckedOut.push_back(RELIGHT_NB_ID + k);
			if (!w || w->width != depthP->width || w->height != depthP->height || (!isGPU && !w->data)) continue;
			nbW[nbN] = w;
			nbOff[nbN] = prd->nbOff[k];
			++nbN;
		}
	}

	const float dsx = (float)in_data->downsample_x.num / (float)in_data->downsample_x.den;
	const float dsy = (float)in_data->downsample_y.num / (float)in_data->downsample_y.den;
	const float layerW = std::max(1.f, (float)in_data->width * dsx);
	const float layerH = std::max(1.f, (float)in_data->height * dsy);

	RenderParams rp;
	std::memset(&rp, 0, sizeof(rp));
	if (!err) {
		PF_Err e = ReadRenderParams(in_data, layerW, layerH, rp);
		if (e) { RenderLog(L"render: param checkout failed err=" + std::to_wstring((long)e)); err = e; }
	}
	const auto t0 = std::chrono::steady_clock::now();

	/* 描画中の C++ の例外を AE へ漏らさない (漏れると AE がこのエフェクトの描画をやめてしまうことがある) */
	try {
	if (!err && inputP && outputP) {
		const float originX = prd ? (float)prd->originX : 0.f;
		const float originY = prd ? (float)prd->originY : 0.f;
		ShadeContext ctx;
		std::memset(&ctx, 0, sizeof(ctx));
		ctx.rp      = rp;
		ctx.outW    = outputP->width;
		ctx.outH    = outputP->height;
		ctx.originX = originX;
		ctx.originY = originY;
		ctx.layerW  = layerW;
		ctx.layerH  = layerH;
		/* 補助レイヤー全体の大きさ。PreRender で取れなかったときは、取り出したワールドの大きさを全体とみなす */
		auto auxMap = [&](int i, const PF_EffectWorld *w, const RLImage &img) {
			float fw = 1.f, fh = 1.f;
			if (prd && prd->auxRect[i].right > prd->auxRect[i].left) {
				fw = (float)(prd->auxRect[i].right - prd->auxRect[i].left);
				fh = (float)(prd->auxRect[i].bottom - prd->auxRect[i].top);
			} else if (w) {
				fw = (float)w->width; fh = (float)w->height;
			}
			return rlMakeAuxMap(img, fw, fh, originX, originY, layerW, layerH);
		};

		if (!isGPU) {
			/* SmartFX ではレンダー要求のビット深度が extra->input->bitdepth で渡される */
			PF_PixelFormat fmt = PF_PixelFormat_ARGB32;
			int code = RL_FMT_ARGB8;
			if (extra->input->bitdepth == 32)      { fmt = PF_PixelFormat_ARGB128; code = RL_FMT_ARGB32F; }
			else if (extra->input->bitdepth == 16) { fmt = PF_PixelFormat_ARGB64;  code = RL_FMT_ARGB16; }
			ctx.normalA = auxMap(1, normalP, ImageOf(normalP, code));
			ctx.maskA   = auxMap(2, maskP,   ImageOf(maskP, code));
			const AuxMap depthMap = auxMap(0, depthP, ImageOf(depthP, code));
			RLImage nbImg[4];
			for (int k = 0; k < nbN; ++k) nbImg[k] = ImageOf(nbW[k], code);
			std::vector<Rgba> buf;
			RenderFrameCPU(ctx, ImageOf(inputP, code), depthMap, nbImg, nbOff, nbN, buf);
			WriteWorld(outputP, fmt, buf);
		} else {
			AEFX_SuiteScoper<PF_GPUDeviceSuite1> gpu(in_data, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1, out_data);
			AEFX_SuiteScoper<PF_WorldSuite2> worldSuite(in_data, kPFWorldSuite, kPFWorldSuiteVersion2, out_data);
			PF_PixelFormat pf = PF_PixelFormat_INVALID;
			worldSuite->PF_GetPixelFormat(inputP, &pf);
			if (pf != PF_PixelFormat_GPU_BGRA128) {
				RenderLog(L"render(gpu): unsupported pixel format " + std::to_wstring((long)pf));
				err = PF_Err_UNRECOGNIZED_PARAM_TYPE;
			} else {
				auto mem = [&](PF_EffectWorld *w) -> void * {
					void *p = NULL;
					if (w && gpu->GetGPUWorldData(in_data->effect_ref, w, &p) != PF_Err_NONE) p = NULL;
					return p;
				};
				void *srcMem = mem(inputP), *dstMem = mem(outputP);
				void *depthMem = mem(depthP), *normalMem = mem(normalP), *maskMem = mem(maskP);
				RLGpuFrame f;
				std::memset(&f, 0, sizeof(f));
				f.ctx = ctx;
				f.ctx.normalA = auxMap(1, normalP, ImageOf(normalMem ? normalP : NULL, RL_FMT_BGRA32F, normalMem));
				f.ctx.maskA   = auxMap(2, maskP,   ImageOf(maskMem ? maskP : NULL, RL_FMT_BGRA32F, maskMem));
				f.depthMap    = auxMap(0, depthP,  ImageOf(depthMem ? depthP : NULL, RL_FMT_BGRA32F, depthMem));
				f.src         = ImageOf(inputP, RL_FMT_BGRA32F, srcMem);
				f.dst         = dstMem;
				f.dstRowbytes = outputP->rowbytes;
				for (int k = 0; k < nbN; ++k) {
					void *m = mem(nbW[k]);
					if (!m) continue;
					f.depthNb[f.depthNbCount] = ImageOf(nbW[k], RL_FMT_BGRA32F, m);
					f.depthNbOff[f.depthNbCount] = nbOff[k];
					f.depthNbCount++;
				}
				if (!srcMem || !dstMem) {
					RenderLog(L"render(gpu): GetGPUWorldData failed");
					err = PF_Err_INTERNAL_STRUCT_DAMAGED;
				} else {
					char msg[256] = {0};
					if (RelightGPU_Render(f, msg, sizeof(msg)) != 0) {
						std::string m = msg;
						RenderLog(L"render(gpu): " + std::wstring(m.begin(), m.end()));
						err = PF_Err_INTERNAL_STRUCT_DAMAGED;
					}
				}
			}
		}
	}
	} catch (PF_Err &e) {
		err = e;
		RenderLog(L"render: PF_Err thrown " + std::to_wstring((long)e));
	} catch (std::exception &ex) {
		std::string what = ex.what();
		RenderLog(L"render: exception " + std::wstring(what.begin(), what.end()));
		err = PF_Err_OUT_OF_MEMORY;
	} catch (...) {
		RenderLog(L"render: unknown exception");
		err = PF_Err_INTERNAL_STRUCT_DAMAGED;
	}

	/* 描画の記録 (同じ設定なら 3 秒に 1 回まで)。プレビューが更新されない不具合の切り分け用 */
	{
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		wchar_t line[320];
		swprintf(line, 320, L"render: t=%ld/%lu %ldx%ld %ls ds=%.2f z1=%.2f i1=%.2f L2=%d L3=%d look=%d out=%d dither=%.2f depth=%d err=%d %.0f ms",
			(long)in_data->current_time, (unsigned long)in_data->time_scale,
			outputP ? (long)outputP->width : 0L, outputP ? (long)outputP->height : 0L, isGPU ? L"gpu" : L"cpu", dsx,
			rp.light[0].z, rp.light[0].intensity, (int)rp.light[1].on, (int)rp.light[2].on, rp.look, rp.outputMode,
			rp.edgeDither, depthP ? 1 + nbN : 0, (int)err, ms);
		RenderLogThrottled(line);
	}

	for (A_long id : nbCheckedOut) ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, id));
	ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, RELIGHT_INPUT));
	if (depthP)  ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, RELIGHT_DEPTH_LAYER));
	if (normalP) ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, RELIGHT_NORMAL_LAYER));
	if (maskP)   ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, RELIGHT_MASK_LAYER));
	return err;
}

/* ------------------------------------------------------------------ */
/* コンポビューの光源ギズモ (PF_Cmd_EVENT)                               */
/* ------------------------------------------------------------------ */

/*
 * 有効な光源ごとに、光の色の輪と番号を描く。
 *   - 輪の大きさ = 光の玉の見かけの大きさ (Light Height が大きいほどカメラに近く大きい)
 *   - 光源が物の後ろ (Height < 0) なら破線の輪
 *   - 番号の横に高さの値
 * 位置のドラッグは AE 標準の 2D ポイントのハンドルに任せ、ここでは描くだけ (クリックは処理しない)。
 */

/* レイヤー座標 → 表示中の窓のピクセル座標 */
static void LayerToFrame(PF_InData *in_data, PF_EventExtra *ex, float lx, float ly, float &fx, float &fy)
{
	PF_FixedPoint p;
	p.x = FLOAT2FIX(lx);
	p.y = FLOAT2FIX(ly);
	if ((*ex->contextH)->w_type == PF_Window_COMP)
		ex->cbs.layer_to_comp(ex->cbs.refcon, ex->contextH, in_data->current_time, in_data->time_scale, &p);
	ex->cbs.source_to_frame(ex->cbs.refcon, ex->contextH, &p);
	fx = (float)FIX_2_FLOAT(p.x);
	fy = (float)FIX_2_FLOAT(p.y);
}

static PF_Err DrawLightGizmos(PF_InData *in_data, PF_ParamDef *params[], PF_EventExtra *ex)
{
	PF_Err err = PF_Err_NONE;
	AEGP_SuiteHandler suites(in_data->pica_basicP);
	DRAWBOT_DrawRef drawRef = NULL;
	DRAWBOT_SupplierRef supplier = NULL;
	DRAWBOT_SurfaceRef surface = NULL;
	ERR(suites.EffectCustomUISuite1()->PF_GetDrawingReference(ex->contextH, &drawRef));
	if (err || !drawRef) return err;
	ERR(suites.DrawbotSuiteCurrent()->GetSupplier(drawRef, &supplier));
	ERR(suites.DrawbotSuiteCurrent()->GetSurface(drawRef, &surface));
	if (err || !supplier || !surface) return err;

	DRAWBOT_SupplierSuiteCurrent *sup = suites.SupplierSuiteCurrent();
	DRAWBOT_SurfaceSuiteCurrent  *srf = suites.SurfaceSuiteCurrent();
	DRAWBOT_PathSuiteCurrent     *pth = suites.PathSuiteCurrent();

	DRAWBOT_Boolean hasText = FALSE;
	sup->SupportsText(supplier, &hasText);
	DRAWBOT_FontRef font = NULL;
	if (hasText) {
		float size = 11.f;
		sup->GetDefaultFontSize(supplier, &size);
		if (sup->NewDefaultFont(supplier, size, &font) != kSPNoError) font = NULL;
	}

	const float layerH = (float)std::max<A_long>(1, in_data->height);
	for (int k = 0; k < RELIGHT_MAX_LIGHTS && !err; ++k) {
		const LightSlots &s = LIGHT_IDX[k];
		if (s.enable >= 0 && !params[s.enable]->u.bd.value) continue;
		const float lx = (float)FIX_2_FLOAT(params[s.position]->u.td.x_value);
		const float ly = (float)FIX_2_FLOAT(params[s.position]->u.td.y_value);
		const float z = clampf((float)params[s.height]->u.fs_d.value, LIGHT_Z_MIN, LIGHT_Z_MAX);
		const PF_Pixel col = params[s.color]->u.cd.value;
		const float range = std::max(0.05f, (float)params[s.range]->u.fs_d.value);
		const float spread = std::max(0.05f, (float)params[RELIGHT_LIGHT_SPREAD]->u.fs_d.value);

		/* 光の玉の見かけの半径 (原典の bulbRadius) をレイヤーのピクセルにして、窓の座標へ */
		const float bulbR = BULB_WORLD_RADIUS * ((BULB_CAMERA_Z - BULB_REFERENCE_Z) / (BULB_CAMERA_Z - z));
		float cx, cy, rx, ry;
		LayerToFrame(in_data, ex, lx, ly, cx, cy);
		LayerToFrame(in_data, ex, lx + bulbR * layerH, ly, rx, ry);
		const float r = std::max(6.f, std::sqrt((rx - cx) * (rx - cx) + (ry - cy) * (ry - cy)));

		DRAWBOT_ColorRGBA c;
		c.red = col.red / 255.f; c.green = col.green / 255.f; c.blue = col.blue / 255.f; c.alpha = 1.f;
		DRAWBOT_ColorRGBA shade;
		shade.red = shade.green = shade.blue = 0.f; shade.alpha = 0.6f;

		DRAWBOT_PathRef path = NULL;
		if (sup->NewPath(supplier, &path) != kSPNoError || !path) continue;
		DRAWBOT_PointF32 center;
		center.x = cx; center.y = cy;
		if (z >= 0.f) {
			pth->AddArc(path, &center, r, 0.f, 360.f);
		} else {
			/* 物の後ろ: 破線 (15 度ずつ描いて 15 度空ける) */
			for (int s = 0; s < 12; ++s) {
				const float a0 = s * 30.f * 3.14159265f / 180.f;
				pth->MoveTo(path, cx + r * std::cos(a0), cy + r * std::sin(a0));
				pth->AddArc(path, &center, r, s * 30.f, 15.f);
			}
		}
		/* 下に暗い縁を付けてから色で描き、明るい素材の上でも見えるようにする */
		DRAWBOT_PenRef under = NULL, pen = NULL;
		if (sup->NewPen(supplier, &shade, 3.5f, &under) == kSPNoError && under) {
			srf->StrokePath(surface, under, path);
			sup->ReleaseObject((DRAWBOT_ObjectRef)under);
		}
		if (sup->NewPen(supplier, &c, 1.5f, &pen) == kSPNoError && pen) {
			srf->StrokePath(surface, pen, path);
			sup->ReleaseObject((DRAWBOT_ObjectRef)pen);
		}
		sup->ReleaseObject((DRAWBOT_ObjectRef)path);

		/* 範囲の目安: 撮影寄りのグラデーションが半分の明るさになる距離 (Light Spread × Range) を細い点線の輪で */
		{
			float ex2, ey2;
			LayerToFrame(in_data, ex, lx + spread * range * layerH, ly, ex2, ey2);
			const float rr = std::sqrt((ex2 - cx) * (ex2 - cx) + (ey2 - cy) * (ey2 - cy));
			DRAWBOT_PathRef rp2 = NULL;
			if (rr > r + 4.f && sup->NewPath(supplier, &rp2) == kSPNoError && rp2) {
				for (int s2 = 0; s2 < 48; ++s2) {
					const float a0 = s2 * 7.5f * 3.14159265f / 180.f;
					pth->MoveTo(rp2, cx + rr * std::cos(a0), cy + rr * std::sin(a0));
					pth->AddArc(rp2, &center, rr, s2 * 7.5f, 3.f);
				}
				DRAWBOT_ColorRGBA faint = c;
				faint.alpha = 0.55f;
				DRAWBOT_PenRef thin = NULL;
				if (sup->NewPen(supplier, &faint, 1.f, &thin) == kSPNoError && thin) {
					srf->StrokePath(surface, thin, rp2);
					sup->ReleaseObject((DRAWBOT_ObjectRef)thin);
				}
				sup->ReleaseObject((DRAWBOT_ObjectRef)rp2);
			}
		}

		if (font) {
			wchar_t label[32];
			const A_long lk = params[s.look]->u.pd.value;
			swprintf(label, 32, L"%d  h %.2f%s%s", k + 1, z, z < 0.f ? L" (behind)" : L"",
			         lk == 2 ? L"  Anime" : (lk == 3 ? L"  Cine" : L""));
			DRAWBOT_UTF16Char text[32];
			for (int i = 0; i < 32; ++i) { text[i] = (DRAWBOT_UTF16Char)label[i]; if (!label[i]) break; }
			DRAWBOT_PointF32 org;
			org.x = cx + r + 6.f; org.y = cy + 4.f;
			DRAWBOT_BrushRef dark = NULL, brush = NULL;
			if (sup->NewBrush(supplier, &shade, &dark) == kSPNoError && dark) {
				DRAWBOT_PointF32 o2 = org;
				o2.x += 1.f; o2.y += 1.f;
				srf->DrawString(surface, dark, font, text, &o2, kDRAWBOT_TextAlignment_Left, kDRAWBOT_TextTruncation_None, 0.f);
				sup->ReleaseObject((DRAWBOT_ObjectRef)dark);
			}
			if (sup->NewBrush(supplier, &c, &brush) == kSPNoError && brush) {
				srf->DrawString(surface, brush, font, text, &org, kDRAWBOT_TextAlignment_Left, kDRAWBOT_TextTruncation_None, 0.f);
				sup->ReleaseObject((DRAWBOT_ObjectRef)brush);
			}
		}
	}
	if (font) sup->ReleaseObject((DRAWBOT_ObjectRef)font);
	return err;
}

static PF_Err HandleEvent(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_EventExtra *ex)
{
	PF_Err err = PF_Err_NONE;
	if (!ex || !ex->contextH) return err;
	/* 描くだけ。クリックやドラッグは処理しない (AE 標準のポイントのハンドルがそのまま使える) */
	if (ex->e_type == PF_Event_DRAW && (*ex->contextH)->w_type == PF_Window_COMP) {
		try {
			err = DrawLightGizmos(in_data, params, ex);
		} catch (...) {
			err = PF_Err_NONE;   /* 描けなくても本体の動作には影響させない */
		}
		/* evt_out_flags は付けない (AE 標準のポイントのハンドルの描画を止めないため) */
	}
	return err;
}

/* ------------------------------------------------------------------ */
/* エントリポイント                                                    */
/* ------------------------------------------------------------------ */

extern "C" DllExport
PF_Err PluginDataEntryFunction2(
	PF_PluginDataPtr inPtr,
	PF_PluginDataCB2 inPluginDataCallBackPtr,
	SPBasicSuite    *inSPBasicSuitePtr,
	const char      *inHostName,
	const char      *inHostVersion)
{
	PF_Err result = PF_Err_INVALID_CALLBACK;
	result = PF_REGISTER_EFFECT_EXT2(
		inPtr,
		inPluginDataCallBackPtr,
		RELIGHT_NAME,
		RELIGHT_MATCH_NAME,
		RELIGHT_CATEGORY,
		AE_RESERVED_INFO,
		"EffectMain",
		RELIGHT_SUPPORT_URL);
	return result;
}

PF_Err EffectMain(
	PF_Cmd       cmd,
	PF_InData   *in_data,
	PF_OutData  *out_data,
	PF_ParamDef *params[],
	PF_LayerDef *output,
	void        *extra)
{
	PF_Err err = PF_Err_NONE;
	try {
		switch (cmd) {
		case PF_Cmd_ABOUT:
			err = About(in_data, out_data, params, output);
			break;
		case PF_Cmd_GLOBAL_SETUP:
			err = GlobalSetup(in_data, out_data, params, output);
			break;
		case PF_Cmd_PARAMS_SETUP:
			err = ParamsSetup(in_data, out_data, params, output);
			break;
		case PF_Cmd_SEQUENCE_SETUP:
			/* エフェクト適用時 */
			out_data->sequence_data = NULL;
			Auto_Request(in_data, false);
			break;
		case PF_Cmd_SEQUENCE_RESETUP:
			/* プロジェクトを開いたとき。結線済みなら何もしない (深度の素材が見つからなければ作り直す) */
			Auto_Request(in_data, false);
			break;
		case PF_Cmd_USER_CHANGED_PARAM: {
			const PF_UserChangedParamExtra *uc = (const PF_UserChangedParamExtra *)extra;
			if (!uc) break;
			if (uc->param_index == RELIGHT_AUTO_GENERATE) {
				Auto_Request(in_data, true, (int)params[RELIGHT_AUTO_ENGINE]->u.pd.value);
			} else if (uc->param_index == RELIGHT_AUTO_CLEAN) {
				Auto_CleanCache(in_data);
			} else if (uc->param_index == RELIGHT_PRESET) {
				A_long sel = params[RELIGHT_PRESET]->u.pd.value;
				if (sel >= 2 && sel - 2 < NUM_PRESETS) ApplyPreset(in_data, params, PRESETS[sel - 2]);
				/* メニューのように使う: 適用したら「Select...」に戻す */
				params[RELIGHT_PRESET]->u.pd.value = 1;
				params[RELIGHT_PRESET]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
				err = UpdateLightUI(in_data, params);
			} else if (uc->param_index == RELIGHT_LIGHT2_ENABLE || uc->param_index == RELIGHT_LIGHT3_ENABLE) {
				err = UpdateLightUI(in_data, params);
			}
			break;
		}
		case PF_Cmd_UPDATE_PARAMS_UI:
			err = UpdateLightUI(in_data, params);
			break;
		case PF_Cmd_EVENT:
			err = HandleEvent(in_data, out_data, params, (PF_EventExtra *)extra);
			break;
		case PF_Cmd_SMART_PRE_RENDER:
			err = PreRender(in_data, out_data, (PF_PreRenderExtra *)extra);
			break;
		case PF_Cmd_SMART_RENDER:
			err = SmartRender(in_data, out_data, (PF_SmartRenderExtra *)extra, false);
			break;
		case PF_Cmd_SMART_RENDER_GPU:
			err = SmartRender(in_data, out_data, (PF_SmartRenderExtra *)extra, true);
			break;
		case PF_Cmd_GPU_DEVICE_SETUP: {
			/* GPU 版は CUDA だけ。ほか (DirectX / OpenCL / Metal) のときは CPU 版で描く */
			PF_GPUDeviceSetupExtra *gx = (PF_GPUDeviceSetupExtra *)extra;
			out_data->out_flags2 = 0;
#ifdef RELIGHT_HAS_CUDA
			if (gx && gx->input->what_gpu == PF_GPU_Framework_CUDA) out_data->out_flags2 = PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
#endif
			Relight_Log(std::wstring(L"gpu: device setup framework=") + std::to_wstring(gx ? (long)gx->input->what_gpu : -1L) +
			            (out_data->out_flags2 ? L" (use CUDA)" : L" (CPU)"));
			break;
		}
		case PF_Cmd_GPU_DEVICE_SETDOWN:
			RelightGPU_Release();
			break;
		default:
			break;
		}
	} catch (PF_Err &thrown_err) {
		err = thrown_err;
	} catch (...) {
		Relight_Log(L"EffectMain: exception in cmd=" + std::to_wstring((long)cmd));
		err = PF_Err_INTERNAL_STRUCT_DAMAGED;
	}
	return err;
}
