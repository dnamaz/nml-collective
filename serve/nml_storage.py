#!/usr/bin/env python3
"""
NML Nebula Storage — three-layer persistent storage.

Layer 1 (Truth):  Content-addressed tensor objects + per-agent transaction chains
Layer 2 (Speed):  SQLite indexes (derived, rebuildable from Layer 1)
Layer 3 (Intel):  Vector embeddings for semantic search (optional)

All data flows down: Layer 1 is the source of truth. Layers 2 and 3
are derived views. If lost, rebuild from Layer 1.

Usage:
    from nml_storage import NebulaDisk, TransactionLog, NebulaIndex, NebulaVectors
"""

import hashlib
import json
import math
import os
import sqlite3
import struct
import time
from pathlib import Path


# ═══════════════════════════════════════════
# Layer 1a: Content-Addressed Tensor Object Store
# ═══════════════════════════════════════════

OBJECT_MAGIC = b"NML\x02"

OBJ_PROGRAM = 1
OBJ_DATA = 2
OBJ_MANIFEST = 3

DTYPE_F32 = 1
DTYPE_F64 = 2
DTYPE_I32 = 3
DTYPE_TEXT = 4


class NebulaDisk:
    """Content-addressed binary object store on the filesystem."""

    def __init__(self, base_path):
        self.base_path = Path(base_path)
        self.objects_dir = self.base_path / "objects"
        self.objects_dir.mkdir(parents=True, exist_ok=True)

    def _obj_path(self, hash_hex):
        prefix = hash_hex[:2]
        (self.objects_dir / prefix).mkdir(exist_ok=True)
        return self.objects_dir / prefix / f"{hash_hex}.obj"

    def store(self, content, obj_type=OBJ_DATA, author="system",
              shape=None, dtype=DTYPE_TEXT, name="", context=None):
        """Store content as a binary object with optional context sidecar. Returns hash."""
        content_bytes = content.encode("utf-8") if isinstance(content, str) else content
        hash_hex = hashlib.sha256(content_bytes).hexdigest()[:16]

        path = self._obj_path(hash_hex)
        if path.exists():
            return hash_hex

        author_bytes = author.encode("utf-8")[:63]
        shape = shape or []
        ts = struct.pack("!d", time.time())

        header = bytearray()
        header += OBJECT_MAGIC
        header += bytes.fromhex(hash_hex)
        header += struct.pack("B", obj_type)
        header += struct.pack("B", len(author_bytes)) + author_bytes
        header += ts
        header += struct.pack("B", len(shape))
        for dim in shape:
            header += struct.pack("!i", dim)
        header += struct.pack("B", dtype)
        header += struct.pack("!I", len(content_bytes))

        with open(path, "wb") as f:
            f.write(bytes(header))
            f.write(content_bytes)

        if context:
            ctx_path = path.with_suffix(".ctx.json")
            with open(ctx_path, "w") as f:
                json.dump(context, f)

        return hash_hex

    def load_context(self, hash_hex):
        """Load the context sidecar for an object, or None."""
        ctx_path = self._obj_path(hash_hex).with_suffix(".ctx.json")
        if ctx_path.exists():
            with open(ctx_path) as f:
                return json.load(f)
        return None

    def load(self, hash_hex):
        """Load an object by hash. Returns (header_dict, content_bytes) or None."""
        path = self._obj_path(hash_hex)
        if not path.exists():
            return None

        with open(path, "rb") as f:
            magic = f.read(4)
            if magic != OBJECT_MAGIC:
                return None

            stored_hash = f.read(8).hex()
            obj_type = struct.unpack("B", f.read(1))[0]
            author_len = struct.unpack("B", f.read(1))[0]
            author = f.read(author_len).decode("utf-8")
            timestamp = struct.unpack("!d", f.read(8))[0]
            ndim = struct.unpack("B", f.read(1))[0]
            shape = [struct.unpack("!i", f.read(4))[0] for _ in range(ndim)]
            dtype = struct.unpack("B", f.read(1))[0]
            content_len = struct.unpack("!I", f.read(4))[0]
            content = f.read(content_len)

        header = {
            "hash": hash_hex,
            "type": obj_type,
            "author": author,
            "timestamp": timestamp,
            "shape": shape,
            "dtype": dtype,
        }
        return header, content

    def exists(self, hash_hex):
        return self._obj_path(hash_hex).exists()

    def list_all(self):
        """Iterate all stored object hashes."""
        hashes = []
        for prefix_dir in sorted(self.objects_dir.iterdir()):
            if prefix_dir.is_dir():
                for obj_file in sorted(prefix_dir.glob("*.obj")):
                    hashes.append(obj_file.stem)
        return hashes

    def stats(self):
        hashes = self.list_all()
        total_size = sum(
            self._obj_path(h).stat().st_size for h in hashes
        )
        return {
            "object_count": len(hashes),
            "total_bytes": total_size,
            "storage_path": str(self.objects_dir),
        }


