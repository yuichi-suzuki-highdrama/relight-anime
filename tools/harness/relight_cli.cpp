/*
 * relight_cli.cpp - プラグインの照明コード (RelightAE.cpp) を AE なしで実行する検証用ツール
 *
 * RelightAE.cpp をそのまま取り込み、CPU 版 (RenderFrameCPU) か GPU 版 (RelightGPU_Render、gpu=1) で描く。入出力は生の float32 RGBA。
 *   relight_cli src.f32 depth.f32 out.f32 W H key=value ...
 *   既定値はプリセット 0 (Anime Standard)。preset=N で別のプリセットから始める (0 始まり)。
 *   キー: preset, mode (1..5), look (1 Anime / 2 Cinematic)
 *         光源 1: lx ly (レイヤー px), lz, intensity, r g b (0..1)
 *         光源 2/3: l2 (0/1), l2x l2y l2z l2i l2r l2g l2b (l3 も同様)
 *         lrange / l2range / l3range (光の届く範囲の倍率)、lshow / l2show / l3show (光の玉、show は全部の光源)
 *         llook / l2look / l3look (光源ごとの当て方 0: Scene と同じ 1: Anime 2: Cinematic)
 *         ambient, relief, specular, shadow, occlusion,
 *         tint (影色の強さ), sr sg sb (影色 0..1), smooth (陰影のならし), dither (輪郭のディザ), cel, steps, soft, line, lineth, volume
 *         リム (頭に何も付けなければ全部の光源、l2 / l3 を付けるとその光源だけ。強さは rim、光源ごとは l2rimamt):
 *           rim, rimw (1080p での px), rimsoft, rimsmooth (輪郭のならし), rimplace (1 Auto / 2 Light Side / 3 All Around),
 *           rimr rimg rimb, rimspread, rimreach
 *         lrim / l2rim / l3rim (0/1、光源ごとにリムを付けるか)
 *         gpu=1 (GPU 版で描く)、form (立体的な陰影の割合)、spread (撮影寄りのグラデーションの広がり)
 *         nb=時刻の差:ファイル (前後のフレームの深度、深度を時間方向にならす。4 個まで)
 * src.f32 は sRGB の RGBA (非乗算、alpha 込み)、depth.f32 は 1 チャンネル (近い = 1)。
 */
#include "../../plugin/src/RelightAE.cpp"
#ifdef RELIGHT_HAS_CUDA
#include <cuda_runtime.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <string>
#include <chrono>

