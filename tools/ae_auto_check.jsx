/*
 * ae_auto_check.jsx - 自動パス生成の検証 (後半: 結果確認)
 *
 *   AfterFX.exe -r C:\dev\RelightAEPlugin\tools\ae_auto_check.jsx
 *
 * 起動中の AE に転送され、コンポ "AutoTest" のレイヤー構成と Relight Anime の
 * Normal Layer / Depth Layer の結線状態を記録し、合成結果を 1 フレーム書き出す。
 * cache/auto_test/quit.flag があれば最後に AE を終了する。
 */
(function () {
    var ROOT = "C:/dev/RelightAEPlugin/cache/auto_test/";
    var lines = [];
    function log(s) { lines.push(s); }
    function flush() {
        try {
            var f = new File(ROOT + "check.log"); f.encoding = "UTF-8"; f.open("w"); f.write(lines.join("\n") + "\n"); f.close();
        } catch (e) {}
    }
    function collect(group, name, out) {
        for (var i = 1; i <= group.numProperties; i++) {
            var p = group.property(i);
            if (p.name == name) out.push(p);
            if (p.propertyType != PropertyType.PROPERTY) collect(p, name, out);
        }
        return out;
    }
    try {
        var proj = app.project;
        var comp = null;
        for (var i = 1; i <= proj.numItems; i++) {
            var it = proj.item(i);
            if (it instanceof CompItem && it.name == "AutoTest") { comp = it; break; }
        }
        if (!comp) throw new Error("comp AutoTest not found");
        log("time: " + new Date().toString());
        log("project items: " + proj.numItems);
        for (var j = 1; j <= proj.numItems; j++) {
            var pi = proj.item(j);
            log("  item " + j + ": " + pi.name + " (" + pi.typeName + ")" + (pi instanceof FootageItem ? " " + pi.width + "x" + pi.height + " dur=" + pi.duration : ""));
        }
        log("comp layers: " + comp.numLayers);
        var srcLayer = null;
        for (var k = 1; k <= comp.numLayers; k++) {
            var L = comp.layer(k);
            log("  layer " + k + ": " + L.name + " enabled=" + L.enabled + " shy=" + L.shy + " in=" + L.inPoint + " out=" + L.outPoint);
            if (L.name == "src") srcLayer = L;
        }
        if (!srcLayer) throw new Error("src layer not found");
        var fx = srcLayer.property("ADBE Effect Parade").property(1);
        log("effect: " + fx.name);
        function P(name) { var a = collect(fx, name, []); if (!a.length) throw new Error("param not found: " + name); return a[0]; }
        var nl = P("Normal Layer").value, dl = P("Depth Layer").value, pe = P("Pass Encoding").value;
        log("Normal Layer=" + nl + " Depth Layer=" + dl + " Pass Encoding=" + pe + " Auto Passes=" + P("Auto Passes").value);
        var wired = (nl > 0 && dl > 0);
        log("WIRED=" + wired);

        if (wired) {
            // 確認用レンダー: 法線プレビューと合成 (強めのライト)
            P("Azimuth").setValue(135); P("Elevation").setValue(30); P("Intensity").setValue(1.2);
            P("Rim Intensity").setValue(0.6);
            var modes = [[4, "normal_preview"], [1, "composite"]];
            for (var m = 0; m < modes.length; m++) {
                P("Output").setValue(modes[m][0]);
                var rq = app.project.renderQueue;
                var item = rq.items.add(comp);
                item.timeSpanStart = 0;
                item.timeSpanDuration = comp.frameDuration;
                var om = item.outputModule(1);
                om.applyTemplate("TIFF シーケンス (アルファ付き)");
                om.file = new File(ROOT + "auto_" + modes[m][1] + "_[#####].tif");
                rq.render();
                log("rendered " + modes[m][1] + " status=" + item.status);
                item.remove();
            }
            P("Output").setValue(1);
        }
        log("DONE");
    } catch (err) {
        var msg = ""; try { msg = String(err.message); } catch (ea) {}
        var ln = ""; try { ln = String(err.line); } catch (eb) {}
        log("ERROR: " + msg + " (line " + ln + ")");
    }
    flush();
    if (new File(ROOT + "quit.flag").exists) {
        try { app.project.close(CloseOptions.DO_NOT_SAVE_CHANGES); } catch (e3) {}
        try { app.quit(); } catch (e4) {}
    }
})();
