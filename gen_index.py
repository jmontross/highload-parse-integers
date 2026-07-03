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
