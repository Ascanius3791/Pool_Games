// Replay round trip against the real engine (node, no browser): bots play a frame through the
// same engine calls as the page (live, frame by frame, with human error), the page's own
// repText/repParse code writes and reads the record, and the replay must end bit-identical.
// usage: node tests/replay_node.js [snooker|8-ball] [budget 300] [seed 12345]   (after ./build.sh)
const fs = require("fs"), path = require("path");
const root = path.join(__dirname, "..");
const game = process.argv[2] || "snooker", budget = +(process.argv[3] || 300), seed = +(process.argv[4] || 12345);
const type = game === "snooker" ? 1 : 0;

(async () => {
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(path.join(root, "build/engine.wasm")), { env: {} });
  const E = instance.exports, OUT = new Float64Array(E.memory.buffer, E.outPtr(), 1024);
  // the page's replay text code, taken verbatim from the template
  const tpl = fs.readFileSync(path.join(root, "web/template.html"), "utf8");
  const code = tpl.slice(tpl.indexOf("const REP_FORMAT"), tpl.indexOf("function recNew("))
             + tpl.slice(tpl.indexOf("function repText("), tpl.indexOf("// ---- replay player"));
  const S = { type };
  const { repText, repParse } = new Function("S", "E", code + "\nreturn { repText, repParse };")(S, E);

  const noise = [0.00035, 0.000525, 0.0001505, 0.0001505, 0.0028, 0.002625, 0.000945, 0.000945, 1.6, 1.6, 1.6, 1.6, 0, 0, 0, 0, 0.003, 3];
  const setup = (s) => {
    E.newGame(s, 0, type);
    for (let i = 0; i < 18; i++) OUT[i] = noise[i];
    E.setNoiseProfile(); E.refSettings(0, 0.5); if (type) E.setNomRequired(1);
  };
  // game state: header, ball positions/flags/velocities (not the display orientation), snooker block
  const snap = () => {
    E.sync();
    const v = Array.from(OUT.subarray(0, 16)).filter((_, i) => i !== 14);
    for (let i = 0; i < OUT[0]; i++) { const o = 16 + 12 * i; v.push(OUT[o], OUT[o + 1], OUT[o + 2], OUT[o + 3], OUT[o + 8], OUT[o + 9]); }
    return type ? v.concat(Array.from(OUT.subarray(300, 320))) : v;
  };

  // ---- live game
  setup(seed);
  const rec = { game, seed, mode: "watch", players: ["Bot A", "Bot B"], humanError: "Pro", noise, effort: budget, ref: [0, 0.5], nom: 1, result: "", id: "test", date: "2026-10-25 00:00", moves: [] };
  let fouls = 0, choices = { 1: 0, 2: 0, 4: 0 }, t0 = Date.now();
  for (let n = 0; n < 400; n++) {
    E.sync(); if (OUT[7] >= 0) break;
    E.botThink(1, 1, budget);
    const d = { place: OUT[0], px: OUT[1], py: OUT[2], phi: OUT[3], V: OUT[4], a: OUT[5], b: OUT[6], value: OUT[7], tgt: OUT[16] };
    if (d.place) E.placeCue(d.px, d.py);
    E.sync();
    if (type && OUT[312] && d.tgt > 0) E.setFreeNom(d.tgt); else if (type && OUT[303] === 1 && d.tgt >= 16) E.setNominated(d.tgt);
    E.sync();
    const m = { p: OUT[1], est: d.value };
    if (OUT[5]) m.at = [OUT[16], OUT[17]];
    if (type && OUT[303] === 1 && OUT[334] >= 16) m.nom = OUT[334];
    if (type && OUT[312] && OUT[313] > 0) m.fb = OUT[313];
    E.shootV(d.phi, d.V, d.a, d.b, 1);
    m.s = [OUT[0], OUT[1], OUT[2], OUT[3]]; m.i = [d.phi, d.V, d.a, d.b];
    while (E.advance(1 / 60)) {}
    E.endShot(1, budget);
    E.sync();
    if (OUT[8]) fouls++;
    if (type && OUT[315] && OUT[7] < 0) { const o = OUT[329]; m.ch = o; choices[o]++; E.snkChoose(o); }
    E.sync();
    m.c = type ? `${OUT[300]}-${OUT[301]}` : "";
    rec.moves.push(m);
  }
  E.sync();
  rec.result = OUT[7] >= 0 ? `${rec.players[OUT[7]]} wins` : "*";
  const live = snap();
  const text = repText(rec);
  fs.writeFileSync(path.join(root, "build", `replay_test_${game}.txt`), text);

  // ---- replay from the text: instant (one advance per stroke) and frame by frame
  const r = repParse(text);
  const play = (frameDt) => {
    setup(r.seed);
    for (const m of r.moves) {
      if (m.at) E.placeCue(m.at[0], m.at[1]);
      if (m.nom) E.setNominated(m.nom);
      if (m.fb) E.setFreeNom(m.fb);
      E.shootV(m.s[0], m.s[1], m.s[2], m.s[3], 0);
      if (frameDt) { while (E.advance(frameDt)) {} } else E.advance(1e6);
      E.endShot(0, 0);
      if (m.ch) E.snkForceChoice(m.ch);
    }
    return snap();
  };
  const same = (a, b) => a.length === b.length && a.every((v, i) => Object.is(v, b[i]));
  const inst = play(0), frames = play(1 / 37);
  console.log(`${game}: ${rec.moves.length} strokes, ${fouls} fouls, choices play ${choices[1]} put back ${choices[2]} replace ${choices[4]}, result "${rec.result}", text ${text.length} bytes, ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  console.log(`replay instant: ${same(live, inst) ? "identical" : "DIFFERENT"}, frame by frame: ${same(live, frames) ? "identical" : "DIFFERENT"}`);
  const diff = (a, b) => a.map((v, i) => Object.is(v, b[i]) ? null : `[${i}] ${v} vs ${b[i]}`).filter(Boolean).slice(0, 8).join("; ");
  if (!same(live, inst) || !same(live, frames)) { console.log("instant:", diff(live, inst)); console.log("frames:", diff(live, frames)); process.exit(1); }
})();