# ═══════════════════════════════════════════
# Layer 1b: Per-Agent Transaction Chain
# ═══════════════════════════════════════════

TX_AGENT_JOIN = 0x01
TX_AGENT_LEAVE = 0x02
TX_PROGRAM_PUBLISH = 0x10
TX_PROGRAM_BROADCAST = 0x11
TX_DATA_SUBMIT = 0x20
TX_DATA_APPROVE = 0x21
TX_DATA_REJECT = 0x22
TX_DATA_TRANSFER = 0x23
TX_EXECUTION = 0x30
TX_CONSENSUS = 0x31
TX_MANIFEST_CREATE = 0x40

TX_TYPE_NAMES = {
    TX_AGENT_JOIN: "AGENT_JOIN", TX_AGENT_LEAVE: "AGENT_LEAVE",
    TX_PROGRAM_PUBLISH: "PROGRAM_PUBLISH", TX_PROGRAM_BROADCAST: "PROGRAM_BROADCAST",
    TX_DATA_SUBMIT: "DATA_SUBMIT", TX_DATA_APPROVE: "DATA_APPROVE",
    TX_DATA_REJECT: "DATA_REJECT", TX_DATA_TRANSFER: "DATA_TRANSFER",
    TX_EXECUTION: "EXECUTION", TX_CONSENSUS: "CONSENSUS",
    TX_MANIFEST_CREATE: "MANIFEST_CREATE",
}

ZERO_HASH = b"\x00" * 32


class TransactionLog:
    """Per-agent append-only transaction chain with hash integrity."""

    def __init__(self, base_path, agent_name):
        self.base_path = Path(base_path)
        self.agent_name = agent_name
        self.chain_dir = self.base_path / "agents" / agent_name
        self.chain_dir.mkdir(parents=True, exist_ok=True)
        self.chain_file = self.chain_dir / "chain.binlog"
        self.tx_count = 0
        self.prev_hash = ZERO_HASH
        self.transactions = []
        self._load_existing()

    def _load_existing(self):
        if not self.chain_file.exists():
            return
        with open(self.chain_file, "rb") as f:
            while True:
                header = f.read(4)
                if len(header) < 4:
                    break
                tx_id = struct.unpack("!I", header)[0]
                tx_hash = f.read(32)
                prev_hash = f.read(32)
                timestamp = struct.unpack("!d", f.read(8))[0]
                agent_len = struct.unpack("B", f.read(1))[0]
                agent = f.read(agent_len).decode("utf-8")
                tx_type = struct.unpack("B", f.read(1))[0]
                refs_count = struct.unpack("B", f.read(1))[0]
                refs = [f.read(16).hex() for _ in range(refs_count)]
                content_len = struct.unpack("!I", f.read(4))[0]
                content = f.read(content_len).decode("utf-8")

                self.transactions.append({
                    "tx_id": tx_id,
                    "hash": tx_hash.hex(),
                    "prev_hash": prev_hash.hex(),
                    "timestamp": timestamp,
                    "agent": agent,
                    "type": TX_TYPE_NAMES.get(tx_type, f"0x{tx_type:02x}"),
                    "type_code": tx_type,
                    "refs": refs,
                    "content": content,
                })
                self.tx_count = tx_id + 1
                self.prev_hash = tx_hash

    def append(self, tx_type, content, refs=None):
        """Append a transaction to the chain. Returns the transaction dict."""
        refs = refs or []
        timestamp = time.time()
        agent_bytes = self.agent_name.encode("utf-8")[:63]
        content_str = json.dumps(content) if isinstance(content, dict) else str(content)
        content_bytes = content_str.encode("utf-8")

        hash_input = (self.prev_hash +
                      struct.pack("B", tx_type) +
                      content_bytes +
                      struct.pack("!d", timestamp))
        tx_hash = hashlib.sha256(hash_input).digest()

        ref_bytes = b""
        for r in refs[:255]:
            ref_hex = r if isinstance(r, str) else r.hex()
            ref_bytes += bytes.fromhex(ref_hex.ljust(32, '0')[:32])

        record = bytearray()
        record += struct.pack("!I", self.tx_count)
        record += tx_hash
        record += self.prev_hash
        record += struct.pack("!d", timestamp)
        record += struct.pack("B", len(agent_bytes)) + agent_bytes
        record += struct.pack("B", tx_type)
        record += struct.pack("B", len(refs))
        record += ref_bytes
        record += struct.pack("!I", len(content_bytes)) + content_bytes

        with open(self.chain_file, "ab") as f:
            f.write(bytes(record))

        tx = {
            "tx_id": self.tx_count,
            "hash": tx_hash.hex(),
            "prev_hash": self.prev_hash.hex(),
            "timestamp": timestamp,
            "agent": self.agent_name,
            "type": TX_TYPE_NAMES.get(tx_type, f"0x{tx_type:02x}"),
            "type_code": tx_type,
            "refs": refs,
            "content": content_str,
        }

        self.transactions.append(tx)
        self.prev_hash = tx_hash
        self.tx_count += 1
        return tx

    def verify(self):
        """Verify the chain's hash integrity. Returns (valid, error_msg)."""
        prev = ZERO_HASH
        for tx in self.transactions:
            content_bytes = tx["content"].encode("utf-8")
            expected_input = (prev +
                              struct.pack("B", tx["type_code"]) +
                              content_bytes +
                              struct.pack("!d", tx["timestamp"]))
            expected_hash = hashlib.sha256(expected_input).digest()
            if expected_hash.hex() != tx["hash"]:
                return False, f"Chain broken at tx#{tx['tx_id']}: expected {expected_hash.hex()[:16]}, got {tx['hash'][:16]}"
            prev = expected_hash
        return True, "Chain integrity verified"

    def get_chain(self):
        return list(self.transactions)

    def stats(self):
        return {
            "agent": self.agent_name,
            "tx_count": self.tx_count,
            "chain_file": str(self.chain_file),
            "file_size": self.chain_file.stat().st_size if self.chain_file.exists() else 0,
        }


