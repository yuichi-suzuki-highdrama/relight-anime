/*
 * ae_dup_late.jsx - 深度ができあがった後で、同じ動画をもう一度置く / 複製する場合の確認
 *
 *   AfterFX.exe -r C:\dev\RelightAEPlugin\tools\ae_dup_late.jsx          (step=1: A を置いて効果を付ける)
 *   もう一度実行すると step=2: A の深度が結線済みなら、複製した B と、素材から置いた C を 8 秒に足す
 * 結果は cache/dup_test/late.log
 */
(function () {
    var ROOT = "C:/dev/RelightAEPlugin/cache/dup_test/";
    var lines = [];
    function log(s) { lines.push(s); }
    function flush() {
        try {
            var f = new File(ROOT + "late.log"); f.encoding = "UTF-8"; f.open("a"); f.write(lines.join("\n") + "\n"); f.close();
        } catch (e) {}
    }
    function findComp(name) {
        for (var i = 1; i <= app.project.numItems; i++)
            if (app.project.item(i) instanceof CompItem && app.project.item(i).name == name) return app.project.item(i);
        return null;
    }
    function findSrc() {
        for (var i = 1; i <= app.project.numItems; i++)
            if (app.project.item(i) instanceof FootageItem && app.project.item(i).name == "dupsrc.mp4") return app.project.item(i);
        return null;
    }
    app.beginSuppressDialogs();
    try {
        var src = findSrc();
        var comp = findComp("DupLate");
        if (!comp) {
            comp = app.project.items.addComp("DupLate", src.width, src.height, 1.0, src.duration * 2, src.frameRate);
            var a = comp.layers.add(src); a.name = "A"; a.startTime = 0;
            a.property("ADBE Effect Parade").addProperty("SRLM Relight Anime");
            log("step1: A を置いた");
        } else {
            var A = comp.layer("A");
            var dl = A.property("ADBE Effect Parade").property("SRLM Relight Anime").property("Depth Layer").value;
            log("step2: A の Depth Layer = " + dl);
            var b = A.duplicate(); b.name = "B_dup"; b.startTime = src.duration;
            var c = comp.layers.add(src); c.name = "C_new"; c.startTime = src.duration;
            c.property("ADBE Effect Parade").addProperty("SRLM Relight Anime");
            log("step2: B_dup (複製) と C_new (素材から) を 8 秒に置いた");
        }
        app.project.save();
        log("DONE");
    } catch (err) {
        log("ERROR: " + err.toString() + " (line " + err.line + ")");
    }
    app.endSuppressDialogs(false);
    flush();
})();
