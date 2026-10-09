/*
 * RelightGPU.cu
 * GPU 版 (CUDA)。CPU 版 (RelightAE.cpp の RenderFrameCPU) と同じ手順を、RelightCore.h の同じ関数で行う。
 * AE が GPU で描くとき (Mercury GPU アクセラレーション = CUDA) に PF_Cmd_SMART_RENDER_GPU から呼ばれる。
 * 入力・出力・深度などのワールドは AE が GPU のメモリに置いたもの (BGRA float) をそのまま使う。
 *
 * 手順:
 *   深度の点 → 出力の大きさへ引き伸ばし (線画に合わせる) → 線画のマスク
 *   → 格子: 深度の平均 → 膨らみ (輪郭からの距離) → 勾配と凹み → 落ち影 → ならし → 当たり具合 → リムの下準備
 *   → 光の玉の隠れ具合 → 画素の照明 → (Edge Dither) → 出力
 */
#include "RelightGPU.h"
#include <cuda_runtime.h>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstring>

/* ------------------------------------------------------------------ */
/* GPU のメモリ                                                        */
/* ------------------------------------------------------------------ */

/* 作業領域はフレームごとに確保せず使い回す (cudaMalloc は遅い)。同時に描くフレームの数だけ持つ */
struct DevBlock {
	void  *p;
	size_t size;
	bool   inUse;
};
static std::mutex            g_memMtx;
static std::vector<DevBlock> g_blocks;
static float                *g_lut = NULL;   /* ガンマの表 (dec, enc を続けて置く) */

static void *AcquireBlock(size_t size)
{
	std::lock_guard<std::mutex> lk(g_memMtx);
	for (DevBlock &b : g_blocks)
		if (!b.inUse && b.size >= size) { b.inUse = true; return b.p; }
	/* 足りなければ、空いている小さいものを捨てて大きく取り直す */
	for (size_t i = 0; i < g_blocks.size(); ++i)
		if (!g_blocks[i].inUse) { cudaFree(g_blocks[i].p); g_blocks.erase(g_blocks.begin() + i); break; }
	void *p = NULL;
	if (cudaMalloc(&p, size) != cudaSuccess) return NULL;
	DevBlock b = {p, size, true};
	g_blocks.push_back(b);
	return p;
}

static void ReleaseBlock(void *p)
{
	std::lock_guard<std::mutex> lk(g_memMtx);
	for (DevBlock &b : g_blocks)
		if (b.p == p) { b.inUse = false; return; }
}

static const float *DeviceLut()
{
	std::lock_guard<std::mutex> lk(g_memMtx);
	if (g_lut) return g_lut;
	std::vector<float> host(RL_LUT_SIZE * 2);
	rlBuildLut(host.data(), host.data() + RL_LUT_SIZE);
	if (cudaMalloc(&g_lut, sizeof(float) * host.size()) != cudaSuccess) { g_lut = NULL; return NULL; }
	cudaMemcpy(g_lut, host.data(), sizeof(float) * host.size(), cudaMemcpyHostToDevice);
	return g_lut;
}

void RelightGPU_Release()
{
	std::lock_guard<std::mutex> lk(g_memMtx);
	for (DevBlock &b : g_blocks) cudaFree(b.p);
	g_blocks.clear();
	if (g_lut) { cudaFree(g_lut); g_lut = NULL; }
}

/* ------------------------------------------------------------------ */
/* カーネル                                                             */
/* ------------------------------------------------------------------ */

#define RL_XY(W, H)                                         \
	const int x = blockIdx.x * blockDim.x + threadIdx.x;    \
	const int y = blockIdx.y * blockDim.y + threadIdx.y;    \
	if (x >= (W) || y >= (H)) return;

static dim3 Grid2(int W, int H) { return dim3((W + 15) / 16, (H + 15) / 16, 1); }
static const dim3 BLOCK2(16, 16, 1);

/* 前後のフレームの深度 (カーネルへ値で渡す) */
struct NbSet {
	RLImage img[4];
	int     off[4];
	int     n;
};

