# Relight Anime - After Effects エフェクトプラグイン (Windows x64)
#
# 作るもの:
#   RelightAnime.aex        エフェクト本体 (照明、深度の自動生成のジョブ管理)
#   RelightAnimeHelper.aex  補助 AEGP (アイドルフックを受け取り、エフェクトのジョブを AE の正式な文脈で進める)
#
# 使い方:
#   AE SDK を展開し、環境変数 AE_SDK_ROOT か -DAE_SDK_ROOT=... でルートを渡す
#   (ルート直下か Examples/ 直下に Headers/ Util/ Resources/ がある構成を想定)
#   cmake -S plugin -B plugin/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build plugin/build
cmake_minimum_required(VERSION 3.20)
project(RelightAE LANGUAGES CXX C RC)

if(NOT WIN32)
  message(FATAL_ERROR "現状 Windows x64 のみ対応")
endif()

set(AE_SDK_ROOT "$ENV{AE_SDK_ROOT}" CACHE PATH "After Effects SDK のルートフォルダ")
if(NOT AE_SDK_ROOT)
  set(AE_SDK_ROOT "C:/dev/AE_SDK")
endif()

find_path(AE_SDK_HEADERS AE_Effect.h
  PATHS "${AE_SDK_ROOT}/Examples/Headers" "${AE_SDK_ROOT}/Headers"
  NO_DEFAULT_PATH)
if(NOT AE_SDK_HEADERS)
  message(FATAL_ERROR "AE SDK が見つかりません: AE_SDK_ROOT=${AE_SDK_ROOT} に Examples/Headers/AE_Effect.h か Headers/AE_Effect.h が必要です")
endif()
get_filename_component(AE_SDK_EXAMPLES "${AE_SDK_HEADERS}" DIRECTORY)
set(AE_SDK_UTIL      "${AE_SDK_EXAMPLES}/Util")
set(AE_SDK_RESOURCES "${AE_SDK_EXAMPLES}/Resources")

find_program(PIPLTOOL NAMES PiPLtool PiPLtool.exe
  PATHS "${AE_SDK_RESOURCES}" "${AE_SDK_ROOT}/Resources"
  NO_DEFAULT_PATH)
if(NOT PIPLTOOL)
  message(FATAL_ERROR "PiPLtool.exe が ${AE_SDK_RESOURCES} に見つかりません")
endif()

# ---- GPU 版 (CUDA)。CUDA Toolkit が無ければ CPU 版だけを作る ----
include(CheckLanguage)
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  # Turing / Ampere / Ada / Blackwell (RTX 50 シリーズ = sm_120 は CUDA 12.8 以降)
  set(CMAKE_CUDA_ARCHITECTURES 75 86 89 120)
endif()
check_language(CUDA)
if(CMAKE_CUDA_COMPILER)
  enable_language(CUDA)
  find_package(CUDAToolkit REQUIRED)
  set(RELIGHT_HAS_CUDA ON)
  message(STATUS "CUDA           : ${CMAKE_CUDA_COMPILER} (${CMAKE_CUDA_ARCHITECTURES})")
else()
  set(RELIGHT_HAS_CUDA OFF)
  message(STATUS "CUDA           : 見つからないので CPU 版のみ")
endif()

message(STATUS "AE SDK Headers : ${AE_SDK_HEADERS}")
message(STATUS "AE SDK Util    : ${AE_SDK_UTIL}")
message(STATUS "PiPLtool       : ${PIPLTOOL}")

# ---- PiPL リソース生成 (.r -> .rr -> .rrc -> .rc) ----
function(add_pipl NAME OUT_VAR)
  set(R   "${CMAKE_CURRENT_SOURCE_DIR}/src/${NAME}.r")
  set(RR  "${CMAKE_CURRENT_BINARY_DIR}/${NAME}.rr")
  set(RRC "${CMAKE_CURRENT_BINARY_DIR}/${NAME}.rrc")
  set(RC  "${CMAKE_CURRENT_BINARY_DIR}/${NAME}.rc")
  add_custom_command(
    OUTPUT "${RC}"
    COMMAND "${CMAKE_COMMAND}"
            -DCL=${CMAKE_CXX_COMPILER}
            "-DINCLUDES=${AE_SDK_HEADERS};${CMAKE_CURRENT_SOURCE_DIR}/src"
            -DIN=${R} -DOUT=${RR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/preprocess.cmake"
    COMMAND "${PIPLTOOL}" "${RR}" "${RRC}"
    COMMAND "${CMAKE_COMMAND}"
            -DCL=${CMAKE_CXX_COMPILER}
            -DDEFINES=MSWindows
            -DIN=${RRC} -DOUT=${RC}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/preprocess.cmake"
    DEPENDS "${R}" "${CMAKE_CURRENT_SOURCE_DIR}/src/RelightAE_Flags.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/preprocess.cmake"
    COMMENT "PiPL リソースを生成 (${NAME})"
    VERBATIM)
  set(${OUT_VAR} "${RC}" PARENT_SCOPE)
endfunction()

add_pipl(RelightAE_PiPL PIPL_RC)
add_pipl(RelightHelper_PiPL HELPER_PIPL_RC)

function(setup_aex TARGET OUTNAME)
  set_target_properties(${TARGET} PROPERTIES OUTPUT_NAME "${OUTNAME}" PREFIX "" SUFFIX ".aex")
  target_include_directories(${TARGET} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${AE_SDK_HEADERS}"
    "${AE_SDK_HEADERS}/SP"
    "${AE_SDK_UTIL}")
  target_compile_definitions(${TARGET} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(${TARGET} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/W3 /EHsc /utf-8 /fp:fast /MP>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/O2>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/utf-8 -Xcompiler=/EHsc -O3>)
  target_compile_features(${TARGET} PRIVATE cxx_std_17)
endfunction()

# ---- エフェクト本体 ----
add_library(RelightAE SHARED
  src/RelightAE.cpp
  src/RelightAE.h
  src/RelightAuto.cpp
  src/RelightAuto.h
  src/RelightAE_Flags.h
  "${AE_SDK_UTIL}/AEGP_SuiteHandler.cpp"
  "${AE_SDK_UTIL}/MissingSuiteError.cpp"
  "${PIPL_RC}")
setup_aex(RelightAE "RelightAnime")
target_link_libraries(RelightAE PRIVATE shell32 user32 gdi32)
if(RELIGHT_HAS_CUDA)
  target_sources(RelightAE PRIVATE src/RelightGPU.cu src/RelightGPU.h src/RelightCore.h)
  target_compile_definitions(RelightAE PRIVATE RELIGHT_HAS_CUDA)
  # CUDA の実行時ライブラリは静的に入れる (AE のフォルダに DLL を置かなくてよい)
  target_link_libraries(RelightAE PRIVATE CUDA::cudart_static)
endif()

# ---- 補助 AEGP ----
add_library(RelightHelper SHARED
  src/RelightHelper.cpp
  "${AE_SDK_UTIL}/AEGP_SuiteHandler.cpp"
  "${AE_SDK_UTIL}/MissingSuiteError.cpp"
  "${HELPER_PIPL_RC}")
setup_aex(RelightHelper "RelightAnimeHelper")

# ---- インストール先 (AE 共通プラグインフォルダ) ----
set(AE_PLUGIN_DIR "C:/Program Files/Adobe/Common/Plug-ins/7.0/MediaCore/SRLM" CACHE PATH "インストール先")
install(TARGETS RelightAE RelightHelper RUNTIME DESTINATION "${AE_PLUGIN_DIR}")
