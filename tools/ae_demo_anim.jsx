/*
 * ae_demo_anim.jsx - 解説動画用: 一時コンポ「Relight Demo」を Cinematic・リムなしにし、ライトの動きを付けて書き出す
 *   AfterFX.exe -r C:\dev\RelightAEPlugin\tools\ae_demo_anim.jsx
 *  1. 横の移動 (高さ 0.4 のまま、左 → 上 → 右)       → renders/demo_cinematic_light_sweep.mp4
 *  2. 高さの変化 (左上に置いたまま、1.2 → -0.3 → 1.2) → renders/demo_cinematic_height_sweep.mp4
 * 書き出し後は 1 の動きを付けた状態に戻す (画面録画のプレビュー用)。プロジェクトは保存しない。
 */
(function () {
    var OUT = "D:/video_projects/relight_explainer/renders/";
    var lines = [];
    function log(s) { lines.push(s); }
    app.beginSuppressDialogs();
    try {
        var comp = null;
        for (var i = 1; i <= app.project.numItems; i++)
            if (app.project.item(i) instanceof CompItem && app.project.item(i).name == "Relight Demo") comp = app.project.item(i);
        var L = comp.layer("demo");
        var fx = L.property("ADBE Effect Parade").property("SRLM Relight Anime");
        fx.property("Look").setValue(2);          // Cinematic
        fx.property("Use Rim").setValue(0);       // Light 1 のリムを切る
        fx.property("Show Light").setValue(1);
        fx.property("Output").setValue(1);
        var pos = fx.property("Light Position"), hgt = fx.property("Light Height");
        function clear(p) { while (p.numKeys > 0) p.removeKey(1); }
        function sweep() {
            clear(pos); clear(hgt);
            pos.setValueAtTime(0, [200, 200]); pos.setValueAtTime(4, [690, 110]); pos.setValueAtTime(8, [1180, 200]);
            hgt.setValue(0.4);
        }
        function height() {
            clear(pos); clear(hgt);
            pos.setValue([430, 160]);
            hgt.setValueAtTime(0, 1.2); hgt.setValueAtTime(4, -0.3); hgt.setValueAtTime(8, 1.2);
        }
        var jobs = [[sweep, "demo_cinematic_light_sweep"], [height, "demo_cinematic_height_sweep"]];
        var rq = app.project.renderQueue;
        for (var j = 0; j < jobs.length; j++) {
            jobs[j][0]();
            while (rq.numItems > 0) rq.item(1).remove();
            var it = rq.items.add(comp);
            it.outputModule(1).file = new File(OUT + jobs[j][1] + ".mp4");
            rq.render();
            log(jobs[j][1]);
        }
        while (rq.numItems > 0) rq.item(1).remove();
        sweep();
        comp.time = 0;
        log("DONE");
    } catch (err) {
        log("ERROR: " + err.toString() + " (line " + err.line + ")");
    }
    app.endSuppressDialogs(false);
    var f = new File(OUT + "demo_anim.log"); f.encoding = "UTF-8"; f.open("w"); f.write(lines.join("\n") + "\n"); f.close();
})();
