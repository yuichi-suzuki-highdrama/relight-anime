/*
 * RelightAuto.h
 * 自動パス生成: エフェクト適用時にソース素材のパスを調べ、外部ツール (tools/gen_passes.py) を
 * バックグラウンドで起動し、完了したら生成された法線・深度の連番をプロジェクトに読み込んで
 * コンポに非表示レイヤーとして追加し、エフェクトの Normal Layer / Depth Layer に結線する。
 *
 * プロジェクトを書き換える処理は AE のアイドルフック (メインスレッド) でのみ行う。
 */
#pragma once

#include "RelightAE.h"

/* GlobalSetup から 1 回呼ぶ。AEGP プラグイン ID の取得とアイドルフック登録 */
void Auto_Init(PF_InData *in_data);

/* エフェクト適用時 (SEQUENCE_SETUP) やボタン押下時に呼ぶ。force = 再生成。
   engine = Depth Engine の選択 (1: この PC、2: 別の PC の深度サーバーで小さいモデル、3: 同じく大きいモデル)。
   別の PC は設定の remote_server。つながらなければこの PC で作り直す */
void Auto_Request(PF_InData *in_data, bool force, int engine = 1);

/* <cache>\relight_auto.log へ 1 行書く (描画スレッドからも呼べる) */
#include <string>
void Relight_Log(const std::wstring &msg);

/* 「Clean Cache...」ボタン。今のプロジェクトで使っていない深度キャッシュを確認のうえ消す */
void Auto_CleanCache(PF_InData *in_data);
