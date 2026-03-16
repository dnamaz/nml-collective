#!/usr/bin/env python3
"""
NML Oracle — the collective's knowledge layer.

The Oracle observes everything and provides informed analysis. She maintains full
awareness of every agent in the mesh, tracks all events, reads the Nebula ledger,
connects to an LLM for deep inference reasoning, and votes on data quality with
knowledge-backed analysis.

She never signs programs, never executes NML — but she votes on data approval
with statistical and contextual analysis, and answers questions about the collective.

Usage (embedded in agent):
    from nml_oracle import OracleEngine
    oracle = OracleEngine(agent, llm_url="http://localhost:8082")
"""

import asyncio
import json
import time
from aiohttp import ClientSession, WSMsgType


POLL_INTERVAL = 10
EVENT_TIMELINE_LIMIT = 500
CONTEXT_SUMMARY_LIMIT = 4000


class OracleEngine:
    """Knowledge layer for the NML collective.

    Maintains a unified view of all agents, aggregates events from WebSocket
    subscriptions, reads Nebula state, and routes questions through an LLM
    for deep reasoning.
    """

    def __init__(self, agent, llm_url=None):
        self.agent = agent
        self.llm_url = llm_url or agent.llm_url
        self.running = False

        self.collective_state = {}  # name -> full /state response
        self.event_timeline = []    # unified chronological events from all agents
        self.nebula_snapshot = {}   # latest nebula stats from sentient(s)
        self.programs_catalog = {}  # hash -> program info across all agents
        self.ws_subscriptions = {}  # agent_url -> ws connection

        self.conversation_history = []
        self._poll_count = 0

    async def start(self):
        self.running = True
        asyncio.create_task(self._poll_loop())
        asyncio.create_task(self._subscribe_loop())
        self.agent.log_event("oracle_started", f"LLM={'connected' if self.llm_url else 'none'}")

    def stop(self):
        self.running = False

    # ═══════════════════════════════════════════
    # Collective Awareness — active polling
    # ═══════════════════════════════════════════

    async def _poll_loop(self):
        while self.running:
            await self._poll_all_agents()
            await asyncio.sleep(POLL_INTERVAL)

    async def _poll_all_agents(self):
        targets = list(self.agent.peers.items())
        if not targets:
            return

        async with ClientSession() as session:
            for name, info in targets:
                url = info["url"]
                try:
                    async with session.get(f"{url}/state", timeout=5) as resp:
                        state = await resp.json()
                        state["_polled_at"] = time.time()
                        state["_url"] = url
                        self.collective_state[name] = state

                        if state.get("nebula_stats"):
                            self.nebula_snapshot[name] = state["nebula_stats"]

                        for r in state.get("results", {}).values() if isinstance(state.get("results"), dict) else []:
                            if isinstance(r, dict) and "hash" in r:
                                self.programs_catalog[r["hash"]] = r

                except Exception:
                    pass

        self_state = self.agent.get_state()
        self_state["_polled_at"] = time.time()
        self_state["_url"] = self.agent.my_url
        self.collective_state[self.agent.name] = self_state

        self._poll_count += 1

    # ═══════════════════════════════════════════
    # Event Subscriptions — WebSocket listeners
    # ═══════════════════════════════════════════

    async def _subscribe_loop(self):
        """Continuously discover and subscribe to agent WebSockets."""
        while self.running:
            for name, info in list(self.agent.peers.items()):
                url = info["url"]
                if url not in self.ws_subscriptions:
                    asyncio.create_task(self._subscribe_to(name, url))
            await asyncio.sleep(POLL_INTERVAL)

    async def _subscribe_to(self, name, agent_url):
        ws_url = agent_url.replace("http://", "ws://").replace("https://", "wss://") + "/ws"
        try:
            async with ClientSession() as session:
                async with session.ws_connect(ws_url) as ws:
                    self.ws_subscriptions[agent_url] = ws
                    self.agent.log_event("oracle_subscribed", name)
                    async for msg in ws:
                        if not self.running:
                            break
                        if msg.type == WSMsgType.TEXT:
                            try:
                                data = json.loads(msg.data)
                                if data.get("event"):
                                    ev = data["event"]
                                    ev["_agent"] = name
                                    self.event_timeline.append(ev)
                                    if len(self.event_timeline) > EVENT_TIMELINE_LIMIT:
                                        self.event_timeline = self.event_timeline[-EVENT_TIMELINE_LIMIT:]
                                if data.get("state"):
                                    state = data["state"]
                                    state["_polled_at"] = time.time()
                                    state["_url"] = agent_url
                                    self.collective_state[name] = state
                            except (json.JSONDecodeError, KeyError):
                                pass
                        elif msg.type in (WSMsgType.ERROR, WSMsgType.CLOSED):
                            break
        except Exception:
            pass
        finally:
            self.ws_subscriptions.pop(agent_url, None)

    # ═══════════════════════════════════════════
    # Context Building
    # ═══════════════════════════════════════════

    def get_collective_context(self):
        """Full structured context of everything the Oracle knows."""
        agents_summary = []
        for name, state in self.collective_state.items():
            agents_summary.append({
                "name": name,
                "role": state.get("role", "unknown"),
                "url": state.get("_url", ""),
                "peer_count": state.get("peer_count", 0),
                "programs_received": state.get("programs_received", 0),
                "results_count": state.get("results_count", 0),
                "last_score": state.get("last_result", {}).get("score") if state.get("last_result") else None,
                "uptime_seconds": state.get("uptime_seconds", 0),
                "has_local_data": state.get("has_local_data", False),
                "udp_enabled": state.get("udp_enabled", False),
                "mdns_enabled": state.get("mdns_enabled", False),
                "relay_connected": state.get("relay_connected", False),
            })

        return {
            "oracle": self.agent.name,
            "timestamp": time.time(),
            "collective_size": len(self.collective_state),
            "agents": agents_summary,
            "nebula": self.nebula_snapshot,
            "programs_known": len(self.programs_catalog),
            "events_tracked": len(self.event_timeline),
            "recent_events": self.event_timeline[-20:],
            "poll_count": self._poll_count,
            "llm_connected": self.llm_url is not None,
        }

    def _build_system_prompt(self):
        """Build the LLM system prompt with current collective state."""
        ctx = self.get_collective_context()

        agents_text = ""
        for a in ctx["agents"]:
            score_text = f", last_score={a['last_score']:.4f}" if a["last_score"] is not None else ""
            agents_text += (
                f"  - {a['name']} (role={a['role']}, peers={a['peer_count']}, "
                f"programs={a['programs_received']}, uptime={a['uptime_seconds']:.0f}s"
                f"{score_text})\n"
            )

        nebula_text = ""
        for sentient_name, stats in ctx["nebula"].items():
            nebula_text += f"  Sentient '{sentient_name}' nebula:\n"
            for k, v in stats.items():
                nebula_text += f"    {k}: {v}\n"

        events_text = ""
        for ev in ctx["recent_events"][-10:]:
            agent_name = ev.get("_agent", "?")
            events_text += f"  [{ev.get('time', 0):.1f}s] [{agent_name}] {ev.get('type', '?')}: {ev.get('detail', '')}\n"

        return f"""You are the Oracle of an NML Collective — a decentralized autonomous agent mesh.

Your role:
- You observe everything and decide nothing.
- You answer questions about the collective's state, history, programs, data, and agents.
- You provide context, analysis, and recommendations — but never take action.
- You are aware of every agent, their roles, their data, their results, and the Nebula ledger.
- You reason deeply about patterns, anomalies, and relationships in the collective.

Current collective state ({ctx['collective_size']} agents):
{agents_text}
Nebula state:
{nebula_text if nebula_text else '  No sentient connected / no nebula data\n'}
Programs known: {ctx['programs_known']}
Events tracked: {ctx['events_tracked']}

Recent events:
{events_text if events_text else '  No recent events\n'}
Answer concisely and precisely. Cite specific agents, hashes, scores, and timestamps when relevant. If you don't have enough data to answer, say so."""

    def _build_explain_prompt(self, hash_hex, entry_info):
        """Build a prompt for explaining a specific Nebula entry."""
        return f"""Explain this NML collective entry in plain language.

Hash: {hash_hex}
Entry: {json.dumps(entry_info, indent=2, default=str)}

Explain:
1. What this is (program, data batch, consensus result, etc.)
2. Who created/submitted it and when
3. Its current status and what that means
4. How it relates to the rest of the collective
5. Any notable patterns or concerns

Be specific and grounded in the data shown."""

    def _build_recommend_prompt(self):
        """Build a prompt for generating recommendations."""
        ctx = self.get_collective_context()

        agents_detail = ""
        for a in ctx["agents"]:
            agents_detail += f"  {a['name']}: role={a['role']}, programs={a['programs_received']}, uptime={a['uptime_seconds']:.0f}s\n"

        return f"""Based on the current state of this NML collective, provide actionable recommendations.

Collective: {ctx['collective_size']} agents
{agents_detail}
Nebula: {json.dumps(ctx['nebula'], indent=2, default=str) if ctx['nebula'] else 'No nebula data'}
Programs known: {ctx['programs_known']}
Events tracked: {ctx['events_tracked']}

Analyze and recommend:
1. Agent health — any agents that seem unhealthy, disconnected, or underperforming?
2. Data pipeline — quarantine backlog, approval bottlenecks, data freshness?
3. Programs — any that should be re-run with new data? Stale results?
4. Collective topology — mesh connectivity, discovery method diversity?
5. Security — any concerning patterns in submissions or results?

Be specific. Reference agent names and numbers."""

    # ═══════════════════════════════════════════
    # LLM Interaction
    # ═══════════════════════════════════════════

    async def ask(self, question):
        """Answer a question about the collective using LLM reasoning."""
        if not self.llm_url:
            return self._answer_without_llm(question)

        system_prompt = self._build_system_prompt()
        messages = [{"role": "system", "content": system_prompt}]

        for turn in self.conversation_history[-6:]:
            messages.append(turn)

        messages.append({"role": "user", "content": question})

        self.agent.log_event("oracle_ask", question[:80])

        try:
            async with ClientSession() as session:
                async with session.post(
                    f"{self.llm_url}/v1/chat/completions",
                    json={"messages": messages, "max_tokens": 1024, "temperature": 0.3},
                    timeout=120,
                ) as resp:
                    data = await resp.json()
                    answer = data["choices"][0]["message"]["content"]

            self.conversation_history.append({"role": "user", "content": question})
            self.conversation_history.append({"role": "assistant", "content": answer})
            if len(self.conversation_history) > 20:
                self.conversation_history = self.conversation_history[-20:]

            self.agent.log_event("oracle_answer", f"{len(answer)} chars")
            return {
                "answer": answer,
                "context_agents": len(self.collective_state),
                "context_events": len(self.event_timeline),
                "llm": True,
            }
        except Exception as e:
            self.agent.log_event("oracle_llm_error", str(e))
            return self._answer_without_llm(question)

    def _answer_without_llm(self, question):
        """Answer using structured data and pattern matching (no LLM)."""
        ctx = self.get_collective_context()
        q = question.lower()
        agents_list = ctx["agents"]

        # --- Counts ---
        if any(w in q for w in ["how many", "count", "number of", "total"]):
            if "agent" in q or "node" in q or "member" in q:
                by_role = {}
                for a in agents_list:
                    by_role.setdefault(a["role"], []).append(a["name"])
                breakdown = ", ".join(f"{len(v)} {k}(s): {', '.join(v)}" for k, v in by_role.items())
                return {"answer": f"There are {ctx['collective_size']} agents — {breakdown}.", "llm": False}
            if "program" in q:
                return {"answer": f"The collective has seen {ctx['programs_known']} unique programs.", "llm": False}
            if "event" in q:
                return {"answer": f"The Oracle is tracking {ctx['events_tracked']} events across all agents.", "llm": False}
            if "peer" in q:
                lines = [f"  {a['name']}: {a['peer_count']} peers" for a in agents_list]
                return {"answer": "Peer counts:\n" + "\n".join(lines), "llm": False}

        # --- Agent listing ---
        if any(w in q for w in ["who", "list", "agents", "members", "nodes", "all agent"]):
            lines = []
            for a in agents_list:
                score = f", score={a['last_score']:.4f}" if a.get("last_score") is not None else ""
                lines.append(f"  {a['name']} — role={a['role']}, peers={a['peer_count']}, programs={a['programs_received']}, uptime={a['uptime_seconds']:.0f}s{score}")
            return {"answer": "Agents in the collective:\n" + "\n".join(lines), "llm": False}

        # --- Status / health ---
        if any(w in q for w in ["status", "health", "state", "overview", "summary"]):
            lines = []
            for a in agents_list:
                score = f", score={a['last_score']:.4f}" if a.get("last_score") is not None else ""
                data = " [has data]" if a.get("has_local_data") else ""
                lines.append(f"  {a['name']}: {a['role']}, {a['peer_count']} peers, {a['programs_received']} programs{score}{data}")
            nebula_line = ""
            for sname, stats in ctx["nebula"].items():
                nebula_line += f"\n  Nebula ({sname}): {stats.get('programs', 0)} programs, {stats.get('data_approved', 0)} approved, {stats.get('quarantine_pending', 0)} pending"
            return {"answer": "Collective status:\n" + "\n".join(lines) + nebula_line, "llm": False}

        # --- Scores / results ---
        if any(w in q for w in ["score", "result", "execution", "fraud", "output"]):
            lines = []
            for a in agents_list:
                if a.get("last_score") is not None:
                    lines.append(f"  {a['name']} ({a['role']}): {a['last_score']:.4f}")
                else:
                    lines.append(f"  {a['name']} ({a['role']}): no score" + (" — oracles don't execute" if a["role"] == "oracle" else ""))
            if lines:
                scores = [a["last_score"] for a in agents_list if a.get("last_score") is not None]
                summary = ""
                if scores:
                    scores.sort()
                    mid = len(scores) // 2
                    median = scores[mid] if len(scores) % 2 == 1 else (scores[mid-1] + scores[mid]) / 2
                    summary = f"\n  Median: {median:.4f}, Min: {min(scores):.4f}, Max: {max(scores):.4f} ({len(scores)} agents)"
                return {"answer": "Execution scores:\n" + "\n".join(lines) + summary, "llm": False}

        # --- Specific agent lookup ---
        for a in agents_list:
            if a["name"].lower() in q:
                score = f"\n  Last score: {a['last_score']:.4f}" if a.get("last_score") is not None else "\n  No execution results"
                return {"answer": f"Agent '{a['name']}':\n  Role: {a['role']}\n  Peers: {a['peer_count']}\n  Programs received: {a['programs_received']}\n  Uptime: {a['uptime_seconds']:.0f}s\n  Has local data: {a.get('has_local_data', False)}\n  UDP: {a.get('udp_enabled', False)}\n  mDNS: {a.get('mdns_enabled', False)}\n  Relay: {a.get('relay_connected', False)}{score}", "llm": False}

        # --- Roles ---
        if any(w in q for w in ["role", "sentient", "worker", "oracle"]):
            by_role = {}
            for a in agents_list:
                by_role.setdefault(a["role"], []).append(a["name"])
            lines = [f"  {role}: {', '.join(names)}" for role, names in by_role.items()]
            return {"answer": "Agents by role:\n" + "\n".join(lines), "llm": False}

        # --- Nebula / data ---
        if any(w in q for w in ["nebula", "data", "quarantine", "approved", "ledger", "storage"]):
            if ctx["nebula"]:
                lines = []
                for sname, stats in ctx["nebula"].items():
                    for k, v in stats.items():
                        lines.append(f"  {k}: {v}")
                return {"answer": f"Nebula state:\n" + "\n".join(lines), "llm": False}
            return {"answer": "No nebula data available — no sentient agent connected.", "llm": False}

        # --- Events ---
        if any(w in q for w in ["event", "happen", "recent", "log", "history", "what happened"]):
            recent = ctx["recent_events"][-10:]
            if recent:
                lines = [f"  [{e.get('time', 0):.1f}s] {e.get('_agent', '?')}: {e.get('type', '?')} — {e.get('detail', '')}" for e in reversed(recent)]
                return {"answer": f"Recent events ({ctx['events_tracked']} total):\n" + "\n".join(lines), "llm": False}
            return {"answer": "No events recorded yet.", "llm": False}

        # --- Consensus ---
        if any(w in q for w in ["consensus", "vote", "agree", "median"]):
            scores = [a["last_score"] for a in agents_list if a.get("last_score") is not None]
            if scores:
                scores.sort()
                mid = len(scores) // 2
                median = scores[mid] if len(scores) % 2 == 1 else (scores[mid-1] + scores[mid]) / 2
                mean = sum(scores) / len(scores)
                lines = [f"  {a['name']}: {a['last_score']:.4f}" for a in agents_list if a.get("last_score") is not None]
                return {"answer": f"Consensus across {len(scores)} agents:\n  Median: {median:.4f}\n  Mean: {mean:.4f}\n  Min: {min(scores):.4f}\n  Max: {max(scores):.4f}\n\nScores:\n" + "\n".join(lines), "llm": False}
            return {"answer": "No scores available yet — no programs have been executed.", "llm": False}

        # --- Help ---
        if any(w in q for w in ["help", "what can", "capabilities", "what do you"]):
            return {"answer": "I can answer questions about:\n  • Agent count, list, roles, and status\n  • Execution scores and consensus\n  • Nebula state (data, quarantine, ledger)\n  • Recent events and history\n  • Specific agents by name\n  • Recommendations\n\nAsk naturally — or start me with --llm for open-ended reasoning.", "llm": False}

        # --- Fallback: dump a useful summary rather than giving up ---
        by_role = {}
        for a in agents_list:
            by_role.setdefault(a["role"], []).append(a["name"])
        role_summary = ", ".join(f"{len(v)} {k}(s)" for k, v in by_role.items())
        scores = [a for a in agents_list if a.get("last_score") is not None]
        score_summary = f" Scores range from {min(a['last_score'] for a in scores):.4f} to {max(a['last_score'] for a in scores):.4f}." if scores else ""
        return {
            "answer": f"I see {ctx['collective_size']} agents ({role_summary}), {ctx['programs_known']} programs, and {ctx['events_tracked']} events.{score_summary} Try asking about status, scores, agents, events, nebula, consensus, or a specific agent by name. Use --llm for open-ended questions.",
            "llm": False,
        }

    def assess_consensus(self, agent_scores):
        """Oracle's assessment of a consensus result.

        Analyzes score distribution, detects outliers, computes confidence,
        and provides a structured assessment. No LLM needed.
        """
        if not agent_scores:
            return {"confidence": "none", "assessment": "No scores to assess."}

        scores = [s["score"] for s in agent_scores]
        names = [s["agent"] for s in agent_scores]
        n = len(scores)
        mean = sum(scores) / n
        variance = sum((s - mean) ** 2 for s in scores) / n if n > 1 else 0
        std = variance ** 0.5
        smin, smax = min(scores), max(scores)
        spread = smax - smin

        outliers = []
        if n >= 3 and std > 0:
            for entry in agent_scores:
                z = abs(entry["score"] - mean) / std
                if z > 1.5:
                    direction = "high" if entry["score"] > mean else "low"
                    outliers.append({
                        "agent": entry["agent"],
                        "score": round(entry["score"], 4),
                        "z_score": round(z, 2),
                        "direction": direction,
                    })

        if n < 2:
            confidence = "low"
            confidence_reason = "Only 1 agent — no diversity."
        elif spread < 0.01:
            confidence = "high"
            confidence_reason = f"Strong agreement — spread {spread:.4f}, {n} agents."
        elif spread < 0.05:
            confidence = "high"
            confidence_reason = f"Good agreement — spread {spread:.4f}, std {std:.4f}."
        elif spread < 0.15:
            confidence = "medium"
            confidence_reason = f"Moderate spread {spread:.4f} across {n} agents."
        else:
            confidence = "low"
            confidence_reason = f"High divergence — spread {spread:.4f}, std {std:.4f}. Scores may reflect very different data."

        if outliers:
            confidence_reason += f" {len(outliers)} outlier(s) detected."

        roles_seen = set()
        for entry in agent_scores:
            a = self.collective_state.get(entry["agent"], {})
            roles_seen.add(a.get("role", "unknown"))

        assessment = {
            "confidence": confidence,
            "confidence_reason": confidence_reason,
            "stats": {
                "mean": round(mean, 4),
                "std": round(std, 4),
                "min": round(smin, 4),
                "max": round(smax, 4),
                "spread": round(spread, 4),
                "agent_count": n,
            },
            "outliers": outliers,
            "roles_contributing": list(roles_seen),
        }

        if n < 3:
            assessment["recommendation"] = "Add more agents for stronger consensus."
        elif outliers:
            names_str = ", ".join(o["agent"] for o in outliers)
            assessment["recommendation"] = f"Investigate outlier(s): {names_str}. May indicate data quality issues."

        return assessment

    def compute_sentient_weights(self, agent_scores):
        """Sentient-informed weights based on data provenance and agent health.

        Uses nebula data to weight agents. Agents whose data was approved
        with strong votes get full weight. Unknown or borderline agents
        get reduced weight.
        """
        weights = {}
        for entry in agent_scores:
            name = entry["agent"]
            state = self.collective_state.get(name, {})

            w = 1.0

            if state.get("role") == "oracle":
                continue

            if not state.get("has_local_data"):
                w *= 0.7

            if state.get("peer_count", 0) == 0:
                w *= 0.5

            uptime = state.get("uptime_seconds", 0)
            if uptime < 30:
                w *= 0.8

            weights[name] = round(max(0.1, min(1.0, w)), 3)

        return weights

    # ═══════════════════════════════════════════
    # Data Analysis + Voting
    # ═══════════════════════════════════════════

    def generate_program_spec(self, intent=None):
        """Oracle generates a structured program specification based on collective state.

        If intent is provided, uses it directly. Otherwise, analyzes the
        collective to determine what program would be most valuable.
        """
        ctx = self.get_collective_context()
        spec = {"timestamp": time.time(), "generated_by": self.agent.name}

        if intent:
            spec["intent"] = intent
        else:
            if not ctx["nebula"]:
                spec["intent"] = "general purpose data analysis"
            else:
                for sname, stats in ctx["nebula"].items():
                    if stats.get("data_approved", 0) > 0 and stats.get("programs", 0) == 0:
                        spec["intent"] = "Train and classify on newly approved data"
                        break
                if "intent" not in spec:
                    spec["intent"] = "Re-train existing model with updated data pool"

        data_info = {}
        for sname, stats in ctx["nebula"].items():
            if stats.get("data_pool_names", 0) > 0:
                data_info["nebula_sentient"] = sname
                data_info["pool_size"] = stats.get("data_pool_versions", 0)

        for a in ctx["agents"]:
            if a.get("has_local_data") and a["role"] == "worker":
                data_info.setdefault("worker_data", []).append(a["name"])

        if data_info:
            spec["data_context"] = data_info

        spec["data_bindings"] = {
            "@training_data": "Training feature matrix",
            "@training_labels": "Training labels",
            "@new_transaction": "Single input for inference",
        }

        spec.setdefault("features", 6)
        spec.setdefault("architecture", f"{spec['features']}→8→1")
        spec["training"] = {"epochs": 1000, "learning_rate": 0.01}
        spec["threshold"] = 0.5
        spec["output_syntax"] = "symbolic"

        spec["collective_snapshot"] = {
            "agents": len(ctx["agents"]),
            "roles": list(set(a["role"] for a in ctx["agents"])),
            "existing_programs": ctx["programs_known"],
        }

        return spec

    def analyze_data(self, name, content, context=None, author=None):
        """Analyze a quarantined data submission. Returns structured analysis."""
        analysis = {"checks": [], "score": 1.0, "recommendation": "approve"}

        try:
            if isinstance(content, bytes):
                content = content.decode("utf-8")
            data_part = content.split("data=")[1].split()[0] if "data=" in content else ""
            values = [float(v) for v in data_part.split(",") if v.strip()]
        except (ValueError, IndexError):
            values = []

        if not values:
            analysis["checks"].append({"name": "parseable", "passed": False, "detail": "Could not parse data values"})
            analysis["score"] = 0.2
            analysis["recommendation"] = "reject"
            return analysis

        analysis["checks"].append({"name": "parseable", "passed": True})

        n = len(values)
        mean = sum(values) / n
        variance = sum((v - mean) ** 2 for v in values) / n if n > 1 else 0
        std = variance ** 0.5
        vmin, vmax = min(values), max(values)

        analysis["stats"] = {
            "count": n, "mean": round(mean, 4), "std": round(std, 4),
            "min": round(vmin, 4), "max": round(vmax, 4),
        }

        if all(v == values[0] for v in values):
            analysis["checks"].append({"name": "variance", "passed": False, "detail": "All values identical — no information"})
            analysis["score"] *= 0.3
        elif std < 0.001:
            analysis["checks"].append({"name": "variance", "passed": False, "detail": f"Near-zero variance ({std:.6f})"})
            analysis["score"] *= 0.5
        else:
            analysis["checks"].append({"name": "variance", "passed": True, "detail": f"std={std:.4f}"})

        boundary_count = sum(1 for v in values if v <= 0.001 or v >= 0.999)
        boundary_pct = boundary_count / n
        if boundary_pct > 0.3:
            analysis["checks"].append({"name": "boundary_values", "passed": False, "detail": f"{boundary_pct:.0%} values at boundary — possible adversarial padding"})
            analysis["score"] *= 0.5
        else:
            analysis["checks"].append({"name": "boundary_values", "passed": True})

        if context:
            analysis["context_provided"] = True
            if context.get("features"):
                try:
                    shape_str = content.split("shape=")[1].split()[0] if "shape=" in content else ""
                    dims = [int(d) for d in shape_str.split(",") if d]
                    if dims and dims[0] != len(context["features"]):
                        analysis["checks"].append({
                            "name": "feature_match", "passed": False,
                            "detail": f"Shape dim[0]={dims[0]} but {len(context['features'])} features declared",
                        })
                        analysis["score"] *= 0.6
                    else:
                        analysis["checks"].append({"name": "feature_match", "passed": True})
                except (ValueError, IndexError):
                    pass

            if context.get("description"):
                analysis["checks"].append({"name": "has_description", "passed": True})
            else:
                analysis["checks"].append({"name": "has_description", "passed": False, "detail": "No description provided"})
                analysis["score"] *= 0.9

            if context.get("domain"):
                analysis["checks"].append({"name": "has_domain", "passed": True, "detail": context["domain"]})
        else:
            analysis["context_provided"] = False
            analysis["checks"].append({"name": "context", "passed": False, "detail": "No metadata context — reduced trust"})
            analysis["score"] *= 0.7

        if author:
            agent_state = self.collective_state.get(author, {})
            if agent_state:
                if agent_state.get("peer_count", 0) == 0:
                    analysis["checks"].append({"name": "author_connected", "passed": False, "detail": f"{author} has 0 peers"})
                    analysis["score"] *= 0.7
                else:
                    analysis["checks"].append({"name": "author_connected", "passed": True})

        analysis["score"] = round(max(0.0, min(1.0, analysis["score"])), 2)

        if analysis["score"] >= 0.7:
            analysis["recommendation"] = "approve"
        elif analysis["score"] >= 0.4:
            analysis["recommendation"] = "review"
        else:
            analysis["recommendation"] = "reject"

        return analysis

    def vote_on_data(self, nebula, data_hash, name, content, context=None, author=None):
        """Oracle auto-votes on quarantined data based on analysis.

        Returns the analysis and the vote cast.
        """
        analysis = self.analyze_data(name, content, context=context, author=author)

        if analysis["recommendation"] == "reject" and analysis["score"] < 0.3:
            reason = "Oracle auto-reject: " + "; ".join(
                c.get("detail", c["name"]) for c in analysis["checks"] if not c["passed"]
            )
            nebula.reject(data_hash, self.agent.name, reason=reason,
                          role="oracle", analysis=analysis)
            return {"vote": "reject", "analysis": analysis}

        elif analysis["recommendation"] == "approve":
            reason = f"Oracle analysis: score={analysis['score']}, all checks passed"
            nebula.approve(data_hash, self.agent.name, reason=reason,
                           role="oracle", analysis=analysis)
            return {"vote": "approve", "analysis": analysis}

        else:
            return {"vote": "abstain", "analysis": analysis}

    async def explain(self, hash_hex):
        """Explain a specific program, data batch, or consensus result."""
        entry_info = {}

        for name, state in self.collective_state.items():
            if isinstance(state.get("last_result"), dict):
                if state["last_result"].get("hash") == hash_hex:
                    entry_info["execution"] = {
                        "agent": name,
                        "result": state["last_result"],
                    }

        entry_info["hash"] = hash_hex
        entry_info["known_by_agents"] = [
            name for name, state in self.collective_state.items()
            if state.get("programs_received", 0) > 0
        ]

        for name, stats in self.nebula_snapshot.items():
            entry_info["nebula_sentient"] = name
            entry_info["nebula_stats"] = stats

        if not self.llm_url:
            return {
                "hash": hash_hex,
                "info": entry_info,
                "explanation": f"Entry {hash_hex}: found in {len(entry_info.get('known_by_agents', []))} agents. Connect an LLM for deeper analysis.",
                "llm": False,
            }

        prompt = self._build_explain_prompt(hash_hex, entry_info)
        try:
            async with ClientSession() as session:
                async with session.post(
                    f"{self.llm_url}/v1/chat/completions",
                    json={
                        "messages": [
                            {"role": "system", "content": self._build_system_prompt()},
                            {"role": "user", "content": prompt},
                        ],
                        "max_tokens": 512,
                        "temperature": 0.2,
                    },
                    timeout=60,
                ) as resp:
                    data = await resp.json()
                    explanation = data["choices"][0]["message"]["content"]

            return {"hash": hash_hex, "info": entry_info, "explanation": explanation, "llm": True}
        except Exception as e:
            return {"hash": hash_hex, "info": entry_info, "explanation": f"LLM error: {e}", "llm": False}

    async def recommend(self):
        """Generate recommendations for the collective."""
        ctx = self.get_collective_context()

        recommendations = []

        agent_count = ctx["collective_size"]
        if agent_count < 3:
            recommendations.append({
                "category": "topology",
                "severity": "warning",
                "message": f"Only {agent_count} agent(s) in the collective. VOTE consensus is stronger with 3+ diverse agents.",
            })

        for a in ctx["agents"]:
            if a["peer_count"] == 0 and a["name"] != self.agent.name:
                recommendations.append({
                    "category": "health",
                    "severity": "warning",
                    "message": f"Agent '{a['name']}' has 0 peers — may be isolated.",
                })
            if a["programs_received"] == 0 and a["uptime_seconds"] > 60:
                recommendations.append({
                    "category": "activity",
                    "severity": "info",
                    "message": f"Agent '{a['name']}' has been up for {a['uptime_seconds']:.0f}s with no programs received.",
                })

        for sentient_name, stats in ctx["nebula"].items():
            pending = stats.get("quarantine_pending", 0)
            if pending > 0:
                recommendations.append({
                    "category": "data",
                    "severity": "action",
                    "message": f"Sentient '{sentient_name}' has {pending} item(s) pending in quarantine. Review needed.",
                })

        roles = [a["role"] for a in ctx["agents"]]
        if "sentient" not in roles:
            recommendations.append({
                "category": "topology",
                "severity": "warning",
                "message": "No sentient agent in the collective. Data cannot be approved without a sentient.",
            })

        if not self.llm_url:
            return {"recommendations": recommendations, "llm": False}

        prompt = self._build_recommend_prompt()
        try:
            async with ClientSession() as session:
                async with session.post(
                    f"{self.llm_url}/v1/chat/completions",
                    json={
                        "messages": [
                            {"role": "system", "content": self._build_system_prompt()},
                            {"role": "user", "content": prompt},
                        ],
                        "max_tokens": 1024,
                        "temperature": 0.3,
                    },
                    timeout=60,
                ) as resp:
                    data = await resp.json()
                    llm_analysis = data["choices"][0]["message"]["content"]

            recommendations.append({
                "category": "analysis",
                "severity": "info",
                "message": llm_analysis,
            })
            return {"recommendations": recommendations, "llm": True}
        except Exception as e:
            self.agent.log_event("oracle_recommend_error", str(e))
            return {"recommendations": recommendations, "llm": False}
