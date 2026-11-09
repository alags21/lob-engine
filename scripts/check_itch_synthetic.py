#!/usr/bin/env python3
"""Checks the JSON produced by `lob_itch validate` on the SYNTHETIC ITCH file (CI never uses Nasdaq data).

The synthetic stream is derived from our own engine, so a correct pipeline must show: all messages
decoded, zero order-level violations, every execution matched by the engine, zero reference-model
disagreements, and some executions actually checked (so the test is not vacuous).
"""
import json, sys

j = json.load(open(sys.argv[1]))
t = j["total"]
cont = t["priority_check"]["continuous"]
problems = []
if j["malformed_messages"] or j["unknown_type_messages_skipped"]:
    problems.append("malformed or unknown messages in the synthetic file")
if any(t["violations"].values()):
    problems.append(f"order-level violations: {t['violations']}")
if cont["checked"] < 100:
    problems.append(f"only {cont['checked']} continuous executions checked")
if cont["mismatches"] or cont["engine_matched_exchange_choice"] != cont["checked"]:
    problems.append(f"priority mismatches on synthetic data: {cont}")
if t["reference_comparisons"] < 10:
    problems.append("reference model was barely compared")
if t["reference_level_mismatches"] or t["reference_queue_order_mismatches"]:
    problems.append("optimised book and reference book disagree")
if problems:
    print("synthetic ITCH check FAILED:\n  " + "\n  ".join(problems))
    sys.exit(1)
print(f"synthetic ITCH check ok: {j['decoded_messages']} messages, {cont['checked']} continuous executions all matched, "
      f"{t['reference_comparisons']} reference comparisons clean")
