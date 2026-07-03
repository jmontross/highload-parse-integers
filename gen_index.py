#!/usr/bin/env python3
"""Generate index.html — a HighLoad.fun-styled leaderboard of OUR variants,
ranked against a static snapshot of the real parse_integers leaderboard.

Usage: gen_index.py <results.tsv> <out.html> <expected_sum>

results.tsv rows: NAME<TAB>SECONDS<TAB>STATUS   (SECONDS is '-' for fails)

The champion's measured wall-time is inserted into the snapshot by nanoseconds
so you can see, at a glance, where we'd land and how far we are from RANK 18.

NOTE ON COMPARABILITY: the numbers we measure are `./prog < input.txt` best-of-N
wall-clock on THIS machine. On the owner's Apple-Silicon Mac these are NOT
comparable to the x86 judge (no AVX, different core). The meaningful index.html
is the one produced by the x86 cloud run. The banner states the arch so a local
preview is never mistaken for a real standing.
"""
import sys, os, subprocess, platform, html

N_LINES = 50_000_000

# --- Static snapshot of the real leaderboard (ranks 11-21), captured 2026-07-03.
# cols: rank, user, tier, score, compiler, wall_ns, mem_bytes. Update as it moves.
SNAPSHOT = [
    (11, "Danylo Mocherniuk", "Apprentice", 4558, "g++13.3.0",   65_048_742, 2_400_256),
    (12, "vahook",            "Skilled",    4566, "g++13.3.0",   62_794_723, 2_416_640),
    (13, "caando",            "Apprentice", 4616, "g++13.3.0",   65_457_349, 2_473_984),
    (14, "Jack Frigaard",     "Apprentice", 4679, "clang++18.1.3",67_202_103, 2_519_040),
    (15, "Mikołaj Uzarski",   "Skilled",    5112, "clang++18.1.3",65_550_791, 2_277_376),
    (16, "Maya Rebholz",      "Skilled",    5259, "g++10.5.0",   53_341_520, 2_117_632),
    (17, "Matt Stuchlik",     "Apprentice", 5288, "clang++10.0.0",109_689_379,2_289_664),
    (18, "olliecrow",         "Master",     5400, "g++13.3.0",   69_301_147, 2_301_952),
    (19, "flavio87",          "Apprentice", 5560, "g++14.2.0",   55_454_846, 2_113_536),
    (20, "Adam Folwarczny",   "Skilled",    5694, "g++14.2.0",   67_209_826, 2_117_632),
    (21, "lipapipa",          "Skilled",    5812, "g++13.3.0",   0,          0),
]
TARGET_RANK = 18
TARGET_NS = 69_301_147  # olliecrow's shown wall-time — our bar to clear

BASELINE_NSL = 11.2   # scalar/SWAR ns/line — the start of the climb
TARGET_NSL   = 1.4    # ≈69 ms / 50M lines — the rank-18 bar (x86 PROXY, judge is truth)
ARM_FLOOR_MS = 82     # `cat input.txt > /dev/null` on the owner's ARM Mac (observed)

