#!/usr/bin/env python3
"""
NML Collective Agent — autonomous gossip-based peer in a decentralized mesh.

Each agent:
  - Discovers peers via seed list + gossip (HTTP)
  - Discovers peers via UDP multicast (zero-config on LAN)
  - Broadcasts signed programs to all peers (UDP multicast + HTTP fallback)
  - Executes programs locally with its own data
  - Generates NML via a shared central LLM
  - Participates in VOTE consensus

Transport:
  - UDP multicast (239.78.77.76:7776) for discovery + compact program broadcast
  - HTTP (aiohttp) for dashboard API, LLM proxy, consensus, and fallback

An NML program fits in a single UDP packet (~500-700 bytes in compact form).

Usage:
    python3 nml_collective.py --name agent_1 --port 9001
    python3 nml_collective.py --name agent_2 --port 9002 --seeds http://localhost:9001
    python3 nml_collective.py --name agent_3 --port 9003 --udp-only --data demos/agent3.nml.data
"""

import argparse
import asyncio
import atexit
import hashlib
import json
import signal
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path
from aiohttp import web, ClientSession

PROJECT_ROOT = Path(__file__).parent.parent

def _find_nml():
    """Find the NML binary — check sibling nml/ repo, then PATH."""
    candidates = [
        PROJECT_ROOT / "nml-crypto",
        PROJECT_ROOT / "nml",
        PROJECT_ROOT.parent / "nml" / "nml-crypto",
        PROJECT_ROOT.parent / "nml" / "nml",
    ]
    for c in candidates:
        if c.exists():
            return c
    import shutil
    found = shutil.which("nml-crypto") or shutil.which("nml")
    return Path(found) if found else Path("nml")

NML_BINARY = _find_nml()
DASHBOARD_PATH = PROJECT_ROOT / "dashboard" / "nml_collective_dashboard.html"

HEARTBEAT_INTERVAL = 5
HEARTBEAT_MISS_LIMIT = 3

MDNS_SERVICE_TYPE = "_nml._tcp.local."

# UDP Multicast config
UDP_MULTICAST_GROUP = "239.78.77.76"
UDP_MULTICAST_PORT = 7776
UDP_MAX_PACKET = 65000

# UDP message types
MSG_ANNOUNCE = 1
MSG_PROGRAM = 2
MSG_RESULT = 3
MSG_HEARTBEAT = 4


async def execute_nml(program: str, data: str = None) -> dict:
    with tempfile.NamedTemporaryFile(mode="w", suffix=".nml", delete=False) as pf:
        pf.write(program)
        prog_path = pf.name

    data_path = None
    if data:
        with tempfile.NamedTemporaryFile(mode="w", suffix=".nml.data", delete=False) as df:
            df.write(data)
            data_path = df.name

    cmd = [str(NML_BINARY), prog_path]
    if data_path:
        cmd.append(data_path)
    cmd.extend(["--max-cycles", "1000000"])

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        output = result.stdout + result.stderr

        memory = {}
        in_memory = False
        for line in output.split("\n"):
            if "=== MEMORY ===" in line:
                in_memory = True
                continue
            if "=== STATS ===" in line:
                in_memory = False
                continue
            if in_memory and ":" in line:
                parts = line.strip().split(":", 1)
                memory[parts[0].strip()] = parts[1].strip()

        return {
            "success": result.returncode == 0,
            "output": output,
            "memory": memory,
            "exit_code": result.returncode,
        }
    except subprocess.TimeoutExpired:
        return {"success": False, "output": "Timeout", "memory": {}, "exit_code": -1}
    finally:
        Path(prog_path).unlink(missing_ok=True)
        if data_path:
            Path(data_path).unlink(missing_ok=True)


def program_hash(program: str) -> str:
    return hashlib.sha256(program.encode()).hexdigest()[:16]


def to_compact(program: str) -> str:
    """Convert multi-line NML to compact pilcrow-delimited form."""
    lines = [l.strip() for l in program.split("\n") if l.strip() and not l.strip().startswith(";")]
    return "\xb6".join(lines)


def from_compact(compact: str) -> str:
    """Convert compact form back to multi-line."""
    return compact.replace("\xb6", "\n")


def encode_udp_message(msg_type: int, agent_name: str, http_port: int, payload: str = "") -> bytes:
    """Encode a UDP message: 4-byte magic + 1-byte type + 1-byte name_len + name + 2-byte port + payload."""
    name_bytes = agent_name.encode("utf-8")[:63]
    port_bytes = struct.pack("!H", http_port)
    payload_bytes = payload.encode("utf-8")
    return b"NML\x01" + bytes([msg_type, len(name_bytes)]) + name_bytes + port_bytes + payload_bytes


def decode_udp_message(data: bytes):
    """Decode a UDP message. Returns (msg_type, agent_name, http_port, payload) or None."""
    if len(data) < 8 or data[:4] != b"NML\x01":
        return None
    msg_type = data[4]
    name_len = data[5]
    if len(data) < 6 + name_len + 2:
        return None
    agent_name = data[6:6 + name_len].decode("utf-8")
    http_port = struct.unpack("!H", data[6 + name_len:8 + name_len])[0]
    payload = data[8 + name_len:].decode("utf-8", errors="replace")
    return msg_type, agent_name, http_port, payload