__global__ void kLowDepth(RLImage img, NbSet nb, int depthNear, bool dec, float *low)
{
	RL_XY(img.width, img.height);
	float d = rlLowDepth(img, x, y, depthNear, dec);
	if (nb.n > 0) d = rlStabilizeDepth(d, nb.img, nb.off, nb.n, x, y, depthNear, dec);
	low[(size_t)y * img.width + x] = d;
}

__global__ void kUpsample(ShadeContext c, const float *low, int dw, int dh, AuxMap map, RLImage src, float *depth)
{
	RL_XY(c.outW, c.outH);
	depth[(size_t)y * c.outW + x] = rlUpsampleDepth(&c, low, dw, dh, map, src, x, y);
}

__global__ void kLuma(ShadeContext c, RLImage src, float *luma)
{
	RL_XY(c.outW, c.outH);
	luma[(size_t)y * c.outW + x] = rlLuma(&c, src, x, y);
}

__global__ void kBox(const float *in, float *out, int W, int H, int rad, bool horizontal)
{
	RL_XY(W, H);
	out[(size_t)y * W + x] = rlBox(in, W, H, rad, horizontal, x, y);
}

__global__ void kLineMask(const float *luma, const float *blur, int W, int H, float threshold, float protect, float *mask)
{
	RL_XY(W, H);
	const size_t n = (size_t)y * W + x;
	mask[n] = rlLineMask(luma[n], blur[n], threshold, protect);
}

__global__ void kCellDepth(ShadeContext c, float *gDepth)
{
	RL_XY(c.gw, c.gh);
	gDepth[(size_t)y * c.gw + x] = rlCellDepth(&c, x, y);
}

__global__ void kVolumeSeed(ShadeContext c, float *dist)
{
	RL_XY(c.gw, c.gh);
	dist[(size_t)y * c.gw + x] = rlVolumeSeed(&c, x, y);
}

/* 輪郭からの距離: 隣 8 マスから 1 歩ずつ広げる (縦横 1、斜め √2)。
   VOLUME_RADIUS 回で、それより近いマスはすべて CPU 版の 2 パスの距離変換と同じ値になる (それより遠いと膨らみは同じ) */
__global__ void kRelax(const float *in, float *out, int GW, int GH)
{
	RL_XY(GW, GH);
	float v = in[(size_t)y * GW + x];
	for (int dy = -1; dy <= 1; ++dy)
		for (int dx = -1; dx <= 1; ++dx) {
			if (!dx && !dy) continue;
			const int xx = x + dx, yy = y + dy;
			if (xx < 0 || yy < 0 || xx >= GW || yy >= GH) continue;
			v = fminf(v, in[(size_t)yy * GW + xx] + ((dx && dy) ? 1.41421356f : 1.f));
		}
	out[(size_t)y * GW + x] = v;
}

__global__ void kBulge(float *gDepth, float *gBulge, const float *dist, int GN, float volume)
{
	const int n = blockIdx.x * blockDim.x + threadIdx.x;
	if (n >= GN) return;
	const float b = rlBulge(dist[n], gDepth[n], volume);
	gBulge[n] = b;
	gDepth[n] += b;
}

__global__ void kSurface(ShadeContext c, float *gSurf)
{
	RL_XY(c.gw, c.gh);
	rlCellSurface(&c, x, y, &gSurf[((size_t)y * c.gw + x) * 3]);
}

__global__ void kShadow(ShadeContext c, float *gShadow)
{
	RL_XY(c.gw, c.gh);
	for (int k = 0; k < c.nL; ++k) gShadow[((size_t)y * c.gw + x) * c.nL + k] = rlCellShadow(&c, x, y, k);
}

__global__ void kSmooth(ShadeContext c, const float *src, float *dst, int stride, int R, bool horizontal)
{
	RL_XY(c.gw, c.gh);
	rlSmoothCell(&c, src, dst, stride, R, horizontal, x, y);
}

__global__ void kKey(ShadeContext c, float *gKey)
{
	RL_XY(c.gw, c.gh);
	for (int l = 0; l < c.nL; ++l) gKey[((size_t)y * c.gw + x) * c.nL + l] = rlCellKey(&c, x, y, l);
}

