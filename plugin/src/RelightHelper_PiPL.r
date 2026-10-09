/* Relight Anime Helper (AEGP) - PiPL リソース定義 */
#include "AEConfig.h"

#ifndef AE_OS_WIN
	#include "AE_General.r"
#endif

resource 'PiPL' (16000) {
	{
		Kind {
			AEGP
		},
		Name {
			"Relight Anime Helper"
		},
		Category {
			"General Plugin"
		},
		Version {
			196608
		},
#ifdef AE_OS_WIN
	#ifdef AE_PROC_INTELx64
		CodeWin64X86 {"EntryPointFunc"},
	#endif
#else
	#ifdef AE_OS_MAC
		CodeMacIntel64 {"EntryPointFunc"},
		CodeMacARM64 {"EntryPointFunc"},
	#endif
#endif
	}
};
