#!/usr/bin/env python3
"""Summarises the per-symbol CSV written by `lob_itch validate --symbols-csv` into a small JSON file.

The CSV itself (one row per symbol, derived from Nasdaq data) is not committed; only these aggregates are.

  python scripts/analyze_itch_symbols.py PATH/val_full_symbols.csv bench/results/realdata/symbols_analysis.json
"""
import csv, json, sys


def ranks(v):
    order = sorted(range(len(v)), key=lambda i: v[i])
    r = [0.0] * len(v)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and v[order[j + 1]] == v[order[i]]:
            j += 1
        for k in range(i, j + 1):
            r[order[k]] = (i + j) / 2 + 1
        i = j + 1
    return r


def spearman(x, y):
    rx, ry = ranks(x), ranks(y)
    n = len(x)
    mx, my = sum(rx) / n, sum(ry) / n
    num = sum((a - mx) * (b - my) for a, b in zip(rx, ry))
    den = (sum((a - mx) ** 2 for a in rx) * sum((b - my) ** 2 for b in ry)) ** 0.5
    return num / den if den else 0.0


def main():
    rows = list(csv.DictReader(open(sys.argv[1])))
    for r in rows:
        for k in r:
            if k != "symbol":
                r[k] = int(r[k])
    tot_checked = sum(r["continuous_checked"] for r in rows)
    tot_mm = sum(r["continuous_mismatched"] for r in rows)
    active = [r for r in rows if r["continuous_checked"] >= 200]
    for r in active:
        r["rate"] = r["continuous_mismatched"] / r["continuous_checked"]
        r["p_ratio"] = r["shares_P"] / r["exec_shares"] if r["exec_shares"] else 0.0
        r["p_per_exec"] = r["trades_P"] / r["executions"] if r["executions"] else 0.0
    out = {"symbols_total": len(rows), "symbols_with_continuous_executions": sum(1 for r in rows if r["continuous_checked"] > 0),
           "symbols_with_at_least_200_continuous_executions": len(active), "continuous_checked": tot_checked, "continuous_mismatched": tot_mm}
    out["symbols_with_zero_mismatches_among_active"] = sum(1 for r in active if r["continuous_mismatched"] == 0)
    rates = sorted(r["rate"] for r in active)
    q = lambda p: rates[min(len(rates) - 1, int(p * len(rates)))]
    out["active_symbol_mismatch_rate_quantiles"] = {"p10": q(0.1), "p25": q(0.25), "p50": q(0.5), "p75": q(0.75), "p90": q(0.9), "max": rates[-1]}
    by_mm = sorted(rows, key=lambda r: -r["continuous_mismatched"])
    out["share_of_mismatches_in_top_10_symbols"] = sum(r["continuous_mismatched"] for r in by_mm[:10]) / tot_mm if tot_mm else 0
    out["share_of_mismatches_in_top_100_symbols"] = sum(r["continuous_mismatched"] for r in by_mm[:100]) / tot_mm if tot_mm else 0
    out["top_symbols_by_mismatches"] = [{k: r[k] for k in ("symbol", "continuous_checked", "continuous_mismatched")} for r in by_mm[:10]]
    by_rate = sorted([r for r in active if r["continuous_checked"] >= 1000], key=lambda r: -r["rate"])
    out["highest_rate_symbols_min_1000_checked"] = [{k: r[k] for k in ("symbol", "etp", "continuous_checked", "continuous_mismatched")} for r in by_rate[:10]]
    # non-displayed-activity proxy: shares printed in P (non-cross) trades relative to displayed executed shares
    if len(active) > 10:
        out["spearman_mismatch_rate_vs_P_shares_per_executed_share"] = spearman([r["rate"] for r in active], [r["p_ratio"] for r in active])
        out["spearman_mismatch_rate_vs_P_trades_per_execution"] = spearman([r["rate"] for r in active], [r["p_per_exec"] for r in active])
        out["spearman_mismatch_rate_vs_continuous_executions"] = spearman([r["rate"] for r in active], [r["continuous_checked"] for r in active])
        srt = sorted(active, key=lambda r: r["p_ratio"])
        quart = []
        for i in range(4):
            part = srt[i * len(srt) // 4:(i + 1) * len(srt) // 4]
            ck, mm = sum(r["continuous_checked"] for r in part), sum(r["continuous_mismatched"] for r in part)
            quart.append({"symbols": len(part), "p_ratio_min": part[0]["p_ratio"], "p_ratio_max": part[-1]["p_ratio"], "checked": ck, "mismatched": mm,
                          "rate": mm / ck if ck else 0})
        out["by_quartile_of_P_shares_per_executed_share"] = quart
    for flag, name in ((0, "stocks"), (1, "etps")):
        part = [r for r in rows if r["etp"] == flag]
        ck, mm = sum(r["continuous_checked"] for r in part), sum(r["continuous_mismatched"] for r in part)
        out["etp_" + name] = {"symbols": len(part), "checked": ck, "mismatched": mm, "rate": mm / ck if ck else 0}
    json.dump(out, open(sys.argv[2], "w"), indent=1)
    print("wrote", sys.argv[2])


if __name__ == "__main__":
    main()
