#!/usr/bin/env python3
"""The walker's MOVEMENT POLICY, learnt from the best players (2026-11-09; the user: "[the best player] simply moves more nimble,
and engages faster ... our walker does not move with that much *authority* and confidence"; "this is a m4 mac so make sure the special
GPU is being used ... M4 has AI cores... use them for walker logic").

Behaviour cloning on the match recordings (match-recorder.py, 10 Hz): for every player sample in a fight (the nearest enemy within 25
units), the player's next move in the ENEMY-LINE frame (u = toward the enemy, v = to the left of that line) and whether it jumps, from
what it saw: distance, height, both shields / healths, whether the enemy looks at it, its own view against the line, its last two moves,
the enemy's last move, a second enemy, enemies within 10 units, its weapon class. Samples are WEIGHTED by the player's match: the top
fifth of each match by (kills + 1) / (deaths + 1) count 1, everyone else NHOOK-free 0.15 (--others) -- "mimic the best players". The
walker's own slot ("you" in its recordings) is left out. Trained on the GPU (PyTorch MPS) when there is one. Writes a small JSON the
walker's brain reads live (nbot.c pol_*): normalisation, the layers, the validation numbers.
    python3 shim/tools/policy-train.py [--dirs DIR,DIR] [--out FILE] [--epochs 8] [--max-files N] [--others 0.15] [--cpu]"""
import glob, gzip, json, math, os, random, sys, time, zlib

def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d
SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
DIRS = arg('--dirs', ','.join([os.path.join(SUP, 'tracks'), os.path.join(SUP, 'battles', 'follow', 'tracks')])).split(',')
OUT = arg('--out', os.path.join(SUP, 'battles', 'follow', 'policy.json'))
EPOCHS = int(arg('--epochs', '8')); MAXF = int(arg('--max-files', '400')); OTHERS = float(arg('--others', '0.15'))
CLS = ['auto', 'precision', 'shotgun', 'sniper', 'rocket', 'melee', 'other']
KEYS = [('sniper', 'sniper'), ('beam_rifle', 'sniper'), ('rocket', 'rocket'), ('spartan_laser', 'rocket'), ('fuel_rod', 'rocket'), ('flak_cannon', 'rocket'),
        ('missile', 'rocket'), ('shotgun', 'shotgun'), ('mauler', 'shotgun'), ('excavator', 'shotgun'), ('energy_sword', 'melee'), ('energy_blade', 'melee'),
        ('hammer', 'melee'), ('battle_rifle', 'precision'), ('carbine', 'precision'), ('magnum', 'precision'), ('assault_rifle', 'auto'), ('smg', 'auto'),
        ('plasma_rifle', 'auto'), ('spike_rifle', 'auto'), ('spiker', 'auto'), ('needler', 'auto'), ('brute_shot', 'auto'), ('machinegun', 'auto'), ('plasma_pistol', 'auto'), ('sentinel', 'auto')]
def wcls(w):
    w = (w or '').lower().replace(' ', '_')
    for k, c in KEYS:
        if k in w: return c
    return 'other'
FEAT = ['dist/10', 'dz', 'my_shield', 'my_health', 'en_shield', 'en_health', 'en_faces_me', 'my_view_cos', 'my_view_sin', 'my_u1', 'my_v1', 'my_u2', 'my_v2',
        'en_u1', 'en_v1', 'e2_dist/10', 'e2_cos', 'e2_sin', 'near10/4', 'my_dz1'] + ['w_' + c for c in CLS] + ['my_shield_d3', 'aimed_at_me/3']
# (11:45: +2 -- under fire: the shield's change over the last 0.3 s, and how many enemies within 25 units look within 15 deg of the player)

def load(path):
    S, you = [], None
    try:
        with gzip.open(path, 'rt') as f:
            for line in f:
                try: d = json.loads(line)
                except Exception: continue
                if 'p' in d: S.append((d['t'], {r[0]: r for r in d['p']})); you = d.get('you', you)
    except (OSError, EOFError, zlib.error): pass   # (a recording still being written / cut short: what was read counts)
    return S, you