static bool readFile(const char *path, std::vector<float> &buf, size_t count)
{
	FILE *f = std::fopen(path, "rb");
	if (!f) return false;
	buf.resize(count);
	size_t n = std::fread(buf.data(), sizeof(float), count, f);
	std::fclose(f);
	return n == count;
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		std::fprintf(stderr, "usage: relight_cli src.f32 depth.f32 out.f32 W H [key=value ...]\n");
		return 2;
	}
	const A_long W = std::atol(argv[4]), H = std::atol(argv[5]);
	/* 深度は推定したときの大きさのまま受け取り (dw, dh)、プラグインと同じ処理で引き伸ばす */
	A_long dw = W, dh = H;
	bool snap = true;
	for (int i = 6; i < argc; ++i) {
		std::string a = argv[i];
		if (a.rfind("dw=", 0) == 0) dw = std::atol(a.c_str() + 3);
		else if (a.rfind("dh=", 0) == 0) dh = std::atol(a.c_str() + 3);
		else if (a.rfind("snap=", 0) == 0) snap = std::atoi(a.c_str() + 5) != 0;
	}
	std::vector<float> src, lowDepth, depth;
	if (!readFile(argv[1], src, (size_t)W * H * 4)) { std::fprintf(stderr, "src read failed\n"); return 1; }
	bool hasDepth = std::string(argv[2]) != "-";
	if (hasDepth && !readFile(argv[2], lowDepth, (size_t)dw * dh)) { std::fprintf(stderr, "depth read failed\n"); return 1; }
	const float layerW = (float)W, layerH = (float)H;

	/* プリセットを先に決める */
	int preset = 0;
	for (int i = 6; i < argc; ++i)
		if (std::string(argv[i]).rfind("preset=", 0) == 0) preset = std::atoi(argv[i] + 7);
	preset = std::max(0, std::min(preset, NUM_PRESETS - 1));

	RenderParams rp;
	std::memset(&rp, 0, sizeof(rp));
	PresetToRenderParams(PRESETS[preset], layerW, layerH, rp);
	rp.depthNear = 1; rp.normalSource = 1; rp.passEncoding = 1; rp.maskSource = 1;
	rp.outputMode = OUTPUT_RELIT; rp.mix = 1.f;

	/* 光源の位置はレイヤー px で受ける */
	float lx[3], ly[3];
	for (int k = 0; k < 3; ++k) {
		lx[k] = (rp.light[k].u - 0.5f) * layerH + layerW * 0.5f;
		ly[k] = rp.light[k].v * layerH;
	}

	for (int i = 6; i < argc; ++i) {
		std::string kv = argv[i];
		size_t eq = kv.find('=');
		if (eq == std::string::npos) continue;
		std::string k = kv.substr(0, eq);
		float v = (float)std::atof(kv.c_str() + eq + 1);
		int li = 0;
		std::string key = k;
		if (k.size() >= 2 && k[0] == 'l' && (k[1] == '2' || k[1] == '3')) { li = k[1] - '1'; key = k.substr(2); }
		LightParams &L = rp.light[li];
		/* リムの値: 頭に l2 / l3 が付けばその光源だけ、付かなければ全部の光源 (lrim / l2rim / l3rim はオンオフ) */
		auto rimSet = [&](const std::string &name, float val, bool all) -> bool {
			for (int q = 0; q < 3; ++q) {
				if (!all && q != li) continue;
				LightParams &R = rp.light[q];
				if (name == "rim" || name == "rimamt") R.rimAmount = val;
				else if (name == "rimw") R.rimWidth = val;
				else if (name == "rimsoft") R.rimSoft = val;
				else if (name == "rimsmooth") R.rimSmooth = val;
				else if (name == "rimspread") R.rimSpread = val;
				else if (name == "rimreach") R.rimReach = val;
				else if (name == "rimplace") R.rimPlacement = (int)val;
				else if (name == "rimr") R.rimColor[0] = val;
				else if (name == "rimg") R.rimColor[1] = val;
				else if (name == "rimb") R.rimColor[2] = val;
				else if (name == "show") R.show = val != 0.f;
				else return false;
			}
			return true;
		};
		if (li > 0 && key.empty()) L.on = v != 0.f;
		else if (k == "lrim" || (li > 0 && key == "rim")) L.rim = v != 0.f;
		else if (k == "lrange" || (li > 0 && key == "range")) L.range = v;
		else if (k == "lshow") L.show = v != 0.f;
		else if (k == "llook" || (li > 0 && key == "look")) L.look = (int)v;   /* 0: Scene と同じ  1: Anime  2: Cinematic */
		else if (li > 0 && key != "rim" && rimSet(key, v, false)) {}
		else if (li == 0 && rimSet(k, v, true)) {}
		else if (key == "lx" || (li > 0 && key == "x")) lx[li] = v;
		else if (key == "ly" || (li > 0 && key == "y")) ly[li] = v;
		else if (key == "lz" || (li > 0 && key == "z")) L.z = v;
		else if (key == "intensity" || (li > 0 && key == "i")) L.intensity = v;
		else if (key == "r") L.tint[0] = v;
		else if (key == "g") L.tint[1] = v;
		else if (key == "b") L.tint[2] = v;
		else if (k == "mode") rp.outputMode = (int)v;
		else if (k == "look") rp.look = (int)v;
		else if (k == "ambient") rp.exposure = v;
		else if (k == "relief") rp.relief = v;
		else if (k == "specular") rp.specular = v;
		else if (k == "shadow") rp.shadow = v;
		else if (k == "occlusion") rp.occlusion = v;
		else if (k == "tint") rp.shadowTint = v;
		else if (k == "sr") rp.shadowColor[0] = v;
		else if (k == "sg") rp.shadowColor[1] = v;
		else if (k == "sb") rp.shadowColor[2] = v;
		else if (k == "cel") rp.cel = v;
		else if (k == "steps") rp.celSteps = (int)v;
		else if (k == "soft") rp.celSoft = v;
		else if (k == "form") rp.form = v;
		else if (k == "spread") rp.lightSpread = v;
		else if (k == "smooth") rp.shadeSmooth = v;
		else if (k == "dither") rp.edgeDither = v;
		else if (k == "line") rp.lineProtect = v;
		else if (k == "lineth") rp.lineThreshold = v;
		else if (k == "volume") rp.volume = v;
	}
	for (int k = 0; k < 3; ++k) {
		rp.light[k].u = 0.5f + (lx[k] - layerW * 0.5f) / layerH;
		rp.light[k].v = ly[k] / layerH;
	}

	rp.depthSnap = snap;
	/* 前後のフレームの深度 (nb=時刻の差:ファイル、深度と同じ大きさの float)。深度を時間方向にならす */
	std::vector<std::vector<float>> nbData;
	std::vector<int> nbOffs;
	for (int i = 6; i < argc && nbData.size() < 4; ++i) {
		std::string a = argv[i];
		if (a.rfind("nb=", 0) != 0) continue;
		const size_t colon = a.find(':', 3);
		if (colon == std::string::npos) continue;
		std::vector<float> d;
		if (!readFile(a.substr(colon + 1).c_str(), d, (size_t)dw * dh)) { std::fprintf(stderr, "nb read failed\n"); return 1; }
		nbData.push_back(d);
		nbOffs.push_back(std::atoi(a.substr(3, colon - 3).c_str()));
	}
	const int nbN = (int)nbData.size();
	bool useGpu = false;
	for (int i = 6; i < argc; ++i)
		if (std::string(argv[i]) == "gpu=1") useGpu = true;

	/* AE と同じく premultiplied の画像にする (CPU は ARGB float、GPU は BGRA float) */
	const size_t NP = (size_t)W * H;
	std::vector<float> argb(NP * 4), bgra(NP * 4), depthBgra((size_t)dw * dh * 4);
	for (size_t n = 0; n < NP; ++n) {
		const float *s = &src[n * 4];
		const float a = s[3];
		argb[n * 4 + 0] = a; argb[n * 4 + 1] = s[0] * a; argb[n * 4 + 2] = s[1] * a; argb[n * 4 + 3] = s[2] * a;
		bgra[n * 4 + 0] = s[2] * a; bgra[n * 4 + 1] = s[1] * a; bgra[n * 4 + 2] = s[0] * a; bgra[n * 4 + 3] = a;
	}
	if (hasDepth)
		for (size_t n = 0; n < (size_t)dw * dh; ++n) {
			depthBgra[n * 4 + 0] = depthBgra[n * 4 + 1] = depthBgra[n * 4 + 2] = lowDepth[n];
			depthBgra[n * 4 + 3] = 1.f;
		}

	ShadeContext ctx;
	std::memset(&ctx, 0, sizeof(ctx));
	ctx.rp = rp;
	ctx.outW = W; ctx.outH = H;
	ctx.originX = 0.f; ctx.originY = 0.f;
	ctx.layerW = layerW; ctx.layerH = layerH;

	std::vector<Rgba> buf;
	auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
	if (!useGpu) {
		RLImage srcImg = {(const unsigned char *)argb.data(), (int)W, (int)H, (int)(W * 16), RL_FMT_ARGB32F};
		RLImage depthImg = {hasDepth ? (const unsigned char *)lowDepth.data() : NULL, (int)dw, (int)dh, (int)(dw * 4), RL_FMT_GRAY32F};
		const AuxMap depthMap = rlMakeAuxMap(depthImg, (float)dw, (float)dh, 0.f, 0.f, layerW, layerH);
		auto t0 = std::chrono::steady_clock::now();
		RLImage nbImg[4];
		for (int k = 0; k < nbN; ++k) nbImg[k] = {(const unsigned char *)nbData[k].data(), (int)dw, (int)dh, (int)(dw * 4), RL_FMT_GRAY32F};
		RenderFrameCPU(ctx, srcImg, depthMap, nbImg, nbOffs.data(), nbN, buf);
		auto t1 = std::chrono::steady_clock::now();
		std::fprintf(stderr, "%ldx%ld preset=%d lights=%d: CPU %.1f ms (%u スレッド)\n",
			(long)W, (long)H, preset, ctx.nL, ms(t0, t1), std::max(1u, std::min(std::thread::hardware_concurrency(), 32u)));
	} else {
#ifdef RELIGHT_HAS_CUDA
		/* AE の GPU 描画と同じく、GPU のメモリに置いた BGRA float を渡す */
		void *dSrc = NULL, *dDst = NULL, *dDepth = NULL;
		cudaMalloc(&dSrc, NP * 16);
		cudaMalloc(&dDst, NP * 16);
		cudaMemcpy(dSrc, bgra.data(), NP * 16, cudaMemcpyHostToDevice);
		if (hasDepth) {
			cudaMalloc(&dDepth, (size_t)dw * dh * 16);
			cudaMemcpy(dDepth, depthBgra.data(), (size_t)dw * dh * 16, cudaMemcpyHostToDevice);
		}
		RLGpuFrame f;
		std::memset(&f, 0, sizeof(f));
		f.ctx = ctx;
		f.src = {(const unsigned char *)dSrc, (int)W, (int)H, (int)(W * 16), RL_FMT_BGRA32F};
		RLImage depthImg = {(const unsigned char *)dDepth, (int)dw, (int)dh, (int)(dw * 16), RL_FMT_BGRA32F};
		f.depthMap = rlMakeAuxMap(depthImg, (float)dw, (float)dh, 0.f, 0.f, layerW, layerH);
		f.dst = dDst;
		f.dstRowbytes = (int)(W * 16);
		std::vector<void *> dNb;
		for (int k = 0; k < nbN; ++k) {
			std::vector<float> b((size_t)dw * dh * 4);
			for (size_t n = 0; n < (size_t)dw * dh; ++n) { b[n * 4 + 0] = b[n * 4 + 1] = b[n * 4 + 2] = nbData[k][n]; b[n * 4 + 3] = 1.f; }
			void *p = NULL;
			cudaMalloc(&p, b.size() * 4);
			cudaMemcpy(p, b.data(), b.size() * 4, cudaMemcpyHostToDevice);
			dNb.push_back(p);
			f.depthNb[k] = {(const unsigned char *)p, (int)dw, (int)dh, (int)(dw * 16), RL_FMT_BGRA32F};
			f.depthNbOff[k] = nbOffs[k];
		}
		f.depthNbCount = nbN;
		char msg[256] = {0};
		/* 1 回目は表の用意などを含むので、2 回目の時間を出す */
		RLGpuFrame f1 = f;
		if (RelightGPU_Render(f1, msg, sizeof(msg)) != 0) { std::fprintf(stderr, "GPU failed: %s\n", msg); return 1; }
		auto t0 = std::chrono::steady_clock::now();
		RLGpuFrame f2 = f;
		if (RelightGPU_Render(f2, msg, sizeof(msg)) != 0) { std::fprintf(stderr, "GPU failed: %s\n", msg); return 1; }
		auto t1 = std::chrono::steady_clock::now();
		std::vector<float> outBgra(NP * 4);
		cudaMemcpy(outBgra.data(), dDst, NP * 16, cudaMemcpyDeviceToHost);
		cudaFree(dSrc); cudaFree(dDst); if (dDepth) cudaFree(dDepth);
		for (void *p : dNb) cudaFree(p);
		RelightGPU_Release();
		buf.resize(NP);
		for (size_t n = 0; n < NP; ++n) {
			buf[n].b = outBgra[n * 4 + 0]; buf[n].g = outBgra[n * 4 + 1]; buf[n].r = outBgra[n * 4 + 2]; buf[n].a = outBgra[n * 4 + 3];
		}
		std::fprintf(stderr, "%ldx%ld preset=%d lights=%d: GPU %.2f ms\n", (long)W, (long)H, preset, f2.ctx.nL, ms(t0, t1));
#else
		std::fprintf(stderr, "GPU 版なしでビルドされています\n");
		return 1;
#endif
	}

	/* premultiplied → 非乗算の RGBA で書き出す */
	std::vector<float> out(NP * 4);
	for (size_t n = 0; n < NP; ++n) {
		const Rgba &o = buf[n];
		const float a = o.a > 1e-6f ? o.a : 1.f;
		out[n * 4 + 0] = o.r / a; out[n * 4 + 1] = o.g / a; out[n * 4 + 2] = o.b / a; out[n * 4 + 3] = o.a;
	}
	FILE *f = std::fopen(argv[3], "wb");
	if (!f) return 1;
	std::fwrite(out.data(), sizeof(float), out.size(), f);
	std::fclose(f);
	return 0;
}