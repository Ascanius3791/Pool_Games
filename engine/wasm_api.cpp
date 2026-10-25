// wasm_api.cpp — C ABI for the browser game (compiled with clang --target=wasm32)
// One engine, two games: TYPE 0 = 8-ball, TYPE 1 = snooker.
#include "bot.h"
#include "../train/out/std_weights.h"
#include "../train/out/noisy_weights.h"
#include "../train/out/snk_eval.h"
#include "../train/out/snk_net.h"

extern "C" {
void* memcpy(void* d, const void* s, unsigned long n) {
    unsigned char* a = (unsigned char*)d; const unsigned char* b = (const unsigned char*)s;
    while (n--) *a++ = *b++;
    return d;
}
void* memset(void* d, int c, unsigned long n) {
    unsigned char* a = (unsigned char*)d;
    while (n--) *a++ = (unsigned char)c;
    return d;
}
void* memmove(void* d, const void* s, unsigned long n) {
    unsigned char* a = (unsigned char*)d; const unsigned char* b = (const unsigned char*)s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
}

using namespace pool;

static int TYPE = 0;
static Game8 G8;
static GameSnk GS;
static GameSnk PRE;            // snooker position as struck (for the miss rule / replacing balls)
static ShotParams INTENDED;    // the stroke as intended, before human error
static RefInfo REF;
static int REF_MODE = 0;       // 0 judged by player profile, 1 pro rules
static double REF_TAU = 0.5;
static double CHOICE_VALS[3];
static int CHOICE_BEST = 1;
static TableVis VIS;
static NoiseModel NM;
static Rng shotRng;
static double OUT[1024];
static double accum = 0;
static EvEngine LIVE;          // the shot in progress (event-driven), advanced frame by frame
static int liveOn = 0;

static Sim& SIM() { return TYPE ? GS.sim : G8.sim; }

#define EXPORT(name) extern "C" __attribute__((export_name(#name)))

EXPORT(outPtr) double* outPtr() { return OUT; }

EXPORT(newGame) void newGame(double seed, int practice, int type) {
    TYPE = type;
    static Table tmp;
    if (TYPE) { newGameSnk(GS, (u64)seed, practice); TableSpec sp; snookerSpec(sp); buildTable(tmp, GS.sim.P, sp, &VIS); }
    else { newGame8(G8, (u64)seed, practice); TableSpec sp; poolSpec(sp); buildTable(tmp, G8.sim.P, sp, &VIS); }
    defaultNoise(NM);
    shotRng.seed((u64)seed * 7919 + 17);
    accum = 0;
    liveOn = 0;
}

// Visible state -> OUT. Header [0..15], balls from 16 (12 doubles each), snooker block at 300.
EXPORT(sync) int sync() {
    const Sim& s = SIM();
    if (TYPE) {
        const GameSnk& g = GS;
        OUT[0] = s.n; OUT[1] = g.turn; OUT[2] = 0; OUT[3] = 0;
        OUT[4] = g.isBreak; OUT[5] = g.ballInHand; OUT[6] = 1; OUT[7] = g.winner;
        OUT[8] = g.lastFoul; OUT[9] = g.keptTurn; OUT[10] = g.shots; OUT[15] = g.practice;
        OUT[300] = g.score[0]; OUT[301] = g.score[1]; OUT[302] = g.brk; OUT[303] = g.phase;
        OUT[304] = g.nextColour; OUT[305] = g.foulPts; OUT[306] = g.lastPts; OUT[307] = snkRemaining(g);
        OUT[308] = snkRedsOn(g); OUT[309] = g.highBreak[0]; OUT[310] = g.highBreak[1]; OUT[311] = g.blackGame;
        OUT[312] = g.freeBall; OUT[313] = g.freeNom; OUT[314] = g.freeAvail; OUT[315] = g.pendingChoice;
        OUT[316] = g.lastMiss; OUT[317] = g.offender; OUT[318] = g.missCount[0]; OUT[319] = g.missCount[1];
        OUT[320] = REF.checked; OUT[321] = REF.reason; OUT[322] = REF.direct; OUT[323] = REF.pStar; OUT[324] = REF.pPlayed;
        OUT[325] = REF.escType; OUT[326] = REF.escR1; OUT[327] = REF.escR2; OUT[328] = REF.forfeit;
        OUT[329] = CHOICE_BEST; OUT[330] = CHOICE_VALS[0]; OUT[331] = CHOICE_VALS[1]; OUT[332] = CHOICE_VALS[2];
        OUT[333] = snkNeedsSnookers(g, g.turn);
        OUT[334] = g.nominated; OUT[335] = g.nomRequired;
    } else {
        const Game8& g = G8;
        OUT[0] = s.n; OUT[1] = g.turn; OUT[2] = g.group[0]; OUT[3] = g.group[1];
        OUT[4] = g.isBreak; OUT[5] = g.ballInHand; OUT[6] = g.kitchenOnly; OUT[7] = g.winner;
        OUT[8] = g.lastFoul; OUT[9] = g.keptTurn; OUT[10] = g.shots; OUT[15] = g.practice;
    }
    OUT[11] = s.P.R; OUT[12] = s.T.L; OUT[13] = s.T.W; OUT[14] = s.t;
    for (int i = 0; i < s.n; ++i) {
        const Ball& b = s.b[i]; double* o = OUT + 16 + 12 * i;
        o[0] = b.x; o[1] = b.y; o[2] = b.on; o[3] = b.pocket;
        o[4] = b.qw; o[5] = b.qx; o[6] = b.qy; o[7] = b.qz;
        o[8] = b.vx; o[9] = b.vy; o[10] = b.wz; o[11] = 0;
    }
    return s.n;
}

// Table: [nseg, npt, npk, ncont], segments(6), noses(3), pockets(8), contours(n, x0,y0,...)
EXPORT(tableInfo) int tableInfo() {
    const Table& T = SIM().T;
    int k = 0;
    OUT[k++] = T.nseg; OUT[k++] = T.npt; OUT[k++] = T.npk; OUT[k++] = VIS.n;
    for (int i = 0; i < T.nseg; ++i) { const Seg& g = T.seg[i]; OUT[k++] = g.ax; OUT[k++] = g.ay; OUT[k++] = g.bx; OUT[k++] = g.by; OUT[k++] = g.nx; OUT[k++] = g.ny; }
    for (int i = 0; i < T.npt; ++i) { OUT[k++] = T.ptx[i]; OUT[k++] = T.pty[i]; OUT[k++] = T.ptr[i]; }
    for (int i = 0; i < T.npk; ++i) { const Pocket& p = T.pk[i]; OUT[k++] = p.cx; OUT[k++] = p.cy; OUT[k++] = p.r; OUT[k++] = p.mx; OUT[k++] = p.my; OUT[k++] = p.nx; OUT[k++] = p.ny; OUT[k++] = p.halfw; }
    for (int c = 0; c < VIS.n; ++c) { OUT[k++] = VIS.cnt[c]; for (int i = 0; i < VIS.cnt[c]; ++i) { OUT[k++] = VIS.x[c][i]; OUT[k++] = VIS.y[c][i]; } }
    return k;
}

// Snooker markings: baulk x, D radius, colour spots (yellow..black)
EXPORT(snookerGeo) void snookerGeo() {
    SnkGeo G; snkGeo(GS.sim.T, G);
    OUT[0] = G.baulkX; OUT[1] = G.dR;
    for (int i = 0; i < 6; ++i) { OUT[2 + 2 * i] = G.spotX[16 + i]; OUT[3 + 2 * i] = G.spotY[16 + i]; }
}

EXPORT(powerToSpeed) double jsPowerToSpeed(double p) { return powerToSpeed(SIM().P, p); }
EXPORT(vmax) double jsVmax() { return SIM().P.vmax; }
EXPORT(simDt) double jsSimDt() { return SIM_DT; }

EXPORT(setNoise) void setNoise(double skill) { NM.skill = skill; }
// Full human-error profile from OUT: lo[4], hi[4], k[4], mu[4], missProb, missMul
EXPORT(setNoiseProfile) void setNoiseProfile() {
    for (int c = 0; c < 4; ++c) { NM.lo[c] = OUT[c]; NM.hi[c] = OUT[4 + c]; NM.k[c] = OUT[8 + c]; NM.mu[c] = OUT[12 + c]; }
    NM.missProb = OUT[16]; NM.missMul = OUT[17]; NM.skill = 1;
}
// sigma of every channel at a power-bar setting -> OUT[0..3], biases OUT[4..7], speed OUT[8]
EXPORT(noiseSigmas) void noiseSigmas(double power) {
    double V = powerToSpeed(SIM().P, power), s = V / SIM().P.vmax;
    for (int c = 0; c < 4; ++c) { OUT[c] = noiseSigma(NM, c, s); OUT[4 + c] = NM.mu[c]; }
    OUT[8] = V;
}

static void shootParams(ShotParams sp, int noisy) {
    ShotParams ex; int miscue;
    if (TYPE) { PRE = GS; INTENDED = sp; REF.checked = 0; REF.reason = 0; }
    if (TYPE) miscue = executeShot(GS, sp, noisy != 0, NM, shotRng, &ex);
    else miscue = executeShot(G8, sp, noisy != 0, NM, shotRng, &ex);
    OUT[0] = ex.phi; OUT[1] = ex.V; OUT[2] = ex.a; OUT[3] = ex.b; OUT[4] = miscue;
    accum = 0;
    LIVE.logOn = 0; evInit(LIVE, SIM()); liveOn = 1;
}
EXPORT(shoot) void shoot(double phi, double power, double a, double b, int noisy) {
    ShotParams sp; sp.phi = phi; sp.V = powerToSpeed(SIM().P, power); sp.a = a; sp.b = b;
    shootParams(sp, noisy);
}
EXPORT(shootV) void shootV(double phi, double V, double a, double b, int noisy) {
    ShotParams sp; sp.phi = phi; sp.V = V; sp.a = a; sp.b = b;
    shootParams(sp, noisy);
}

// snooker evaluator from training (train/export.py); legacy points evaluation if not accepted yet.
// The neural evaluator judges positions for the human-error level of the current profile.
static const SnkEval* snkEvaluator(int noisy) {
#if SNK_USE_TABLE
    static SnkEval ev; static int init = 0;
    if (!init) {
        snkEvalDefaults(ev);
        ev.a0 = SNK_PARAMS[0]; ev.a1 = SNK_PARAMS[1]; ev.a2 = SNK_PARAMS[2]; ev.a3 = SNK_PARAMS[3]; ev.a4 = SNK_PARAMS[4]; ev.eScale = SNK_PARAMS[5];
        ev.table = SNK_TABLE; ev.tD0 = SNK_TD0; ev.tND = SNK_TND; ev.tNR = SNK_TNR; init = 1;
#if SNK_USE_NET
        ev.net = SNK_NET;
#endif
    }
    ev.skill = snkSkillOf(NM, noisy != 0);
    return &ev;
#else
    (void)noisy;
    return 0;
#endif
}
// which snooker evaluator the bots use: 0 legacy points, 1 win table, 2 neural (values are win chances for 1 and 2)
EXPORT(snkEvalKind) int snkEvalKind() { return SNK_USE_TABLE ? (SNK_USE_NET ? 2 : 1) : 0; }
static BotCtx BOT;
static u64 botSeed = 1;
// Bot decision -> OUT: place, px, py, phi, V, a, b, value, sims, cands,
//                      full, partial, secant, refine, switches, phase-1 sims
EXPORT(botThink) void botThink(int noisy, int useNN, double budget) {
    botDefaults(BOT, noisy != 0, (useNN && !TYPE) ? (noisy ? NN_NOISY : NN_STD) : 0, botSeed++);
    BOT.nm = NM;     // the bot plays with the same unsteady arm
    BOT.refMode = REF_MODE; BOT.refTau = REF_TAU;
    if (TYPE) BOT.snk = snkEvaluator(noisy);
    BOT.budget = budget;
    BotDecision d;
    if (TYPE) pool::botThink(BOT, GS, d); else pool::botThink(BOT, G8, d);
    OUT[0] = d.place; OUT[1] = d.px; OUT[2] = d.py; OUT[3] = d.sp.phi; OUT[4] = d.sp.V;
    OUT[5] = d.sp.a; OUT[6] = d.sp.b; OUT[7] = d.value; OUT[8] = d.sims; OUT[9] = d.nCand;
    OUT[10] = d.nFull; OUT[11] = d.nPartial; OUT[12] = d.nSecant; OUT[13] = d.nRefine; OUT[14] = d.nSwitch; OUT[15] = d.phase1;
    OUT[16] = d.tgt;
}

// Advance the shot in progress by frameDt seconds (event-driven: all events up to that
// time are processed, then every ball is placed on its analytic trajectory). The result does
// not depend on how the time is split into frames, so it is identical to the bots' sims.
// Returns 1 while moving.
EXPORT(advance) int advance(double frameDt) {
    Sim& s = SIM();
    s.nSnd = 0;
    if (!liveOn) {
        if (!anyMoving(s)) return 0;
        LIVE.logOn = 0; evInit(LIVE, s); liveOn = 1;
    }
    int moving = evRun(LIVE, s, LIVE.tDisp + frameDt, 0);
    if (!moving) liveOn = 0;
    OUT[600] = s.nSnd;
    for (int i = 0; i < s.nSnd; ++i) {
        OUT[601 + 4 * i] = s.snd[i].type; OUT[602 + 4 * i] = s.snd[i].strength;
        OUT[603 + 4 * i] = s.snd[i].x; OUT[604 + 4 * i] = s.snd[i].y;
    }
    return moving;
}

// noisy/budget: how the computer judges the foul options (a quarter of the budget per option;
// budget 0 skips that, e.g. when a replay supplies the choice)
EXPORT(endShot) void endShot(int noisy, double budget) {
    Sim& s = SIM();
    liveOn = 0;
    for (int i = 0; i < s.n; ++i) { Ball& b = s.b[i]; b.vx = b.vy = b.wx = b.wy = b.wz = 0; }
    s.ev.tEnd = s.t;
    const Events e = s.ev;
    if (TYPE) {
        endShotSnk(GS);
        snkReferee(PRE, GS, INTENDED, NM, REF_MODE, REF_TAU, botSeed * 977 + 13, REF);
        CHOICE_BEST = CH_PLAY; CHOICE_VALS[0] = CHOICE_VALS[1] = CHOICE_VALS[2] = -1e9;
        if (GS.pendingChoice && budget > 0) {
            BotCtx c; botDefaults(c, noisy != 0, 0, botSeed++); c.nm = NM; c.budget = budget;
            c.refMode = REF_MODE; c.refTau = REF_TAU; c.snk = snkEvaluator(noisy);
            CHOICE_BEST = snkChooseOption(c, GS, PRE, CHOICE_VALS);
        }
    } else endShot8(G8);
    OUT[0] = e.firstHit; OUT[1] = e.nPot;
    for (int k = 0; k < e.nPot; ++k) OUT[2 + k] = e.pot[k];
}

static bool placeOkNow(double x, double y, int self) {
    if (TYPE) return placeValid(GS, x, y, self, self == 0);
    return placeValid(G8, x, y, self, self == 0 && G8.kitchenOnly != 0);
}
// ---- snooker: referee settings, foul options, free ball
EXPORT(refSettings) void refSettings(int mode, double tau) { REF_MODE = mode; REF_TAU = tau; }
EXPORT(snkChoose) void snkChoose(int opt) { snkApplyChoice(GS, PRE, opt); }
// replays: apply the recorded choice even if this referee would not have offered it
EXPORT(snkForceChoice) void snkForceChoice(int opt) { GS.pendingChoice |= opt; snkApplyChoice(GS, PRE, opt); }
// colour nomination (after a red)
// player p concedes the game (frame)
EXPORT(resign) void resign(int p) {
    if (TYPE) { if (GS.winner < 0) { GS.winner = 1 - p; GS.pendingChoice = 0; } }
    else if (G8.winner < 0) G8.winner = 1 - p;
}
EXPORT(setNomRequired) void setNomRequired(int on) { GS.nomRequired = on ? 1 : 0; }
EXPORT(setNominated) int setNominated(int id) {
    if (!TYPE || GS.phase != SNK_COLOUR_ON || id < 16 || id > 21 || !GS.sim.b[id].on) return 0;
    GS.nominated = id; return 1;
}
// Is the intended nomination obvious? Plays the stroke N times with 3x the profile's slips,
// only until first contact. OUT[0] = number of distinct first balls, then pairs (ball id, count),
// id -1 = nothing hit.
EXPORT(nomGuess) int nomGuess(double phi, double power, double a, double b, int N) {
    NoiseModel nm = NM; nm.skill = NM.skill * 3;
    int ids[24], cnt[24], nd = 0;
    for (int k = 0; k < N; ++k) {
        Rng r; r.seed(424242ULL + 7919ULL * (u64)k);
        ShotParams sp; sp.phi = phi; sp.V = powerToSpeed(GS.sim.P, power); sp.a = a; sp.b = b;
        applyNoise(nm, GS.sim.P, r, sp);
        Sim s = GS.sim; s.trackOrient = 0;
        strike(s, sp.phi, sp.V, sp.a, sp.b);
        simulateFirstHit(s, 12);
        int fh = s.ev.firstHit;
        int j = 0; while (j < nd && ids[j] != fh) ++j;
        if (j == nd && nd < 24) { ids[nd] = fh; cnt[nd] = 0; nd++; }
        if (j < 24) cnt[j]++;
    }
    OUT[0] = nd;
    for (int j = 0; j < nd; ++j) { OUT[1 + 2 * j] = ids[j]; OUT[2 + 2 * j] = cnt[j]; }
    return nd;
}
EXPORT(setFreeNom) int setFreeNom(int id) {
    if (!TYPE || !GS.freeBall || id <= 0 || id >= GS.sim.n || !GS.sim.b[id].on) return 0;
    GS.freeNom = id; return 1;
}
// the computer's favourite target for the player to move (used to nominate a free ball)
EXPORT(botFavourite) int botFavourite(int noisy, double budget) {
    BotCtx c; botDefaults(c, noisy != 0, 0, botSeed++); c.nm = NM; c.budget = budget;
    c.refMode = REF_MODE; c.refTau = REF_TAU;
    if (TYPE) c.snk = snkEvaluator(noisy);
    BotDecision d;
    if (TYPE) pool::botThink(c, GS, d); else pool::botThink(c, G8, d);
    return d.tgt;
}

EXPORT(placeValid) int jsPlaceValid(double x, double y) { return placeOkNow(x, y, 0) ? 1 : 0; }
EXPORT(placeCue) int placeCue(double x, double y) {
    if (!placeOkNow(x, y, 0)) return 0;
    Ball& c = SIM().b[0];
    double qw = c.qw, qx = c.qx, qy = c.qy, qz = c.qz;
    resetBall(c, x, y);
    c.qw = qw; c.qx = qx; c.qy = qy; c.qz = qz;
    return 1;
}
EXPORT(headStringX) double jsHeadString() { return headStringX(G8.sim.T); }

EXPORT(aim) int aim(double phi, double a) {
    AimInfo ai; aimInfo(SIM(), phi, a, ai);
    OUT[0] = ai.type; OUT[1] = ai.target; OUT[2] = ai.gx; OUT[3] = ai.gy;
    OUT[4] = ai.ox; OUT[5] = ai.oy; OUT[6] = ai.cx; OUT[7] = ai.cy; OUT[8] = ai.dirx; OUT[9] = ai.diry;
    return ai.type;
}

// Practice helpers
EXPORT(moveBall) int moveBall(int id, double x, double y) {
    Sim& s = SIM();
    if (id < 0 || id >= s.n) return 0;
    bool ok = TYPE ? placeValid(GS, x, y, id, false) : placeValid(G8, x, y, id, false);
    if (!ok) return 0;
    Ball& b = s.b[id];
    double qw = b.qw, qx = b.qx, qy = b.qy, qz = b.qz;
    resetBall(b, x, y);
    b.qw = qw; b.qx = qx; b.qy = qy; b.qz = qz;
    return 1;
}
EXPORT(respotAll) void respotAll() {
    if (TYPE) { int p = GS.practice; u64 sd = GS.rng.next(); newGameSnk(GS, sd, p); }
    else { rack8(G8); G8.isBreak = 1; G8.ballInHand = 1; G8.kitchenOnly = 1; }
}