# ═══════════════════════════════════════════
# Layer 2: SQLite Indexes
# ═══════════════════════════════════════════

SCHEMA_SQL = """
CREATE TABLE IF NOT EXISTS objects (
    hash TEXT PRIMARY KEY,
    type TEXT,
    name TEXT,
    author TEXT,
    timestamp REAL,
    status TEXT,
    content_size INTEGER,
    shape TEXT,
    domain TEXT,
    description TEXT,
    tags TEXT,
    features TEXT
);

CREATE TABLE IF NOT EXISTS transactions (
    tx_id INTEGER,
    agent TEXT,
    hash TEXT,
    prev_hash TEXT,
    timestamp REAL,
    type TEXT,
    refs TEXT,
    content TEXT,
    PRIMARY KEY (agent, tx_id)
);

CREATE TABLE IF NOT EXISTS executions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    program_hash TEXT,
    data_context TEXT,
    agent TEXT,
    score REAL,
    success INTEGER,
    timestamp REAL
);

CREATE TABLE IF NOT EXISTS consensus (
    program_hash TEXT,
    strategy TEXT,
    result REAL,
    agent_count INTEGER,
    timestamp REAL,
    PRIMARY KEY (program_hash, timestamp)
);

CREATE INDEX IF NOT EXISTS idx_objects_status ON objects(status);
CREATE INDEX IF NOT EXISTS idx_objects_name ON objects(name);
CREATE INDEX IF NOT EXISTS idx_objects_author ON objects(author);
CREATE INDEX IF NOT EXISTS idx_objects_domain ON objects(domain);
CREATE INDEX IF NOT EXISTS idx_objects_tags ON objects(tags);
CREATE INDEX IF NOT EXISTS idx_tx_agent ON transactions(agent);
CREATE INDEX IF NOT EXISTS idx_tx_type ON transactions(type);
CREATE INDEX IF NOT EXISTS idx_exec_program ON executions(program_hash);
"""