class NMLMulticastProtocol(asyncio.DatagramProtocol):
    """asyncio-native UDP multicast protocol for the collective."""

    def __init__(self, agent):
        self.agent = agent
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        decoded = decode_udp_message(data)
        if not decoded:
            return

        msg_type, peer_name, peer_port, payload = decoded
        if peer_name == self.agent.name:
            return

        peer_url = f"http://{addr[0]}:{peer_port}"

        if msg_type == MSG_ANNOUNCE or msg_type == MSG_HEARTBEAT:
            self.agent._add_peer(peer_name, peer_url)
            if msg_type == MSG_ANNOUNCE:
                self.agent.log_event("udp_peer", f"{peer_name} at {peer_url}")

        elif msg_type == MSG_PROGRAM:
            program = from_compact(payload)
            self.agent.log_event("udp_program", f"from {peer_name}, {len(data)} bytes")
            asyncio.ensure_future(
                self.agent.broadcast_program(program, source_name="__udp__")
            )

        elif msg_type == MSG_RESULT:
            parts = payload.split(":", 1)
            if len(parts) == 2:
                try:
                    phash, score_str = parts
                    score = float(score_str)
                    self.agent.log_event("udp_result", f"{peer_name}: {phash[:8]}={score:.4f}")
                except ValueError:
                    pass


class UDPTransport:
    """UDP multicast transport for zero-config discovery and compact program broadcast."""

    def __init__(self, agent):
        self.agent = agent
        self.send_sock = None
        self.protocol = None
        self.running = False

    async def start(self):
        loop = asyncio.get_event_loop()

        self.send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        self.send_sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
        self.send_sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)

        recv_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        recv_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            recv_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except AttributeError:
            pass
        recv_sock.bind(("", UDP_MULTICAST_PORT))
        mreq = struct.pack("4sL", socket.inet_aton(UDP_MULTICAST_GROUP), socket.INADDR_ANY)
        recv_sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

        transport, self.protocol = await loop.create_datagram_endpoint(
            lambda: NMLMulticastProtocol(self.agent),
            sock=recv_sock,
        )
        self.running = True
        asyncio.create_task(self._announce_loop())

    def send(self, msg_type: int, payload: str = ""):
        msg = encode_udp_message(msg_type, self.agent.name, self.agent.port, payload)
        if len(msg) > UDP_MAX_PACKET:
            return False
        try:
            self.send_sock.sendto(msg, (UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT))
            return True
        except Exception as e:
            self.agent.log_event("udp_send_error", str(e))
            return False

    def announce(self):
        self.send(MSG_ANNOUNCE)

    def broadcast_program(self, program: str):
        compact = to_compact(program)
        sent = self.send(MSG_PROGRAM, compact)
        if sent:
            self.agent.log_event("udp_broadcast", f"{len(compact)} bytes compact")
        return sent

    def broadcast_result(self, phash: str, score: float):
        self.send(MSG_RESULT, f"{phash}:{score:.6f}")

    async def _announce_loop(self):
        while self.running:
            self.announce()
            await asyncio.sleep(HEARTBEAT_INTERVAL)

    def stop(self):
        self.running = False


class RelayTransport:
    """WebSocket relay transport for WAN-scale collectives."""

    def __init__(self, agent, relay_url):
        self.agent = agent
        self.relay_url = relay_url
        self.ws = None
        self.running = False

    async def start(self):
        import aiohttp
        self.running = True
        asyncio.create_task(self._connect_loop())

    async def _connect_loop(self):
        import aiohttp
        while self.running:
            try:
                async with ClientSession() as session:
                    async with session.ws_connect(self.relay_url) as ws:
                        self.ws = ws
                        await ws.send_json({
                            "type": "register",
                            "name": self.agent.name,
                            "port": self.agent.port,
                        })
                        self.agent.log_event("relay_connected", self.relay_url)

                        async for msg in ws:
                            if not self.running:
                                break
                            if msg.type == aiohttp.WSMsgType.TEXT:
                                try:
                                    data = json.loads(msg.data)
                                except json.JSONDecodeError:
                                    continue

                                if data.get("type") == "peers":
                                    for p in data.get("peers", []):
                                        if p["name"] != self.agent.name:
                                            self.agent.log_event("relay_peer", p["name"])
                                elif data.get("msg_type") == "program":
                                    program = data.get("program", "")
                                    source = data.get("source", "relay")
                                    if program:
                                        asyncio.create_task(
                                            self.agent.broadcast_program(program, source_name=source)
                                        )
                                elif data.get("msg_type") == "announce":
                                    peer_name = data.get("name")
                                    peer_port = data.get("port")
                                    if peer_name and peer_name != self.agent.name:
                                        self.agent._add_peer(peer_name, f"relay:{peer_port}")

                            elif msg.type == aiohttp.WSMsgType.BINARY:
                                decoded = decode_udp_message(msg.data)
                                if decoded:
                                    msg_type, peer_name, peer_port, payload = decoded
                                    if peer_name != self.agent.name:
                                        if msg_type == MSG_ANNOUNCE:
                                            self.agent._add_peer(peer_name, f"relay:{peer_port}")
                                        elif msg_type == MSG_PROGRAM:
                                            program = from_compact(payload)
                                            asyncio.create_task(
                                                self.agent.broadcast_program(program, source_name=peer_name)
                                            )

                            elif msg.type in (aiohttp.WSMsgType.ERROR, aiohttp.WSMsgType.CLOSED):
                                break

            except Exception as e:
                self.agent.log_event("relay_error", str(e))
            self.ws = None
            if self.running:
                await asyncio.sleep(5)

    def send_program(self, program: str):
        if self.ws and not self.ws.closed:
            asyncio.create_task(self.ws.send_json({
                "msg_type": "program",
                "program": program,
                "source": self.agent.name,
            }))
            return True
        return False

    def send_binary(self, data: bytes):
        if self.ws and not self.ws.closed:
            asyncio.create_task(self.ws.send_bytes(data))
            return True
        return False

    def stop(self):
        self.running = False


