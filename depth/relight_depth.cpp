/*
 * relight_depth.cpp - Relight Anime の深度推定プログラム (Python / ComfyUI 不要)
 *
 * Video Depth Anything (Small, Apache-2.0) の ONNX を ONNX Runtime で実行し、
 * 元の実装 (video_depth.py の infer_video_depth) と同じ手順で動画全体の深度を作る。
 *   - 32 フレームの窓を 22 フレームずつずらす。窓の先頭 10 枠には前の窓のキーフレームを入れる
 *   - 窓ごとの出力を、前の窓のキーフレーム 2 枚に最小二乗でスケール・シフト合わせする
 *   - 重なり 8 フレームは線形に混ぜる
 * 続いて、元の絵の色が変わっていない画素だけ前後のフレームと混ぜて、深度の小さな揺れを抑える (照明のちらつき対策)。
 * 最後にショット全体の 2%〜98% で 0..1 に正規化し、16bit で書き出す (近い = 1)。
 *
 * 使い方:
 *   relight_depth --frames <frames.bin> --out <depth.bin> --encoder <enc.onnx> --head <head.onnx> --ort <onnxruntime.dll>
 *                 [--png-dir <dir>] [--manifest <manifest.json>] [--temporal 0.9 (0 で無し)]
 *                 [--provider cuda|dml|cpu] [--dll-dir <dir>]... [--status <status.json>] [--chunk 4]
 *
 * frames.bin: 先頭に FramesHeader、続いて RGB8 のフレームを count 枚 (width x height x 3)
 * depth.bin : 先頭に DepthHeader、続いて uint16 のフレームを count 枚 (width x height)
 *
 * ONNX Runtime の DLL は実行時に読み込む (LoadLibrary + OrtGetApiBase)。
 * CUDA 版 (ComfyUI 同梱の 1.23 など) と、DirectML 版 (AE 同梱) のどちらでも動く。
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <wincodec.h>

#include "onnxruntime_c_api.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

/* ------------------------------------------------------------------ */
/* ファイル形式                                                         */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)
struct FramesHeader {
	char     magic[4];     /* "RLFR" */
	uint32_t version;      /* 1 */
	uint32_t width, height, count;
	uint32_t channels;     /* 3 (RGB8) */
	float    fps;
};
struct DepthHeader {
	char     magic[4];     /* "RLDP" */
	uint32_t version;      /* 1 */
	uint32_t width, height, count;
	float    rangeLo, rangeHi;   /* 正規化前の逆深度の 2% / 98% 点 */
	float    fps;
};
#pragma pack(pop)

/* 元の実装の定数 */
static const int INFER_LEN = 32;
static const int OVERLAP = 10;
static const int KEYFRAMES[OVERLAP] = {0, 12, 24, 25, 26, 27, 28, 29, 30, 31};
static const int INTERP_LEN = 8;
static const int ALIGN_LEN = OVERLAP - INTERP_LEN;   /* 2: キーフレーム 0 と 12 で合わせる */
static const int FRAME_STEP = INFER_LEN - OVERLAP;   /* 22 */

/* ------------------------------------------------------------------ */
/* ユーティリティ                                                       */
/* ------------------------------------------------------------------ */

static std::wstring widen(const std::string &s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
	std::wstring w(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

static std::string narrow(const std::wstring &w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
	std::string s(n, 0);
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
	return s;
}

static std::string jsonEscape(const std::string &s)
{
	std::string o;
	for (char c : s) {
		if (c == '"' || c == '\\') { o += '\\'; o += c; }
		else if (c == '\n') o += "\\n";
		else if (c == '\r') {}
		else if ((unsigned char)c < 0x20) o += ' ';
		else o += c;
	}
	return o;
}

static std::wstring g_statusPath;

/* プラグインが進捗を読むための状態ファイル (一時ファイルに書いてから置き換える) */
static void writeStatus(const char *state, double progress, const std::string &message)
{
	if (g_statusPath.empty()) return;
	std::wstring tmp = g_statusPath + L".tmp";
	FILE *f = _wfopen(tmp.c_str(), L"wb");
	if (!f) return;
	std::fprintf(f, "{\"state\": \"%s\", \"progress\": %.4f, \"message\": \"%s\", \"pid\": %lu}",
	             state, progress, jsonEscape(message).c_str(), GetCurrentProcessId());
	std::fclose(f);
	MoveFileExW(tmp.c_str(), g_statusPath.c_str(), MOVEFILE_REPLACE_EXISTING);
}

static void logf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stdout, fmt, ap);
	va_end(ap);
	std::fputc('\n', stdout);
	std::fflush(stdout);
}

[[noreturn]] static void fail(const std::string &msg)
{
	logf("エラー: %s", msg.c_str());
	writeStatus("error", 0.0, msg);
	std::exit(1);
}

/* ------------------------------------------------------------------ */
/* ONNX Runtime                                                         */
/* ------------------------------------------------------------------ */

static const OrtApi *g_ort = NULL;

static void check(OrtStatus *st, const char *what)
{
	if (!st) return;
	std::string msg = std::string(what) + ": " + g_ort->GetErrorMessage(st);
	g_ort->ReleaseStatus(st);
	fail(msg);
}

