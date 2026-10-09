/*
 * ae_auto_test.jsx - 自動パス生成の検証 (前半: 適用するだけ)
 *
 *   AfterFX.exe -r C:\dev\RelightAEPlugin\tools\ae_auto_test.jsx
 *
 * テスト動画を読み込んでコンポ "AutoTest" を作り、Relight Anime を適用するだけで終了する。
 * (スクリプト実行中は AE のアイドルフックが動かないので、生成の完了待ちと確認は
 *  ae_auto_check.jsx を後から別に実行する)
 */
(function () {
    var ROOT = "C:/dev/RelightAEPlugin/cache/auto_test/";
    var SRC = "C:/ComfyUI_windows_portable/ComfyUI/input/anime_dance_base.mp4";
    var lines = [];
    function log(s) { lines.push(s); }
    function flush() {
        try {
            var d = new Folder(ROOT); if (!d.exists) d.create();
            var f = new File(ROOT + "apply.log"); f.encoding = "UTF-8"; f.open("w"); f.write(lines.join("\n") + "\n"); f.close();
        } catch (e) {}
    }
    try {
        try { app.preferences.savePrefAsLong("Main Pref Section", "Pref_SCRIPTING_FILE_NETWORK_SECURITY", 1); } catch (e0) {}
        var proj = app.project ? app.project : app.newProject();
        proj.bitsPerChannel = 32;
        var src = proj.importFile(new ImportOptions(new File(SRC)));
        var comp = proj.items.addComp("AutoTest", src.width, src.height, 1.0, 2.0, src.frameRate);
        var layer = comp.layers.add(src);
        layer.name = "src";
        var fx = layer.property("ADBE Effect Parade").addProperty("SRLM Relight Anime");
        log("applied: " + (fx ? fx.name : "null") + " at " + new Date().toString());
        log("comp layers: " + comp.numLayers);
        // コンポを開いておく (プロジェクトが dirty になるのは問題ない)
        comp.openInViewer();
        log("DONE");
    } catch (err) {
        var msg = ""; try { msg = String(err.message); } catch (ea) {}
        var ln = ""; try { ln = String(err.line); } catch (eb) {}
        log("ERROR: " + msg + " (line " + ln + ")");
    }
    flush();
})();
