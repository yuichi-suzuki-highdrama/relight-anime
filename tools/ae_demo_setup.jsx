/*
 * ae_demo_setup.jsx - 解説動画の画面録画用に、一時的なコンポ「Relight Demo」を作って開く (保存しない)
 *   AfterFX.exe -r C:\dev\RelightAEPlugin\tools\ae_demo_setup.jsx
 * 素材は、開いているプロジェクトの最初の動画フッテージ。Relight Anime (既定 = Anime Standard) を付け、ライトを見せる。
 * 片付けは ae_demo_cleanup.jsx
 */
(function () {
    var LOG = "D:/video_projects/relight_explainer/demo_setup.log";
    var lines = [];
    function log(s) { lines.push(s); }
    app.beginSuppressDialogs();
    try {
        var src = null;
        for (var i = 1; i <= app.project.numItems; i++) {
            var it = app.project.item(i);
            if (it instanceof FootageItem && it.mainSource && !it.mainSource.isStill && it.name.match(/\.mp4$/i)) { src = it; break; }
        }
        var comp = app.project.items.addComp("Relight Demo", src.width, src.height, 1.0, src.duration, src.frameRate);
        var L = comp.layers.add(src);
        L.name = "demo";
        var fx = L.property("ADBE Effect Parade").addProperty("SRLM Relight Anime");
        comp.openInViewer();
        comp.time = 4.0;
        L.selected = true;
        try { app.activeViewer.views[0].options.zoom = 0.75; } catch (e) { log("zoom: " + e.toString()); }
        log("OK " + comp.width + "x" + comp.height);
    } catch (err) {
        log("ERROR: " + err.toString() + " (line " + err.line + ")");
    }
    app.endSuppressDialogs(false);
    var f = new File(LOG); f.encoding = "UTF-8"; f.open("w"); f.write(lines.join("\n") + "\n"); f.close();
})();
