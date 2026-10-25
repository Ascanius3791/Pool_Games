#!/usr/bin/env python3
"""snknet.py - train the neural snooker evaluator (engine/bot.h: snkNetForward / snkNetWin).

usage: python3 train/snknet.py --data PREFIX... --out net.bin [--init net.bin]
                               [--params snk_params.txt] [--table snk_table.bin]
                               [--lambda 0.7] [--epochs 8] [--threads 4]
PREFIX.x / PREFIX.meta come from train/bin/snkfeat. Targets are TD(lambda) returns built from
the search values of the following positions and the frame result (as in common.h: tdTargets).
Frames are split 90/10 into train/validation. The model mirrors the C++ forward pass exactly;
it starts as the linear evaluator (last layer zero) and is exported as float32 in the C++ layout.
Reports validation losses of the net and of the linear evaluator (pOld) per skill level and,
where PREFIX.fam exists (snkfeat from posrec version 3), per starting-position family.
"""
import argparse, os, struct, time, zlib
import numpy as np
import torch
import torch.nn as nn

NB, NG, HE, H1, H2 = 12, 23, 24, 48, 24
NP = 4 * HE + NG
NX = 21 * NB + NG
NMETA = 13
NW = HE * NB + HE + H1 * NP + H1 + H2 * H1 + H2 + 2 * H2 + 2 + 6
# starting-position families, as in train/positions.h (255 = unknown / written before version 3)
FAMILIES = ['break', 'random', 'snookers', 'lead', 'snookered', 'freeball', 'safety', 'clearance', 'black', 'perturbed']


def load_table(path):
    raw = open(path, 'rb').read()
    d0, nd, nr = struct.unpack('<3i', raw[:12])
    t = np.frombuffer(raw[12:], dtype=np.uint8).reshape(nr, nd).astype(np.float32) / 255.0
    return d0, torch.tensor(t)


class SnkNet(nn.Module):
    def __init__(self, lin, table):
        super().__init__()
        self.enc = nn.Linear(NB, HE)
        self.l1 = nn.Linear(NP, H1)
        self.l2 = nn.Linear(H1, H2)
        self.l3 = nn.Linear(H2, 2)
        nn.init.zeros_(self.l3.weight); nn.init.zeros_(self.l3.bias)
        self.lin = nn.Parameter(torch.tensor(lin, dtype=torch.float32))
        self.d0, self.tab = table
        self.nr, self.nd = self.tab.shape

    def forward(self, x, f):
        B = x.shape[0]
        bf = x[:, :21 * NB].view(B, 21, NB)
        gf = x[:, 21 * NB:]
        h = torch.relu(self.enc(bf)) * bf[:, :, :1]          # absent balls contribute nothing
        red, col = h[:, :15], h[:, 15:]
        z = torch.cat([red.sum(1) * 0.125, red.amax(1), col.sum(1) * 0.25, col.amax(1), gf], 1)
        out = self.l3(torch.relu(self.l2(torch.relu(self.l1(z)))))
        e, g = out[:, 0], out[:, 1]
        L = self.lin
        E = L[5] * (L[0] + L[1] * f[:, 2] * (f[:, 3] + f[:, 4]) + L[2] * f[:, 5] + L[3] * f[:, 6] + L[4] * f[:, 7]) + e
        xi = (f[:, 0] + E + self.d0).clamp(0, self.nd - 1)
        i = xi.detach().floor().clamp(max=self.nd - 2)
        fr = xi - i
        R = f[:, 1].long().clamp(0, self.nr - 1)
        i = i.long()
        p = (1 - fr) * self.tab[R, i] + fr * self.tab[R, i + 1]
        p = p.clamp(0.5 / 255, 254.5 / 255)
        return torch.sigmoid(torch.logit(p) * torch.exp(g.clamp(-3, 3)))

    def parts(self):   # C++ layout: weight matrices input-major (transposed torch weights)
        return [(self.enc.weight, True), (self.enc.bias, False), (self.l1.weight, True), (self.l1.bias, False),
                (self.l2.weight, True), (self.l2.bias, False), (self.l3.weight, True), (self.l3.bias, False), (self.lin, False)]

    def export(self, path):
        w = torch.cat([(p.detach().t() if tr else p.detach()).reshape(-1) for p, tr in self.parts()]).numpy().astype('<f4')
        assert w.size == NW, (w.size, NW)
        w.tofile(path)

    def load(self, path):
        w = torch.tensor(np.fromfile(path, dtype='<f4'))
        assert w.numel() == NW, (w.numel(), NW)
        k = 0
        for p, tr in self.parts():
            n = p.numel()
            v = w[k:k + n]
            p.data.copy_(v.view(p.shape[1], p.shape[0]).t() if tr else v.view_as(p)); k += n