# The honest-win plan, rendered as a live checklist inside index.html.
# auto rules — only UNAMBIGUOUS signals tick a box:
#   "x86"       -> a cloud/x86 run has executed (proves the GitHub grant works)
#   "committed" -> already in the champion (a fact, not a measurement)
#   "nsle"      -> x86 champion ns/line <= target (a PROXY — confirm at the judge)
#   "manual"    -> never auto-ticks; has a crisp human/agent done-criterion instead
PLAN = [
  {"id": 0, "owner": "you", "when": "DO THIS NOW", "auto": "x86", "open": True,
   "title": "Unblock the cloud — grant the Claude GitHub App access to this repo",
   "crit": "A routine run completes on the x86 cloud box (this page then shows x86 timings instead of ARM).",
   "body": """
   <p>The repo is <b>private</b>, so every 2-hour cloud run currently dies with
   <code>HTTP 400 github_repo_access_denied</code> — the agent can't even clone it.
   Re-authorizing your GitHub <i>login</i> does NOT fix this; a private repo needs the
   Claude GitHub <b>App installation</b> granted access to this specific repo.</p>
   <ol>
     <li>Open <a href="https://github.com/settings/installations">github.com/settings/installations</a></li>
     <li>Click <b>Configure</b> next to the <b>Claude</b> (Anthropic) app.</li>
     <li>Under <b>Repository access</b>: choose <b>All repositories</b>, or
         <b>Only select repositories</b> → add <code>jmontross/highload-parse-integers</code>.</li>
     <li>Click the green <b>Save</b>.</li>
     <li><b>App not listed?</b> It may be installed on an organization — check that org's
         <i>Settings → GitHub Apps</i>, or reinstall from
         <a href="https://claude.ai/settings/connectors">claude.ai → Settings → Connectors → GitHub</a>.</li>
     <li><b>Verify:</b> ask Claude to re-trigger the routine — expect <code>HTTP 200</code>, not 400.
         Once a cloud run completes, this box ticks itself.</li>
   </ol>"""},

  {"id": 1, "owner": "agent", "when": "done", "auto": "committed", "open": False,
   "title": "SWAR number parse — break the per-digit latency chain",
   "crit": "Committed as champion v2 (~11% over scalar, portable, no intrinsics).",
   "body": """<p>The scalar champion is latency-bound on the serial <code>v = v*10 + d</code>
   chain (~11 ns/line). SWAR parses a whole number in a short shift/add tree instead.
   Modest but real, and it's the portable baseline the SIMD work builds on.</p>"""},

  {"id": 2, "owner": "agent", "when": "FIRST, on x86", "auto": "manual", "open": False,
   "title": "Read the bandwidth floor — set the honest ceiling before writing SIMD",
   "crit": "run.sh's floor is recorded. If the x86 floor > 69 ms, rank 18 is UNREACHABLE on that box — report that, don't write more SIMD.",
   "body": """<p><code>run.sh</code> prints a <b>bandwidth floor</b> (<code>cat &gt; /dev/null</code>) —
   the time to merely stream every byte once. A correct parser can never beat it.</p>
   <p>On the owner's ARM Mac that floor is <b>~82 ms &gt; the 69 ms target</b>, which is exactly
   why the win cannot be shown locally. The agent's first job on the x86 box is to read the
   floor there: if it's already above 69 ms, the honest finding is
   <i>"bandwidth-bound below target on this hardware"</i> — a real result, not a failure.</p>"""},

  {"id": 3, "owner": "agent", "when": "the main win", "auto": "manual", "open": False,
   "title": "Hybrid AVX2 — find newlines with movemask, parse each span with SWAR",
   "crit": "Correct (canonical sum + 9/9 edge cases) AND faster than the SWAR champion on x86, re-verified with RUNS=5, then promoted.",
   "body": """<p>The realistic, gettable-correct first SIMD milestone — it captures most of the win:</p>
   <ul>
     <li>Load 32 bytes: <code>_mm256_loadu_si256</code>.</li>
     <li>Newline mask: <code>_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, '\\n'))</code> → a 32-bit mask.</li>
     <li>Walk the set bits (<code>tzcnt</code>/<code>_tzcnt_u32</code>) to get each number's span, and parse
         that span with the SWAR routine. This removes the per-byte branch that dominates the scalar loop.</li>
     <li>Keep 2–4 independent <code>uint64</code> accumulators so the adds don't serialize.</li>
     <li>Guard with <code>#ifdef __AVX2__</code>; keep the scalar loop as the <code>#else</code> fallback.</li>
   </ul>
   <p>ARM COMPILE_FAIL on this variant is expected — it's validated on the x86 cloud box, which matches the judge.</p>"""},

  {"id": 4, "owner": "agent", "when": "stretch", "auto": "manual", "open": False,
   "title": "Full vectorized digit-weight reduction (staged madd tree / AVX-512 VNNI)",
   "crit": "Correct AND beats the hybrid champion. The variable-length boundary handling is the hard part — nail it against the correctness gate.",
   "body": """<p>The math: <code>sum = Σ digit · 10^(distance to the number's end)</code>, summed over
   <i>all</i> digit bytes — no need to isolate individual numbers, just weight each digit by its
   distance to the next newline.</p>
   <p><b>Direction, not gospel</b> (the agent nails the exact sequence empirically):</p>
   <ul>
     <li>You <b>cannot</b> do the whole reduction in one <code>vpdpbusd</code>/<code>maddubs</code> —
         int8/int16 weights overflow past two digits (10² &gt; 127). The real technique is a
         <b>staged tree</b>: <code>maddubs</code>[10,1] → <code>madd</code>[100,1] → <code>madd</code>[10000,1],
         which only works on <b>fixed-width, known-alignment</b> chunks.</li>
     <li>So the hard, valuable part is deriving per-byte 10^k weights from the newline mask and
         handling numbers that straddle a block boundary (carry into the next block).</li>
     <li>AVX-512: 64-byte loads, <code>__mmask64</code> from <code>_mm512_cmpeq_epi8_mask</code>,
         <code>_mm512_dpbusd_epi32</code> (VNNI) for the staged dot-products. Guard with
         <code>__AVX512F__/__AVX512BW__/__AVX512VNNI__</code> + scalar fallback.</li>
   </ul>"""},

  {"id": 5, "owner": "both", "when": "🎯 target", "auto": "nsle", "target": TARGET_NSL, "open": False,
   "title": "Champion beats ~1.4 ns/line on the cloud box",
   "crit": "x86 champion ≤ 1.4 ns/line — a PROXY for rank 18. Local hardware ≠ the judge; confirm only at the judge (next step).",
   "body": """<p>This ticks automatically once the x86 champion crosses ~1.4 ns/line. Treat it as a
   strong signal, not a standing — the cloud box is not the judge's exact hardware, so the real
   position is only known after submitting.</p>"""},

  {"id": 6, "owner": "you", "when": "ground truth", "auto": "manual", "open": False,
   "title": "Submit the champion to the judge & record the real time",
   "crit": "champion/main.cpp pasted into highload.fun, the judge's reported ns written into SCOREBOARD.md.",
   "body": """<p>The agent cannot submit (needs your logged-in browser). When it flags a champion ready:
   paste <code>champion/main.cpp</code> into the
   <a href="https://highload.fun/challenges/compute/parse_integers">judge</a>, run it, and record the
   real number in <code>SCOREBOARD.md</code>. That number — not any local time — is whether we hit rank 18.</p>"""},
]


