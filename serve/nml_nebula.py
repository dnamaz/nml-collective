#!/usr/bin/env python3
"""
NML Nebula — the collective's brain.

Content-addressed ledger with quarantine, sentient approval, data pool,
and execution triggers. The nebula is the shared memory at the center
of the collective.

Every agent can embed a nebula instance. Sentient agents replicate the
full ledger. Worker agents cache what they need.

Usage (standalone):
    python3 nml_nebula.py --port 7700

Usage (embedded in agent):
    from nml_nebula import Nebula
    nebula = Nebula()
"""

import hashlib
import json
import time
from pathlib import Path
from enum import Enum


class Status(str, Enum):
    PENDING = "pending"
    APPROVED = "approved"
    REJECTED = "rejected"
    UNREVIEWED = "unreviewed"
    SUPERSEDED = "superseded"


class EntryType(str, Enum):
    PROGRAM = "program"
    DATA = "data"
    EXECUTION = "execution"
    CONSENSUS = "consensus"
    MANIFEST = "manifest"


class LedgerEntry:
    __slots__ = ("type", "hash", "timestamp", "author", "status",
                 "content", "meta", "votes", "reason")

    def __init__(self, entry_type, content, author="system", meta=None):
        self.type = entry_type
        self.content = content
        self.hash = hashlib.sha256(
            content.encode() if isinstance(content, str) else json.dumps(content).encode()
        ).hexdigest()[:16]
        self.timestamp = time.time()
        self.author = author
        self.status = Status.PENDING if entry_type == EntryType.DATA else Status.APPROVED
        self.meta = meta or {}
        self.votes = []
        self.reason = None

    def to_dict(self):
        return {
            "type": self.type,
            "hash": self.hash,
            "timestamp": self.timestamp,
            "author": self.author,
            "status": self.status,
            "meta": self.meta,
            "votes": self.votes,
            "reason": self.reason,
            "content_size": len(self.content) if isinstance(self.content, str) else len(json.dumps(self.content)),
        }


class AutoCheckResult:
    def __init__(self, passed, checks):
        self.passed = passed
        self.checks = checks

    def to_dict(self):
        return {"passed": self.passed, "checks": self.checks}