__global__ void kRowMin(ShadeContext c, int RC, float *rowMin)
{
	RL_XY(c.gw, c.gh);
	rowMin[(size_t)y * c.gw + x] = rlRowMin(&c, RC, x, y);
}

__global__ void kColMin(ShadeContext c, const float *rowMin, int RC, float *out)
{
	RL_XY(c.gw, c.gh);
	out[(size_t)y * c.gw + x] = rlColMin(&c, rowMin, RC, x, y);
}

__global__ void kRimMask(ShadeContext c, float *rimRaw)
{
	RL_XY(c.outW, c.outH);
	for (int k = 0; k < c.nL; ++k) rimRaw[((size_t)y * c.outW + x) * c.nL + k] = rlRimMask(&c, x, y, k);
}

/* Rim Smoothing: 深度の中央値 (1 方向) と、実際の輪郭の内側か */
__global__ void kMedian(const float *in, float *out, int W, int H, int rad, bool horizontal)
{
	RL_XY(W, H);
	out[(size_t)y * W + x] = rlMedian1D(in, W, H, rad, horizontal, x, y);
}

__global__ void kRimInside(const float *depth, const float *smoothed, float *inside, int W, int H)
{
	RL_XY(W, H);
	const size_t n = (size_t)y * W + x;
	inside[n] = rlRimInside(depth[n], smoothed[n]);
}

/* 光源ごとのぼかしの半径 (カーネルへ値で渡す) */
struct RadSet { int r[RELIGHT_MAX_LIGHTS]; };

__global__ void kBoxStrided(const float *in, float *out, int W, int H, int stride, RadSet rad, bool horizontal)
{
	RL_XY(W, H);
	for (int k = 0; k < stride; ++k) out[((size_t)y * W + x) * stride + k] = rlBoxStrided(in, W, H, stride, k, rad.r[k], horizontal, x, y);
}

__global__ void kRimClip(const float *raw, const float *blurred, float *out, int W, int H, int stride)
{
	RL_XY(W, H);
	for (int k = 0; k < stride; ++k) out[((size_t)y * W + x) * stride + k] = rlRimClip(raw, blurred, W, H, stride, k, x, y);
}

__global__ void kBulb(ShadeContext c, float *exposure)
{
	const int k = threadIdx.x;
	if (k < c.nL) exposure[k] = computeBulbExposure(&c, c.L[k]);
}

/* BGRA float のワールドへ書く */
__device__ __forceinline__ void writeBGRA(void *dst, int rowbytes, int x, int y, const Rgba &o)
{
	float *p = (float *)((unsigned char *)dst + (size_t)y * rowbytes) + (size_t)x * 4;
	p[0] = o.b; p[1] = o.g; p[2] = o.r; p[3] = o.a;
}

/* 照明。Edge Dither を掛けるときは結果と差分を作業領域に置き、掛けないときは出力へ直接書く */
__global__ void kShade(ShadeContext c, RLImage src, void *dst, int dstRowbytes, float *buf, float *delta)
{
	RL_XY(c.outW, c.outH);
	const Rgba s = rlRead(src, x, y);
	Rgba o;
	shadePixel(&c, x, y, s, o);
	if (buf) {
		const size_t n = (size_t)y * c.outW + x;
		buf[n * 4 + 0] = o.a; buf[n * 4 + 1] = o.r; buf[n * 4 + 2] = o.g; buf[n * 4 + 3] = o.b;
		delta[n * 3 + 0] = o.r - s.r; delta[n * 3 + 1] = o.g - s.g; delta[n * 3 + 2] = o.b - s.b;
	} else {
		writeBGRA(dst, dstRowbytes, x, y, o);
	}
}

__global__ void kDither(ShadeContext c, RLImage src, const float *buf, const float *delta, void *dst, int dstRowbytes)
{
	RL_XY(c.outW, c.outH);
	const size_t n = (size_t)y * c.outW + x;
	Rgba o;
	o.a = buf[n * 4 + 0]; o.r = buf[n * 4 + 1]; o.g = buf[n * 4 + 2]; o.b = buf[n * 4 + 3];
	rlEdgeDither(&c, delta, rlRead(src, x, y), x, y, o);
	writeBGRA(dst, dstRowbytes, x, y, o);
}