class MDNSDiscovery:
    """mDNS/Bonjour service registration and discovery for NML agents."""

    def __init__(self, agent):
        self.agent = agent
        self.azc = None
        self.info = None
        self.browser = None

    async def start(self):
        try:
            from zeroconf import ServiceInfo, ServiceStateChange
            from zeroconf.asyncio import AsyncZeroconf, AsyncServiceBrowser
        except ImportError:
            self.agent.log_event("mdns_unavailable", "pip install zeroconf")
            return False

        self.azc = AsyncZeroconf()
        local_ip = self._get_local_ip()

        self.info = ServiceInfo(
            MDNS_SERVICE_TYPE,
            f"{self.agent.name}.{MDNS_SERVICE_TYPE}",
            addresses=[socket.inet_aton(local_ip)],
            port=self.agent.port,
            properties={
                b"name": self.agent.name.encode(),
                b"version": b"0.8.1",
            },
            host_ttl=10,
            other_ttl=10,
        )
        await self.azc.async_register_service(self.info)
        self.agent.log_event("mdns_registered", f"{self.agent.name} on {local_ip}")

        my_agent = self.agent
        my_azc = self.azc

        def on_service_state_change(zeroconf, service_type, name, state_change):
            from zeroconf import ServiceStateChange
            if state_change == ServiceStateChange.Added:
                asyncio.ensure_future(_resolve(service_type, name))
            elif state_change == ServiceStateChange.Removed:
                peer_part = name.replace(f".{MDNS_SERVICE_TYPE}", "")
                if peer_part in my_agent.peers:
                    my_agent.log_event("mdns_removed", peer_part)
                    del my_agent.peers[peer_part]

        async def _resolve(service_type, name):
            info = await my_azc.async_get_service_info(service_type, name)
            if info and info.properties:
                peer_name = info.properties.get(b"name", b"").decode()
                if peer_name and peer_name != my_agent.name:
                    addr = socket.inet_ntoa(info.addresses[0]) if info.addresses else "127.0.0.1"
                    peer_url = f"http://{addr}:{info.port}"
                    try:
                        async with ClientSession() as session:
                            async with session.get(f"{peer_url}/peer/heartbeat", timeout=3) as resp:
                                if resp.status == 200:
                                    my_agent._add_peer(peer_name, peer_url)
                                    my_agent.log_event("mdns_discovered", f"{peer_name} at {peer_url} (verified)")
                                else:
                                    my_agent.log_event("mdns_stale", f"{peer_name} at {peer_url} (heartbeat failed)")
                    except Exception:
                        my_agent.log_event("mdns_stale", f"{peer_name} at {peer_url} (unreachable)")

        self.browser = AsyncServiceBrowser(self.azc.zeroconf, MDNS_SERVICE_TYPE, handlers=[on_service_state_change])
        return True

    def _get_local_ip(self):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except Exception:
            return "127.0.0.1"

    async def stop(self):
        if self.azc:
            if self.info:
                await self.azc.async_unregister_service(self.info)
            await self.azc.async_close()