def td_targets(meta, lam):
    """TD(lambda) return per position, mover's view. Rows are in frame order; consecutive rows
    with the same frame id are one frame. result = 1 if the mover won, so the winner's view of a
    position's value is value if result else 1 - value."""
    y = np.empty(len(meta), dtype=np.float32)
    frame, val, res = meta[:, 12], meta[:, 9], meta[:, 10]
    vw = np.where(res > 0.5, val, 1 - val)
    starts = np.flatnonzero(np.r_[True, frame[1:] != frame[:-1]])
    ends = np.r_[starts[1:], len(meta)]
    for s, e in zip(starts, ends):
        G = 1.0
        for t in range(e - 1, s - 1, -1):
            if t < e - 1:
                G = (1 - lam) * vw[t + 1] + lam * G
            y[t] = G if res[t] > 0.5 else 1 - G
    return y


def bce(p, y):
    p = np.clip(p, 1e-6, 1 - 1e-6)
    return -(y * np.log(p) + (1 - y) * np.log(1 - p))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', nargs='+', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--init')
    ap.add_argument('--params', default='train/baseline/snk_params.txt')
    ap.add_argument('--table', default='train/baseline/snk_table.bin')
    ap.add_argument('--lambda', dest='lam', type=float, default=0.7)
    ap.add_argument('--epochs', type=int, default=8)
    ap.add_argument('--lr', type=float, default=2e-3)
    ap.add_argument('--batch', type=int, default=512)
    ap.add_argument('--chunk', type=int, default=60000)
    ap.add_argument('--threads', type=int, default=4)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    torch.set_num_threads(a.threads)
    torch.manual_seed(a.seed)
    rng = np.random.default_rng(a.seed)

    # datasets: memory-mapped inputs, metadata + targets in memory
    sets = []
    for di, pre in enumerate(a.data):
        meta = np.fromfile(pre + '.meta', dtype='<f4').reshape(-1, NMETA)
        x = np.memmap(pre + '.x', dtype='<f2', mode='r', shape=(len(meta), NX))
        # frames with a search value or target outside [0, 1] (or NaN) are dropped, loudly
        badrow = ~((meta[:, 9] >= 0) & (meta[:, 9] <= 1))
        y = td_targets(meta, a.lam)
        badrow |= ~((y >= 0) & (y <= 1))
        if badrow.any():
            badf = np.unique(meta[badrow, 12])
            keep = ~np.isin(meta[:, 12], badf)
            v = meta[badrow, 9]
            print('WARNING %s: %d rows out of range (values %s .. %s, targets %s .. %s, results %s), dropping %d frames / %d rows'
                  % (pre, badrow.sum(), np.nanmin(v) if np.isfinite(v).any() else 'nan', np.nanmax(v) if np.isfinite(v).any() else 'nan',
                     np.nanmin(y[badrow]), np.nanmax(y[badrow]), np.unique(meta[badrow, 10])[:5], len(badf), (~keep).sum()), flush=True)
        else:
            keep = np.ones(len(meta), bool)
        fam = np.full(len(meta), 255, dtype=np.uint8)
        if os.path.exists(pre + '.fam') and os.path.getsize(pre + '.fam') == len(meta):
            fam = np.fromfile(pre + '.fam', dtype=np.uint8)
        fid = meta[:, 12].astype(np.int64)
        salt = zlib.crc32(os.path.basename(pre).encode())    # stable split when shards are added
        isval = ((fid * 2654435761 + salt) % 1000) < 100
        isval &= keep
        sets.append(dict(x=x, meta=meta, y=y, val=isval, fam=fam, keep=keep))
        print('%s: %d positions, %d frames, %d validation positions' % (pre, len(meta), fid.max() + 1, isval.sum()), flush=True)

    lin = [float(v) for v in open(a.params).read().split()]
    model = SnkNet(lin, load_table(a.table))
    if a.init:
        model.load(a.init)
    opt = torch.optim.Adam(model.parameters(), lr=a.lr, weight_decay=0)
    chunks = [(si, s0) for si, S in enumerate(sets) for s0 in range(0, len(S['meta']), a.chunk)]
    total_steps = a.epochs * sum(int((~S['val']).sum()) for S in sets) // a.batch
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=a.lr, total_steps=total_steps + 10, pct_start=0.1)

    def validate():
        model.eval()
        P, Y, R, O, K, FM = [], [], [], [], [], []
        with torch.no_grad():
            for si, s0 in chunks:
                S = sets[si]; s1 = min(s0 + a.chunk, len(S['meta']))
                m = S['val'][s0:s1]
                if not m.any():
                    continue
                xb = torch.from_numpy(np.asarray(S['x'][s0:s1])[m].astype(np.float32))
                mb = S['meta'][s0:s1][m]
                p = model(xb, torch.from_numpy(mb)).numpy()
                P.append(p); Y.append(S['y'][s0:s1][m]); R.append(mb[:, 10]); O.append(mb[:, 8]); K.append(mb[:, 11]); FM.append(S['fam'][s0:s1][m])
        model.train()
        P, Y, R, O, K, FM = map(np.concatenate, (P, Y, R, O, K, FM))
        rows = []
        groups = [('all', np.ones_like(K, bool))] + [('skill %.1f' % k, np.isclose(K, k)) for k in np.unique(K)]
        if (FM != 255).any():
            groups += [(FAMILIES[f] if f < len(FAMILIES) else 'unknown', FM == f) for f in np.unique(FM)]
        for name, sel in groups:
            rows.append('%-10s n=%7d  net: target %.4f result %.4f | linear: target %.4f result %.4f' % (
                name, sel.sum(), bce(P[sel], Y[sel]).mean(), bce(P[sel], R[sel]).mean(), bce(O[sel], Y[sel]).mean(), bce(O[sel], R[sel]).mean()))
        return bce(P, Y).mean(), rows, (bce(P, R).mean(), bce(O, Y).mean(), bce(O, R).mean())

    l0, rows, extra = validate()
    print('validation before training:'); print('\n'.join('  ' + r for r in rows), flush=True)
    bestL, bestExtra = l0, extra
    model.export(a.out)
    step, t0 = 0, time.time()
    for ep in range(a.epochs):
        order = rng.permutation(len(chunks))
        for ci in range(0, len(order), 2):          # two chunks mixed at a time
            X, F, Y = [], [], []
            for cj in order[ci:ci + 2]:
                si, s0 = chunks[cj]; S = sets[si]; s1 = min(s0 + a.chunk, len(S['meta']))
                m = ~S['val'][s0:s1] & S['keep'][s0:s1]
                X.append(np.asarray(S['x'][s0:s1])[m]); F.append(S['meta'][s0:s1][m]); Y.append(S['y'][s0:s1][m])
            X = np.concatenate(X)                      # float16; converted per batch (memory)
            F = torch.from_numpy(np.concatenate(F)); Y = torch.from_numpy(np.concatenate(Y))
            perm = torch.randperm(len(Y))
            for b0 in range(0, len(Y) - a.batch + 1, a.batch):
                idx = perm[b0:b0 + a.batch]
                xb = torch.from_numpy(X[idx.numpy()].astype(np.float32))
                p = model(xb, F[idx]).clamp(1e-6, 1 - 1e-6)
                loss = nn.functional.binary_cross_entropy(p, Y[idx])
                opt.zero_grad(); loss.backward(); opt.step()
                if step < total_steps + 9:
                    sched.step()
                step += 1
        lv, rows, extra = validate()
        print('epoch %d (%.0f s, %d steps): validation target loss %.4f' % (ep + 1, time.time() - t0, step, lv))
        print('\n'.join('  ' + r for r in rows), flush=True)
        if lv < bestL:
            bestL, bestExtra = lv, extra; model.export(a.out)
    print('RESULT snknet val_target_loss %.4f (linear %.4f) val_result_loss %.4f (linear %.4f)' % (bestL, bestExtra[1], bestExtra[0], bestExtra[2]))


if __name__ == '__main__':
    main()
