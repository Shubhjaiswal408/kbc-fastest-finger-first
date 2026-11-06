"""KBC swarm test: a full class of buzzers, played by 2 ESP32 boards running
tools/KBC_Swarm. Runs many rounds against the real host and checks every press,
the timing, the winner and the web page's speed.

    python tools/swarm_test.py --host 192.168.1.19 --ports COM22 COM27

Turns the host's test mode on, changes rules for the test, and puts everything
back (and removes the test buzzers) when it's done.
"""
import argparse, json, random, statistics as st, sys, threading, time, urllib.parse, urllib.request
import serial

ap = argparse.ArgumentParser()
ap.add_argument("--host", default="192.168.1.19")
ap.add_argument("--ports", nargs="+", default=["COM22", "COM27"])
ap.add_argument("--per-board", type=int, default=15)
ap.add_argument("--rounds", type=float, default=1.0, help="scale the number of rounds")
ap.add_argument("--browsers", type=int, default=4)
ap.add_argument("--only", default="", help="run only scenarios whose name contains this")
args = ap.parse_args()
H = "http://" + args.host
random.seed(7)


api_retries, slow = [0], []
_local = threading.local()


def api(path, data=None, timeout=5):
    """One request over a kept-alive connection (like a browser); retried if it drops.
    Every reply slower than 1 s is recorded with its time."""
    import http.client
    body = urllib.parse.urlencode(data) if data is not None else None
    for attempt in range(4):
        t = time.time()
        try:
            c = getattr(_local, "c", None)
            if c is None:
                c = _local.c = http.client.HTTPConnection(args.host, 80, timeout=timeout)
            if body is None: c.request("GET", path)
            else: c.request("POST", path, body, {"Content-Type": "application/x-www-form-urlencoded"})
            r = c.getresponse(); raw = r.read()
            if (r.getheader("Connection") or "").lower() == "close": c.close(); _local.c = None
            dt = time.time() - t
            if dt > 1: slow.append((time.strftime("%H:%M:%S"), path, round(dt, 1)))
            if r.status == 503: raise ConnectionError("busy " + raw[:60].decode(errors="replace"))
            return json.loads(raw)
        except (ConnectionError, OSError, http.client.HTTPException, ValueError) as e:
            slow.append((time.strftime("%H:%M:%S"), path, f"failed: {type(e).__name__} {e}"[:90]))
            try: _local.c.close()
            except Exception: pass
            _local.c = None
            if attempt == 3: raise
            api_retries[0] += 1
            time.sleep(0.3)


def state():
    return api("/api/state")


# ---------------------------------------------------------------- swarm boards
class Board:
    def __init__(self, port, first, count):
        self.port, self.first, self.count = port, first, count
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 921600, 0.1
        self.s.dtr = False; self.s.rts = False          # don't reset the board on open
        self.s.open()
        self.lines, self.linked, self.acks = [], set(), []
        threading.Thread(target=self._read, daemon=True).start()

    def setup(self, kinds):
        """Configure the sims; retries until the board confirms (it may be booting)."""
        for _ in range(20):
            n = len(self.lines)
            self.cmd(f"C {self.count} {self.first}")
            time.sleep(0.5)
            if any(t.startswith("OK C") for _, t in self.lines[n:]):
                for i in range(self.first, self.first + self.count):
                    self.cmd(f"Y {i} {kinds[i]}")
                return True
        return False

    def _read(self):
        buf = b""
        while True:
            buf += self.s.read(4096)
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                t = ln.decode(errors="replace").strip()
                self.lines.append((time.time(), t))
                p = t.split()
                if p[:1] == ["L"]: self.linked.add(int(p[1]))
                if p[:1] == ["A"]: self.acks.append((int(p[1]), int(p[2]), int(p[3])))

    def cmd(self, line):
        self.s.write((line + "\n").encode())

    def owns(self, i):
        return self.first <= i < self.first + self.count


# ---------------------------------------------------------------- browsers
lat, web_err, stop, blk = [], [0], [False], []


def browser():
    """Polls like the web page: every 200 ms, reusing one connection (keep-alive)."""
    import http.client
    c = None
    while not stop[0]:
        t = time.perf_counter()
        try:
            if c is None: c = http.client.HTTPConnection(args.host, 80, timeout=4)
            c.request("GET", "/api/state")
            r = c.getresponse(); raw = r.read()
            lat.append((time.perf_counter() - t) * 1000)
            if r.status == 200 and len(blk) < 100000:
                try: blk.append(json.loads(raw).get("blk", 0) or 0)
                except ValueError: web_err[0] += 1
            if (r.getheader("Connection") or "").lower() == "close": c.close(); c = None
        except Exception:
            web_err[0] += 1; c = None
        time.sleep(0.2)