class CollectiveAgent:
    def __init__(self, name, port, llm_url, seeds, data_path, signing_key,
                 enable_udp=True, enable_mdns=True, relay_url=None,
                 role="worker", nebula_url=None):
        self.name = name
        self.port = port
        self.llm_url = llm_url
        self.seeds = seeds
        self.signing_key = signing_key
        self.enable_udp = enable_udp
        self.enable_mdns = enable_mdns
        self.relay_url = relay_url
        self.role = role
        self.nebula_url = nebula_url
        self.start_time = time.time()

        self.local_data = None
        if data_path and Path(data_path).exists():
            self.local_data = Path(data_path).read_text()

        self.peers = {}  # name -> {url, last_seen, misses}
        self.seen_programs = {}  # hash -> program text
        self.results = {}  # hash -> {score, decision, ...}
        self.event_log = []  # [{time, type, detail}]
        self.my_url = f"http://localhost:{port}"
        self.udp = None
        self.mdns = None
        self.relay = None
        self.ws_clients = set()

        # Sentients embed a local nebula instance with persistent storage
        self.nebula = None
        if role == "sentient":
            from nml_nebula import Nebula
            storage = str(PROJECT_ROOT / ".nebula")
            self.nebula = Nebula(storage_path=storage)
            self.nebula.sentients.add(name)
            self.nebula.event_callbacks.append(
                lambda t, d: self.log_event(f"nebula_{t}", d)
            )

        # Oracles embed a knowledge engine with LLM connector
        self.oracle = None
        if role == "oracle":
            from nml_oracle import OracleEngine
            self.oracle = OracleEngine(self, llm_url=llm_url)

        # Architects embed a program builder with NML LLM connector
        self.architect = None
        if role == "architect":
            from nml_architect import ArchitectEngine
            self.architect = ArchitectEngine(self, llm_url=llm_url)

    def log_event(self, event_type, detail=""):
        entry = {
            "time": round(time.time() - self.start_time, 1),
            "type": event_type,
            "detail": detail,
        }
        self.event_log.append(entry)
        if len(self.event_log) > 200:
            self.event_log = self.event_log[-200:]
        ts = f"{entry['time']:>7.1f}s"
        print(f"  [{self.name}] {ts}  {event_type}: {detail}")
        self._notify_ws(entry)

    def _notify_ws(self, event):
        if not self.ws_clients:
            return
        msg = json.dumps({"type": "event", "event": event, "state": self.get_state()})
        dead = set()
        for ws in self.ws_clients:
            try:
                asyncio.ensure_future(ws.send_str(msg))
            except Exception:
                dead.add(ws)
        self.ws_clients -= dead

    async def discover_from_seeds(self):
        async with ClientSession() as session:
            for seed_url in self.seeds:
                try:
                    async with session.get(f"{seed_url}/peers", timeout=3) as resp:
                        data = await resp.json()
                        for p in data.get("peers", []):
                            if p["url"] != self.my_url and p["name"] != self.name:
                                self._add_peer(p["name"], p["url"])
                    await self._announce_to(session, seed_url)
                    self.log_event("seed_connected", seed_url)
                except Exception:
                    self.log_event("seed_failed", seed_url)

    async def _announce_to(self, session, peer_url):
        try:
            await session.post(f"{peer_url}/peer/join", json={
                "name": self.name,
                "url": self.my_url,
                "role": self.role,
            }, timeout=3)
        except Exception:
            pass

    def _add_peer(self, name, url, role=None):
        if name == self.name or url == self.my_url:
            return
        is_new = name not in self.peers
        self.peers[name] = {
            "url": url,
            "last_seen": time.time(),
            "misses": 0,
            "role": role or self.peers.get(name, {}).get("role"),
        }
        if role == "oracle":
            self._oracle_peer_cache = url
        if is_new:
            self.log_event("peer_discovered", f"{name} at {url}")

    async def heartbeat_loop(self):
        while True:
            await asyncio.sleep(HEARTBEAT_INTERVAL)
            dead = []
            async with ClientSession() as session:
                for name, info in list(self.peers.items()):
                    try:
                        async with session.get(
                            f"{info['url']}/peer/heartbeat", timeout=3
                        ) as resp:
                            if resp.status == 200:
                                info["last_seen"] = time.time()
                                info["misses"] = 0
                            else:
                                info["misses"] += 1
                    except Exception:
                        info["misses"] += 1

                    if info["misses"] >= HEARTBEAT_MISS_LIMIT:
                        dead.append(name)

            for name in dead:
                self.log_event("peer_lost", name)
                del self.peers[name]

    async def broadcast_program(self, program: str, source_name: str = None):
        phash = program_hash(program)
        if phash in self.seen_programs:
            return

        self.seen_programs[phash] = program
        self.log_event("program_received", f"hash={phash} from={source_name or 'local'}")

        # Oracle and Architect observe programs but never execute them
        if self.role in ("oracle", "architect"):
            self.log_event(f"{self.role}_observed", f"hash={phash} (no execution)")
            self._forward_program(program, phash, source_name)
            return

        result = await execute_nml(program, self.local_data)
        self.results[phash] = {
            "agent": self.name,
            "hash": phash,
            "success": result["success"],
            "memory": result["memory"],
            "exit_code": result["exit_code"],
        }

        score = None
        for key in ["fraud_score", "risk_score", "score", "result"]:
            if key in result["memory"]:
                try:
                    val_str = result["memory"][key]
                    import re
                    nums = re.findall(r'[\d.]+', val_str)
                    if nums:
                        score = float(nums[-1])
                except (ValueError, IndexError):
                    pass
                break

        if score is not None:
            self.results[phash]["score"] = score
        self.log_event("program_executed", f"hash={phash} success={result['success']} score={score}")

        # Broadcast result via UDP
        if self.udp and score is not None:
            self.udp.broadcast_result(phash, score)

        self._forward_program(program, phash, source_name)

    def _forward_program(self, program, phash, source_name):
        """Forward a program to peers via UDP, relay, and HTTP fallback."""
        # Forward via relay (WAN)
        if self.relay and source_name != "relay":
            self.relay.send_program(program)

        # Forward program to peers via UDP (single multicast packet)
        udp_sent = False
        if self.udp and source_name != "__udp__":
            udp_sent = self.udp.broadcast_program(program)

        # HTTP fallback for peers not on the multicast group
        if not udp_sent:
            asyncio.ensure_future(self._http_forward(program, source_name))

    async def _http_forward(self, program, source_name):
        async with ClientSession() as session:
            for name, info in list(self.peers.items()):
                if name == source_name:
                    continue
                try:
                    await session.post(f"{info['url']}/broadcast", json={
                        "program": program,
                        "source": self.name,
                    }, timeout=5)
                except Exception:
                    pass

    async def generate_from_llm(self, prompt: str) -> str:
        if not self.llm_url:
            return None

        messages = [
            {"role": "system", "content": "You are an NML code generator. Write only valid NML programs."},
            {"role": "user", "content": prompt},
        ]

        async with ClientSession() as session:
            try:
                async with session.post(
                    f"{self.llm_url}/v1/chat/completions",
                    json={"messages": messages, "max_tokens": 512, "mode": "nml"},
                    timeout=60,
                ) as resp:
                    data = await resp.json()
                    return data["choices"][0]["message"]["content"]
            except Exception as e:
                self.log_event("llm_error", str(e))
                return None

    async def collect_consensus(self, phash: str, strategy: str = "median") -> dict:
        """Two-phase VOTE: collect raw scores, then apply weights + oracle assessment."""

        # Phase 1: Collect raw scores from all executing agents
        all_scores = []

        if phash in self.results and "score" in self.results[phash]:
            all_scores.append({
                "agent": self.name,
                "score": self.results[phash]["score"],
            })

        async with ClientSession() as session:
            for name, info in list(self.peers.items()):
                try:
                    async with session.get(
                        f"{info['url']}/results", params={"program": phash}, timeout=5
                    ) as resp:
                        data = await resp.json()
                        for r in data.get("results", []):
                            if "score" in r:
                                all_scores.append({"agent": r.get("agent", name), "score": r["score"]})
                except Exception:
                    pass

        if not all_scores:
            return {"error": "No scores available", "strategy": strategy}

        scores = [s["score"] for s in all_scores]
        raw_sorted = sorted(scores)
        mid = len(raw_sorted) // 2
        if strategy == "median":
            raw_consensus = raw_sorted[mid] if len(raw_sorted) % 2 == 1 else (raw_sorted[mid-1] + raw_sorted[mid]) / 2
        elif strategy == "mean":
            raw_consensus = sum(scores) / len(scores)
        elif strategy == "min":
            raw_consensus = min(scores)
        elif strategy == "max":
            raw_consensus = max(scores)
        else:
            raw_consensus = sum(scores) / len(scores)

        result = {
            "strategy": strategy,
            "agents": all_scores,
            "raw_consensus": raw_consensus,
            "consensus": raw_consensus,
            "weighted": False,
            "count": len(all_scores),
        }

        # Phase 2: Oracle assessment + sentient weights (if oracle is in the mesh)
        oracle = self._find_oracle()
        if oracle:
            assessment = oracle.assess_consensus(all_scores)
            result["assessment"] = assessment

            weights = oracle.compute_sentient_weights(all_scores)
            if weights:
                weighted_scores = []
                weighted_agents = []
                for s in all_scores:
                    w = weights.get(s["agent"], 1.0)
                    weighted_agents.append({**s, "weight": round(w, 3)})
                    weighted_scores.extend([s["score"]] * max(1, round(w * 10)))

                if weighted_scores:
                    ws = sorted(weighted_scores)
                    wmid = len(ws) // 2
                    if strategy == "median":
                        weighted_consensus = ws[wmid] if len(ws) % 2 == 1 else (ws[wmid-1] + ws[wmid]) / 2
                    elif strategy == "mean":
                        weighted_consensus = sum(ws) / len(ws)
                    elif strategy == "min":
                        weighted_consensus = min(ws)
                    elif strategy == "max":
                        weighted_consensus = max(ws)
                    else:
                        weighted_consensus = sum(ws) / len(ws)

                    result["consensus"] = weighted_consensus
                    result["weighted"] = True
                    result["agents"] = weighted_agents
                    result["weights"] = weights

        return result

    def _find_oracle(self):
        """Find the local oracle engine (if this agent is oracle) or None."""
        if self.oracle:
            return self.oracle
        return None

    def get_state(self):
        last_result = None
        if self.results:
            last_key = list(self.results.keys())[-1]
            last_result = self.results[last_key]

        return {
            "name": self.name,
            "port": self.port,
            "url": self.my_url,
            "peers": [
                {"name": n, "url": i["url"]} for n, i in self.peers.items()
            ],
            "peer_count": len(self.peers),
            "programs_received": len(self.seen_programs),
            "results_count": len(self.results),
            "last_result": last_result,
            "uptime_seconds": round(time.time() - self.start_time, 1),
            "llm_url": self.llm_url,
            "has_local_data": self.local_data is not None,
            "udp_enabled": self.udp is not None,
            "udp_multicast": f"{UDP_MULTICAST_GROUP}:{UDP_MULTICAST_PORT}" if self.udp else None,
            "mdns_enabled": self.mdns is not None and self.mdns.azc is not None,
            "mdns_service": MDNS_SERVICE_TYPE if self.mdns else None,
            "role": self.role,
            "nebula_stats": self.nebula.stats() if self.nebula else None,
            "architect_stats": {
                "programs_built": self.architect.build_count,
                "catalog_size": len(self.architect.catalog),
                "llm_connected": self.architect.llm_url is not None,
            } if self.architect else None,
            "oracle_stats": {
                "collective_size": len(self.oracle.collective_state),
                "events_tracked": len(self.oracle.event_timeline),
                "programs_known": len(self.oracle.programs_catalog),
                "llm_connected": self.oracle.llm_url is not None,
                "poll_count": self.oracle._poll_count,
            } if self.oracle else None,
            "relay_url": self.relay_url,
            "relay_connected": self.relay is not None and self.relay.ws is not None and not self.relay.ws.closed if self.relay else False,
            "recent_events": self.event_log[-20:],
        }


