// Injected into build/ui_resign.html by tests/ui_replay.sh resign: pause, resign and the bots'
// concession (forced by making the engine report a 5% win estimate), results into #tlog.
(function () {
  const out = document.createElement("pre"); out.id = "tlog";
  out.style.cssText = "position:fixed;left:0;top:0;z-index:99;background:#000;color:#0f0;font:12px monospace;margin:0;padding:6px;max-width:60vw;white-space:pre-wrap";
  document.body.appendChild(out);
  const log = (t) => { out.textContent += t + "\n"; };
  window.addEventListener("error", (e) => log("PAGEERROR " + e.message + " @" + e.lineno));
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const pump = () => {
    if (S.paused) return;
    if (S.phase === "botaim" && S.botMove) { const m = S.botMove; S.phi = m.d.phi; S.botShot = m.d; S.botMove = null; startStroke(); }
    if (S.phase === "stroke") { stroke = null; fireShot(); }
    if (S.phase === "moving") { while (E.advance(0.5)) {} finishShot(); }
  };
  const until = async (f, n = 600) => { for (let t = 0; t < n && !f(); t++) { pump(); await sleep(25); } return f(); };
  let lowFor = -1;   // player whose decisions report a 5% win estimate (-1 nobody)
  (async () => {
    try {
      while (!E || !$("loading").hidden) await sleep(50);
      const E0 = E;
      E = Object.assign({}, E0, { botThink: (...a) => { E0.botThink(...a); if (lowFor === 2 || S.game.turn === lowFor) OUT[7] = 0.05; } });
      $("gSnk").click(); setEffort(150);

      // pause: the bots don't move while paused
      startGame("watch"); togglePause();
      const n0 = S.rec.moves.length; await sleep(1500);
      log(`paused: phase ${S.phase}, strokes ${S.rec.moves.length - n0}, msg "${$("msg").textContent}", button "${$("bPause").textContent}", resign hidden ${$("bResign").hidden}`);
      togglePause();
      log(`resumed: bot to move ${await until(() => S.rec.moves.length >= 1)}, msg "${$("msg").textContent}"`);

      // bots concede to each other on their second low estimate in a row
      lowFor = 2; startGame("watch");
      await until(() => S.phase === "over", 2000);
      log(`watch: phase ${S.phase}, strokes ${S.rec.moves.length}, result "${S.rec.result}", over "${$("overTitle").textContent}: ${$("overText").textContent}"`);

      // versus the bot: it offers once, you play on, then you resign yourself
      lowFor = 1; startGame("bot");
      log(`bot game: resign visible ${!$("bResign").hidden}`);
      let offers = 0;
      for (let t = 0; t < 4000 && S.phase !== "over" && S.rec.moves.length < 30; t++) {
        if (S.phase === "aim" && S.game.turn === 0) { S.phi = Math.PI * (t % 7) / 3.5; S.power = 0.4; startStroke(); }
        else if (S.phase === "choice") $("chBtns").querySelector("button").click();
        else if (S.phase === "offer") {
          offers++;
          log(`offer: "${$("chTitle").textContent}" / ${$("chRef").textContent.slice(0, 70)} / buttons: ${[...$("chBtns").children].map((b) => b.textContent).join(" | ")}`);
          [...$("chBtns").children].find((b) => b.textContent.startsWith("Play on")).click();
          log(`played on: phase ${S.phase}`);
        }
        pump(); await sleep(25);
      }
      log(`offers ${offers}, strokes ${S.rec.moves.length}, phase ${S.phase}`);
      await until(() => S.phase === "aim");
      $("bResign").click(); log(`first click: "${$("bResign").textContent}", phase ${S.phase}`);
      $("bResign").click();
      E.sync();
      log(`resigned: phase ${S.phase}, engine winner ${OUT[7]}, result "${S.rec.result}", last note "${S.rec.moves[S.rec.moves.length - 1].c}"`);
      const r = repParse(repText(S.rec));
      log(`replay text keeps it: Result "${r.result}"`);
      log("DONE");
    } catch (e) { log("ERROR " + e.stack); }
  })();
})();
