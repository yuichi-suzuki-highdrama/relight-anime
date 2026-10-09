/*
 * ae_dup_apply.jsx - 同じ動画を 2 回置いたときの確認 (前半): コンポを作って Relight Anime を付けるだけ
 *
 *   AfterFX.exe -r C:\dev\RelightAEPlugin\tools\ae_dup_apply.jsx
 *
 * DupSeparate: プロジェクトの同じ素材から 2 つのレイヤーを作る (0 秒と 8 秒)
 * DupCopy    : 1 つ目に効果を付けてから複製し (Ctrl+D と同じ)、複製を 8 秒へずらす
 * 深度の自動生成はスクリプトが終わってから AE のアイドルで進むので、後半 (ae_dup_check.jsx) は別に実行する。
 */
(function () {
    var ROOT = "C:/dev/RelightAEPlugin/cache/dup_test/";
    var SRC = "C:/dev/RelightAEPlugin/cache/dup_test/dupsrc.mp4";
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
        var dur = src.duration;

        /* パターン 1: 同じ素材から 2 つのレイヤー */
        var c1 = proj.items.addComp("DupSeparate", src.width, src.height, 1.0, dur * 2, src.frameRate);
        var a1 = c1.layers.add(src); a1.name = "A"; a1.startTime = 0;
        var b1 = c1.layers.add(src); b1.name = "B"; b1.startTime = dur;
        a1.property("ADBE Effect Parade").addProperty("SRLM Relight Anime");
        b1.property("ADBE Effect Parade").addProperty("SRLM Relight Anime");

        /* パターン 2: 効果を付けてから複製 */
        var c2 = proj.items.addComp("DupCopy", src.width, src.height, 1.0, dur * 2, src.frameRate);
        var a2 = c2.layers.add(src); a2.name = "A"; a2.startTime = 0;
        a2.property("ADBE Effect Parade").addProperty("SRLM Relight Anime");
        var b2 = a2.duplicate(); b2.name = "B"; b2.startTime = dur;

        log("dur " + dur + " fps " + src.frameRate + " size " + src.width + "x" + src.height);
        c1.openInViewer();
        proj.save(new File(ROOT + "dup.aep"));
        log("DONE");
    } catch (err) {
        log("ERROR: " + err.toString() + " (line " + err.line + ")");
    }
    app.endSuppressDialogs(false);
    flush();
})();