class NebulaIndex:
    """SQLite index layer over the nebula's content store."""

    def __init__(self, base_path):
        self.base_path = Path(base_path)
        self.db_path = self.base_path / "index.db"
        self.base_path.mkdir(parents=True, exist_ok=True)
        self.conn = sqlite3.connect(str(self.db_path))
        self.conn.row_factory = sqlite3.Row
        self.conn.executescript(SCHEMA_SQL)
        self.conn.commit()

    def index_object(self, hash_hex, obj_type, name, author, timestamp,
                     status, content_size, shape=None, context=None):
        shape_str = ",".join(str(d) for d in shape) if shape else ""
        ctx = context or {}
        domain = ctx.get("domain", "")
        description = ctx.get("description", "")
        tags = ",".join(ctx.get("tags", []))
        features = ",".join(ctx.get("features", []))
        self.conn.execute(
            "INSERT OR REPLACE INTO objects VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
            (hash_hex, obj_type, name, author, timestamp, status, content_size,
             shape_str, domain, description, tags, features)
        )
        self.conn.commit()

    def update_status(self, hash_hex, status):
        self.conn.execute("UPDATE objects SET status=? WHERE hash=?", (status, hash_hex))
        self.conn.commit()

    def index_transaction(self, tx):
        self.conn.execute(
            "INSERT OR REPLACE INTO transactions VALUES (?,?,?,?,?,?,?,?)",
            (tx["tx_id"], tx["agent"], tx["hash"], tx["prev_hash"],
             tx["timestamp"], tx["type"], json.dumps(tx["refs"]), tx["content"])
        )
        self.conn.commit()

    def index_execution(self, program_hash, data_context, agent, score, success):
        self.conn.execute(
            "INSERT INTO executions (program_hash, data_context, agent, score, success, timestamp) VALUES (?,?,?,?,?,?)",
            (program_hash, data_context, agent, score, 1 if success else 0, time.time())
        )
        self.conn.commit()

    def index_consensus(self, program_hash, strategy, result, agent_count):
        self.conn.execute(
            "INSERT OR REPLACE INTO consensus VALUES (?,?,?,?,?)",
            (program_hash, strategy, result, agent_count, time.time())
        )
        self.conn.commit()

    def query_objects(self, status=None, name=None, author=None, obj_type=None, limit=100):
        sql = "SELECT * FROM objects WHERE 1=1"
        params = []
        if status:
            sql += " AND status=?"; params.append(status)
        if name:
            sql += " AND name=?"; params.append(name)
        if author:
            sql += " AND author=?"; params.append(author)
        if obj_type:
            sql += " AND type=?"; params.append(obj_type)
        sql += " ORDER BY timestamp DESC LIMIT ?"
        params.append(limit)
        return [dict(row) for row in self.conn.execute(sql, params)]

    def query_transactions(self, agent=None, tx_type=None, limit=100):
        sql = "SELECT * FROM transactions WHERE 1=1"
        params = []
        if agent:
            sql += " AND agent=?"; params.append(agent)
        if tx_type:
            sql += " AND type=?"; params.append(tx_type)
        sql += " ORDER BY timestamp DESC LIMIT ?"
        params.append(limit)
        return [dict(row) for row in self.conn.execute(sql, params)]

    def query_executions(self, program_hash=None, agent=None, limit=100):
        sql = "SELECT * FROM executions WHERE 1=1"
        params = []
        if program_hash:
            sql += " AND program_hash=?"; params.append(program_hash)
        if agent:
            sql += " AND agent=?"; params.append(agent)
        sql += " ORDER BY timestamp DESC LIMIT ?"
        params.append(limit)
        return [dict(row) for row in self.conn.execute(sql, params)]

    def stats(self):
        counts = {}
        for table in ["objects", "transactions", "executions", "consensus"]:
            row = self.conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()
            counts[table] = row[0]
        db_size = self.db_path.stat().st_size if self.db_path.exists() else 0
        return {"tables": counts, "db_size": db_size, "db_path": str(self.db_path)}

    def close(self):
        self.conn.close()


# ═══════════════════════════════════════════
# Layer 3: Vector Embeddings
# ═══════════════════════════════════════════