typedef OrtStatus *(ORT_API_CALL *AppendDmlFn)(OrtSessionOptions *, int);

/*
 * モデルはエンコーダ (DINOv2、フレームごとに独立) とヘッド (時間方向、32 フレームまとめて) の 2 つ。
 * 32 フレームを一度にエンコーダへ通すと注意機構の中間データで VRAM 32GB を使い切り、
 * メインメモリに溢れて遅くなるため、エンコーダは数フレームずつ通す。
 */
struct Model {
	OrtEnv            *env = NULL;
	OrtSession        *encoder = NULL;
	OrtSession        *head = NULL;
	OrtMemoryInfo     *cpuMem = NULL;
	std::string        provider;
};

static std::wstring g_profilePrefix;
static std::vector<std::pair<std::string, std::string>> g_cudaOpts;

static Model loadModel(const std::wstring &ortDll, const std::wstring &encPath, const std::wstring &headPath,
                       const std::string &provider)
{
	HMODULE h = LoadLibraryExW(ortDll.c_str(), NULL,
	                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!h) fail("ONNX Runtime を読み込めません: " + narrow(ortDll) + " (Win32 error " + std::to_string(GetLastError()) + ")");
	typedef const OrtApiBase *(ORT_API_CALL *GetApiBaseFn)(void);
	GetApiBaseFn getBase = (GetApiBaseFn)GetProcAddress(h, "OrtGetApiBase");
	if (!getBase) fail("OrtGetApiBase が見つかりません");
	const OrtApiBase *base = getBase();
	/* 構造体は追記のみなので、DLL が対応する範囲で最新の版を取る (AE 2025 同梱の 1.22 は 22 まで) */
	for (int v = ORT_API_VERSION; v >= 17 && !g_ort; --v) g_ort = base->GetApi(v);
	if (!g_ort) fail("この ONNX Runtime は古すぎます");
	logf("ONNX Runtime %s (%s)", base->GetVersionString(), narrow(ortDll).c_str());

	Model m;
	m.provider = provider;
	check(g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "relight_depth", &m.env), "CreateEnv");
	OrtSessionOptions *so = NULL;
	check(g_ort->CreateSessionOptions(&so), "CreateSessionOptions");
	check(g_ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), "SetSessionGraphOptimizationLevel");
	if (!g_profilePrefix.empty()) check(g_ort->EnableProfiling(so, g_profilePrefix.c_str()), "EnableProfiling");

	if (provider == "cuda") {
		OrtCUDAProviderOptionsV2 *cuda = NULL;
		check(g_ort->CreateCUDAProviderOptions(&cuda), "CreateCUDAProviderOptions");
		/* 既定値。--cuda-opt key=value で上書きできる */
		std::vector<std::pair<std::string, std::string>> opts = {
			{"device_id", "0"}, {"cudnn_conv_algo_search", "EXHAUSTIVE"}, {"do_copy_in_default_stream", "1"},
			{"use_tf32", "1"},
			/* 作業領域を最大にすると 1 窓 0.5 秒と 3.3 秒の間でばらついた。切ると安定して 0.5 秒 */
			{"cudnn_conv_use_max_workspace", "0"},
			/* 既定の kNextPowerOfTwo は 2 窓目以降で VRAM を溢れさせ、1 窓 24 秒以上かかった。必要量だけ確保すると 0.5 秒 */
			{"arena_extend_strategy", "kSameAsRequested"},
		};
		for (auto &kv : g_cudaOpts) {
			bool found = false;
			for (auto &o : opts) if (o.first == kv.first) { o.second = kv.second; found = true; }
			if (!found) opts.push_back(kv);
		}
		std::vector<const char *> keys, vals;
		std::string desc;
		for (auto &o : opts) { keys.push_back(o.first.c_str()); vals.push_back(o.second.c_str()); desc += o.first + "=" + o.second + " "; }
		logf("CUDA 設定: %s", desc.c_str());
		check(g_ort->UpdateCUDAProviderOptions(cuda, keys.data(), vals.data(), keys.size()), "UpdateCUDAProviderOptions");
		check(g_ort->SessionOptionsAppendExecutionProvider_CUDA_V2(so, cuda), "CUDA を有効にできません");
		g_ort->ReleaseCUDAProviderOptions(cuda);
	} else if (provider == "dml") {
		AppendDmlFn dml = (AppendDmlFn)GetProcAddress(h, "OrtSessionOptionsAppendExecutionProvider_DML");
		if (!dml) fail("この ONNX Runtime は DirectML に対応していません");
		check(g_ort->DisableMemPattern(so), "DisableMemPattern");
		check(g_ort->SetSessionExecutionMode(so, ORT_SEQUENTIAL), "SetSessionExecutionMode");
		check(dml(so, 0), "DirectML を有効にできません");
	} else if (provider != "cpu") {
		fail("--provider は cuda / dml / cpu のいずれか");
	}

	auto t0 = std::chrono::steady_clock::now();
	check(g_ort->CreateSession(m.env, encPath.c_str(), so, &m.encoder), "エンコーダを読み込めません");
	check(g_ort->CreateSession(m.env, headPath.c_str(), so, &m.head), "ヘッドを読み込めません");
	g_ort->ReleaseSessionOptions(so);
	check(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m.cpuMem), "CreateCpuMemoryInfo");
	logf("モデル読み込み %.1f s (%s)",
	     std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), provider.c_str());
	return m;
}

