/*
 * ae_render_each_layer.jsx - Relight Anime を付けたレイヤーが同じ時刻に重なっているコンポで、
 * 1 つずつだけ表示して書き出す (ほかのレイヤーは一時的に非表示にし、書き出し後に元へ戻す。プロジェクトは保存しない)
 *
 *   AfterFX.exe -r C:\dev\RelightAEPlugin\tools\ae_render_each_layer.jsx
 * 書き出し先 D:/video_projects/relight_explainer/renders/Relit_<番号>_<Look>.mp4、記録は each.log
 */
(function () {
    var OUT = "D:/video_projects/relight_explainer/renders/";
    var lines = [];
    function log(s) { lines.push(s); }
    function flush() {
        try {
            var f = new File(OUT + "each.log"); f.encoding = "UTF-8"; f.open("w"); f.write(lines.join("\n") + "\n"); f.close();
        } catch (e) {}
    }
    app.beginSuppressDialogs();
    try {
        var comp = null;
        for (var i = 1; i <= app.project.numItems; i++) {
            var it = app.project.item(i);
            if (it instanceof CompItem && it.name != "Normal") comp = it;
        }
        var fxLayers = [], was = [];
        for (var l = 1; l <= comp.numLayers; l++) {
            var L = comp.layer(l);
            was.push(L.enabled);
            if (L.property("ADBE Effect Parade").property("SRLM Relight Anime")) fxLayers.push(l);
        }
        var rq = app.project.renderQueue;
        while (rq.numItems > 0) rq.item(1).remove();
        for (var k = 0; k < fxLayers.length; k++) {
            for (var m = 0; m < fxLayers.length; m++) comp.layer(fxLayers[m]).enabled = (m == k);
            var look = comp.layer(fxLayers[k]).property("ADBE Effect Parade").property("SRLM Relight Anime").property("Look").value;
            var name = "Relit_" + (k + 1) + "_" + (look == 2 ? "Cinematic" : "Anime");
            var rqi = rq.items.add(comp);
            rqi.outputModule(1).file = new File(OUT + name + ".mp4");
            rq.render();
            while (rq.numItems > 0) rq.item(1).remove();
            log(name + " ← レイヤー " + fxLayers[k]);
        }
        for (var r = 1; r <= comp.numLayers; r++) comp.layer(r).enabled = was[r - 1];
        log("DONE (表示は元に戻した。保存はしていない)");
    } catch (err) {
        log("ERROR: " + err.toString() + " (line " + err.line + ")");
    }
    app.endSuppressDialogs(false);
    flush();
})();