class Nebula:
    """Content-addressed ledger with quarantine and approval."""

    def __init__(self, quorum=0.5, quarantine_timeout=3600):
        self.ledger = {}       # hash -> LedgerEntry
        self.quarantine = {}   # hash -> LedgerEntry (pending data)
        self.data_pool = {}    # @name -> [hash, hash, ...] (approved data versions)
        self.programs = {}     # hash -> LedgerEntry (signed programs)
        self.executions = []   # append-only execution log
        self.consensus = {}    # (program_hash, data_context) -> VOTE result
        self.manifests = {}    # hash -> manifest entry
        self.quorum = quorum
        self.quarantine_timeout = quarantine_timeout
        self.sentients = set()
        self.event_callbacks = []

    def _emit(self, event_type, detail):
        for cb in self.event_callbacks:
            try:
                cb(event_type, detail)
            except Exception:
                pass

    # ═══════════════════════════════════════════
    # Programs
    # ═══════════════════════════════════════════

    def store_program(self, program_text, author="authority"):
        entry = LedgerEntry(EntryType.PROGRAM, program_text, author=author)
        entry.status = Status.APPROVED
        self.ledger[entry.hash] = entry
        self.programs[entry.hash] = entry
        self._emit("program_stored", f"{entry.hash} by {author}")
        return entry

    def get_program(self, phash):
        return self.programs.get(phash)

    def list_programs(self):
        return [e.to_dict() for e in self.programs.values()]

    # ═══════════════════════════════════════════
    # Data Submission + Quarantine
    # ═══════════════════════════════════════════

    def submit_data(self, name, content, author="worker"):
        """Worker submits data — goes to quarantine."""
        entry = LedgerEntry(EntryType.DATA, content, author=author,
                            meta={"name": name})
        entry.status = Status.PENDING

        auto = self._auto_check(name, content)
        entry.meta["auto_checks"] = auto.to_dict()

        self.quarantine[entry.hash] = entry
        self.ledger[entry.hash] = entry
        self._emit("data_quarantined", f"@{name} hash={entry.hash} by {author} auto={'PASS' if auto.passed else 'FAIL'}")

        if not auto.passed:
            entry.status = Status.REJECTED
            entry.reason = "auto-check failed: " + ", ".join(
                c["name"] for c in auto.checks if not c["passed"]
            )
            self._emit("data_auto_rejected", f"@{name} hash={entry.hash}: {entry.reason}")

        return entry

    def _auto_check(self, name, content):
        """Run automatic validation checks on submitted data."""
        checks = []

        if isinstance(content, str) and "shape=" in content:
            checks.append({"name": "has_shape", "passed": True})
        else:
            checks.append({"name": "has_shape", "passed": False, "detail": "missing shape declaration"})

        if isinstance(content, str) and "data=" in content:
            checks.append({"name": "has_data", "passed": True})
            try:
                data_part = content.split("data=")[1].split()[0]
                values = [float(v) for v in data_part.split(",")]
                has_nan = any(v != v for v in values)
                checks.append({"name": "no_nan", "passed": not has_nan})
            except (ValueError, IndexError):
                checks.append({"name": "no_nan", "passed": False, "detail": "parse error"})
        else:
            checks.append({"name": "has_data", "passed": False})

        all_passed = all(c["passed"] for c in checks)
        return AutoCheckResult(all_passed, checks)

    # ═══════════════════════════════════════════
    # Sentient Approval / Rejection
    # ═══════════════════════════════════════════

    def approve(self, data_hash, sentient, reason=""):
        """Sentient votes to approve quarantined data."""
        entry = self.quarantine.get(data_hash)
        if not entry or entry.status != Status.PENDING:
            return None

        entry.votes.append({"sentient": sentient, "vote": "approve", "reason": reason, "time": time.time()})
        self._emit("vote_approve", f"{data_hash} by {sentient}")

        if self._check_quorum(entry):
            self._promote(entry)

        return entry

    def reject(self, data_hash, sentient, reason=""):
        """Sentient votes to reject quarantined data."""
        entry = self.quarantine.get(data_hash)
        if not entry or entry.status != Status.PENDING:
            return None

        entry.votes.append({"sentient": sentient, "vote": "reject", "reason": reason, "time": time.time()})
        entry.status = Status.REJECTED
        entry.reason = reason
        self._emit("vote_reject", f"{data_hash} by {sentient}: {reason}")
        return entry

    def _check_quorum(self, entry):
        if not self.sentients:
            return len(entry.votes) >= 1
        approve_count = sum(1 for v in entry.votes if v["vote"] == "approve")
        needed = max(1, int(len(self.sentients) * self.quorum) + 1)
        return approve_count >= needed

    def _promote(self, entry):
        """Promote quarantined data to the approved data pool."""
        entry.status = Status.APPROVED
        name = entry.meta.get("name", "unknown")

        if name not in self.data_pool:
            self.data_pool[name] = []
        self.data_pool[name].append(entry.hash)

        self._emit("data_approved", f"@{name} hash={entry.hash} ({len(entry.votes)} votes)")
        return entry

    # ═══════════════════════════════════════════
    # Data Pool Queries
    # ═══════════════════════════════════════════

    def get_data(self, name, version=-1):
        """Get approved data by @name. version=-1 for latest."""
        hashes = self.data_pool.get(name, [])
        if not hashes:
            return None
        target_hash = hashes[version]
        return self.ledger.get(target_hash)

    def get_data_by_hash(self, dhash):
        return self.ledger.get(dhash)

    def list_data_pool(self):
        result = {}
        for name, hashes in self.data_pool.items():
            result[name] = [self.ledger[h].to_dict() for h in hashes if h in self.ledger]
        return result

    def list_quarantine(self):
        return [e.to_dict() for e in self.quarantine.values() if e.status == Status.PENDING]

    def list_all_data(self):
        return [e.to_dict() for e in self.ledger.values() if e.type == EntryType.DATA]

    # ═══════════════════════════════════════════
    # Execution Log
    # ═══════════════════════════════════════════

    def log_execution(self, program_hash, data_context, agent, score, success):
        entry = {
            "program_hash": program_hash,
            "data_context": data_context,
            "agent": agent,
            "score": score,
            "success": success,
            "timestamp": time.time(),
        }
        self.executions.append(entry)
        self._emit("execution_logged", f"prog={program_hash} agent={agent} score={score}")
        return entry

    def get_executions(self, program_hash=None):
        if program_hash:
            return [e for e in self.executions if e["program_hash"] == program_hash]
        return list(self.executions)

    # ═══════════════════════════════════════════
    # Consensus
    # ═══════════════════════════════════════════

    def compute_consensus(self, program_hash, strategy="median"):
        execs = [e for e in self.executions
                 if e["program_hash"] == program_hash and e["score"] is not None]
        if not execs:
            return None

        scores = [e["score"] for e in execs]
        if strategy == "median":
            scores.sort()
            mid = len(scores) // 2
            result = scores[mid] if len(scores) % 2 == 1 else (scores[mid-1] + scores[mid]) / 2
        elif strategy == "mean":
            result = sum(scores) / len(scores)
        elif strategy == "min":
            result = min(scores)
        elif strategy == "max":
            result = max(scores)
        else:
            result = sum(scores) / len(scores)

        consensus = {
            "program_hash": program_hash,
            "strategy": strategy,
            "consensus": result,
            "count": len(scores),
            "agents": [{"agent": e["agent"], "score": e["score"]} for e in execs],
            "timestamp": time.time(),
        }
        self.consensus[program_hash] = consensus
        self._emit("consensus", f"prog={program_hash} {strategy}={result:.4f} ({len(scores)} agents)")
        return consensus

    # ═══════════════════════════════════════════
    # Manifests
    # ═══════════════════════════════════════════

    def create_manifest(self, program_hash, data_bindings, author="sentient"):
        """Sentient creates a data manifest for a program."""
        manifest = {
            "program_hash": program_hash,
            "data_bindings": data_bindings,
            "author": author,
            "timestamp": time.time(),
        }
        mhash = hashlib.sha256(json.dumps(manifest).encode()).hexdigest()[:16]
        entry = LedgerEntry(EntryType.MANIFEST, json.dumps(manifest), author=author,
                            meta={"program_hash": program_hash})
        entry.status = Status.APPROVED
        self.ledger[mhash] = entry
        self.manifests[mhash] = manifest
        self._emit("manifest_created", f"prog={program_hash} by {author}")
        return mhash, manifest

    # ═══════════════════════════════════════════
    # Stats
    # ═══════════════════════════════════════════

    def stats(self):
        return {
            "programs": len(self.programs),
            "quarantine_pending": sum(1 for e in self.quarantine.values() if e.status == Status.PENDING),
            "quarantine_total": len(self.quarantine),
            "data_pool_names": len(self.data_pool),
            "data_pool_versions": sum(len(v) for v in self.data_pool.values()),
            "executions": len(self.executions),
            "consensus_results": len(self.consensus),
            "manifests": len(self.manifests),
            "sentients": list(self.sentients),
            "ledger_entries": len(self.ledger),
            "data_approved": sum(1 for e in self.ledger.values() if e.type == EntryType.DATA and e.status == Status.APPROVED),
            "data_rejected": sum(1 for e in self.ledger.values() if e.type == EntryType.DATA and e.status == Status.REJECTED),
        }


