/*
 * ae_sofa_apply.jsx - 揺らぎの確認 (前半): ソファの動画に Relight Anime を掛けるだけ
 *
 *   AfterFX.exe -r C:\dev\RelightAEPlugin\tools\ae_sofa_apply.jsx
 *
 * 深度の自動生成はスクリプトが終わってから AE のアイドルで進むので、後半 (ae_sofa_render.jsx) は別に実行する。
 */
(function () {
    var ROOT = "C:/dev/RelightAEPlugin/cache/sofa_ae/";
    // 確認に使う動画。使う前に書き換える (空なら開くダイアログで選ぶ)
    var SRC = "";
    if (!SRC) { var picked = File.openDialog("確認に使う動画を選んでください"); if (!picked) return; SRC = picked.fsName; }
    var lines = [];
    function log(s) { lines.push(s); }
    function flush() {
        try {
            var d = new Folder(ROOT); if (!d.exists) d.create();
            var f = new File(ROOT + "apply.log"); f.encoding = "UTF-8"; f.open("w"); f.write(lines.join("\n") + "\n"); f.close();
        } catch (e) {}
    }
    app.beginSuppressDialogs();
    try {
        if (app.project) app.project.close(CloseOptions.DO_NOT_SAVE_CHANGES);
        var proj = app.newProject();
        proj.bitsPerChannel = 32;
        var src = proj.importFile(new ImportOptions(new File(SRC)));
        var comp = proj.items.addComp("SofaTest", src.width, src.height, 1.0, src.duration, src.frameRate);
        var layer = comp.layers.add(src);
        layer.name = "src";
        var fx = layer.property("ADBE Effect Parade").addProperty("SRLM Relight Anime");
        log("applied: " + (fx ? fx.name : "null") + " fps " + src.frameRate + " size " + src.width + "x" + src.height);
        comp.openInViewer();
        proj.save(new File(ROOT + "sofa.aep"));
        log("DONE");
    } catch (err) {
        log("ERROR: " + err.toString() + " (line " + err.line + ")");
    }
    app.endSuppressDialogs(false);
    flush();
})();
