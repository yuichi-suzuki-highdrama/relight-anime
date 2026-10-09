/*
 * RelightHelper.cpp
 * Relight Anime の補助プラグイン (AEGP)。
 *
 * エフェクトから登録したアイドルフックは AE に呼ばれないため、深度の自動生成
 * (素材の書き出し、レイヤーの追加と結線) を正式な AE の文脈で行う場所が無かった。
 * WM_TIMER から AEGP を呼ぶと「no current context」で失敗し、AE が固まることがあった。
 * この AEGP がアイドルフックを受け取り、エフェクト (RelightAnime.aex) が書き出している
 * RelightAnime_Idle を呼ぶ。エフェクトがまだ読み込まれていなければ何もしない。
 */
#include "AEConfig.h"
#include "AE_GeneralPlug.h"
#include "AEGP_SuiteHandler.h"
#include "entry.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef A_Err (*RelightIdleFn)(SPBasicSuite *basic, AEGP_PluginID id, A_long *max_sleepPL);

static SPBasicSuite *s_basic = NULL;
static AEGP_PluginID s_id = 0;

static A_Err IdleHook(AEGP_GlobalRefcon, AEGP_IdleRefcon, A_long *max_sleepPL)
{
	/* エフェクトは使われたときに読み込まれる。読み込まれていなければジョブも無い */
	HMODULE fx = GetModuleHandleW(L"RelightAnime.aex");
	if (!fx) return A_Err_NONE;
	static RelightIdleFn fn = NULL;
	if (!fn) fn = (RelightIdleFn)GetProcAddress(fx, "RelightAnime_Idle");
	if (!fn) return A_Err_NONE;
	return fn(s_basic, s_id, max_sleepPL);
}

extern "C" DllExport A_Err EntryPointFunc(
	struct SPBasicSuite *pica_basicP,
	A_long               major_versionL,
	A_long               minor_versionL,
	AEGP_PluginID        aegp_plugin_id,
	AEGP_GlobalRefcon   *global_refconV)
{
	s_basic = pica_basicP;
	s_id = aegp_plugin_id;
	A_Err err = A_Err_NONE;
	try {
		AEGP_SuiteHandler suites(pica_basicP);
		err = suites.RegisterSuite5()->AEGP_RegisterIdleHook(aegp_plugin_id, IdleHook, NULL);
	} catch (...) {
		err = A_Err_GENERIC;
	}
	return err;
}
