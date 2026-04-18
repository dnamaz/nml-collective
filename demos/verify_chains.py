#!/usr/bin/env python3
"""
verify_chains.py — Cross-check the Custodian and Sentient transaction chains.

Both agents log DATA_SUBMIT / DATA_APPROVE / DATA_REJECT records for the same
object hashes (the Custodian when it ingests and when it observes Sentient's
MQTT decisions; the Sentient when it admits to quarantine and when the
operator approves or rejects). If the mesh is healthy the two sets agree.

Drift between them means something interesting happened: a dropped MQTT
message, a clock skew, an agent restart with lost state, or tampering.

Usage:
    python demos/verify_chains.py
    python demos/verify_chains.py --custodian 10.0.0.5:9004 \\
                                  --sentient  10.0.0.5:9001

Exits 0 when both chains are intact and their event sets match; 1 otherwise.
"""

import argparse
import json
import sys
import urllib.request
import urllib.error


def fetch_json(host_port: str, path: str) -> object:
    url = f"http://{host_port}{path}"
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return json.loads(r.read().decode("utf-8"))
    except (urllib.error.URLError, ConnectionError, TimeoutError) as e:
        print(f"  ! {url}: {e}", file=sys.stderr)
        return None


def hashes_by_type(ledger: list) -> dict:
    """Return {tx_type: set(hashes)} extracted from the payload JSON field."""
    out = {"DATA_SUBMIT": set(), "DATA_APPROVE": set(), "DATA_REJECT": set()}
    for rec in ledger or []:
        tt = rec.get("tx_type")
        if tt not in out:
            continue
        payload_raw = rec.get("payload", "")
        try:
            payload = json.loads(payload_raw)
            h = payload.get("hash")
        except json.JSONDecodeError:
            h = None
        if h:
            out[tt].add(h)
    return out


def diff_section(title: str, a_name: str, a_set: set,
                 b_name: str, b_set: set) -> int:
    """Print the symmetric difference; return number of discrepancies."""
    only_a = a_set - b_set
    only_b = b_set - a_set
    n = len(only_a) + len(only_b)
    print(f"\n  {title}: {len(a_set)} in {a_name}, {len(b_set)} in {b_name}")
    if not n:
        print(f"    OK — sets agree ({len(a_set)} hashes)")
        return 0
    if only_a:
        print(f"    In {a_name} but NOT {b_name}: {len(only_a)}")
        for h in sorted(only_a):
            print(f"      {h}")
    if only_b:
        print(f"    In {b_name} but NOT {a_name}: {len(only_b)}")
        for h in sorted(only_b):
            print(f"      {h}")
    return n


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--custodian", default="127.0.0.1:9004",
                    help="custodian host:port (default 127.0.0.1:9004)")
    ap.add_argument("--sentient", default="127.0.0.1:9001",
                    help="sentient host:port (default 127.0.0.1:9001)")
    ap.add_argument("--limit", type=int, default=1000,
                    help="max records to fetch from each /ledger")
    args = ap.parse_args()

    drift = 0

    # ── Integrity ────────────────────────────────────────────────────────
    print("=== chain integrity ===")
    for label, host in (("custodian", args.custodian),
                        ("sentient",  args.sentient)):
        result = fetch_json(host, "/ledger/verify")
        if result is None:
            print(f"  {label}: UNREACHABLE")
            drift += 1
            continue
        if result.get("ok"):
            print(f"  {label}: OK ({result.get('total')} records)")
        else:
            bad = result.get("bad_tx_id")
            print(f"  {label}: CORRUPT at tx_id={bad}")
            drift += 1

    # ── Ledgers ──────────────────────────────────────────────────────────
    print("\n=== fetching ledgers ===")
    c_ledger = fetch_json(args.custodian, f"/ledger?limit={args.limit}")
    s_ledger = fetch_json(args.sentient,  f"/ledger?limit={args.limit}")

    if c_ledger is None or s_ledger is None:
        print("  ! cannot cross-check — at least one ledger is unreachable")
        return 1

    print(f"  custodian: {len(c_ledger)} records")
    print(f"  sentient : {len(s_ledger)} records")

    if len(c_ledger) == args.limit or len(s_ledger) == args.limit:
        print(f"  ! hit --limit={args.limit}; older records may be missing "
              "— rerun with a higher limit for full coverage")

    c = hashes_by_type(c_ledger)
    s = hashes_by_type(s_ledger)

    # ── Cross-check ──────────────────────────────────────────────────────
    print("\n=== cross-check ===")
    drift += diff_section("DATA_SUBMIT",
                          "custodian", c["DATA_SUBMIT"],
                          "sentient",  s["DATA_SUBMIT"])
    drift += diff_section("DATA_APPROVE",
                          "custodian", c["DATA_APPROVE"],
                          "sentient",  s["DATA_APPROVE"])
    drift += diff_section("DATA_REJECT",
                          "custodian", c["DATA_REJECT"],
                          "sentient",  s["DATA_REJECT"])

    print("\n=== summary ===")
    if drift == 0:
        print("  chains agree — no drift, both intact")
        return 0
    print(f"  {drift} discrepancy/ies — investigate above")
    return 1


if __name__ == "__main__":
    sys.exit(main())