/* ------------------------------------------------------------------ */
/* 全体の手順                                                           */
/* ------------------------------------------------------------------ */

/* 作業領域の割り付け (1 つの塊から切り出す) */
struct Carver {
	unsigned char *base;
	size_t used;
	float *take(size_t count)
	{
		float *p = (float *)(base + used);
		used += (count * sizeof(float) + 255) & ~(size_t)255;
		return p;
	}
};

#define RL_CHECK(what)                                                                       \
	do {                                                                                     \
		cudaError_t e_ = cudaGetLastError();                                                 \
		if (e_ != cudaSuccess) {                                                             \
			if (err) snprintf(err, errLen, "%s: %s", what, cudaGetErrorString(e_));          \
			if (block) ReleaseBlock(block);                                                  \
			return (int)e_;                                                                  \
		}                                                                                    \
	} while (0)

int RelightGPU_Render(RLGpuFrame &f, char *err, int errLen)
{
	ShadeContext &c = f.ctx;
	const RenderParams &rp = c.rp;
	const int W = c.outW, H = c.outH;
	const size_t NP = (size_t)W * H;
	void *block = NULL;

	c.lutDec = DeviceLut();
	if (!c.lutDec) { if (err) snprintf(err, errLen, "LUT alloc failed"); return -1; }
	c.lutEnc = c.lutDec + RL_LUT_SIZE;
	c.depth = NULL;
	c.hasDepth = false;
	c.lineMask = NULL;
	rlSetupFrame(c);

	const bool hasDepth = f.depthMap.img.data != NULL;
	const int dw = f.depthMap.img.width, dh = f.depthMap.img.height;
	const bool anyRim = rlAnyRim(c);
	const bool wantLines = rp.lineProtect > 0.f || rp.outputMode == OUTPUT_LINES || anyRim;
	const int GW = c.gw, GH = c.gh, nL = c.nL;
	const size_t GN = (size_t)GW * GH;
	const int stride = rlMax(3, nL);
	/* Rim Smoothing: 輪郭をならした深度を作る光源の数 (同じ半径なら使い回す) */
	int nSmooth = 0;
	for (int k = 0; k < nL; ++k)
		if (c.L[k].rim && c.L[k].rimSmoothR > 0 && rlRimSmoothShare(c, k) == k) ++nSmooth;

	/* 必要な量を数えてから 1 つの塊で取る */
	size_t floats = 0;
	auto count = [&](size_t n) { floats += n + 64; };
	if (hasDepth) { count((size_t)dw * dh); count(NP); }
	if (wantLines) { count(NP); count(NP); count(NP); count(NP); }
	if (hasDepth) {
		count(GN); count(GN); count(GN); count(GN);   /* gDepth, gBulge, dist, dist2 */
		count(GN * 3); count(GN * rlMax(1, nL)); count(GN * rlMax(1, nL)); count(GN * stride);   /* surf, shadow, key, tmp */
		count(GN); count(GN);                          /* rowMin, rimMin */
	}
	count(NP * 4); count(NP * 3); count(8);
	if (hasDepth && anyRim) {
		count(NP * nL); count(NP * nL); count(NP * nL);
		/* 作業 1 枚と、光源ごとのならした深度・内側 */
		if (nSmooth > 0) { count(NP); for (int i = 0; i < nSmooth; ++i) { count(NP); count(NP); } }
	}
	block = AcquireBlock(floats * sizeof(float));
	if (!block) { if (err) snprintf(err, errLen, "work alloc failed (%zu MB)", floats * 4 / 1048576); return -2; }
	Carver cv = {(unsigned char *)block, 0};

	/* 1. 深度 */
	float *depth = NULL;
	if (hasDepth) {
		float *low = cv.take((size_t)dw * dh);
		NbSet nb;
		std::memset(&nb, 0, sizeof(nb));
		for (int k = 0; k < f.depthNbCount && k < 4; ++k) { nb.img[nb.n] = f.depthNb[k]; nb.off[nb.n] = f.depthNbOff[k]; nb.n++; }
		kLowDepth<<<Grid2(dw, dh), BLOCK2>>>(f.depthMap.img, nb, rp.depthNear, rp.passEncoding == 2, low);
		depth = cv.take(NP);
		kUpsample<<<Grid2(W, H), BLOCK2>>>(c, low, dw, dh, f.depthMap, f.src, depth);
		RL_CHECK("depth");
		c.depth = depth;
		c.hasDepth = true;
	}

	/* 2. 線画のマスク */
	if (wantLines) {
		float *luma = cv.take(NP), *tmp = cv.take(NP), *blur = cv.take(NP), *mask = cv.take(NP);
		const int rad = rlLineRadius(c.layerH);
		const float protect = 1.f;   /* 線らしさそのもの。保護の強さは画素で掛ける */
		kLuma<<<Grid2(W, H), BLOCK2>>>(c, f.src, luma);
		kBox<<<Grid2(W, H), BLOCK2>>>(luma, tmp, W, H, rad, true);
		kBox<<<Grid2(W, H), BLOCK2>>>(tmp, blur, W, H, rad, false);
		kLineMask<<<Grid2(W, H), BLOCK2>>>(luma, blur, W, H, rp.lineThreshold, protect, mask);
		RL_CHECK("line mask");
		c.lineMask = mask;
	}

	/* 3. 格子 */
	float *bulbDev = NULL;
	if (hasDepth) {
		float *gDepth = cv.take(GN), *gBulge = cv.take(GN), *dist = cv.take(GN), *dist2 = cv.take(GN);
		float *gSurf = cv.take(GN * 3), *gShadow = cv.take(GN * rlMax(1, nL)), *gKey = cv.take(GN * rlMax(1, nL));
		float *tmp = cv.take(GN * stride), *rowMin = cv.take(GN), *rimMin = cv.take(GN);
		const dim3 gg = Grid2(GW, GH);

		kCellDepth<<<gg, BLOCK2>>>(c, gDepth);
		c.gDepth = gDepth;
		if (rp.volume > 0.f) {
			kVolumeSeed<<<gg, BLOCK2>>>(c, dist);
			for (int it = 0; it < (int)VOLUME_RADIUS; ++it) {
				kRelax<<<gg, BLOCK2>>>(dist, dist2, GW, GH);
				float *t = dist; dist = dist2; dist2 = t;
			}
			kBulge<<<(unsigned)((GN + 255) / 256), 256>>>(gDepth, gBulge, dist, (int)GN, rp.volume);
			c.gBulge = gBulge;
		}
		kSurface<<<gg, BLOCK2>>>(c, gSurf);
		c.gSurf = gSurf;
		if (rp.shadow > 0.f && nL > 0) {
			kShadow<<<gg, BLOCK2>>>(c, gShadow);
		} else {
			/* 落ち影なし = 1 */
			std::vector<float> ones(GN * rlMax(1, nL), 1.f);
			cudaMemcpy(gShadow, ones.data(), sizeof(float) * ones.size(), cudaMemcpyHostToDevice);
		}
		RL_CHECK("grid");
		const int R = rlSmoothRadius(rp);
		if (R > 0) {
			kSmooth<<<gg, BLOCK2>>>(c, gSurf, tmp, 3, R, true);
			kSmooth<<<gg, BLOCK2>>>(c, tmp, gSurf, 3, R, false);
			if (rp.shadow > 0.f && nL > 0) {
				kSmooth<<<gg, BLOCK2>>>(c, gShadow, tmp, nL, R, true);
				kSmooth<<<gg, BLOCK2>>>(c, tmp, gShadow, nL, R, false);
			}
		}
		c.gShadow = gShadow;
		if (rlUseKeyGrid(c)) {
			kKey<<<gg, BLOCK2>>>(c, gKey);
			const int RK = rlKeyRadius(rp);
			kSmooth<<<gg, BLOCK2>>>(c, gKey, tmp, nL, RK, true);
			kSmooth<<<gg, BLOCK2>>>(c, tmp, gKey, nL, RK, false);
			c.gKey = gKey;
		}
		if (anyRim) {
			const int RC = rlRimCells(c);
			kRowMin<<<gg, BLOCK2>>>(c, RC, rowMin);
			kColMin<<<gg, BLOCK2>>>(c, rowMin, RC, rimMin);
			c.gRimMin = rimMin;
		}
		RL_CHECK("smooth");
	}

	/* 4. 光の玉の隠れ具合 (光源ごとに 1 回)。CPU へ戻して画素の計算に渡す */
	float *buf = cv.take(NP * 4), *delta = cv.take(NP * 3);
	bulbDev = cv.take(8);
	if (nL > 0) {
		kBulb<<<1, 32>>>(c, bulbDev);
		float host[RELIGHT_MAX_LIGHTS] = {1.f, 1.f, 1.f};
		cudaMemcpy(host, bulbDev, sizeof(float) * nL, cudaMemcpyDeviceToHost);
		for (int k = 0; k < nL; ++k) c.L[k].bulbExposure = host[k];
		RL_CHECK("bulb");
	}

	/* 4b. リムの下地: 光源の方へずらした深度との差で帯を作り、少しぼかす */
	if (c.gRimMin && nL > 0) {
		float *rimRaw = cv.take(NP * nL), *rimTmp = cv.take(NP * nL), *rimMask = cv.take(NP * nL);
		/* Rim Smoothing: 輪郭をならした深度 (箱形 2 回、縦横) */
		if (nSmooth > 0) {
			float *t = cv.take(NP);
			for (int k = 0; k < nL; ++k) {
				LightCtx &L = c.L[k];
				if (!L.rim || L.rimSmoothR <= 0) continue;
				const int share = rlRimSmoothShare(c, k);
				if (share != k) { L.rimDepth = c.L[share].rimDepth; L.rimInside = c.L[share].rimInside; continue; }
				float *med = cv.take(NP), *inside = cv.take(NP);
				const int r = L.rimSmoothR;
				kMedian<<<Grid2(W, H), BLOCK2>>>(depth, t, W, H, r, true);
				kMedian<<<Grid2(W, H), BLOCK2>>>(t, med, W, H, r, false);
				kMedian<<<Grid2(W, H), BLOCK2>>>(med, t, W, H, r, true);
				kMedian<<<Grid2(W, H), BLOCK2>>>(t, med, W, H, r, false);
				kRimInside<<<Grid2(W, H), BLOCK2>>>(depth, med, inside, W, H);
				L.rimDepth = med;
				L.rimInside = inside;
			}
			RL_CHECK("rim smooth");
		}
		RadSet rr;
		for (int k = 0; k < RELIGHT_MAX_LIGHTS; ++k) rr.r[k] = k < nL ? rlRimBlurRadius(c.L[k]) : 1;
		kRimMask<<<Grid2(W, H), BLOCK2>>>(c, rimRaw);
		kBoxStrided<<<Grid2(W, H), BLOCK2>>>(rimRaw, rimTmp, W, H, nL, rr, true);
		kBoxStrided<<<Grid2(W, H), BLOCK2>>>(rimTmp, rimMask, W, H, nL, rr, false);
		kRimClip<<<Grid2(W, H), BLOCK2>>>(rimRaw, rimMask, rimTmp, W, H, nL);   /* 輪郭の外へにじんだ分を消す */
		rimMask = rimTmp;
		RL_CHECK("rim");
		c.rimMask = rimMask;
	}

	/* 5. 照明 (と Edge Dither) */
	if (rlUseEdgeDither(c)) {
		kShade<<<Grid2(W, H), BLOCK2>>>(c, f.src, f.dst, f.dstRowbytes, buf, delta);
		kDither<<<Grid2(W, H), BLOCK2>>>(c, f.src, buf, delta, f.dst, f.dstRowbytes);
	} else {
		kShade<<<Grid2(W, H), BLOCK2>>>(c, f.src, f.dst, f.dstRowbytes, NULL, NULL);
	}
	RL_CHECK("shade");
	cudaError_t e = cudaDeviceSynchronize();
	ReleaseBlock(block);
	if (e != cudaSuccess) {
		if (err) snprintf(err, errLen, "sync: %s", cudaGetErrorString(e));
		return (int)e;
	}
	return 0;
}