def read_results(path):
    rows = []
    if os.path.exists(path):
        for line in open(path):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 3:
                name, secs, status = parts[0], parts[1], parts[2]
                rows.append((name, secs, status))
    return rows


def cxx_version():
    try:
        out = subprocess.check_output([os.environ.get("CXX", "c++"), "--version"],
                                      stderr=subprocess.STDOUT, text=True)
        return out.splitlines()[0].strip()
    except Exception:
        return "unknown"


def cur_time():
    # Date.now() is unavailable in some harnesses; use `date` which always works.
    try:
        return subprocess.check_output(["date", "-u", "+%Y-%m-%d %H:%M UTC"], text=True).strip()
    except Exception:
        return ""


def esc(s):
    return html.escape(str(s))


def render_plan(champ_nsl, is_x86):
    """Render the honest-win plan as a live checklist. Milestones auto-tick only
    on unambiguous signals; engineering phases show a crisp done-criterion."""
    owner_chip = {
        "you":   ('👤 you',    '#f0b429'),
        "agent": ('🤖 cloud agent', '#58a6ff'),
        "both":  ('🤖+👤',     '#a970ff'),
    }
    done_count = 0
    cards = []
    for p in PLAN:
        rule = p["auto"]
        if rule == "x86":
            done = is_x86
        elif rule == "committed":
            done = True
        elif rule == "nsle":
            done = bool(is_x86 and champ_nsl is not None and champ_nsl <= p["target"])
        else:  # manual
            done = False
        if done:
            done_count += 1
        box = "☑" if done else "☐"
        olabel, ocolor = owner_chip.get(p["owner"], ("?", "#8b949e"))
        opendisp = " open" if p.get("open") else ""
        cards.append(f"""
      <div class="task {'done' if done else ''}">
        <div class="thead">
          <span class="box">{box}</span>
          <span class="tnum">Phase {p['id']}</span>
          <span class="ttitle">{esc(p['title'])}</span>
          <span class="owner" style="color:{ocolor};border-color:{ocolor}55;background:{ocolor}18">{olabel}</span>
          <span class="when">{esc(p['when'])}</span>
        </div>
        <div class="crit"><b>Done when:</b> {esc(p['crit'])}</div>
        <details{opendisp}><summary>how</summary><div class="detail">{p['body']}</div></details>
      </div>""")

    # Progress toward the target — meaningful only on x86; labeled as a proxy.
    if is_x86 and champ_nsl:
        pct = max(0.0, min(100.0, (BASELINE_NSL - champ_nsl) / (BASELINE_NSL - TARGET_NSL) * 100))
        bar = f"""
      <div class="barwrap">
        <div class="barlabel">climb: {champ_nsl:.2f} ns/line → {TARGET_NSL} ns/line target
          <span class="dim">(x86 proxy — the judge is ground truth)</span></div>
        <div class="bar"><div class="fill" style="width:{pct:.0f}%"></div></div>
      </div>"""
    else:
        bar = f"""
      <div class="barwrap">
        <div class="barlabel dim">no x86 timing yet — progress is measured only on the cloud box.
          On this ARM Mac, merely reading the input (~{ARM_FLOOR_MS} ms) already exceeds the
          {TARGET_NS/1e6:.0f} ms target, so the win can't be shown here.</div>
      </div>"""

    return f"""
  <h2>🏁 Plan to an honest rank {TARGET_RANK} — {done_count}/{len(PLAN)} done</h2>
  <div class="reality">
    <b>Reality check.</b> The rank-{TARGET_RANK} bar is ≈{TARGET_NS/1e6:.0f} ms for a full 50M-line parse
    (~{TARGET_NSL} ns/line on x86). That is only physically reachable on the <b>x86 cloud box</b> —
    on the owner's ARM Mac just <i>reading</i> the input (~{ARM_FLOOR_MS} ms) already costs more than
    the whole target. And even on x86, local ns/line predicts leaderboard <i>position</i> only loosely:
    the one ground-truth number is submitting to the <b>judge</b> (Phase 6). Everything below is honest
    about which hardware proves what.
  </div>
  {bar}
  <div class="tasks">{''.join(cards)}</div>"""