def create_collective_app(agent: CollectiveAgent):
    def _cors():
        return {
            "Access-Control-Allow-Origin": "*",
            "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
            "Access-Control-Allow-Headers": "Content-Type",
        }

    def _json(data, status=200):
        return web.Response(
            text=json.dumps(data), content_type="application/json",
            status=status, headers=_cors(),
        )

    async def handle_options(request):
        return web.Response(status=204, headers=_cors())

    async def handle_health(request):
        return _json({
            "status": "healthy",
            "agent": agent.name,
            "peers": len(agent.peers),
            "programs": len(agent.seen_programs),
        })

    async def handle_peers(request):
        peers_list = [{"name": n, "url": i["url"]} for n, i in agent.peers.items()]
        peers_list.append({"name": agent.name, "url": agent.my_url})
        return _json({"peers": peers_list})

    async def handle_peer_join(request):
        body = await request.json()
        name = body.get("name")
        url = body.get("url")
        if not name or not url:
            return _json({"error": "name and url required"}, 400)
        agent._add_peer(name, url, role=body.get("role"))
        return _json({"status": "welcomed", "collective_size": len(agent.peers) + 1})

    async def handle_heartbeat(request):
        return _json({"status": "alive", "name": agent.name})

    async def handle_broadcast(request):
        body = await request.json()
        program = body.get("program", "")
        source = body.get("source", "unknown")
        if not program:
            return _json({"error": "No program"}, 400)

        asyncio.create_task(agent.broadcast_program(program, source_name=source))
        return _json({"status": "accepted", "hash": program_hash(program)})

    async def handle_submit(request):
        """Submit a program directly to this agent (entry point for external callers)."""
        body = await request.json()
        program = body.get("program", "")
        if not program:
            return _json({"error": "No program"}, 400)

        await agent.broadcast_program(program, source_name="external")
        phash = program_hash(program)
        return _json({"status": "submitted", "hash": phash})

    async def handle_generate(request):
        body = await request.json()
        prompt = body.get("prompt", "")
        if not prompt:
            return _json({"error": "No prompt"}, 400)

        agent.log_event("llm_request", prompt[:80])
        code = await agent.generate_from_llm(prompt)
        if not code:
            return _json({"error": "LLM generation failed"}, 500)

        agent.log_event("llm_response", f"{len(code)} chars")

        if agent.signing_key:
            signed = subprocess.run(
                [str(NML_BINARY), "--sign", "/dev/stdin", "--key", agent.signing_key, "--agent", agent.name],
                input=code, capture_output=True, text=True,
            )
            if signed.returncode == 0:
                code = signed.stdout

        await agent.broadcast_program(code, source_name=agent.name)
        return _json({"status": "generated_and_broadcast", "hash": program_hash(code), "program": code})

    async def handle_results(request):
        phash = request.query.get("program")
        if phash and phash in agent.results:
            return _json({"results": [agent.results[phash]]})
        elif phash:
            return _json({"results": []})
        return _json({"results": list(agent.results.values())})

    async def handle_consensus(request):
        body = await request.json()
        phash = body.get("program_hash", "")
        strategy = body.get("strategy", "median")
        if not phash:
            if agent.results:
                phash = list(agent.results.keys())[-1]
            elif agent.seen_programs:
                phash = list(agent.seen_programs.keys())[-1]
            else:
                return _json({"error": "No programs seen yet"}, 400)

        result = await agent.collect_consensus(phash, strategy)

        # If this agent doesn't have an oracle, try fetching assessment from oracle peer
        if "assessment" not in result and not agent.oracle:
            oracle_url = _find_oracle_peer()
            if oracle_url:
                try:
                    async with ClientSession() as session:
                        async with session.post(
                            f"{oracle_url}/assess",
                            json={"scores": result.get("agents", [])},
                            timeout=5,
                        ) as resp:
                            oracle_data = await resp.json()
                            if "assessment" in oracle_data:
                                result["assessment"] = oracle_data["assessment"]
                                if oracle_data.get("weights"):
                                    result["weights"] = oracle_data["weights"]
                except Exception:
                    pass

        return _json(result)

    def _find_oracle_peer():
        """Find an oracle peer's URL from cached peer roles."""
        return getattr(agent, '_oracle_peer_cache', None)

    async def handle_state(request):
        return _json(agent.get_state())

    async def handle_ws(request):
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        agent.ws_clients.add(ws)
        try:
            await ws.send_json({"type": "snapshot", "state": agent.get_state()})
            async for msg in ws:
                if msg.type == web.WSMsgType.TEXT:
                    pass
                elif msg.type in (web.WSMsgType.ERROR, web.WSMsgType.CLOSE):
                    break
        finally:
            agent.ws_clients.discard(ws)
        return ws

    # ═══════════════════════════════════════════
    # Nebula endpoints (role-aware)
    # ═══════════════════════════════════════════

    async def handle_data_submit(request):
        """Submit data to the nebula's quarantine. Accepts optional context metadata."""
        body = await request.json()
        name = body.get("name", "")
        content = body.get("content", "")
        if not name or not content:
            return _json({"error": "name and content required"}, 400)

        context = body.get("context")
        submitter = body.get("author", agent.name)

        nebula = _get_nebula()
        if not nebula:
            return _json({"error": "No nebula available. Connect to a sentient or --nebula URL"}, 503)

        entry = nebula.submit_data(name, content, author=submitter, context=context)

        result = {"hash": entry.hash, "status": entry.status,
                  "auto_checks": entry.meta.get("auto_checks")}

        # Oracle auto-analyzes and votes on new quarantine entries
        if agent.oracle and entry.status == "pending":
            vote_result = agent.oracle.vote_on_data(
                nebula, entry.hash, name, content,
                context=context, author=submitter,
            )
            result["oracle_vote"] = vote_result
            # Re-read status in case oracle vote triggered promotion or rejection
            result["status"] = entry.status

        return _json(result)

    async def handle_data_approve(request):
        """Sentient or oracle approves quarantined data."""
        if agent.role not in ("sentient", "oracle"):
            return _json({"error": "Only sentients and oracles can approve data"}, 403)
        body = await request.json()
        nebula = _get_nebula()
        if not nebula:
            return _json({"error": "No nebula"}, 503)
        entry = nebula.approve(body.get("hash", ""), agent.name,
                               body.get("reason", ""), role=agent.role)
        if entry:
            return _json({"hash": entry.hash, "status": entry.status, "votes": entry.votes})
        return _json({"error": "Not found or not pending"}, 404)

    async def handle_data_reject(request):
        """Sentient or oracle rejects quarantined data."""
        if agent.role not in ("sentient", "oracle"):
            return _json({"error": "Only sentients and oracles can reject data"}, 403)
        body = await request.json()
        nebula = _get_nebula()
        if not nebula:
            return _json({"error": "No nebula"}, 503)
        entry = nebula.reject(body.get("hash", ""), agent.name,
                              body.get("reason", ""), role=agent.role)
        if entry:
            return _json({"hash": entry.hash, "status": entry.status, "reason": entry.reason})
        return _json({"error": "Not found or not pending"}, 404)

    async def handle_quarantine(request):
        nebula = _get_nebula()
        if not nebula:
            return _json({"error": "No nebula"}, 503)
        return _json({"pending": nebula.list_quarantine()})

    async def handle_data_pool(request):
        nebula = _get_nebula()
        if not nebula:
            return _json({"error": "No nebula"}, 503)
        return _json({"pool": nebula.list_data_pool()})

    async def handle_nebula_stats(request):
        nebula = _get_nebula()
        if not nebula:
            return _json({"error": "No nebula"}, 503)
        return _json(nebula.stats())

    def _get_nebula():
        """Get the nebula instance — local for sentients, remote proxy for workers."""
        if agent.nebula:
            return agent.nebula
        return None

    # ═══════════════════════════════════════════
    # Oracle endpoints (role-aware)
    # ═══════════════════════════════════════════

    async def handle_ask(request):
        """Ask the Oracle a question about the collective."""
        if not agent.oracle:
            return _json({"error": "This agent is not an Oracle. Connect to an oracle agent."}, 403)
        body = await request.json()
        question = body.get("question", "")
        if not question:
            return _json({"error": "question required"}, 400)
        result = await agent.oracle.ask(question)
        return _json(result)

    async def handle_context(request):
        """Get the Oracle's full collective awareness."""
        if not agent.oracle:
            return _json({"error": "This agent is not an Oracle"}, 403)
        return _json(agent.oracle.get_collective_context())

    async def handle_explain(request):
        """Ask the Oracle to explain a program, data batch, or consensus result."""
        if not agent.oracle:
            return _json({"error": "This agent is not an Oracle"}, 403)
        hash_hex = request.query.get("hash", "")
        if not hash_hex:
            return _json({"error": "hash query parameter required"}, 400)
        result = await agent.oracle.explain(hash_hex)
        return _json(result)

    async def handle_recommend(request):
        """Ask the Oracle for recommendations about the collective."""
        if not agent.oracle:
            return _json({"error": "This agent is not an Oracle"}, 403)
        result = await agent.oracle.recommend()
        return _json(result)

    async def handle_spec(request):
        """Oracle generates a program specification for the Architect."""
        if not agent.oracle:
            return _json({"error": "This agent is not an Oracle"}, 403)
        body = await request.json() if request.content_length else {}
        intent = body.get("intent")
        spec = agent.oracle.generate_program_spec(intent=intent)
        return _json(spec)

    # ═══════════════════════════════════════════
    # Architect endpoints (role-aware)
    # ═══════════════════════════════════════════

    async def handle_build(request):
        """Architect builds an NML program from a spec."""
        if not agent.architect:
            return _json({"error": "This agent is not an Architect"}, 403)
        body = await request.json()
        if not body.get("intent") and not body.get("spec"):
            return _json({"error": "intent or spec required"}, 400)
        spec = body.get("spec", body)
        result = await agent.architect.build(spec)
        return _json(result)

    async def handle_validate(request):
        """Architect validates an NML program."""
        if not agent.architect:
            return _json({"error": "This agent is not an Architect"}, 403)
        body = await request.json()
        program = body.get("program", "")
        if not program:
            return _json({"error": "program required"}, 400)
        result = await agent.architect.validate_program(program)
        return _json(result)

    async def handle_catalog(request):
        """List all programs the architect has built."""
        if not agent.architect:
            return _json({"error": "This agent is not an Architect"}, 403)
        return _json({"catalog": agent.architect.get_catalog()})

    async def handle_assess(request):
        """Oracle assesses a set of consensus scores (called by other agents)."""
        if not agent.oracle:
            return _json({"error": "This agent is not an Oracle"}, 403)
        body = await request.json()
        scores = body.get("scores", [])
        if not scores:
            return _json({"error": "scores array required"}, 400)
        assessment = agent.oracle.assess_consensus(scores)
        weights = agent.oracle.compute_sentient_weights(scores)
        return _json({"assessment": assessment, "weights": weights})

    async def handle_discover(request):
        """Discovery endpoint for the dashboard — returns all known agent URLs."""
        all_agents = [{"name": agent.name, "url": agent.my_url}]
        for n, i in agent.peers.items():
            all_agents.append({"name": n, "url": i["url"]})
        return _json({"agents": all_agents})

    async def handle_dashboard(request):
        """Serve the dashboard HTML directly from this agent."""
        if DASHBOARD_PATH.exists():
            html = DASHBOARD_PATH.read_text()
            inject = f"""<script>
            window.NML_SEED = "{agent.my_url}";
            window.addEventListener('load', function() {{
                document.getElementById('seedInput').value = "{agent.my_url}";
                connectToAgent();
            }});</script></body>"""
            html = html.replace("</body>", inject)
            return web.Response(text=html, content_type="text/html", headers=_cors())
        return web.Response(text="Dashboard not found", status=404)

    async def on_startup(app):
        await agent.discover_from_seeds()
        asyncio.create_task(agent.heartbeat_loop())

        if agent.enable_udp:
            try:
                agent.udp = UDPTransport(agent)
                await agent.udp.start()
                agent.log_event("udp_started", f"multicast {UDP_MULTICAST_GROUP}:{UDP_MULTICAST_PORT}")
            except Exception as e:
                agent.log_event("udp_failed", str(e))
                agent.udp = None

        if agent.enable_mdns:
            agent.mdns = MDNSDiscovery(agent)
            await agent.mdns.start()

        if agent.relay_url:
            agent.relay = RelayTransport(agent, agent.relay_url)
            await agent.relay.start()

        if agent.oracle:
            await agent.oracle.start()

        if agent.architect:
            await agent.architect.start()

    app = web.Application()
    app.on_startup.append(on_startup)

    app.router.add_get("/health", handle_health)
    app.router.add_get("/peers", handle_peers)
    app.router.add_post("/peer/join", handle_peer_join)
    app.router.add_get("/peer/heartbeat", handle_heartbeat)
    app.router.add_post("/broadcast", handle_broadcast)
    app.router.add_post("/submit", handle_submit)
    app.router.add_post("/generate", handle_generate)
    app.router.add_get("/results", handle_results)
    app.router.add_post("/consensus", handle_consensus)
    app.router.add_get("/state", handle_state)
    app.router.add_get("/ws", handle_ws)
    app.router.add_get("/discover", handle_discover)
    app.router.add_get("/dashboard", handle_dashboard)
    app.router.add_post("/data/submit", handle_data_submit)
    app.router.add_post("/data/approve", handle_data_approve)
    app.router.add_post("/data/reject", handle_data_reject)
    app.router.add_get("/data/quarantine", handle_quarantine)
    app.router.add_get("/data/pool", handle_data_pool)
    app.router.add_get("/nebula/stats", handle_nebula_stats)
    app.router.add_post("/ask", handle_ask)
    app.router.add_get("/context", handle_context)
    app.router.add_get("/explain", handle_explain)
    app.router.add_get("/recommend", handle_recommend)
    app.router.add_post("/spec", handle_spec)
    app.router.add_post("/build", handle_build)
    app.router.add_post("/validate", handle_validate)
    app.router.add_get("/catalog", handle_catalog)
    app.router.add_post("/assess", handle_assess)
    app.router.add_get("/favicon.ico", lambda r: web.Response(status=204))
    app.router.add_route("OPTIONS", "/{path:.*}", handle_options)

    return app