static const int FEAT_C = 384;   /* vits の特徴の次元 */

/* 1 窓分の特徴: 4 層 x [32, 384, ph, pw] */
struct WindowFeatures {
	int ph = 0, pw = 0;
	std::vector<float> f[4];
	size_t perFrame() const { return (size_t)FEAT_C * ph * pw; }
	void resize(int h, int w)
	{
		ph = h; pw = w;
		for (auto &v : f) v.assign((size_t)INFER_LEN * perFrame(), 0.f);
	}
	void copySlot(int dst, const WindowFeatures &src, int srcSlot)
	{
		for (int k = 0; k < 4; ++k)
			std::memcpy(&f[k][(size_t)dst * perFrame()], &src.f[k][(size_t)srcSlot * perFrame()], perFrame() * sizeof(float));
	}
};

/* エンコーダ: 入力 [n, 3, H, W] → 特徴を slot0 から n 枠分に書き込む */
static void runEncoder(Model &m, float *input, int n, int H, int W, WindowFeatures &wf, int slot0)
{
	const int64_t inShape[4] = {n, 3, H, W};
	OrtValue *in = NULL;
	check(g_ort->CreateTensorWithDataAsOrtValue(m.cpuMem, input, (size_t)n * 3 * H * W * sizeof(float),
	                                            inShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in), "入力テンサー");
	const char *inNames[] = {"frames"};
	const char *outNames[] = {"f1", "f2", "f3", "f4"};
	OrtValue *out[4] = {NULL, NULL, NULL, NULL};
	check(g_ort->Run(m.encoder, NULL, inNames, &in, 1, outNames, 4, out), "エンコーダの推論");
	for (int k = 0; k < 4; ++k) {
		float *data = NULL;
		check(g_ort->GetTensorMutableData(out[k], (void **)&data), "エンコーダの出力");
		std::memcpy(&wf.f[k][(size_t)slot0 * wf.perFrame()], data, (size_t)n * wf.perFrame() * sizeof(float));
		g_ort->ReleaseValue(out[k]);
	}
	g_ort->ReleaseValue(in);
}

/* ヘッド: 4 層の特徴 [32, 384, ph, pw] → 深度 [1, 32, 14ph, 14pw] */
static void runHead(Model &m, WindowFeatures &wf, std::vector<float> &output)
{
	const int64_t shape[4] = {INFER_LEN, FEAT_C, wf.ph, wf.pw};
	OrtValue *in[4] = {NULL, NULL, NULL, NULL};
	for (int k = 0; k < 4; ++k)
		check(g_ort->CreateTensorWithDataAsOrtValue(m.cpuMem, wf.f[k].data(), wf.f[k].size() * sizeof(float),
		                                            shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[k]), "特徴テンサー");
	const char *inNames[] = {"f1", "f2", "f3", "f4"};
	const char *outNames[] = {"depth"};
	OrtValue *out = NULL;
	check(g_ort->Run(m.head, NULL, inNames, in, 4, outNames, 1, &out), "ヘッドの推論");
	float *data = NULL;
	check(g_ort->GetTensorMutableData(out, (void **)&data), "ヘッドの出力");
	output.assign(data, data + (size_t)INFER_LEN * (14 * wf.ph) * (14 * wf.pw));
	g_ort->ReleaseValue(out);
	for (auto *v : in) g_ort->ReleaseValue(v);
}

/* ------------------------------------------------------------------ */
/* 前処理                                                               */
/* ------------------------------------------------------------------ */

/* 元の実装の入力サイズ: 短辺 518 以上、14 の倍数 (lower_bound)。16:9 より細長いときは縮める */
static void modelSize(int fw, int fh, int &mw, int &mh)
{
	int inputSize = 518;
	double ratio = (double)std::max(fw, fh) / (double)std::min(fw, fh);
	if (ratio > 1.78) {
		inputSize = (int)(inputSize * 1.777 / ratio);
		inputSize = (int)std::lround(inputSize / 14.0) * 14;
	}
	double sh = (double)inputSize / fh, sw = (double)inputSize / fw;
	double s = std::max(sh, sw);
	auto constrain = [](double x, int minVal) {
		int y = (int)std::lround(x / 14.0) * 14;
		if (y < minVal) y = (int)std::ceil(x / 14.0) * 14;
		return y;
	};
	mh = constrain(s * fh, inputSize);
	mw = constrain(s * fw, inputSize);
}

/* cv2.INTER_CUBIC 相当 (a = -0.75) の 1 次元の重み */
static inline float cubicW(float x)
{
	const float A = -0.75f;
	x = std::fabs(x);
	if (x <= 1.f) return ((A + 2.f) * x - (A + 3.f)) * x * x + 1.f;
	if (x < 2.f) return ((A * x - 5.f * A) * x + 8.f * A) * x - 4.f * A;
	return 0.f;
}