# ---------------------------------------------------------------- rounds
def run_round(boards, name, plan, lim, correct, teacher_lock_at=None):
    """plan: {simId: (delayMs or None, choice)}"""
    for b in boards:
        for i in range(b.first, b.first + b.count):
            d = plan.get(i)
            b.cmd(f"P {i} {d[0]} {d[1]}" if d and d[0] is not None else f"P {i} x")
    time.sleep(0.4)
    api("/api/start", {"q": f"swarm test: {name}", "o0": "A", "o1": "B", "o2": "C", "o3": "D",
                       "lim": lim, "ans": correct})
    t0 = time.time()
    opened = None
    while time.time() - t0 < lim + 12:
        s = state()
        if s["phase"] == 2 and opened is None: opened = time.time()
        if teacher_lock_at and opened and time.time() - opened >= teacher_lock_at and s["phase"] == 2:
            api("/api/lock", {})
        if s["phase"] >= 3: break
        time.sleep(0.05)
    api("/api/reveal", {"ans": correct})        # host waits 0.3 s grace itself
    for _ in range(60):
        s = state()
        if s["phase"] == 4: break
        time.sleep(0.05)
    time.sleep(0.2)
    s = state()
    api("/api/reset", {})
    return s


def evaluate(s, plan, correct, kinds):
    lock_ms = s["elapsed"]
    sims = {int(p["n"][4:]): p for p in s["players"] if p["n"].startswith("Sim ")}
    res = dict(expected=0, ok=0, lost=0, wrongstatus=0, extra=0, edge=0, errs_b=[], errs_u=[], winner_ok=None)
    accepted = []
    for i, (d, ch) in plan.items():
        p = sims.get(i)
        a = p and p["a"]
        if d is None:
            if a: res["extra"] += 1
            continue
        if d < 0:                                   # countdown press -> false start
            res["expected"] += 1
            if a and a["st"] == 2: res["ok"] += 1
            elif not a: res["lost"] += 1
            else: res["wrongstatus"] += 1
            continue
        if abs(d - lock_ms) <= 30:                  # right at the lock: either is fine
            res["edge"] += 1
            if a and a["st"] == 1: accepted.append((d, i, a))
            continue
        res["expected"] += 1
        if d < lock_ms:
            if a and a["st"] == 1:
                res["ok"] += 1
                accepted.append((d, i, a))
                (res["errs_b"] if kinds[i] == "b" else res["errs_u"]).append(a["t"] / 1000 - d)
            elif not a: res["lost"] += 1
            else: res["wrongstatus"] += 1
        else:                                       # after the lock -> must be refused
            if not a: res["ok"] += 1
            else: res["wrongstatus"] += 1
    good = sorted((d, i) for d, i, a in accepted if plan[i][1] == correct)
    if good:
        ok_ids = {i for d, i in good if d - good[0][0] <= 5}     # within 5 ms = a tie
        win = s["winner"]
        wid = next((int(p["n"][4:]) for p in s["players"] if p["m"] == win and p["n"].startswith("Sim ")), None)
        res["winner_ok"] = wid in ok_ids
    return res


def plan_normal(ids, rng=(500, 8000), pc=0.5, correct=0):
    return {i: (random.randint(*rng), correct if random.random() < pc else random.choice([c for c in range(4) if c != correct])) for i in ids}


