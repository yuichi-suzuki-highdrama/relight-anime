/*
 * ae_render_outputs.jsx - 開いているプロジェクトを動画に書き出す (エフェクトなしの Normal と、Relight Anime 付きのコンポ全部)
 *
 *   AfterFX.exe -r C:\dev\RelightAEPlugin\tools\ae_render_outputs.jsx
 *
 * Normal: 素材だけのコンポを作って書き出す。ほかのコンポは名前のまま書き出す。
 * 書き出し先 D:/video_projects/relight_explainer/renders/ 。記録は同じ場所の render.log
 */
(function () {
    var OUT = "D:/video_projects/relight_explainer/renders/";
    var lines = [];
    function log(s) { lines.push(s); }
    function flush() {
        try {
            var f = new File(OUT + "render.log"); f.encoding = "UTF-8"; f.open("w"); f.write(lines.join("\n") + "\n"); f.close();
        } catch (e) {}
    }
    app.beginSuppressDialogs();
    try {
        var d = new Folder(OUT); if (!d.exists) d.create();
        var src = null, comps = [];
        for (var i = 1; i <= app.project.numItems; i++) {
            var it = app.project.item(i);
            if (it instanceof FootageItem && it.mainSource && !it.mainSource.isStill && it.name.match(/\.mp4$/i)) src = it;
            if (it instanceof CompItem && it.name != "Normal") comps.push(it);
        }
        var normal = null;
        for (var j = 1; j <= app.project.numItems; j++)
            if (app.project.item(j) instanceof CompItem && app.project.item(j).name == "Normal") normal = app.project.item(j);
        if (!normal) {
            normal = app.project.items.addComp("Normal", src.width, src.height, 1.0, src.duration, src.frameRate);
            normal.layers.add(src);
        }
        var list = [normal].concat(comps);
        var rq = app.project.renderQueue;
        while (rq.numItems > 0) rq.item(1).remove();
        for (var k = 0; k < list.length; k++) {
            var rqi = rq.items.add(list[k]);
            rqi.outputModule(1).file = new File(OUT + list[k].name + ".mp4");
            log(list[k].name + " " + list[k].duration.toFixed(2) + "s → " + OUT + list[k].name + ".mp4");
        }
        rq.render();
        while (rq.numItems > 0) rq.item(1).remove();
        app.project.save();
        log("DONE");
    } catch (err) {
        log("ERROR: " + err.toString() + " (line " + err.line + ")");
    }
    app.endSuppressDialogs(false);
    flush();
})();
