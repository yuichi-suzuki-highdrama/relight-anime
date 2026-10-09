/*
 * ae_demo_cleanup.jsx - ae_demo_setup.jsx で作った一時コンポ「Relight Demo」と、それだけが使っていた深度の素材を消す (保存しない)
 *   AfterFX.exe -r C:\dev\RelightAEPlugin\tools\ae_demo_cleanup.jsx
 */
(function () {
    var lines = [];
    app.beginSuppressDialogs();
    try {
        var comp = null;
        for (var i = 1; i <= app.project.numItems; i++)
            if (app.project.item(i) instanceof CompItem && app.project.item(i).name == "Relight Demo") comp = app.project.item(i);
        if (comp) {
            var sources = [];
            for (var l = 1; l <= comp.numLayers; l++)
                if (comp.layer(l).source && comp.layer(l).name == "Relight depth") sources.push(comp.layer(l).source);
            comp.remove();
            lines.push("Relight Demo を削除");
            for (var s = 0; s < sources.length; s++)
                if (sources[s].usedIn.length == 0) { sources[s].remove(); lines.push("使われていない深度の素材を削除"); }
        } else lines.push("Relight Demo は無い");
    } catch (err) {
        lines.push("ERROR: " + err.toString());
    }
    app.endSuppressDialogs(false);
    var f = new File("D:/video_projects/relight_explainer/demo_cleanup.log"); f.encoding = "UTF-8"; f.open("w"); f.write(lines.join("\n") + "\n"); f.close();
})();
