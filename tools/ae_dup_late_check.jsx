/*
 * ae_dup_late_check.jsx - ae_dup_late.jsx の結果を調べる: 結線と、A (2 秒) / B_dup・C_new (10 秒) の書き出し
 * B_dup と C_new は同じ時刻に重なっているので、片方ずつ表示して書き出す。結果は cache/dup_test/late_check.log
 */
(function () {
    var ROOT = "C:/dev/RelightAEPlugin/cache/dup_test/";
    var lines = [];
    function log(s) { lines.push(s); }
    function flush() {
        try {
            var f = new File(ROOT + "late_check.log"); f.encoding = "UTF-8"; f.open("w"); f.write(lines.join("\n") + "\n"); f.close();
        } catch (e) {}
    }
    app.beginSuppressDialogs();
    try {
        var comp = null;
        for (var i = 1; i <= app.project.numItems; i++)
            if (app.project.item(i) instanceof CompItem && app.project.item(i).name == "DupLate") comp = app.project.item(i);
        for (var l = 1; l <= comp.numLayers; l++) {
            var L = comp.layer(l);
            var s = l + " " + L.name + " start " + L.startTime.toFixed(2);
            var fx = L.property("ADBE Effect Parade").property("SRLM Relight Anime");
            if (fx) {
                var dl = fx.property("Depth Layer").value;
                s += " / Depth Layer = " + dl + (dl > 0 ? " (" + comp.layer(dl).name + " start " + comp.layer(dl).startTime.toFixed(2) + ")" : "");
            }
            log(s);
        }
        var rq = app.project.renderQueue;
        while (rq.numItems > 0) rq.item(1).remove();
        var B = comp.layer("B_dup"), C = comp.layer("C_new");
        var shots = [["A", 2.0, true, true], ["B_dup", comp.duration / 2 + 2.0, true, false], ["C_new", comp.duration / 2 + 2.0, false, true]];
        for (var k = 0; k < shots.length; k++) {
            B.enabled = shots[k][2];
            C.enabled = shots[k][3];
            var it = rq.items.add(comp);
            it.timeSpanStart = shots[k][1];
            it.timeSpanDuration = 1 / comp.frameRate;
            it.outputModule(1).file = new File(ROOT + "late_" + shots[k][0] + ".mp4");
            rq.render();
            while (rq.numItems > 0) rq.item(1).remove();
        }
        B.enabled = true; C.enabled = true;
        log("DONE");
    } catch (err) {
        log("ERROR: " + err.toString() + " (line " + err.line + ")");
    }
    app.endSuppressDialogs(false);
    flush();
})();