# ═══════════════════════════════════════════
# HTTP Server (standalone mode)
# ═══════════════════════════════════════════

def create_nebula_app(nebula=None):
    from aiohttp import web

    if nebula is None:
        nebula = Nebula()

    def _cors():
        return {"Access-Control-Allow-Origin": "*", "Access-Control-Allow-Methods": "GET, POST, OPTIONS", "Access-Control-Allow-Headers": "Content-Type"}

    def _json(data, status=200):
        return web.Response(text=json.dumps(data), content_type="application/json", status=status, headers=_cors())

    async def handle_health(request):
        return _json({"status": "healthy", "service": "nml-nebula", **nebula.stats()})

    async def handle_store_program(request):
        body = await request.json()
        entry = nebula.store_program(body.get("program", ""), author=body.get("author", "authority"))
        return _json({"stored": entry.hash, "status": entry.status})

    async def handle_list_programs(request):
        return _json({"programs": nebula.list_programs()})

    async def handle_submit_data(request):
        body = await request.json()
        entry = nebula.submit_data(body.get("name", "unknown"), body.get("content", ""), author=body.get("author", "worker"))
        return _json({"hash": entry.hash, "status": entry.status, "auto_checks": entry.meta.get("auto_checks")})

    async def handle_quarantine(request):
        return _json({"pending": nebula.list_quarantine()})

    async def handle_approve(request):
        body = await request.json()
        entry = nebula.approve(body.get("hash", ""), body.get("sentient", ""), body.get("reason", ""))
        if entry:
            return _json({"hash": entry.hash, "status": entry.status, "votes": entry.votes})
        return _json({"error": "Not found or not pending"}, 404)

    async def handle_reject(request):
        body = await request.json()
        entry = nebula.reject(body.get("hash", ""), body.get("sentient", ""), body.get("reason", ""))
        if entry:
            return _json({"hash": entry.hash, "status": entry.status, "reason": entry.reason})
        return _json({"error": "Not found or not pending"}, 404)

    async def handle_data_pool(request):
        return _json({"pool": nebula.list_data_pool()})

    async def handle_get_data(request):
        name = request.query.get("name", "")
        entry = nebula.get_data(name)
        if entry:
            return _json({"hash": entry.hash, "name": name, "content": entry.content, "status": entry.status})
        return _json({"error": f"No approved data for @{name}"}, 404)

    async def handle_log_execution(request):
        body = await request.json()
        entry = nebula.log_execution(
            body.get("program_hash", ""), body.get("data_context", ""),
            body.get("agent", ""), body.get("score"), body.get("success", False))
        return _json(entry)

    async def handle_executions(request):
        phash = request.query.get("program", None)
        return _json({"executions": nebula.get_executions(phash)})

    async def handle_consensus(request):
        body = await request.json()
        result = nebula.compute_consensus(body.get("program_hash", ""), body.get("strategy", "median"))
        if result:
            return _json(result)
        return _json({"error": "No executions for this program"}, 404)

    async def handle_all_data(request):
        return _json({"data": nebula.list_all_data()})

    async def handle_stats(request):
        return _json(nebula.stats())

    async def handle_register_sentient(request):
        body = await request.json()
        name = body.get("name", "")
        if name:
            nebula.sentients.add(name)
            return _json({"registered": name, "sentients": list(nebula.sentients)})
        return _json({"error": "name required"}, 400)

    app = web.Application()
    app.router.add_get("/health", handle_health)
    app.router.add_post("/program", handle_store_program)
    app.router.add_get("/programs", handle_list_programs)
    app.router.add_post("/data/submit", handle_submit_data)
    app.router.add_get("/data/quarantine", handle_quarantine)
    app.router.add_post("/data/approve", handle_approve)
    app.router.add_post("/data/reject", handle_reject)
    app.router.add_get("/data/pool", handle_data_pool)
    app.router.add_get("/data/get", handle_get_data)
    app.router.add_get("/data/all", handle_all_data)
    app.router.add_post("/execution", handle_log_execution)
    app.router.add_get("/executions", handle_executions)
    app.router.add_post("/consensus", handle_consensus)
    app.router.add_get("/stats", handle_stats)
    app.router.add_post("/sentient/register", handle_register_sentient)
    app.router.add_route("OPTIONS", "/{path:.*}", lambda r: web.Response(status=204, headers=_cors()))
    return app


def main():
    import argparse
    from aiohttp import web

    parser = argparse.ArgumentParser(description="NML Nebula — the collective's brain")
    parser.add_argument("--port", type=int, default=7700, help="Nebula port (default: 7700)")
    args = parser.parse_args()

    nebula = Nebula()
    nebula._emit = lambda t, d: print(f"  [nebula] {t}: {d}")

    app = create_nebula_app(nebula)
    print(f"NML Nebula on :{args.port}")
    print(f"  Endpoints: /program /data/submit /data/quarantine /data/approve /data/pool /consensus")
    web.run_app(app, port=args.port, print=None)


if __name__ == "__main__":
    main()