def samples(S, you):
    """(features, target, weight) for every fighting sample of one recording"""
    if len(S) < 50: return []
    kd = {}
    for _, P in S:
        for sl, r in P.items():
            if len(r) > 13: kd[sl] = (max(kd.get(sl, (0, 0))[0], r[12] or 0), max(kd.get(sl, (0, 0))[1], r[13] or 0))
    ranked = sorted([sl for sl, (k, d) in kd.items() if k + d >= 5 and sl != you], key=lambda s: -(kd[s][0] + 1) / (kd[s][1] + 1))
    top = set(ranked[:max(1, len(ranked) // 5)])
    out = []
    for i in range(2, len(S) - 2):
        t0, P0 = S[i]
        if not (0.07 < S[i + 1][0] - t0 < 0.14 and 0.07 < t0 - S[i - 1][0] < 0.14): continue
        for sl, r in P0.items():
            if sl == you or len(r) < 12 or not r[2] or r[11]: continue   # (alive, on foot)
            rows = [S[i + k][1].get(sl) for k in (-2, -1, 1, 2)]; r3 = S[i - 3][1].get(sl) if i >= 3 else None
            if any(q is None or not q[2] or q[11] for q in rows): continue
            pm2, pm1, p1, p2 = rows; x, y, z = r[3], r[4], r[5]
            if max(math.hypot(a[3] - b[3], a[4] - b[4]) for a, b in ((pm2, pm1), (pm1, r), (r, p1))) > 1.2: continue   # (a respawn / teleport)
            en = [(math.hypot(q[3] - x, q[4] - y), q) for s2, q in P0.items() if s2 != sl and len(q) > 9 and q[2] and q[1] != r[1]]
            en = sorted([e for e in en if e[0] < 25], key=lambda e: e[0])
            if not en: continue
            dE, E = en[0]
            if dE < 0.3: continue
            ux, uy = (E[3] - x) / dE, (E[4] - y) / dE; vx, vy = -uy, ux
            uv = lambda dx, dy: (10 * (dx * ux + dy * uy), 10 * (dx * vx + dy * vy))
            m1 = uv(x - pm1[3], y - pm1[4]); m2 = uv(pm1[3] - pm2[3], pm1[4] - pm2[4])
            Ep = S[i - 1][1].get(E[0]); e1 = uv(E[3] - Ep[3], E[4] - Ep[4]) if Ep and Ep[2] else (0.0, 0.0)
            ey = E[6]; ef = math.cos(ey) * (x - E[3]) / dE + math.sin(ey) * (y - E[4]) / dE
            my = r[6]; vc = math.cos(my) * ux + math.sin(my) * uy; vs = math.cos(my) * vx + math.sin(my) * vy
            if len(en) > 1: d2, E2 = en[1]; c2 = ((E2[3] - x) * ux + (E2[4] - y) * uy) / max(d2, 1e-3); s2 = ((E2[3] - x) * vx + (E2[4] - y) * vy) / max(d2, 1e-3)
            else: d2, c2, s2 = 30.0, 0.0, 0.0
            n10 = sum(1 for e in en if e[0] < 10)
            wc = [0.0] * len(CLS); wc[CLS.index(wcls(r[10]))] = 1.0
            sd3 = ((r[8] or 0) - (r3[8] or 0)) if r3 and r3[2] else 0.0
            aimd = 0
            for dq, q in en:
                if dq < 0.3: continue
                cq = (math.cos(q[6]) * (x - q[3]) + math.sin(q[6]) * (y - q[4])) / dq
                if cq > 0.9659: aimd += 1   # (cos 15 deg)
            f = [dE / 10, E[5] - z, r[8] or 0, r[9] or 0, E[8] or 0, E[9] or 0, ef, vc, vs, m1[0], m1[1], m2[0], m2[1], e1[0], e1[1], d2 / 10, c2, s2, n10 / 4, 10 * (z - pm1[5])] + wc + [sd3, aimd / 3]
            tg = uv(p1[3] - x, p1[4] - y); jump = 1.0 if (p2[5] - z) > 0.35 and (p1[5] - z) > 0.12 else 0.0
            out.append((f, [max(-4.0, min(4.0, tg[0])), max(-4.0, min(4.0, tg[1])), jump], 1.0 if sl in top else OTHERS))
    return out

def main():
    t0 = time.time(); files = []
    for d in DIRS: files += glob.glob(os.path.join(d, '*.jsonl.gz'))
    files = sorted(set(files))[-MAXF:]; random.seed(7); random.shuffle(files)
    nval = max(1, len(files) // 10); val_f, tr_f = files[:nval], files[nval:]
    def build(fs):
        X, Y, W = [], [], []
        for p in fs:
            S, you = load(p)
            for f, y, w in samples(S, you): X.append(f); Y.append(y); W.append(w)
        return X, Y, W
    Xt, Yt, Wt = build(tr_f); Xv, Yv, Wv = build(val_f)
    print('%d recordings (%d validation): %d training samples (%d from the best players), %d validation; %.0f s to read' % (
        len(files), nval, len(Xt), sum(1 for w in Wt if w >= 1), len(Xv), time.time() - t0), flush=True)
    if len(Xt) < 5000: print('too few samples'); return 1
    import torch
    dev = torch.device('cpu' if '--cpu' in sys.argv or not torch.backends.mps.is_available() else 'mps')
    Xt_, Yt_, Wt_ = torch.tensor(Xt), torch.tensor(Yt), torch.tensor(Wt); Xv_, Yv_, Wv_ = torch.tensor(Xv), torch.tensor(Yv), torch.tensor(Wv)
    mu, sd = Xt_.mean(0), Xt_.std(0).clamp(min=1e-3)
    nf = Xt_.shape[1]
    # (11:05: a MIXTURE of K Gaussians over the move (--mdn K, default 5): a player strafes left OR right -- one mean move is the
    #  timid average of both; the walker samples a real move from the mixture every 0.1 s. Outputs: K logits, K x 2 means, K x 2 log
    #  sigmas, the jump logit. --mdn 0 = the single mean move (3 outputs))
    K = int(arg('--mdn', '5')); nout = 3 if K == 0 else K * 5 + 1
    net = torch.nn.Sequential(torch.nn.Linear(nf, 128), torch.nn.SiLU(), torch.nn.Linear(128, 128), torch.nn.SiLU(), torch.nn.Linear(128, nout)).to(dev)
    opt = torch.optim.AdamW(net.parameters(), lr=2e-3, weight_decay=1e-4)
    def mean_move(o):
        if K == 0: return o[:, :2]
        pi = torch.softmax(o[:, :K], 1); m = o[:, K:3 * K].reshape(-1, K, 2); return (pi.unsqueeze(2) * m).sum(1)
    def loss_of(X, Y, W):
        o = net((X - mu.to(dev)) / sd.to(dev)); mse = ((mean_move(o) - Y[:, :2]) ** 2).sum(1)
        bce = torch.nn.functional.binary_cross_entropy_with_logits(o[:, -1], Y[:, 2], reduction='none')
        if K == 0: return ((mse + 0.5 * bce) * W).sum() / W.sum(), (mse * W).sum() / W.sum()
        lp = torch.log_softmax(o[:, :K], 1); m = o[:, K:3 * K].reshape(-1, K, 2); ls = o[:, 3 * K:5 * K].reshape(-1, K, 2).clamp(-3, 2)
        z = (Y[:, None, :2] - m) / ls.exp(); comp = lp + (-0.5 * z ** 2 - ls - 0.9189385).sum(2); nll = -torch.logsumexp(comp, 1)
        return ((nll + 0.5 * bce) * W).sum() / W.sum(), (mse * W).sum() / W.sum()
    Xg, Yg, Wg = Xt_.to(dev), Yt_.to(dev), Wt_.to(dev); n = Xg.shape[0]; bs = 4096
    t1 = time.time()
    for ep in range(EPOCHS):
        perm = torch.randperm(n, device=dev); net.train(); tot = 0.0
        for k in range(0, n, bs):
            ix = perm[k:k + bs]; l, _ = loss_of(Xg[ix], Yg[ix], Wg[ix]); opt.zero_grad(); l.backward(); opt.step(); tot += float(l) * len(ix)
        net.eval()
        with torch.no_grad(): _, vm = loss_of(Xv_.to(dev), Yv_.to(dev), Wv_.to(dev))
        print('epoch %d: training loss %.3f, validation move error %.3f (u/s)^2' % (ep + 1, tot / n, float(vm)), flush=True)
    # baselines on the validation set: standing still, and repeating the last move
    Yv2, Wv2 = Yv_[:, :2], Wv_
    still = float(((Yv2 ** 2).sum(1) * Wv2).sum() / Wv2.sum()); rep = float((((Yv2 - Xv_[:, 9:11]) ** 2).sum(1) * Wv2).sum() / Wv2.sum())
    with torch.no_grad():
        o = net((Xv_.to(dev) - mu.to(dev)) / sd.to(dev)).cpu(); jp = torch.sigmoid(o[:, -1]); jy = Yv_[:, 2]
        jprec = float(((jp > 0.5) & (jy > 0.5)).sum()) / max(1.0, float((jp > 0.5).sum())); jrec = float(((jp > 0.5) & (jy > 0.5)).sum()) / max(1.0, float((jy > 0.5).sum()))
    layers = [m for m in net if isinstance(m, torch.nn.Linear)]
    J = {'version': 1, 'trained': time.strftime('%Y-%m-%d %H:%M:%S'), 'device': str(dev), 'train_s': round(time.time() - t1, 1), 'features': FEAT, 'classes': CLS,
         'out': ['toward_u_s', 'left_u_s', 'jump_logit'] if K == 0 else ['mixture logits x%d' % K, 'means x%d x2' % K, 'log sigmas x%d x2' % K, 'jump_logit'], 'mdn': K, 'act': 'silu', 'mean': [round(v, 6) for v in mu.tolist()], 'std': [round(v, 6) for v in sd.tolist()],
         'layers': [{'W': [[round(v, 6) for v in row] for row in L.weight.detach().cpu().tolist()], 'b': [round(v, 6) for v in L.bias.detach().cpu().tolist()]} for L in layers],
         'samples': len(Xt), 'best_samples': sum(1 for w in Wt if w >= 1), 'recordings': len(files),
         'val_move_err': round(float(vm), 4), 'val_still_err': round(still, 4), 'val_repeat_err': round(rep, 4), 'jump_precision': round(jprec, 3), 'jump_recall': round(jrec, 3)}
    tmp = OUT + '.tmp'; json.dump(J, open(tmp, 'w')); os.replace(tmp, OUT)
    print('policy -> %s on %s in %.0f s: move error %.3f vs standing still %.3f, vs repeating the last move %.3f; jumps precision %.2f recall %.2f' % (
        OUT, dev, time.time() - t1, float(vm), still, rep, jprec, jrec), flush=True)
    return 0

if __name__ == '__main__':
    sys.exit(main())
