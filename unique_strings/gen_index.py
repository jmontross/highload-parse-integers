#!/usr/bin/env python3
"""Generate index.html for the unique_strings challenge — a HighLoad.fun-styled
view of OUR variants and the champion's throughput.

Usage: gen_index.py <results.tsv> <out.html> <expected_count>
results.tsv rows: NAME<TAB>SECONDS<TAB>STATUS   (SECONDS is '-' for fails)

TODO: paste a snapshot of the real unique_strings leaderboard into SNAPSHOT
(rank, user, tier, score, compiler, wall_ns, mem_bytes) to get the same
"where we'd land" ranking the parse_integers page has. Until then this shows
our own timings + throughput vs the bandwidth floor.

Local ARM timings are NOT comparable to the x86 judge — the banner says so.
"""
import sys, os, subprocess, platform, html

N_TOKENS = 10_000_000   # matches gen.cpp / run.sh default N

# Fill this from https://highload.fun/challenges/compute/unique_strings/leaderboard
# cols: rank, user, tier, score, compiler, wall_ns, mem_bytes
SNAPSHOT = []           # e.g. (12, "someone", "Skilled", 1234, "g++13.3.0", 50_000_000, 2_000_000)
TARGET_RANK = None      # set once you pick a rank to chase


def read_results(path):
    rows = []
    if os.path.exists(path):
        for line in open(path):
            p = line.rstrip("\n").split("\t")
            if len(p) >= 3:
                rows.append((p[0], p[1], p[2]))
    return rows


def cxx_version():
    try:
        out = subprocess.check_output([os.environ.get("CXX", "c++"), "--version"],
                                      stderr=subprocess.STDOUT, text=True)
        return out.splitlines()[0].strip()
    except Exception:
        return "unknown"


def cur_time():
    try:
        return subprocess.check_output(["date", "-u", "+%Y-%m-%d %H:%M UTC"], text=True).strip()
    except Exception:
        return ""


def esc(s):
    return html.escape(str(s))


