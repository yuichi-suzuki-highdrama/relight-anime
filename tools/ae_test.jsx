/*
 * ae_test.jsx - After Effects を GUI 操作なしで使った Relight Anime の自動検証
 *
 * 実行:
 *   "C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\AfterFX.exe" -r C:\dev\RelightAEPlugin\tools\ae_test.jsx
 *
 * やること:
 *   1. テスト素材 (anime_dance_base.mp4) と cache/test_anime_dance の法線・深度 EXR 連番を読み込む
 *   2. 32bpc のコンポを作り、素材に Relight Anime を適用して法線・深度レイヤーを指定
 *   3. Output を Normal Preview / Depth Preview / Composite / Light Pass Only に切り替えて
 *      1 フレーム目を PNG に保存 (cache/test_anime_dance/ae_*.png)
 *   4. 結果とエラーを ae_test.log に書く
 */
(function () {
    var ROOT = "C:/dev/RelightAEPlugin/cache/test_anime_dance/";
    var SRC = "C:/ComfyUI_windows_portable/ComfyUI/input/anime_dance_base.mp4";
    var MATCH = "SRLM Relight Anime";
    var logLines = [];
    var lSrc = null;
    function log(s) { logLines.push(s); }
    function flush() {
        try {
            var f = new File(ROOT + "ae_test.log");
            f.encoding = "UTF-8";
            f.open("w");
            f.write(logLines.join("\n") + "\n");
            f.close();
        } catch (e) {}
    }

    try {
        // スクリプトからのファイル書き出しを許可
        try { app.preferences.savePrefAsLong("Main Pref Section", "Pref_SCRIPTING_FILE_NETWORK_SECURITY", 1); } catch (e0) { log("pref: " + e0); }

        log("AE " + app.version);
        var proj = app.project ? app.project : app.newProject();
        proj.bitsPerChannel = 32;
        log("bpc=" + proj.bitsPerChannel);
        // 数値比較のため色管理を切る (作業スペース None)。実運用ではリニア作業空間でよい
        try {
            log("workingSpace before: '" + proj.workingSpace + "' linearize=" + proj.linearizeWorkingSpace);
            proj.workingSpace = "";
            log("workingSpace after: '" + proj.workingSpace + "'");
        } catch (ew) { log("workingSpace: " + String(ew.message)); }

        function importFile(path, seq) {
            var f = new File(path);
            if (!f.exists) throw new Error("not found: " + path);
            var io = new ImportOptions(f);
            if (seq) { io.sequence = true; io.forceAlphabetical = true; }
            return proj.importFile(io);
        }
        var src = importFile(SRC, false);
        var nrm = importFile(ROOT + "normal/normal.00001.exr", true);
        var dep = importFile(ROOT + "depth/depth.00001.exr", true);
        log("src " + src.width + "x" + src.height + " dur=" + src.duration + " fps=" + src.frameRate);
        log("normal " + nrm.width + "x" + nrm.height + " dur=" + nrm.duration);
        // EXR はデータなので色変換を止めたい。スクリプトから Preserve RGB を設定できるか調べる
        try {
            var props = nrm.mainSource.reflect.properties;
            var names = [];
            for (var pi = 0; pi < props.length; pi++) names.push(props[pi].name);
            log("mainSource props: " + names.join(","));
        } catch (er) { log("reflect failed: " + String(er.message)); }
        // 調査結果: FootageSource に preserveRGB は無く、スクリプトからは設定できない (AE 26.5)。
        // AE は EXR をリニア→sRGB 変換して渡すので、エフェクト側の Pass Encoding = sRGB (decode) で打ち消す。
        log("depth " + dep.width + "x" + dep.height + " dur=" + dep.duration);

        var comp = proj.items.addComp("RelightTest", src.width, src.height, 1.0, 1.0, 24);
        var lDep = comp.layers.add(dep); lDep.name = "depth"; lDep.enabled = false;
        var lNrm = comp.layers.add(nrm); lNrm.name = "normal"; lNrm.enabled = false;
        lSrc = comp.layers.add(src); lSrc.name = "src";

        var fx = lSrc.property("ADBE Effect Parade").addProperty(MATCH);
        if (!fx) throw new Error("effect not found: " + MATCH);
        log("effect added: " + fx.name + " props=" + fx.numProperties);

        // 名前でプロパティを再帰的に集める (同名が複数ある場合は出現順)
        function collect(group, name, out) {
            for (var i = 1; i <= group.numProperties; i++) {
                var p = group.property(i);
                if (p.name == name) out.push(p);
                if (p.propertyType != PropertyType.PROPERTY) collect(p, name, out);
            }
            return out;
        }
        function P(name, nth) { var a = collect(fx, name, []); if (!a.length) throw new Error("param not found: " + name); return a[nth || 0]; }

        // 全パラメータ名を記録
        function dump(group, indent) {
            for (var i = 1; i <= group.numProperties; i++) {
                var p = group.property(i);
                var v = "";
                try { if (p.propertyType == PropertyType.PROPERTY) v = " = " + p.value; } catch (e) {}
                log(indent + p.name + " [" + p.matchName + "]" + v);
                if (p.propertyType != PropertyType.PROPERTY) dump(p, indent + "  ");
            }
        }
        dump(fx, "  ");

        P("Pass Encoding").setValue(2);
        P("Normal Layer").setValue(lNrm.index);
        P("Depth Layer").setValue(lDep.index);
        P("Azimuth").setValue(135);
        P("Elevation").setValue(30);
        P("Intensity").setValue(0.8);
        P("Rim Intensity").setValue(0.5);
        try { log("position default: " + P("Position").value.toString() + " z=" + P("Z (px)").value); } catch (ep) { log("position read failed: " + String(ep.message)); }


        // saveFrameToPng は書き出されないことがあるので、レンダーキューで 1 フレームだけ出す
        var pngTemplate = null;
        function renderFrame(name) {
            var rq = app.project.renderQueue;
            var item = rq.items.add(comp);
            item.timeSpanStart = 0;
            item.timeSpanDuration = comp.frameDuration;
            var om = item.outputModule(1);
            if (pngTemplate === null) {
                var ts = om.templates;
                log("output templates: " + ts.join(" | "));
                pngTemplate = "";
                for (var t = 0; t < ts.length; t++) { if (/png/i.test(ts[t]) && !/_HIDDEN/.test(ts[t])) { pngTemplate = ts[t]; break; } }
                if (!pngTemplate) for (var t2 = 0; t2 < ts.length; t2++) { if (/tif/i.test(ts[t2]) && !/_HIDDEN/.test(ts[t2])) { pngTemplate = ts[t2]; break; } }
                log("use template: " + pngTemplate);
            }
            if (pngTemplate) om.applyTemplate(pngTemplate);
            var ext = /tif/i.test(pngTemplate) && !/png/i.test(pngTemplate) ? ".tif" : ".png";
            om.file = new File(ROOT + "ae_" + name + "_[#####]" + ext);
            rq.render();
            log("rendered " + name + " status=" + item.status);
            item.remove();
        }
        var outputProp = P("Output");
        var modes = [[4, "normal_preview"], [5, "depth_preview"], [1, "composite"], [2, "light_pass"]];
        for (var m = 0; m < modes.length; m++) {
            outputProp.setValue(modes[m][0]);
            try { renderFrame(modes[m][1]); } catch (e1) { log("render failed (" + modes[m][1] + "): " + String(e1.message)); }
        }
        // 点光源 + トゥーンも 1 枚
        P("Type").setValue(2);
        P("Position").setValue([60, 200]);
        P("Z (px)").setValue(-400);
        P("Radius (px)").setValue(500);
        P("Intensity").setValue(1.5);
        P("Color").setValue([1, 120 / 255, 80 / 255]);
        P("Toon Steps (0 = off)").setValue(3);
        P("Rim Intensity").setValue(0);
        outputProp.setValue(1);
        try { renderFrame("point_toon"); } catch (e2) { log("render failed (point_toon): " + String(e2.message)); }

        log("DONE");
    } catch (err) {
        /* ExtendScript では Error オブジェクトの直接連結が失敗することがあるので個別に取り出す */
        var msg = "", ln = "";
        try { msg = String(err.message); } catch (ea) { try { msg = String(err.description); } catch (eb) { msg = "(unknown)"; } }
        try { ln = String(err.line); } catch (ec) {}
        log("ERROR: " + msg + " (line " + ln + ")");
    }
    flush();
    if (!new File(ROOT + "ae_test_keep.flag").exists) {
        try { app.project.close(CloseOptions.DO_NOT_SAVE_CHANGES); } catch (e3) {}
        try { app.quit(); } catch (e4) {}
    } else {
        // 画面確認用: 素材レイヤーを選択してエフェクトコントロールを開いたままにする
        try { lSrc.selected = true; } catch (e5) {}
    }
})();