def main():
    parser = argparse.ArgumentParser(description="NML Collective Agent — autonomous gossip peer")
    parser.add_argument("--name", required=True, help="Agent name")
    parser.add_argument("--port", type=int, default=9001, help="Agent port")
    parser.add_argument("--llm", default=None, help="Central LLM URL (e.g. http://localhost:8082)")
    parser.add_argument("--seeds", default="", help="Comma-separated seed peer URLs")
    parser.add_argument("--data", default=None, help="Path to local .nml.data file")
    parser.add_argument("--key", default=None, help="HMAC signing key (hex)")
    parser.add_argument("--no-udp", action="store_true", help="Disable UDP multicast")
    parser.add_argument("--no-mdns", action="store_true", help="Disable mDNS/Bonjour discovery")
    parser.add_argument("--relay", default=None, help="WebSocket relay URL (e.g. ws://relay:7777/ws)")
    parser.add_argument("--role", choices=["sentient", "worker", "oracle", "architect"], default="worker", help="Agent role")
    parser.add_argument("--nebula", default=None, help="Remote nebula URL (workers connect to sentient's nebula)")
    args = parser.parse_args()

    seeds = [s.strip() for s in args.seeds.split(",") if s.strip()]

    agent = CollectiveAgent(
        name=args.name,
        port=args.port,
        llm_url=args.llm,
        seeds=seeds,
        data_path=args.data,
        signing_key=args.key,
        enable_udp=not args.no_udp,
        enable_mdns=not args.no_mdns,
        relay_url=args.relay,
        role=args.role,
        nebula_url=args.nebula,
    )

    role_label = {"sentient": "SENTIENT", "worker": "WORKER", "oracle": "ORACLE", "architect": "ARCHITECT"}[args.role]
    print(f"NML Collective {role_label} '{args.name}' on :{args.port}")
    print(f"  Discovery: ", end="")
    modes = []
    if not args.no_udp:
        modes.append(f"UDP multicast {UDP_MULTICAST_GROUP}:{UDP_MULTICAST_PORT}")
    if not args.no_mdns:
        modes.append(f"mDNS ({MDNS_SERVICE_TYPE})")
    if args.relay:
        modes.append(f"relay ({args.relay})")
    if seeds:
        modes.append(f"seeds ({', '.join(seeds)})")
    print(", ".join(modes) if modes else "none (use --seeds)")
    if args.llm:
        print(f"  LLM: {args.llm}")
    if args.data:
        print(f"  Data: {args.data}")
    print(f"  Dashboard: http://localhost:{args.port}/dashboard")
    print()

    def cleanup():
        if agent.mdns:
            try:
                loop = asyncio.get_event_loop()
                if loop.is_running():
                    asyncio.ensure_future(agent.mdns.stop())
                else:
                    loop.run_until_complete(agent.mdns.stop())
            except Exception:
                pass
        if agent.udp:
            agent.udp.stop()
        if agent.relay:
            agent.relay.stop()
        if agent.oracle:
            agent.oracle.stop()
        if agent.architect:
            agent.architect.stop()

    atexit.register(cleanup)
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda s, f: (cleanup(), exit(0)))

    app = create_collective_app(agent)
    web.run_app(app, port=args.port, print=None)


if __name__ == "__main__":
    main()