/* RGB8 → 正規化済み CHW float (ImageNet の平均・分散)、バイキュービックで (mw, mh) へ */
static void preprocess(const uint8_t *rgb, int fw, int fh, int mw, int mh, float *chw)
{
	static const float mean[3] = {0.485f, 0.456f, 0.406f};
	static const float stdv[3] = {0.229f, 0.224f, 0.225f};
	/* 横方向 → 縦方向の分離フィルタ */
	std::vector<float> tmp((size_t)fh * mw * 3);
	const float sx = (float)fw / mw, sy = (float)fh / mh;
	for (int x = 0; x < mw; ++x) {
		float src = (x + 0.5f) * sx - 0.5f;
		int x0 = (int)std::floor(src);
		float t = src - x0;
		float w[4] = {cubicW(1 + t), cubicW(t), cubicW(1 - t), cubicW(2 - t)};
		for (int y = 0; y < fh; ++y) {
			float acc[3] = {0, 0, 0};
			for (int k = 0; k < 4; ++k) {
				int xx = std::min(std::max(x0 - 1 + k, 0), fw - 1);
				const uint8_t *p = rgb + ((size_t)y * fw + xx) * 3;
				acc[0] += w[k] * p[0]; acc[1] += w[k] * p[1]; acc[2] += w[k] * p[2];
			}
			float *d = &tmp[((size_t)y * mw + x) * 3];
			d[0] = acc[0]; d[1] = acc[1]; d[2] = acc[2];
		}
	}
	const size_t plane = (size_t)mw * mh;
	for (int y = 0; y < mh; ++y) {
		float src = (y + 0.5f) * sy - 0.5f;
		int y0 = (int)std::floor(src);
		float t = src - y0;
		float w[4] = {cubicW(1 + t), cubicW(t), cubicW(1 - t), cubicW(2 - t)};
		for (int x = 0; x < mw; ++x) {
			float acc[3] = {0, 0, 0};
			for (int k = 0; k < 4; ++k) {
				int yy = std::min(std::max(y0 - 1 + k, 0), fh - 1);
				const float *p = &tmp[((size_t)yy * mw + x) * 3];
				acc[0] += w[k] * p[0]; acc[1] += w[k] * p[1]; acc[2] += w[k] * p[2];
			}
			for (int c = 0; c < 3; ++c) {
				float v = std::min(std::max(acc[c], 0.f), 255.f) / 255.f;
				chw[c * plane + (size_t)y * mw + x] = (v - mean[c]) / stdv[c];
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* フレームの読み込み                                                   */
/* ------------------------------------------------------------------ */

struct FrameReader {
	FILE        *f = NULL;
	FramesHeader h;
	std::vector<uint8_t> buf;

	void open(const std::wstring &path)
	{
		f = _wfopen(path.c_str(), L"rb");
		if (!f) fail("フレームを開けません: " + narrow(path));
		if (std::fread(&h, sizeof(h), 1, f) != 1 || std::memcmp(h.magic, "RLFR", 4) != 0 || h.channels != 3)
			fail("フレームの形式が違います: " + narrow(path));
		buf.resize((size_t)h.width * h.height * 3);
	}
	const uint8_t *frame(uint32_t i)
	{
		i = std::min(i, h.count - 1);   /* 末尾は最後のフレームを繰り返す (元の実装と同じ) */
		_fseeki64(f, (long long)sizeof(h) + (long long)i * (long long)buf.size(), SEEK_SET);
		if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) fail("フレームの読み込みに失敗しました");
		return buf.data();
	}
};

/* ------------------------------------------------------------------ */
/* 窓の位置合わせ (元の実装 utils/util.py)                               */
/* ------------------------------------------------------------------ */

static void scaleAndShift(const std::vector<const float *> &pred, const std::vector<const float *> &target,
                          size_t n, double &scale, double &shift)
{
	double a00 = 0, a01 = 0, a11 = 0, b0 = 0, b1 = 0;
	for (size_t k = 0; k < pred.size(); ++k)
		for (size_t i = 0; i < n; ++i) {
			double p = pred[k][i], t = target[k][i];
			a00 += p * p; a01 += p; a11 += 1.0; b0 += p * t; b1 += t;
		}
	scale = 1.0; shift = 0.0;
	double det = a00 * a11 - a01 * a01;
	if (det != 0.0) {
		scale = (a11 * b0 - a01 * b1) / det;
		shift = (-a01 * b0 + a00 * b1) / det;
	}
}

static void applyScaleShift(float *d, size_t n, double scale, double shift)
{
	for (size_t i = 0; i < n; ++i) {
		float v = (float)(d[i] * scale + shift);
		d[i] = v < 0.f ? 0.f : v;
	}
}

/* ------------------------------------------------------------------ */
/* 書き出し (確定したフレームを順に一時ファイルへ。最後に 16bit へ変換)   */
/* ------------------------------------------------------------------ */

struct RawWriter {
	FILE *f = NULL;
	size_t frameSize = 0;
	uint32_t written = 0;
	std::vector<float> samples;   /* 正規化範囲を求めるための間引きサンプル */

	void open(const std::wstring &path, size_t n)
	{
		f = _wfopen(path.c_str(), L"wb+");
		if (!f) fail("一時ファイルを作れません: " + narrow(path));
		frameSize = n;
	}
	void push(const std::vector<float> &d)
	{
		std::fwrite(d.data(), sizeof(float), frameSize, f);
		for (size_t i = (written * 7) % 16; i < frameSize; i += 16) samples.push_back(d[i]);
		++written;
	}
};

/* ------------------------------------------------------------------ */
/* 時間方向のならし (元の絵の色が変わっていない所だけ、前後のフレームと混ぜる) */
/* ------------------------------------------------------------------ */

/* 元のフレーム (fw x fh の RGB) を深度の大きさ (mw x mh) に縮める (面積の平均) */
static void downscaleRGB(const uint8_t *src, int fw, int fh, int mw, int mh, std::vector<uint8_t> &dst)
{
	dst.resize((size_t)mw * mh * 3);
	for (int y = 0; y < mh; ++y) {
		const int y0 = (int)((long long)y * fh / mh), y1 = std::max(y0 + 1, (int)((long long)(y + 1) * fh / mh));
		for (int x = 0; x < mw; ++x) {
			const int x0 = (int)((long long)x * fw / mw), x1 = std::max(x0 + 1, (int)((long long)(x + 1) * fw / mw));
			unsigned acc[3] = {0, 0, 0}, n = 0;
			for (int yy = y0; yy < y1 && yy < fh; ++yy)
				for (int xx = x0; xx < x1 && xx < fw; ++xx) {
					const uint8_t *p = src + ((size_t)yy * fw + xx) * 3;
					acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; ++n;
				}
			for (int c = 0; c < 3; ++c) dst[((size_t)y * mw + x) * 3 + c] = (uint8_t)(n ? acc[c] / n : 0);
		}
	}
}

/* 2 枚の縮めたフレームの、画素ごとの「同じ所が写っている」確からしさ (0..1)。
   色の差が 0.06 (255 段の約 15) で 0。少しぼかして、1 画素だけの判定のばらつきを抑える */
static void colorConfidence(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, int w, int h, std::vector<float> &conf)
{
	std::vector<float> raw((size_t)w * h);
	for (size_t i = 0; i < raw.size(); ++i) {
		int d = 0;
		for (int c = 0; c < 3; ++c) d = std::max(d, std::abs((int)a[i * 3 + c] - (int)b[i * 3 + c]));
		raw[i] = std::max(0.f, 1.f - (float)d / (255.f * 0.06f));
	}
	conf.resize(raw.size());
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			float s = 0.f;
			int n = 0;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx) {
					const int xx = x + dx, yy = y + dy;
					if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
					s += raw[(size_t)yy * w + xx];
					++n;
				}
			conf[(size_t)y * w + x] = s / (float)n;
		}
}

/* 深度の推定は、キャラの動きとは関係なく少しずつ揺れる (Video Depth Anything でも残る)。
   その揺れを照明が明るさの揺れに変え、ろうそくの炎のように見える (とくにライトが人物の後ろで低いとき)。
   元の絵の色が変わっていない画素だけ、前のフレームのならした深度を keep の割合で残す (前向き)。
   後ろ向きにも同じことをして平均する。動いている所は色が変わるので混ぜない (残像にならない)。
   raw (一時ファイル、mw x mh の float が N 枚) をその場で書き換える */
static void temporalSmooth(RawWriter &raw, FrameReader &reader, int fw, int fh, int mw, int mh, float keep, const std::wstring &tmpPath)
{
	const size_t plane = (size_t)mw * mh;
	const uint32_t N = raw.written;
	if (N < 2 || keep <= 0.f) return;
	FILE *fwd = _wfopen(tmpPath.c_str(), L"wb+");
	if (!fwd) fail("一時ファイルを作れません: " + narrow(tmpPath));
	std::vector<float> d(plane), acc(plane), conf;
	std::vector<uint8_t> g, gPrev;
	/* 前向き */
	for (uint32_t i = 0; i < N; ++i) {
		_fseeki64(raw.f, (long long)i * plane * sizeof(float), SEEK_SET);
		if (std::fread(d.data(), sizeof(float), plane, raw.f) != plane) fail("一時ファイルの読み込みに失敗しました");
		downscaleRGB(reader.frame(i), fw, fh, mw, mh, g);
		if (i == 0) {
			acc = d;
		} else {
			colorConfidence(g, gPrev, mw, mh, conf);
			for (size_t p = 0; p < plane; ++p) {
				const float w = keep * conf[p];
				acc[p] = acc[p] * w + d[p] * (1.f - w);
			}
		}
		std::fwrite(acc.data(), sizeof(float), plane, fwd);
		std::swap(g, gPrev);
		if ((i & 15) == 0) writeStatus("running", 0.95 + 0.01 * i / N, "時間方向にならし中");
	}
	/* 後ろ向き (前向きの結果と平均して、raw へ書き戻す) */
	std::vector<float> f(plane);
	for (uint32_t k = 0; k < N; ++k) {
		const uint32_t i = N - 1 - k;
		_fseeki64(raw.f, (long long)i * plane * sizeof(float), SEEK_SET);
		if (std::fread(d.data(), sizeof(float), plane, raw.f) != plane) fail("一時ファイルの読み込みに失敗しました");
		downscaleRGB(reader.frame(i), fw, fh, mw, mh, g);
		if (k == 0) {
			acc = d;
		} else {
			colorConfidence(g, gPrev, mw, mh, conf);
			for (size_t p = 0; p < plane; ++p) {
				const float w = keep * conf[p];
				acc[p] = acc[p] * w + d[p] * (1.f - w);
			}
		}
		_fseeki64(fwd, (long long)i * plane * sizeof(float), SEEK_SET);
		if (std::fread(f.data(), sizeof(float), plane, fwd) != plane) fail("一時ファイルの読み込みに失敗しました");
		for (size_t p = 0; p < plane; ++p) f[p] = 0.5f * (f[p] + acc[p]);
		_fseeki64(raw.f, (long long)i * plane * sizeof(float), SEEK_SET);
		std::fwrite(f.data(), sizeof(float), plane, raw.f);
		std::swap(g, gPrev);
		if ((k & 15) == 0) writeStatus("running", 0.96 + 0.01 * k / N, "時間方向にならし中");
	}
	std::fflush(raw.f);
	std::fclose(fwd);
	_wremove(tmpPath.c_str());
}

/* ------------------------------------------------------------------ */
/* 16bit グレーの PNG (WIC、Windows 標準)                                */
/* ------------------------------------------------------------------ */

static bool writePng16(const std::wstring &path, const uint16_t *data, int w, int h)
{
	static IWICImagingFactory *fac = NULL;
	if (!fac) {
		CoInitializeEx(NULL, COINIT_MULTITHREADED);
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) return false;
	}
	IWICStream *stream = NULL;
	IWICBitmapEncoder *enc = NULL;
	IWICBitmapFrameEncode *frame = NULL;
	IPropertyBag2 *props = NULL;
	bool ok = false;
	do {
		if (FAILED(fac->CreateStream(&stream))) break;
		if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) break;
		if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc))) break;
		if (FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache))) break;
		if (FAILED(enc->CreateNewFrame(&frame, &props))) break;
		if (FAILED(frame->Initialize(props))) break;
		if (FAILED(frame->SetSize(w, h))) break;
		WICPixelFormatGUID fmt = GUID_WICPixelFormat16bppGray;
		if (FAILED(frame->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat16bppGray) break;
		if (FAILED(frame->WritePixels(h, w * 2, (UINT)(w * h * 2), (BYTE *)data))) break;
		if (FAILED(frame->Commit()) || FAILED(enc->Commit())) break;
		ok = true;
	} while (0);
	if (props) props->Release();
	if (frame) frame->Release();
	if (enc) enc->Release();
	if (stream) stream->Release();
	return ok;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
	SetConsoleOutputCP(CP_UTF8);
	std::wstring framesPath, outPath, encPath, headPath, ortDll, pngDir, manifestPath;
	std::string provider = "cuda";
	std::vector<std::wstring> dllDirs;
	float temporalKeep = 0.9f;   /* 時間方向のならし (0 で無し)。前のコマの深度を残す割合 */
	int chunk = 4;   /* エンコーダに一度に通すフレーム数。4 で VRAM ピーク約 11GB (8 で 15GB、16 で 21GB)。速度は同じ */
	for (int i = 1; i < argc; ++i) {
		std::wstring a = argv[i];
		auto next = [&]() -> std::wstring { if (i + 1 >= argc) fail("引数が足りません: " + narrow(a)); return argv[++i]; };
		if (a == L"--frames") framesPath = next();
		else if (a == L"--out") outPath = next();
		else if (a == L"--png-dir") pngDir = next();
		else if (a == L"--manifest") manifestPath = next();
		else if (a == L"--encoder") encPath = next();
		else if (a == L"--head") headPath = next();
		else if (a == L"--temporal") temporalKeep = std::max(0.f, std::min(0.98f, (float)_wtof(next().c_str())));
		else if (a == L"--chunk")chunk = std::max(1, std::min(INFER_LEN, _wtoi(next().c_str())));
		else if (a == L"--provider") provider = narrow(next());
		else if (a == L"--ort") ortDll = next();
		else if (a == L"--dll-dir") dllDirs.push_back(next());
		else if (a == L"--status") g_statusPath = next();
		else if (a == L"--profile") g_profilePrefix = next();
		else if (a == L"--cuda-opt") {
			std::string kv = narrow(next());
			size_t eq = kv.find('=');
			if (eq == std::string::npos) fail("--cuda-opt は key=value");
			g_cudaOpts.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
		}
		else fail("不明な引数: " + narrow(a));
	}
	if (framesPath.empty() || (outPath.empty() && pngDir.empty()) || encPath.empty() || headPath.empty() || ortDll.empty())
		fail("--frames --encoder --head --ort と、--out か --png-dir が必要です");

	/* CUDA / cuDNN の DLL を探す場所 */
	SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
	for (auto &d : dllDirs) AddDllDirectory(d.c_str());

	writeStatus("running", 0.0, "モデルを読み込み中");
	auto tStart = std::chrono::steady_clock::now();
	Model model = loadModel(ortDll, encPath, headPath, provider);

	FrameReader reader;
	reader.open(framesPath);
	const int fw = (int)reader.h.width, fh = (int)reader.h.height, N = (int)reader.h.count;
	int mw, mh;
	modelSize(fw, fh, mw, mh);
	const size_t plane = (size_t)mw * mh;
	logf("フレーム %d 枚 (%dx%d) → モデル入力 %dx%d", N, fw, fh, mw, mh);

	/* 窓の数 (元の実装: 末尾を最後のフレームで埋めて 22 刻み) */
	const int windows = (N + FRAME_STEP - 1) / FRAME_STEP;

	if (!pngDir.empty()) CreateDirectoryW(pngDir.c_str(), NULL);
	RawWriter raw;
	std::wstring rawPath = (!outPath.empty() ? outPath : pngDir + L"\\depth") + L".raw.tmp";
	raw.open(rawPath, plane);

	std::vector<float> chunkInput((size_t)chunk * 3 * plane), output;
	WindowFeatures cur, prev;
	cur.resize(mh / 14, mw / 14);
	prev.resize(mh / 14, mw / 14);
	std::deque<std::vector<float>> aligned;   /* まだ確定していない末尾のフレーム (最大 INTERP_LEN 枚) */
	std::vector<std::vector<float>> refAlign; /* 次の窓の合わせ込みに使う参照 (キーフレーム 0 と 12) */
	double inferSec = 0.0;

	auto emitFinal = [&](std::vector<float> &&d) {
		aligned.push_back(std::move(d));
		while ((int)aligned.size() > INTERP_LEN) {
			if ((int)raw.written < N) raw.push(aligned.front());
			aligned.pop_front();
		}
	};

	for (int w = 0; w < windows; ++w) {
		const int start = w * FRAME_STEP;
		/* 2 つ目以降の窓は、先頭 10 枠に前の窓のキーフレームが入る (元の実装)。
		   エンコーダは 1 フレームずつ独立なので、その 10 枠は前の窓の特徴をそのまま使い回す */
		const int firstNew = (w > 0) ? OVERLAP : 0;
		if (w > 0)
			for (int i = 0; i < OVERLAP; ++i) cur.copySlot(i, prev, KEYFRAMES[i]);
		auto t0 = std::chrono::steady_clock::now();
		for (int i = firstNew; i < INFER_LEN; i += chunk) {
			int n = std::min(chunk, INFER_LEN - i);
			for (int k = 0; k < n; ++k)
				preprocess(reader.frame((uint32_t)(start + i + k)), fw, fh, mw, mh, &chunkInput[(size_t)k * 3 * plane]);
			auto te = std::chrono::steady_clock::now();
			runEncoder(model, chunkInput.data(), n, mh, mw, cur, i);
			inferSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - te).count();
		}
		auto th = std::chrono::steady_clock::now();
		runHead(model, cur, output);
		inferSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - th).count();
		std::swap(prev, cur);
		(void)t0;

		auto outFrame = [&](int i) { return std::vector<float>(output.begin() + (size_t)i * plane, output.begin() + (size_t)(i + 1) * plane); };

		if (w == 0) {
			for (int i = 0; i < INFER_LEN; ++i) emitFinal(outFrame(i));
			refAlign.clear();
			for (int k = 0; k < ALIGN_LEN; ++k) refAlign.push_back(outFrame(KEYFRAMES[k]));
		} else {
			std::vector<const float *> cur, ref;
			std::vector<std::vector<float>> curFrames;
			for (int k = 0; k < ALIGN_LEN; ++k) curFrames.push_back(outFrame(k));
			for (int k = 0; k < ALIGN_LEN; ++k) { cur.push_back(curFrames[k].data()); ref.push_back(refAlign[k].data()); }
			double scale, shift;
			scaleAndShift(cur, ref, plane, scale, shift);

			/* 重なり 8 フレーム: 前の窓の末尾と、この窓の 2..9 を線形に混ぜる */
			for (int i = 0; i < INTERP_LEN; ++i) {
				std::vector<float> post = outFrame(ALIGN_LEN + i);
				applyScaleShift(post.data(), plane, scale, shift);
				float wpost = (float)i / (float)(INTERP_LEN - 1);
				std::vector<float> &pre = aligned[aligned.size() - INTERP_LEN + i];
				for (size_t p = 0; p < plane; ++p) pre[p] = pre[p] * (1.f - wpost) + post[p] * wpost;
			}
			for (int i = OVERLAP; i < INFER_LEN; ++i) {
				std::vector<float> d = outFrame(i);
				applyScaleShift(d.data(), plane, scale, shift);
				emitFinal(std::move(d));
			}
			/* 参照: 先頭のキーフレームは据え置き、2 枚目をこの窓のキーフレーム 12 に */
			std::vector<float> kf = outFrame(KEYFRAMES[1]);
			applyScaleShift(kf.data(), plane, scale, shift);
			refAlign.resize(1);
			refAlign.push_back(std::move(kf));
		}
		double prog = std::min(1.0, (double)(start + FRAME_STEP) / N);
		writeStatus("running", prog * 0.95, "深度を推定中");
		logf("窓 %d/%d 完了 (累計推論 %.2f s)", w + 1, windows, inferSec);
	}
	while (!aligned.empty() && (int)raw.written < N) { raw.push(aligned.front()); aligned.pop_front(); }

	/* 時間方向のならし (照明のちらつき対策)。正規化の範囲はならす前のサンプルのまま (ほとんど変わらない) */
	if (temporalKeep > 0.f) {
		writeStatus("running", 0.95, "時間方向にならし中");
		temporalSmooth(raw, reader, fw, fh, mw, mh, temporalKeep, rawPath + L".fwd");
		logf("時間方向のならし (keep %.2f) 完了", temporalKeep);
	}

	/* 2%〜98% で正規化して 16bit に */
	writeStatus("running", 0.97, "書き出し中");
	std::vector<float> s = raw.samples;
	size_t loI = (size_t)(s.size() * 0.02), hiI = std::min(s.size() - 1, (size_t)(s.size() * 0.98));
	std::nth_element(s.begin(), s.begin() + loI, s.end());
	float lo = s[loI];
	std::nth_element(s.begin(), s.begin() + hiI, s.end());
	float hi = std::max(s[hiI], lo + 1e-6f);

	FILE *out = NULL;
	std::wstring tmpOut = outPath + L".tmp";
	if (!outPath.empty()) {
		out = _wfopen(tmpOut.c_str(), L"wb");
		if (!out) fail("書き出せません: " + narrow(tmpOut));
		DepthHeader dh;
		std::memcpy(dh.magic, "RLDP", 4);
		dh.version = 1; dh.width = (uint32_t)mw; dh.height = (uint32_t)mh; dh.count = raw.written;
		dh.rangeLo = lo; dh.rangeHi = hi; dh.fps = reader.h.fps;
		std::fwrite(&dh, sizeof(dh), 1, out);
	}
	if (!pngDir.empty()) CreateDirectoryW(pngDir.c_str(), NULL);
	std::fseek(raw.f, 0, SEEK_SET);
	std::vector<float> fbuf(plane);
	std::vector<uint16_t> u16(plane);
	for (uint32_t i = 0; i < raw.written; ++i) {
		if (std::fread(fbuf.data(), sizeof(float), plane, raw.f) != plane) fail("一時ファイルの読み込みに失敗しました");
		for (size_t p = 0; p < plane; ++p) {
			float v = (fbuf[p] - lo) / (hi - lo);
			v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
			u16[p] = (uint16_t)(v * 65535.f + 0.5f);
		}
		if (out) std::fwrite(u16.data(), sizeof(uint16_t), plane, out);
		if (!pngDir.empty()) {
			wchar_t name[64];
			swprintf(name, 64, L"\\depth.%05u.png", i + 1);
			if (!writePng16(pngDir + name, u16.data(), mw, mh)) fail("PNG を書き出せません: " + narrow(pngDir + name));
		}
		if ((i & 15) == 0) writeStatus("running", 0.97 + 0.03 * i / raw.written, "書き出し中");
	}
	std::fclose(raw.f);
	_wremove(rawPath.c_str());
	if (out) {
		std::fclose(out);
		if (!MoveFileExW(tmpOut.c_str(), outPath.c_str(), MOVEFILE_REPLACE_EXISTING)) fail("出力を置き換えられません");
	}
	if (!manifestPath.empty()) {
		/* プラグインが読み込みに使う記録 (gen_passes.py と同じ形) */
		FILE *mf = _wfopen(manifestPath.c_str(), L"wb");
		if (mf) {
			std::fprintf(mf,
				"{\n  \"generator\": \"relight_depth\",\n  \"format\": \"png16\",\n  \"fps\": %.6f,\n"
				"  \"frames\": {\"depth\": %u},\n"
				"  \"depth\": {\"model\": \"video_depth_anything_vits (onnx)\", \"near\": \"white\", "
				"\"normalization\": \"shot-global 2-98 percentile\", \"temporal_keep\": %.2f, \"range_applied\": true, "
				"\"range_source\": [%.6f, %.6f], \"size\": [%d, %d], \"file\": \"depth/depth.%%05d.png\"},\n"
				"  \"normal\": null,\n  \"provider\": \"%s\"\n}\n",
				reader.h.fps, raw.written, temporalKeep, lo, hi, mw, mh, provider.c_str());
			std::fclose(mf);
		}
	}

	if (!g_profilePrefix.empty()) {
		OrtAllocator *alloc = NULL;
		char *pf = NULL;
		if (!g_ort->GetAllocatorWithDefaultOptions(&alloc)) {
			for (OrtSession *sess : {model.encoder, model.head}) {
				pf = NULL;
				if (!g_ort->SessionEndProfiling(sess, alloc, &pf) && pf) { logf("プロファイル: %s", pf); alloc->Free(alloc, pf); }
			}
		}
	}
	double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
	char msg[256];
	std::snprintf(msg, sizeof(msg), "frames=%u size=%dx%d infer=%.2fs total=%.2fs provider=%s",
	              raw.written, mw, mh, inferSec, total, provider.c_str());
	logf("完了: %s", msg);
	writeStatus("done", 1.0, msg);
	return 0;
}