def main():
    boards, first = [], 0
    for port in args.ports:
        boards.append(Board(port, first, args.per_board)); first += args.per_board
    N = first
    ids = list(range(N))
    kinds = {i: ("u" if i % 5 == 4 else "b") for i in ids}     # 1 in 5 is a USB buzzer
    orig = state()["set"]
    print(f"host {state()['fw']} | {N} test buzzers on {len(boards)} boards "
          f"({sum(k == 'b' for k in kinds.values())} battery, {sum(k == 'u' for k in kinds.values())} USB)")

    api("/api/test", {"on": 1, "clear": 1})
    api("/api/settings", {"mode": 0, "cd": 3, "auto": 1, "fs": 1, "acc": 1})
    time.sleep(1.0)
    for b in boards:
        if not b.setup(kinds):
            print(f"{b.port}: swarm board not answering"); cleanup(orig); return
    t0 = time.time()
    while time.time() - t0 < 40 and sum(len(b.linked) for b in boards) < N:
        time.sleep(0.5)
    linked = sum(len(b.linked) for b in boards)
    pair_s = time.time() - t0
    print(f"paired {linked}/{N} in {pair_s:.1f} s")
    if linked < N:
        print("not all paired, stopping"); cleanup(orig); return

    for _ in range(args.browsers):
        threading.Thread(target=browser, daemon=True).start()

    R = lambda n: max(1, round(n * args.rounds))
    scen = []
    for _ in range(R(8)):
        c = random.randrange(4); scen.append(("normal", plan_normal(ids, correct=c), 12, c, None))
    for _ in range(R(8)):
        c = random.randrange(4); scen.append(("burst: 30 presses in 40 ms", plan_normal(ids, (1500, 1540), correct=c), 10, c, None))
    for _ in range(R(4)):
        c = random.randrange(4); p = plan_normal(ids[:20], (500, 3500), correct=c)
        p.update({i: (random.randint(3900, 3975), c) for i in ids[20:25]})
        p.update({i: (random.randint(4080, 4300), c) for i in ids[25:30]})
        scen.append(("time-limit edge", p, 4, c, None))
    for _ in range(R(4)):
        c = random.randrange(4); p = plan_normal(ids, correct=c)
        p.update({i: (random.randint(-700, -300), c) for i in random.sample(ids, 5)})
        scen.append(("false starts", p, 12, c, None))
    for _ in range(R(3)):
        c = random.randrange(4); p = plan_normal(ids, correct=c)
        for i in random.sample(ids, 10): p[i] = (None, 0)
        scen.append(("10 absent", p, 6, c, None))
    for _ in range(R(3)):
        c = random.randrange(4); p = plan_normal(ids, (500, 8000), correct=c)
        p.update({i: (random.randint(3850, 3960), c) for i in random.sample(ids, 6)})
        scen.append(("teacher locks at 4 s", p, 20, c, 4.0))

    if args.only: scen = [x for x in scen if args.only in x[0]]
    totals, failed_rounds = {}, []
    t_start = time.time()
    for n, (name, plan, lim, c, tl) in enumerate(scen, 1):
        try:
            s = run_round(boards, name, plan, lim, c, tl)
        except Exception as e:
            print(f"[{n:2}/{len(scen)}] {name:<28} ROUND FAILED: {type(e).__name__} {e}")
            failed_rounds.append((n, name, str(e)))
            try: api("/api/reset", {})
            except Exception: pass
            continue
        r = evaluate(s, plan, c, kinds)
        T = totals.setdefault(name, dict(rounds=0, expected=0, ok=0, lost=0, wrongstatus=0, extra=0, edge=0,
                                         errs_b=[], errs_u=[], win=0, wins=0))
        T["rounds"] += 1
        for k in ("expected", "ok", "lost", "wrongstatus", "extra", "edge"): T[k] += r[k]
        T["errs_b"] += r["errs_b"]; T["errs_u"] += r["errs_u"]
        if r["winner_ok"] is not None: T["wins"] += 1; T["win"] += r["winner_ok"]
        flag = "" if (r["lost"] == r["wrongstatus"] == r["extra"] == 0 and r["winner_ok"] in (True, None)) else "  <-- CHECK"
        print(f"[{n:2}/{len(scen)}] {name:<28} ok {r['ok']}/{r['expected']}  lost {r['lost']}  wrong {r['wrongstatus']}"
              f"  winner {'ok' if r['winner_ok'] else ('-' if r['winner_ok'] is None else 'WRONG')}{flag}")

    stop[0] = True
    dur = time.time() - t_start
    print("\n================ RESULTS ================")
    allok = allexp = 0
    for name, T in totals.items():
        e = [abs(x) for x in T["errs_b"]]; u = [abs(x) for x in T["errs_u"]]
        allok += T["ok"]; allexp += T["expected"]
        print(f"{name:<28} rounds {T['rounds']:2}  correct outcome {T['ok']}/{T['expected']}  lost {T['lost']}"
              f"  wrong {T['wrongstatus']}  winner right {T['win']}/{T['wins']}")
        if e: print(f"{'':<28} timing error battery: median {st.median(e):.1f} ms, max {max(e):.1f} ms")
        if u: print(f"{'':<28} timing error USB:     median {st.median(u):.1f} ms, max {max(u):.1f} ms")
    retries = [t for b in boards for (_, s_, t) in b.acks if s_ != 0]
    noreply = sum(1 for b in boards for (_, s_, _) in b.acks if s_ == 0)
    print(f"\nall rounds: {allok}/{allexp} correct outcomes in {dur/60:.1f} min, rounds failed: {len(failed_rounds)}")
    if retries: print(f"sends per press: average {st.mean(retries):.2f}, max {max(retries)}  | presses with no reply: {noreply}")
    if lat:
        L = sorted(lat)
        print(f"web ({args.browsers} browsers polling): {len(L)} requests, median {st.median(L):.0f} ms, "
              f"95% {L[int(len(L)*0.95)]:.0f} ms, worst {L[-1]:.0f} ms, failed {web_err[0]}")
    print(f"test-script requests retried: {api_retries[0]}")
    for x in slow[:15]: print("   slow/failed request:", *x)
    if lat:
        print(f"browser replies over 1 s: {sum(1 for x in lat if x > 1000)}")
    s = state()
    print(f"host free memory at end: {s['heap']} bytes (largest block {s.get('blk')}), lowest block seen {min(blk) if blk else '?'}, uptime {s['up']} s")
    cleanup(orig)


def cleanup(orig):
    stop[0] = True
    try:
        api("/api/settings", {k: orig[k] for k in ("mode", "cd", "lim", "auto", "fs", "acc")})
        api("/api/test", {"on": 0, "clear": 1})
        print("host settings restored, test buzzers removed")
    except Exception as e:
        print("cleanup failed:", e)


if __name__ == "__main__":
    main()
