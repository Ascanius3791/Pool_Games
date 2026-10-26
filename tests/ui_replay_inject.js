// Injected into build/ui_replay.html by tests/ui_replay.sh: plays a short bot frame, saves and
// reloads its replay through the page's own UI functions, and writes the results into #tlog.
(function () {
  const out = document.createElement("pre"); out.id = "tlog";
  out.style.cssText = "position:fixed;left:0;top:0;z-index:99;background:#000;color:#0f0;font:12px monospace;margin:0;padding:6px;max-width:60vw;white-space:pre-wrap";
  document.body.appendChild(out);
  const log = (t) => { out.textContent += t + "\n"; };
  window.addEventListener("error", (e) => log("PAGEERROR " + e.message + " @" + e.lineno));
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  // no animation frames in headless virtual time: step the page's state machine by hand
  const pump = () => {
    if (S.phase === "botaim" && S.botMove) { const m = S.botMove; S.phi = m.d.phi; S.botShot = m.d; S.botMove = null; startStroke(); }
    if (S.phase === "stroke") { stroke = null; fireShot(); }
    if (S.phase === "moving") { while (E.advance(0.5)) {} finishShot(); }
  };
  const snap = () => { E.sync(); const v = []; for (let i = 0; i < OUT[0]; i++) { const o = 16 + 12 * i; v.push(OUT[o], OUT[o + 1], OUT[o + 2]); } for (let i = 300; i < 312; i++) v.push(OUT[i]); return v.join(","); };
  (async () => {
    try {
      while (!E || !$("loading").hidden) await sleep(50);
      $("gSnk").click(); $("optNoise").checked = true; setEffort(150);
      startGame("watch");
      for (let t = 0; t < 2000 && S.rec.moves.length < 14 && S.phase !== "over"; t++) { pump(); await sleep(25); }
      S.mode = "two";   // stop the bots
      for (let t = 0; t < 200 && S.phase !== "aim" && S.phase !== "over"; t++) { pump(); await sleep(25); }
      const live = snap(), n = S.rec.moves.length;
      log(`played ${n} strokes, score ${S.snk.score.join("-")}`);
      openReplays();
      log(`panel open: ${!$("repPanel").hidden}, save box: ${!$("repSaveBox").hidden}, text ${$("repText").value.length} chars`);
      $("repId").value = "ui test 1";
      await repSave();
      log(`save: "${$("repSaveMsg").textContent}"`);
      await sleep(100);
      if (window.UI_STOP === "panel") { out.remove(); return; }
      log(`library rows: ${document.querySelectorAll("#repList .repItem").length}, where: ${$("repWhere").textContent}`);
      const text = $("repText").value;
      closeReplays();
      $("repPaste").value = text;
      $("repLoadBtn").click();
      log(`replay mode: ${S.mode}, bar visible: ${!$("repBar").hidden}, note: "${$("repNote").textContent.slice(0, 80)}"`);
      if (window.UI_STOP === "bar") { repGoto(9); out.remove(); return; }
      repStep();
      for (let t = 0; t < 400 && S.phase !== "aim"; t++) { pump(); await sleep(25); }
      log(`after one animated stroke: ${$("rpPos").textContent}; note "${$("repNote").textContent.slice(0, 100)}"`);
      repGoto(n);
      log(`end of replay identical to the live game: ${snap() === live}`);
      repGoto(3); log(`seek to 3: ${$("rpPos").textContent}`);
      $("rpSeatA").value = "bot"; $("rpSeatB").value = "human"; $("rpSeatB").onchange();
      log(`setup note: ${$("rpSetup").textContent}`);
      repTakeover();
      log(`take over: mode ${S.mode}, A ${playerName(0)} (bot ${isBot(0)}), B ${playerName(1)} (bot ${isBot(1)}), record keeps ${S.rec.moves.length} strokes, players ${S.rec.players}, phase ${S.phase}, msg "${$("msg").textContent}"`);
      log("DONE");
    } catch (e) { log("ERROR " + e.stack); }
  })();
})();