def main():
    results_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/pi_results.tsv"
    out_path     = sys.argv[2] if len(sys.argv) > 2 else "index.html"
    expected     = sys.argv[3] if len(sys.argv) > 3 else "?"

    rows = read_results(results_path)
    arch = platform.machine()
    is_x86 = arch in ("x86_64", "amd64")
    compiler = cxx_version()
    when = cur_time()

    # Champion wall-time (best correct 'champion' row).
    champ_secs = None
    for name, secs, status in rows:
        if name == "champion" and status == "OK":
            try:
                champ_secs = float(secs)
            except ValueError:
                pass
    champ_ns = int(champ_secs * 1e9) if champ_secs else None
    champ_ns_per_line = (champ_ns / N_LINES) if champ_ns else None

    # Build combined ranking: snapshot rows + our champion, sorted by wall_ns.
    combined = []
    for r in SNAPSHOT:
        combined.append({"kind": "them", "rank": r[0], "user": r[1], "tier": r[2],
                         "score": r[3], "compiler": r[4], "wall_ns": r[5], "mem": r[6]})
    if champ_ns:
        combined.append({"kind": "us", "rank": None, "user": "★ our champion", "tier": "us",
                         "score": None, "compiler": compiler.split()[0] if compiler else "c++",
                         "wall_ns": champ_ns, "mem": None})
    # Sort by wall_ns; rows with 0/None wall_ns sink to the bottom.
    combined.sort(key=lambda x: x["wall_ns"] if x["wall_ns"] else 10**18)

    # gap to target
    if champ_ns:
        ratio = champ_ns / TARGET_NS
        if champ_ns <= TARGET_NS:
            gap = f"CLEARS rank {TARGET_RANK} bar ({champ_ns/1e6:.1f} ms ≤ {TARGET_NS/1e6:.1f} ms)"
            gap_cls = "good"
        else:
            gap = f"{ratio:.1f}× off the rank {TARGET_RANK} bar — need {champ_ns/1e6:.1f} ms → {TARGET_NS/1e6:.1f} ms"
            gap_cls = "bad"
    else:
        gap = "no correct champion timing yet"
        gap_cls = "bad"

    def tier_badge(tier):
        colors = {"Master": "#a970ff", "Skilled": "#3fb950", "Apprentice": "#8b949e", "us": "#f0b429"}
        c = colors.get(tier, "#8b949e")
        return f'<span class="tier" style="background:{c}22;color:{c};border:1px solid {c}55">{esc(tier)}</span>'

    # Leaderboard rows
    lb = []
    for row in combined:
        us = row["kind"] == "us"
        target = row.get("rank") == TARGET_RANK
        cls = "us" if us else ("target" if target else "")
        rank = "—" if row["rank"] is None else f"#{row['rank']}"
        score = "—" if row["score"] is None else f"{row['score']:,}"
        wall = f"{row['wall_ns']:,} ns" if row["wall_ns"] else "—"
        nspl = f"{row['wall_ns']/N_LINES:.2f} ns/line" if row["wall_ns"] else ""
        mem = f"{row['mem']:,} B" if row["mem"] else "—"
        lb.append(f"""
      <tr class="{cls}">
        <td class="rank">{rank}{' 🎯' if target else ''}</td>
        <td class="user">{esc(row['user'])} {tier_badge(row['tier'])}</td>
        <td class="score">{score}</td>
        <td class="cc">{esc(row['compiler'])}</td>
        <td class="wall">{wall}<span class="sub">{nspl}</span></td>
        <td class="mem">{mem}</td>
      </tr>""")

    # Our full variant table (all programs this run, incl. wrong/fail)
    vt = []
    ranked = sorted([r for r in rows if r[2] == "OK"], key=lambda r: float(r[1]))
    best = ranked[0][1] if ranked else None
    for name, secs, status in rows:
        ok = status == "OK"
        try:
            s = float(secs)
            nspl = f"{s*1e9/N_LINES:.2f}"
            secs_disp = f"{s*1000:.1f} ms"
        except ValueError:
            nspl, secs_disp = "—", "—"
        badge = {"OK": "ok", "WRONG": "bad", "COMPILE_FAIL": "bad"}.get(status, "bad")
        champ = " ★" if name == "champion" else ""
        vt.append(f"""
      <tr>
        <td class="user">{esc(name)}{champ}</td>
        <td>{secs_disp}</td>
        <td>{nspl} ns/line</td>
        <td><span class="badge {badge}">{esc(status)}</span></td>
      </tr>""")

    banner = "" if is_x86 else (
        '<div class="warn">⚠ Measured on <b>' + esc(arch) + '</b> (no AVX / not the judge core). '
        'These times are a local preview and are <b>not comparable</b> to the x86 leaderboard above. '
        'The real standing comes from the x86 cloud run.</div>')

    plan_html = render_plan(champ_ns_per_line, is_x86)

    doc = f"""<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>parse_integers — our run vs leaderboard</title>
<style>
  :root {{ --bg:#0d1117; --panel:#161b22; --line:#21262d; --fg:#e6edf3; --dim:#8b949e; --us:#f0b429; }}
  * {{ box-sizing:border-box; }}
  body {{ margin:0; background:var(--bg); color:var(--fg);
         font:14px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace; }}
  .wrap {{ max-width:960px; margin:0 auto; padding:28px 20px 60px; }}
  h1 {{ font-size:22px; margin:0 0 2px; }}
  h2 {{ font-size:15px; margin:34px 0 10px; color:var(--dim); text-transform:uppercase; letter-spacing:.05em; }}
  .meta {{ color:var(--dim); font-size:12px; margin-bottom:18px; }}
  .goal {{ display:flex; gap:14px; flex-wrap:wrap; margin:16px 0 4px; }}
  .card {{ background:var(--panel); border:1px solid var(--line); border-radius:10px;
           padding:12px 16px; flex:1; min-width:180px; }}
  .card .k {{ color:var(--dim); font-size:11px; text-transform:uppercase; letter-spacing:.05em; }}
  .card .v {{ font-size:20px; margin-top:4px; }}
  .good {{ color:#3fb950; }} .bad {{ color:#f85149; }}
  table {{ width:100%; border-collapse:collapse; background:var(--panel);
           border:1px solid var(--line); border-radius:10px; overflow:hidden; }}
  th,td {{ text-align:left; padding:10px 12px; border-bottom:1px solid var(--line); }}
  th {{ color:var(--dim); font-weight:600; font-size:11px; text-transform:uppercase; letter-spacing:.05em; }}
  tr:last-child td {{ border-bottom:0; }}
  td.rank {{ font-weight:700; color:var(--dim); white-space:nowrap; }}
  td.wall .sub {{ display:block; color:var(--dim); font-size:11px; }}
  td.score {{ font-weight:700; }}
  td.cc, td.mem {{ color:var(--dim); }}
  tr.target {{ background:#1f6feb18; }}
  tr.target td.rank {{ color:var(--us); }}
  tr.us {{ background:#f0b42918; outline:1px solid #f0b42955; }}
  tr.us td {{ color:var(--us); font-weight:700; }}
  .tier {{ font-size:10px; padding:1px 6px; border-radius:20px; margin-left:6px; }}
  .badge {{ font-size:11px; padding:2px 8px; border-radius:6px; }}
  .badge.ok {{ background:#3fb95022; color:#3fb950; }}
  .badge.bad {{ background:#f8514922; color:#f85149; }}
  .warn {{ background:#f0b42912; border:1px solid #f0b42944; color:#f0b429;
           padding:10px 14px; border-radius:8px; margin:14px 0; font-size:12.5px; }}
  a {{ color:#58a6ff; }}
  footer {{ color:var(--dim); font-size:11px; margin-top:30px; }}
  /* --- plan / checklist --- */
  .reality {{ background:#1f6feb14; border:1px solid #1f6feb44; border-radius:10px;
              padding:12px 16px; font-size:12.5px; line-height:1.6; margin:6px 0 14px; }}
  .barwrap {{ margin:6px 0 18px; }}
  .barlabel {{ font-size:12px; margin-bottom:6px; }}
  .barlabel .dim, .dim {{ color:var(--dim); }}
  .bar {{ height:10px; background:#21262d; border-radius:6px; overflow:hidden; }}
  .fill {{ height:100%; background:linear-gradient(90deg,#3fb950,#f0b429); }}
  .tasks {{ display:flex; flex-direction:column; gap:10px; }}
  .task {{ background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:12px 14px; }}
  .task.done {{ border-color:#3fb95055; background:#3fb9500c; }}
  .thead {{ display:flex; align-items:center; gap:9px; flex-wrap:wrap; }}
  .box {{ font-size:17px; }}
  .task.done .box {{ color:#3fb950; }}
  .tnum {{ color:var(--dim); font-size:11px; text-transform:uppercase; letter-spacing:.04em; }}
  .ttitle {{ font-weight:700; flex:1; min-width:200px; }}
  .owner {{ font-size:10px; padding:1px 7px; border-radius:20px; border:1px solid; white-space:nowrap; }}
  .when {{ font-size:10px; color:var(--dim); text-transform:uppercase; letter-spacing:.04em; }}
  .crit {{ font-size:12px; color:var(--dim); margin:7px 0 2px; }}
  .crit b {{ color:var(--fg); }}
  details {{ margin-top:6px; }}
  summary {{ cursor:pointer; color:#58a6ff; font-size:12px; width:max-content; }}
  .detail {{ font-size:12.5px; line-height:1.6; padding:8px 2px 2px; }}
  .detail ol, .detail ul {{ margin:6px 0; padding-left:20px; }}
  .detail li {{ margin:3px 0; }}
  .detail code {{ background:#21262d; padding:1px 5px; border-radius:4px; }}
</style></head>
<body><div class="wrap">
  <h1>parse_integers — chasing rank {TARGET_RANK} 🎯</h1>
  <div class="meta">Autonomous optimizer · updated {esc(when)} · sum <code>{esc(expected)}</code> · 50,000,000 lines ·
    <a href="https://highload.fun/challenges/compute/parse_integers/leaderboard">real leaderboard ↗</a></div>

  <div class="goal">
    <div class="card"><div class="k">Champion wall-time</div>
      <div class="v">{(f'{champ_ns/1e6:.1f} ms' if champ_ns else '—')}</div></div>
    <div class="card"><div class="k">Per line</div>
      <div class="v">{(f'{champ_ns_per_line:.2f} ns' if champ_ns_per_line else '—')}</div></div>
    <div class="card"><div class="k">Target (rank {TARGET_RANK})</div>
      <div class="v">{TARGET_NS/1e6:.1f} ms</div></div>
    <div class="card"><div class="k">Gap</div>
      <div class="v {gap_cls}" style="font-size:14px">{esc(gap)}</div></div>
  </div>
  {banner}
  {plan_html}

  <h2>Leaderboard — where we'd land</h2>
  <table>
    <tr><th>Rank</th><th>User</th><th>Score</th><th>Compiler</th><th>Wall</th><th>Mem</th></tr>
    {''.join(lb)}
  </table>

  <h2>This run — all variants</h2>
  <table>
    <tr><th>Program</th><th>Wall (best)</th><th>Per line</th><th>Status</th></tr>
    {''.join(vt) if vt else '<tr><td colspan=4>no results</td></tr>'}
  </table>

  <footer>Generated by <code>gen_index.py</code> on each <code>run.sh</code>.
    Snapshot of ranks 11–21 captured 2026-07-03; refresh <code>SNAPSHOT</code> as the board moves.
    Compiler: {esc(compiler)} · arch {esc(arch)}.</footer>
</div></body></html>"""

    with open(out_path, "w") as f:
        f.write(doc)
    print(f"wrote {out_path} (champion={'%.1f ms'%(champ_ns/1e6) if champ_ns else 'n/a'}, {gap})")


if __name__ == "__main__":
    main()
