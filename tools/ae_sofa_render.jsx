/*
 * ae_sofa_render.jsx - 揺らぎの確認 (後半): ライトを背中側に低く置いて、連続したコマを書き出す
 *
 *   AfterFX.exe -r C:\dev\RelightAEPlugin\tools\ae_sofa_render.jsx
 *
 * 前半 (ae_sofa_apply.jsx) で掛けた Relight Anime の深度ができてから実行する。
 * 60〜79 コマを可逆圧縮で cache/sofa_ae/out_*.avi に書き出す (ライトの位置と高さを何通りか)。
 */
(function () {
    var ROOT = "C:/dev/RelightAEPlugin/cache/sofa_ae/";
    var lines = [];
    function log(s) { lines.push(s); }
    function flush() {
        try {
            var f = new File(ROOT + "render.log"); f.encoding = "UTF-8"; f.open("w"); f.write(lines.join("\n") + "\n"); f.close();
        } catch (e) {}
    }
    app.beginSuppressDialogs();
    try {
        var comp = null;
        for (var i = 1; i <= app.project.numItems; i++) {
            if (app.project.item(i) instanceof CompItem && app.project.item(i).name == "SofaTest") comp = app.project.item(i);
        }
        if (!comp) throw new Error("SofaTest が無い");
        var names = [];
        for (var l = 1; l <= comp.numLayers; l++) names.push(comp.layer(l).name);
        log("レイヤー: " + names.join(", "));
        var layer = comp.layer("src");
        var fx = layer.property("ADBE Effect Parade").property("SRLM Relight Anime");
        // 書き出しのテンプレート (可逆圧縮)
        var cases = [
            ["back_low", [900, 150], -0.1],
            ["back_lower", [900, 150], -0.3],
            ["left_back", [500, 300], -0.15],
            ["grazing", [700, 200], 0.0]
        ];
        for (var c = 0; c < cases.length; c++) {
            fx.property("Light Position").setValue(cases[c][1]);
            fx.property("Light Height").setValue(cases[c][2]);
            var rq = app.project.renderQueue;
            while (rq.numItems > 0) rq.item(1).remove();
            var item = rq.items.add(comp);
            item.timeSpanStart = 60 / comp.frameRate;
            item.timeSpanDuration = 20 / comp.frameRate;
            var om = item.outputModule(1);
            var tpl = null;
            for (var t = 0; t < om.templates.length; t++) {
                if (/可逆|Lossless/i.test(om.templates[t]) && !/アルファ|Alpha/i.test(om.templates[t])) { tpl = om.templates[t]; break; }
            }
            if (tpl) om.applyTemplate(tpl);
            om.file = new File(ROOT + "out_" + cases[c][0] + ".avi");
            rq.render();
            log(cases[c][0] + " 書き出し (テンプレート " + tpl + ")");
        }
        log("DONE");
    } catch (err) {
        log("ERROR: " + err.toString() + " (line " + err.line + ")");
    }
    app.endSuppressDialogs(false);
    flush();
})();
