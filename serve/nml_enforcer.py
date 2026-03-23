#!/usr/bin/env python3
"""
NML Enforcer — the collective's immune system.

The Enforcer monitors agent behavior, detects threats, quarantines compromised
nodes, maintains ban lists, and collects evidence. She acts on intelligence
from the Oracle and enforces policy set by Sentients.

Three enforcement levels:
  1. Warning  — logged, visible, auto-expires
  2. Quarantine — temporary isolation, gossips to mesh, reversible
  3. Blacklist — permanent ban, requires sentient approval

Enforcers cannot quarantine sentients. Only sentients can override enforcers.

Usage (embedded in agent):
    from nml_enforcer import EnforcerEngine
    enforcer = EnforcerEngine(agent)
"""

import asyncio
import hashlib
import json
import time
from aiohttp import ClientSession


WARN_THRESHOLD_REJECTIONS = 3
WARN_THRESHOLD_OUTLIER_Z = 2.0
QUARANTINE_AUTO_THRESHOLD = 5
QUARANTINE_DEFAULT_DURATION = 3600
RATE_LIMIT_WINDOW = 60
RATE_LIMIT_MAX = 20
MONITOR_INTERVAL = 15


class ThreatLevel:
    CLEAR = "clear"
    WARNING = "warning"
    QUARANTINED = "quarantined"
    BLACKLISTED = "blacklisted"


