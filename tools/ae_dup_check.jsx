/*
 * ae_dup_check.jsx - 同じ動画を 2 回置いたときの確認 (後半): 結線を調べ、深度と照明を書き出す
 *
 *   AfterFX.exe -r C:\dev\RelightAEPlugin\tools\ae_dup_check.jsx
 *
 * 各コマで、レイヤー A (2 秒) と B (10 秒 = B の 2 秒目) を書き出す。Output = Depth と Relit の 2 通り。
 * A と B は同じ素材の同じコマなので、深度がそろっていれば同じ絵になる。
 */
(function () {
    var ROOT = "C:/dev/RelightAEPlugin/cache/dup_test/";
    var lines = [];
    function log(s) { lines.push(s); }
    function flush() {
        try {
            var f = new File(ROOT + "check.log"); f.encoding = "UTF-8"; f.open("w"); f.write(lines.join("\n") + "\n"); f.close();
        } catch (e) {}
    }
    function findComp(name) {
        for (var i = 1; i <= app.project.numItems; i++)
            if (app.project.item(i) instanceof CompItem && app.project.item(i).name == name) return app.project.item(i);
        return null;
    }
    app.beginSuppressDialogs();
    try {
        var names = ["DupSeparate", "DupCopy"];
        var rq = app.project.renderQueue;
        while (rq.numItems > 0) rq.item(1).remove();
        for (var n = 0; n < names.length; n++) {
            var comp = findComp(names[n]);
            if (!comp) { log(names[n] + ": 無い"); continue; }
            log("== " + comp.name);
            for (var l = 1; l <= comp.numLayers; l++) {
                var L = comp.layer(l);
                var s = l + " " + L.name + " start " + L.startTime.toFixed(2) + " shy " + L.shy + " enabled " + L.enabled;
                var fx = L.property("ADBE Effect Parade").property("SRLM Relight Anime");
                if (fx) {
                    var dl = fx.property("Depth Layer").value;
                    s += " / Depth Layer = " + dl + (dl > 0 ? " (" + comp.layer(dl).name + " start " + comp.layer(dl).startTime.toFixed(2) + ")" : "");
                }
                log(s);
            }
            /* Output を切り替えて A と B の 2 秒目を書き出す */
            var modes = [[2, "depth"], [1, "relit"]];
            for (var m = 0; m < modes.length; m++) {
                for (var l2 = 1; l2 <= comp.numLayers; l2++) {
                    var fx2 = comp.layer(l2).property("ADBE Effect Parade").property("SRLM Relight Anime");
                    if (fx2) fx2.property("Output").setValue(modes[m][0]);
                }
                var times = [[2.0, "A"], [comp.duration / 2 + 2.0, "B"]];
                for (var t = 0; t < times.length; t++) {
                    var it = rq.items.add(comp);
                    it.timeSpanStart = times[t][0];
                    it.timeSpanDuration = 1 / comp.frameRate;
                    var om = it.outputModule(1);
                    var tpl = null;
                    for (var k = 0; k < om.templates.length; k++)
                        if (/PNG/i.test(om.templates[k])) { tpl = om.templates[k]; break; }
                    if (tpl) om.applyTemplate(tpl);
                    om.file = new File(ROOT + comp.name + "_" + modes[m][1] + "_" + times[t][1] + "_[#####].png");
                }
                rq.render();
                while (rq.numItems > 0) rq.item(1).remove();
            }
        }
        log("DONE");
    } catch (err) {
        log("ERROR: " + err.toString() + " (line " + err.line + ")");
    }
    app.endSuppressDialogs(false);
    flush();
})();