def main():
    results_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/us_results.tsv"
    out_path     = sys.argv[2] if len(sys.argv) > 2 else "index.html"
    expected     = sys.argv[3] if len(sys.argv) > 3 else "?"

    rows = read_results(results_path)
    arch = platform.machine()
    is_x86 = arch in ("x86_64", "amd64")
    compiler = cxx_version()
    when = cur_time()

    champ_secs = None
    for name, secs, status in rows:
        if name == "champion" and status == "OK":
            try: champ_secs = float(secs)
            except ValueError: pass
    champ_ms = champ_secs * 1000 if champ_secs else None
    ns_per_tok = (champ_secs * 1e9 / N_TOKENS) if champ_secs else None
    mtok_s = (N_TOKENS / champ_secs / 1e6) if champ_secs else None

    # variant table
    vt = []
    for name, secs, status in rows:
        try:
            s = float(secs); ms = f"{s*1000:.1f} ms"; nsp = f"{s*1e9/N_TOKENS:.1f} ns/tok"
        except ValueError:
            ms, nsp = "—", "—"
        badge = "ok" if status == "OK" else "bad"
        star = " ★" if name == "champion" else ""
        vt.append(f"""<tr><td class="user">{esc(name)}{star}</td><td>{ms}</td>
          <td>{nsp}</td><td><span class="badge {badge}">{esc(status)}</span></td></tr>""")

    # leaderboard section (only if a snapshot was pasted)
    if SNAPSHOT:
        lb_rows = "".join(
            f"<tr><td class='rank'>#{r[0]}</td><td class='user'>{esc(r[1])}</td>"
            f"<td>{r[3]:,}</td><td class='cc'>{esc(r[4])}</td>"
            f"<td>{r[5]:,} ns</td><td class='mem'>{r[6]:,} B</td></tr>" for r in SNAPSHOT)
        leaderboard = f"""<h2>Leaderboard snapshot</h2>
          <table><tr><th>Rank</th><th>User</th><th>Score</th><th>Compiler</th><th>Wall</th><th>Mem</th></tr>
          {lb_rows}</table>"""
    else:
        leaderboard = """<h2>Leaderboard</h2>
          <div class="todo">No snapshot yet. Paste the real
          <a href="https://highload.fun/challenges/compute/unique_strings/leaderboard">unique_strings leaderboard</a>
          into <code>SNAPSHOT</code> in <code>gen_index.py</code> to rank our champion against it.</div>"""

    banner = "" if is_x86 else (
        f'<div class="warn">⚠ Measured on <b>{esc(arch)}</b> — a local preview, '
        '<b>not comparable</b> to the x86 judge. The real standing comes from the x86 cloud run / the judge.</div>')

    doc = f"""<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>unique_strings — our run</title>
<style>
  :root {{ --bg:#0d1117; --panel:#161b22; --line:#21262d; --fg:#e6edf3; --dim:#8b949e; --us:#f0b429; }}
  * {{ box-sizing:border-box; }}
  body {{ margin:0; background:var(--bg); color:var(--fg);
         font:14px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace; }}
  .wrap {{ max-width:900px; margin:0 auto; padding:28px 20px 60px; }}
  h1 {{ font-size:22px; margin:0 0 2px; }}
  h2 {{ font-size:15px; margin:32px 0 10px; color:var(--dim); text-transform:uppercase; letter-spacing:.05em; }}
  .meta {{ color:var(--dim); font-size:12px; margin-bottom:18px; }}
  .cards {{ display:flex; gap:14px; flex-wrap:wrap; margin:16px 0 4px; }}
  .card {{ background:var(--panel); border:1px solid var(--line); border-radius:10px;
           padding:12px 16px; flex:1; min-width:150px; }}
  .card .k {{ color:var(--dim); font-size:11px; text-transform:uppercase; letter-spacing:.05em; }}
  .card .v {{ font-size:20px; margin-top:4px; }}
  table {{ width:100%; border-collapse:collapse; background:var(--panel);
           border:1px solid var(--line); border-radius:10px; overflow:hidden; }}
  th,td {{ text-align:left; padding:10px 12px; border-bottom:1px solid var(--line); }}
  th {{ color:var(--dim); font-weight:600; font-size:11px; text-transform:uppercase; letter-spacing:.05em; }}
  tr:last-child td {{ border-bottom:0; }}
  td.rank {{ font-weight:700; color:var(--dim); }} td.cc, td.mem {{ color:var(--dim); }}
  .badge {{ font-size:11px; padding:2px 8px; border-radius:6px; }}
  .badge.ok {{ background:#3fb95022; color:#3fb950; }}
  .badge.bad {{ background:#f8514922; color:#f85149; }}
  .warn {{ background:#f0b42912; border:1px solid #f0b42944; color:#f0b429;
           padding:10px 14px; border-radius:8px; margin:14px 0; font-size:12.5px; }}
  .todo {{ background:#1f6feb14; border:1px solid #1f6feb44; padding:10px 14px;
           border-radius:8px; font-size:12.5px; color:var(--dim); }}
  a {{ color:#58a6ff; }}
  footer {{ color:var(--dim); font-size:11px; margin-top:30px; }}
</style></head>
<body><div class="wrap">
  <h1>unique_strings — our run</h1>
  <div class="meta">Count distinct tokens · updated {esc(when)} · {expected} distinct / {N_TOKENS:,} tokens ·
    <a href="https://highload.fun/challenges/compute/unique_strings">challenge ↗</a></div>

  <div class="cards">
    <div class="card"><div class="k">Champion wall</div>
      <div class="v">{(f'{champ_ms:.0f} ms' if champ_ms else '—')}</div></div>
    <div class="card"><div class="k">Per token</div>
      <div class="v">{(f'{ns_per_tok:.1f} ns' if ns_per_tok else '—')}</div></div>
    <div class="card"><div class="k">Throughput</div>
      <div class="v">{(f'{mtok_s:.0f} M/s' if mtok_s else '—')}</div></div>
  </div>
  {banner}

  <h2>This run — all variants</h2>
  <table>
    <tr><th>Program</th><th>Wall (median)</th><th>Per token</th><th>Status</th></tr>
    {''.join(vt) if vt else '<tr><td colspan=4>no results</td></tr>'}
  </table>

  {leaderboard}

  <footer>Generated by <code>gen_index.py</code> on each <code>run.sh</code>.
    Compiler: {esc(compiler)} · arch {esc(arch)}.</footer>
</div></body></html>"""

    with open(out_path, "w") as f:
        f.write(doc)
    print(f"wrote {out_path} (champion={('%.0f ms'%champ_ms) if champ_ms else 'n/a'}, "
          f"{('%.1f ns/tok'%ns_per_tok) if ns_per_tok else 'n/a'})")


if __name__ == "__main__":
    main()