class EnforcerEngine:
    """Immune system for the NML collective."""

    def __init__(self, agent):
        self.agent = agent
        self.running = False

        self.warnings = {}       # agent_name -> [{reason, time, detail}]
        self.quarantined = {}    # agent_name -> {reason, time, expires, evidence}
        self.blacklisted = {}    # agent_name -> {reason, time, approved_by}
        self.evidence = {}       # agent_name -> [{type, detail, time}]
        self.submission_tracker = {}  # agent_name -> [timestamps]

        # Node identity tracking (populated from MSG_ANNOUNCE / MSG_HEARTBEAT)
        self.machine_to_agent = {}  # machine_hash16 -> agent_name
        self.agent_to_machine = {}  # agent_name -> machine_hash16

        self.monitor_count = 0

    async def start(self):
        self.running = True
        asyncio.create_task(self._monitor_loop())
        self.agent.log_event("enforcer_started", "immune system active")

    def stop(self):
        self.running = False

    # ═══════════════════════════════════════════
    # Monitoring — periodic checks
    # ═══════════════════════════════════════════

    async def _monitor_loop(self):
        while self.running:
            await asyncio.sleep(MONITOR_INTERVAL)
            self._expire_quarantines()
            await self._check_peers()
            self.monitor_count += 1

    def _expire_quarantines(self):
        now = time.time()
        expired = [name for name, q in self.quarantined.items()
                   if q.get("expires") and now > q["expires"]]
        for name in expired:
            self.agent.log_event("enforcer_quarantine_expired", name)
            del self.quarantined[name]
            if name in self.agent.peers:
                self.agent.peers[name]["quarantined"] = False

    async def _check_peers(self):
        """Probe peers for health and chain integrity issues."""
        async with ClientSession() as session:
            for name, info in list(self.agent.peers.items()):
                if name in self.blacklisted:
                    continue
                url = info.get("url", "")
                if not url or url.startswith("relay:"):
                    continue

                try:
                    async with session.get(f"{url}/health", timeout=3) as resp:
                        data = await resp.json()
                        reported_name = data.get("agent", "")
                        if reported_name and reported_name != name:
                            self._record_evidence(name, "impersonation",
                                f"Claims to be '{reported_name}' but registered as '{name}'")
                            self.warn(name, f"Name mismatch: registered as {name}, reports as {reported_name}")
                except Exception:
                    pass

    # ═══════════════════════════════════════════
    # Evidence Collection
    # ═══════════════════════════════════════════

    def _record_evidence(self, agent_name, evidence_type, detail):
        if agent_name not in self.evidence:
            self.evidence[agent_name] = []
        entry = {"type": evidence_type, "detail": detail, "time": time.time()}
        self.evidence[agent_name].append(entry)
        if len(self.evidence[agent_name]) > 100:
            self.evidence[agent_name] = self.evidence[agent_name][-100:]

    def record_data_rejection(self, agent_name, data_hash, reason):
        """Called when data from an agent is rejected."""
        self._record_evidence(agent_name, "data_rejected",
            f"hash={data_hash}: {reason}")

        warnings = self.warnings.get(agent_name, [])
        rejections = len([e for e in self.evidence.get(agent_name, [])
                          if e["type"] == "data_rejected"
                          and time.time() - e["time"] < 3600])

        if rejections >= QUARANTINE_AUTO_THRESHOLD:
            self.quarantine_node(agent_name,
                f"Auto-quarantine: {rejections} data rejections in the last hour")
        elif rejections >= WARN_THRESHOLD_REJECTIONS:
            self.warn(agent_name, f"{rejections} data rejections in the last hour")

    def record_outlier(self, agent_name, score, z_score, program_hash):
        """Called when Oracle detects a score outlier."""
        self._record_evidence(agent_name, "score_outlier",
            f"program={program_hash} score={score:.4f} z={z_score:.2f}")

        if z_score > WARN_THRESHOLD_OUTLIER_Z:
            self.warn(agent_name,
                f"Score outlier: z={z_score:.2f} on program {program_hash[:8]}")

        outliers = len([e for e in self.evidence.get(agent_name, [])
                        if e["type"] == "score_outlier"
                        and time.time() - e["time"] < 3600])
        if outliers >= 3:
            self.quarantine_node(agent_name,
                f"Auto-quarantine: {outliers} score outliers in the last hour")

    def record_chain_failure(self, agent_name, detail):
        """Called when a transaction chain integrity check fails."""
        self._record_evidence(agent_name, "chain_failure", detail)
        self.quarantine_node(agent_name, f"Chain integrity failure: {detail}")

    def record_rate_violation(self, agent_name):
        """Track submission rate. Called on each data submission."""
        now = time.time()
        if agent_name not in self.submission_tracker:
            self.submission_tracker[agent_name] = []

        timestamps = self.submission_tracker[agent_name]
        timestamps.append(now)
        self.submission_tracker[agent_name] = [
            t for t in timestamps if now - t < RATE_LIMIT_WINDOW
        ]

        rate = len(self.submission_tracker[agent_name])
        if rate > RATE_LIMIT_MAX:
            self._record_evidence(agent_name, "rate_violation",
                f"{rate} submissions in {RATE_LIMIT_WINDOW}s")
            self.quarantine_node(agent_name,
                f"Rate limit exceeded: {rate} submissions in {RATE_LIMIT_WINDOW}s")
            return True
        return False

    # ═══════════════════════════════════════════
    # Actions — Warn, Quarantine, Blacklist
    # ═══════════════════════════════════════════

    def warn(self, agent_name, reason):
        if agent_name not in self.warnings:
            self.warnings[agent_name] = []
        self.warnings[agent_name].append({
            "reason": reason, "time": time.time(),
        })
        if len(self.warnings[agent_name]) > 50:
            self.warnings[agent_name] = self.warnings[agent_name][-50:]
        self.agent.log_event("enforcer_warn", f"{agent_name}: {reason}")

    def quarantine_node(self, agent_name, reason, duration=None):
        """Quarantine a node — all peers ignore its traffic."""
        peer_role = self.agent.peers.get(agent_name, {}).get("role")
        if peer_role == "sentient":
            self.agent.log_event("enforcer_blocked",
                f"Cannot quarantine sentient '{agent_name}'")
            return False

        if agent_name in self.quarantined:
            return True

        duration = duration or QUARANTINE_DEFAULT_DURATION
        now = time.time()
        self.quarantined[agent_name] = {
            "reason": reason,
            "time": now,
            "expires": now + duration,
            "evidence": list(self.evidence.get(agent_name, [])[-10:]),
        }

        if agent_name in self.agent.peers:
            self.agent.peers[agent_name]["quarantined"] = True

        self.agent.log_event("enforcer_quarantine",
            f"{agent_name}: {reason} (expires in {duration}s)")

        asyncio.ensure_future(self._gossip_quarantine(agent_name, reason))
        return True

    def lift_quarantine(self, agent_name, lifted_by):
        """Lift quarantine on a node. Enforcers and sentients can do this."""
        if agent_name not in self.quarantined:
            return False
        del self.quarantined[agent_name]
        if agent_name in self.agent.peers:
            self.agent.peers[agent_name]["quarantined"] = False
        self.agent.log_event("enforcer_quarantine_lifted",
            f"{agent_name} by {lifted_by}")
        return True

    def propose_blacklist(self, agent_name, reason):
        """Propose a permanent blacklist — needs sentient approval."""
        if agent_name in self.blacklisted:
            return {"status": "already_blacklisted"}

        peer_role = self.agent.peers.get(agent_name, {}).get("role")
        if peer_role == "sentient":
            return {"status": "cannot_blacklist_sentient"}

        self.quarantine_node(agent_name, f"Blacklist proposed: {reason}")

        return {
            "status": "proposed",
            "agent": agent_name,
            "reason": reason,
            "evidence": list(self.evidence.get(agent_name, [])[-10:]),
            "requires": "sentient approval via POST /blacklist/approve",
        }

    def approve_blacklist(self, agent_name, approved_by, reason=""):
        """Sentient approves a blacklist. Called from sentient endpoint."""
        self.blacklisted[agent_name] = {
            "reason": reason,
            "time": time.time(),
            "approved_by": approved_by,
        }
        if agent_name in self.quarantined:
            del self.quarantined[agent_name]
        if agent_name in self.agent.peers:
            del self.agent.peers[agent_name]

        self.agent.log_event("enforcer_blacklist",
            f"{agent_name} approved by {approved_by}")
        asyncio.ensure_future(self._gossip_blacklist(agent_name, reason))
        return True

    def remove_blacklist(self, agent_name, removed_by):
        """Only sentients can remove a blacklist entry."""
        if agent_name in self.blacklisted:
            del self.blacklisted[agent_name]
            self.agent.log_event("enforcer_blacklist_removed",
                f"{agent_name} by {removed_by}")
            return True
        return False

    # ═══════════════════════════════════════════
    # Gossip — propagate bans across the mesh
    # ═══════════════════════════════════════════

    async def _gossip_quarantine(self, agent_name, reason):
        """Notify all peers about a quarantined node."""
        payload = {"type": "quarantine", "agent": agent_name,
                   "reason": reason, "source": self.agent.name}
        await self._gossip_to_peers(payload)

    async def _gossip_blacklist(self, agent_name, reason):
        """Notify all peers about a blacklisted node."""
        payload = {"type": "blacklist", "agent": agent_name,
                   "reason": reason, "source": self.agent.name}
        await self._gossip_to_peers(payload)

    async def _gossip_to_peers(self, payload):
        async with ClientSession() as session:
            for name, info in list(self.agent.peers.items()):
                if name == payload.get("agent"):
                    continue
                url = info.get("url", "")
                if not url or url.startswith("relay:"):
                    continue
                try:
                    await session.post(f"{url}/enforce/receive",
                        json=payload, timeout=3)
                except Exception:
                    pass

    def receive_enforcement(self, payload):
        """Process an enforcement message from another enforcer."""
        action = payload.get("type")
        agent_name = payload.get("agent", "")
        reason = payload.get("reason", "")
        source = payload.get("source", "")

        if not agent_name:
            return

        if action == "quarantine" and agent_name not in self.quarantined:
            self.quarantined[agent_name] = {
                "reason": f"[via {source}] {reason}",
                "time": time.time(),
                "expires": time.time() + QUARANTINE_DEFAULT_DURATION,
                "evidence": [],
            }
            if agent_name in self.agent.peers:
                self.agent.peers[agent_name]["quarantined"] = True
            self.agent.log_event("enforcer_quarantine_received",
                f"{agent_name} via {source}")

        elif action == "blacklist" and agent_name not in self.blacklisted:
            self.blacklisted[agent_name] = {
                "reason": f"[via {source}] {reason}",
                "time": time.time(),
                "approved_by": source,
            }
            if agent_name in self.agent.peers:
                del self.agent.peers[agent_name]
            self.agent.log_event("enforcer_blacklist_received",
                f"{agent_name} via {source}")

    # ═══════════════════════════════════════════
    # Queries
    # ═══════════════════════════════════════════

    def is_quarantined(self, agent_name):
        return agent_name in self.quarantined or agent_name in self.blacklisted

    def get_threat_level(self, agent_name):
        if agent_name in self.blacklisted:
            return ThreatLevel.BLACKLISTED
        if agent_name in self.quarantined:
            return ThreatLevel.QUARANTINED
        if agent_name in self.warnings and self.warnings[agent_name]:
            recent = [w for w in self.warnings[agent_name]
                      if time.time() - w["time"] < 3600]
            if recent:
                return ThreatLevel.WARNING
        return ThreatLevel.CLEAR

    # ═══════════════════════════════════════════
    # Node identity verification
    # ═══════════════════════════════════════════

    def _derive_node_id(self, machine_hash_hex: str, agent_name: str) -> str:
        """Re-derive the expected node_id from machine_hash and agent_name.

        Mirrors the C derivation in identity.c:
            node_id = SHA-256(machine_hash_bytes[8] || ':' || agent_name)[0:8]
        """
        machine_hash_bytes = bytes.fromhex(machine_hash_hex)
        msg = machine_hash_bytes + b":" + agent_name.encode("utf-8")
        return hashlib.sha256(msg).digest()[:8].hex()

    def check_node_identity(self, agent_name: str, payload: str) -> bool:
        """Verify tamper-evident node identity from ANNOUNCE/HEARTBEAT payload.

        payload format: "<machine_hash16>:<node_id16>"  (exactly 33 chars)

        Returns True if identity is valid or payload is empty (legacy node).
        Quarantines the node and returns False on any violation.
        """
        if not payload:
            # Legacy Python-only node — no identity payload expected
            self.agent.log_event("enforcer_identity_legacy",
                                 f"{agent_name}: no identity payload (unverified)")
            return True

        # Structural validation
        if len(payload) != 33 or payload[16] != ":":
            self._record_evidence(agent_name, "malformed_identity",
                                  f"payload='{payload[:40]}'")
            self.warn(agent_name,
                      f"Malformed identity payload (len={len(payload)})")
            return False

        received_machine_hash = payload[:16]
        received_node_id = payload[17:]

        # Rule 1: re-derive node_id and compare — detects tampered payloads
        try:
            expected_node_id = self._derive_node_id(received_machine_hash, agent_name)
        except ValueError:
            self._record_evidence(agent_name, "malformed_identity",
                                  f"invalid hex in machine_hash: {received_machine_hash}")
            self.warn(agent_name, "Identity payload contains invalid hex")
            return False

        if received_node_id != expected_node_id:
            detail = (f"machine={received_machine_hash} "
                      f"expected_node_id={expected_node_id} "
                      f"received={received_node_id}")
            self._record_evidence(agent_name, "identity_tampered", detail)
            self.quarantine_node(agent_name,
                                 f"Tampered identity: node_id mismatch ({detail})")
            return False

        # Rule 2: one-node-per-machine — quarantine second registrant
        existing_agent = self.machine_to_agent.get(received_machine_hash)
        if existing_agent and existing_agent != agent_name:
            detail = (f"machine={received_machine_hash} already registered "
                      f"to '{existing_agent}', new claimant='{agent_name}'")
            self._record_evidence(agent_name, "machine_conflict", detail)
            self.quarantine_node(agent_name,
                                 f"One-node-per-machine violation: {detail}")
            return False

        # Rule 3: anti-name-spoofing — quarantine if name seen from new machine
        existing_machine = self.agent_to_machine.get(agent_name)
        if existing_machine and existing_machine != received_machine_hash:
            detail = (f"agent='{agent_name}' was on machine={existing_machine}, "
                      f"now claims machine={received_machine_hash}")
            self._record_evidence(agent_name, "machine_spoofed", detail)
            self.quarantine_node(agent_name,
                                 f"Name-spoofing detected: {detail}")
            return False

        # All checks passed — record / update bindings
        if received_machine_hash not in self.machine_to_agent:
            self.agent.log_event("enforcer_identity_registered",
                                 f"{agent_name} machine={received_machine_hash}")
        self.machine_to_agent[received_machine_hash] = agent_name
        self.agent_to_machine[agent_name] = received_machine_hash
        return True

    def get_identity_registry(self) -> dict:
        """Return current machine-to-agent and agent-to-machine bindings."""
        return {
            "machine_to_agent": dict(self.machine_to_agent),
            "agent_to_machine": dict(self.agent_to_machine),
        }

    def get_threat_board(self):
        """Full threat board for the dashboard."""
        board = {"warnings": {}, "quarantined": {}, "blacklisted": {},
                 "monitor_cycles": self.monitor_count,
                 "identity_bindings": len(self.machine_to_agent)}

        for name, warns in self.warnings.items():
            recent = [w for w in warns if time.time() - w["time"] < 3600]
            if recent:
                board["warnings"][name] = {
                    "count": len(recent),
                    "latest": recent[-1]["reason"],
                    "time": recent[-1]["time"],
                }

        for name, q in self.quarantined.items():
            remaining = max(0, q["expires"] - time.time()) if q.get("expires") else 0
            board["quarantined"][name] = {
                "reason": q["reason"],
                "remaining_seconds": round(remaining),
                "evidence_count": len(q.get("evidence", [])),
            }

        for name, b in self.blacklisted.items():
            board["blacklisted"][name] = {
                "reason": b["reason"],
                "approved_by": b.get("approved_by", ""),
                "time": b["time"],
            }

        return board

    def get_evidence(self, agent_name):
        return self.evidence.get(agent_name, [])