class NebulaVectors:
    """Vector embedding layer for semantic search over programs and data."""

    def __init__(self, base_path):
        self.base_path = Path(base_path)
        self.vectors_dir = self.base_path / "vectors"
        self.vectors_dir.mkdir(parents=True, exist_ok=True)
        self.dim = 64

    def _vec_path(self, hash_hex):
        return self.vectors_dir / f"{hash_hex}.vec"

    def embed_program(self, hash_hex, program_text):
        """Embed a program by its opcode histogram."""
        opcodes = [
            "MMUL", "MADD", "MSUB", "EMUL", "EDIV", "SDOT", "SCLR", "SDIV",
            "RELU", "SIGM", "TANH", "SOFT", "GELU",
            "LD", "ST", "MOV", "ALLC",
            "RSHP", "TRNS", "SPLT", "MERG",
            "CMPF", "CMP", "CMPI",
            "JMPT", "JMPF", "JUMP", "LOOP", "ENDP",
            "CALL", "RET", "LEAF", "TACC",
            "SYNC", "HALT", "TRAP",
            "CONV", "POOL", "UPSC", "PADZ",
            "ATTN", "NORM", "EMBD",
            "RDUC", "WHER", "CLMP", "CMPR",
            "FFT", "FILT",
            "META", "FRAG", "ENDF", "LINK", "SIGN", "VRFY", "VOTE", "PROJ", "DIST", "GATH", "SCAT",
            "BKWD", "WUPD", "LOSS", "TNET",
        ]

        vec = [0.0] * self.dim
        lines = program_text.upper().split("\n") if isinstance(program_text, str) else []
        for i, op in enumerate(opcodes[:self.dim]):
            vec[i] = sum(1 for line in lines if op in line)

        vec = self._normalize(vec)
        self._save(hash_hex, vec)
        return vec

    def embed_data(self, hash_hex, content, context=None):
        """Embed a data batch by its statistical signature + context semantics."""
        vec = [0.0] * self.dim
        try:
            if isinstance(content, bytes):
                content = content.decode("utf-8")
            data_part = content.split("data=")[1].split()[0] if "data=" in content else ""
            values = [float(v) for v in data_part.split(",") if v.strip()]
            if not values:
                self._save(hash_hex, vec)
                return vec

            n = len(values)
            mean = sum(values) / n
            variance = sum((v - mean) ** 2 for v in values) / n if n > 1 else 0
            std = math.sqrt(variance)
            vmin = min(values)
            vmax = max(values)

            shape_str = content.split("shape=")[1].split()[0] if "shape=" in content else "1"
            dims = [int(d) for d in shape_str.split(",")]

            # Dims 0-9: statistical signature
            vec[0] = mean
            vec[1] = std
            vec[2] = vmin
            vec[3] = vmax
            vec[4] = float(n)
            vec[5] = float(dims[0]) if len(dims) > 0 else 0
            vec[6] = float(dims[1]) if len(dims) > 1 else 0
            vec[7] = float(sum(1 for v in values if v > 0.5))
            vec[8] = float(sum(1 for v in values if v == 0.0))
            vec[9] = float(sum(1 for v in values if v == 1.0))

        except (ValueError, IndexError):
            pass

        # Dims 32-63: context semantic hashing (domain, tags, features → hash-based embedding)
        if context:
            ctx_text = " ".join([
                context.get("description", ""),
                context.get("domain", ""),
                " ".join(context.get("tags", [])),
                " ".join(context.get("features", [])),
                context.get("source", ""),
            ]).lower()
            for ci, ch in enumerate(ctx_text.encode("utf-8")[:32]):
                vec[32 + ci] = (ch % 100) / 100.0

        vec = self._normalize(vec)
        self._save(hash_hex, vec)
        return vec

    def find_similar(self, hash_hex, top_k=5):
        """Find objects with similar embeddings."""
        target = self._load(hash_hex)
        if not target:
            return []

        results = []
        for vec_file in self.vectors_dir.glob("*.vec"):
            other_hash = vec_file.stem
            if other_hash == hash_hex:
                continue
            other = self._load(other_hash)
            if other:
                sim = self._cosine(target, other)
                results.append({"hash": other_hash, "similarity": round(sim, 4)})

        results.sort(key=lambda x: -x["similarity"])
        return results[:top_k]

    def find_compatible(self, program_hash, data_hashes, top_k=10):
        """Rank data batches by compatibility with a program."""
        prog_vec = self._load(program_hash)
        if not prog_vec:
            return []

        results = []
        for dh in data_hashes:
            data_vec = self._load(dh)
            if data_vec:
                sim = self._cosine(prog_vec, data_vec)
                results.append({"hash": dh, "compatibility": round(sim, 4)})

        results.sort(key=lambda x: -x["compatibility"])
        return results[:top_k]

    def _normalize(self, vec):
        magnitude = math.sqrt(sum(v * v for v in vec))
        if magnitude > 0:
            return [v / magnitude for v in vec]
        return vec

    def _cosine(self, a, b):
        dot = sum(x * y for x, y in zip(a, b))
        return max(-1.0, min(1.0, dot))

    def _save(self, hash_hex, vec):
        data = struct.pack(f"!{self.dim}f", *vec)
        with open(self._vec_path(hash_hex), "wb") as f:
            f.write(data)

    def _load(self, hash_hex):
        path = self._vec_path(hash_hex)
        if not path.exists():
            return None
        with open(path, "rb") as f:
            data = f.read()
        if len(data) != self.dim * 4:
            return None
        return list(struct.unpack(f"!{self.dim}f", data))

    def stats(self):
        vecs = list(self.vectors_dir.glob("*.vec"))
        return {
            "vector_count": len(vecs),
            "dim": self.dim,
            "total_bytes": len(vecs) * self.dim * 4,
            "storage_path": str(self.vectors_dir),
        }
