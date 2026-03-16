#!/usr/bin/env python3
"""
NML Architect — the collective's program builder.

The Architect receives structured program specifications (from the Oracle or
external requests), generates valid NML programs via the NML LLM, validates
them by dry-run assembly, and outputs symbolic compact form for minimal
packet size distribution.

She doesn't reason about what to build (that's the Oracle). She builds what
she's told, validates it, and ships it in the smallest possible form.

The NML LLM is trained to generate all three syntaxes:
  - classic:  LD R1 @w1
  - symbolic: ↓ κ @w1
  - verbose:  LOAD R1 @w1

The Architect requests symbolic output directly from the LLM for smallest
packet size. She validates by assembling with the nml-crypto binary.

Usage (embedded in agent):
    from nml_architect import ArchitectEngine
    architect = ArchitectEngine(agent, llm_url="http://localhost:8082")
"""

import asyncio
import hashlib
import json
import subprocess
import tempfile
import time
from pathlib import Path
from aiohttp import ClientSession


PROJECT_ROOT = Path(__file__).parent.parent

def _find_nml():
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

SYSTEM_PROMPT = """You are an NML code generator. You write valid NML programs in symbolic syntax.

NML is a tensor register machine with 82 opcodes. Programs are sequences of instructions
operating on 16 registers (R0-R9, RA-RF) and named memory slots (@name).

Output ONLY valid NML code in symbolic syntax using Unicode opcodes and Greek registers.
No markdown, no explanation, no backticks — just the program.

Symbolic opcode reference (most common):
  ↓ = LD (load)    ↑ = ST (store)    × = MMUL    ⊕ = MADD    ⊖ = MSUB
  ⌐ = RELU         σ = SIGM          τ = TANH    Σ = SOFT
  ⊗ = EMUL         ⊘ = EDIV          · = SDOT    ∗ = SCLR
  ∎ = LEAF         ≺ = CMPI          ↘ = JMPF    → = JUMP    ◼ = HALT
  ⥁ = TNET         ⊞ = RSHP          ⊤ = TRNS    ↻ = LOOP    ↺ = ENDP
  ✦ = SIGN         ✓ = VRFY          ⚖ = VOTE

Symbolic registers: ι=R0 κ=R1 λ=R2 μ=R3 ν=R4 ξ=R5 ο=R6 π=R7 ρ=R8 ς=R9 α=RA β=RB γ=RC δ=RD φ=RE ψ=RF

Rules:
- Each line is one instruction
- Comments start with ;
- Named memory: @name
- Immediate values: #number
- TNET trains the network: ⥁ #epochs #learning_rate #0
- Programs must end with ◼ (HALT)
"""


class ArchitectEngine:
    """Program builder for the NML collective."""

    def __init__(self, agent, llm_url=None):
        self.agent = agent
        self.llm_url = llm_url or agent.llm_url
        self.catalog = {}  # hash -> {spec, program, symbolic, status, timestamp}
        self.build_count = 0

    async def start(self):
        self.agent.log_event("architect_started", f"LLM={'connected' if self.llm_url else 'none'}")

    def stop(self):
        pass

    async def build(self, spec):
        """Build an NML program from a structured specification.

        spec: {
            intent: str,           -- what the program should do
            features: int,         -- input feature count (optional)
            architecture: str,     -- e.g. "6→8→1" (optional)
            data_bindings: dict,   -- @name -> description (optional)
            based_on: str,         -- hash of existing program to adapt (optional)
            output_syntax: str,    -- "symbolic" (default), "classic", "verbose"
        }

        Returns: {program, symbolic, hash, validation, spec, status}
        """
        self.agent.log_event("architect_build", spec.get("intent", "")[:80])

        if not self.llm_url:
            return {"error": "Architect requires an LLM connection (--llm)", "status": "failed"}

        prompt = self._build_prompt(spec)
        syntax = spec.get("output_syntax", "symbolic")

        program = await self._generate(prompt, syntax)
        if not program:
            return {"error": "LLM generation failed", "status": "failed"}

        program = self._clean_output(program)

        validation = self._validate(program)

        if not validation["valid"] and validation.get("classic_fallback"):
            self.agent.log_event("architect_retry", "symbolic failed, retrying classic→convert")
            classic_program = await self._generate(prompt, "classic")
            if classic_program:
                classic_program = self._clean_output(classic_program)
                classic_val = self._validate(classic_program)
                if classic_val["valid"]:
                    program = self._to_symbolic(classic_program)
                    validation = self._validate(program)
                    if not validation["valid"]:
                        program = classic_program
                        validation = classic_val

        phash = hashlib.sha256(program.encode()).hexdigest()[:16]

        symbolic = self._to_compact_symbolic(program)
        byte_size = len(symbolic.encode("utf-8"))

        entry = {
            "hash": phash,
            "program": program,
            "symbolic_compact": symbolic,
            "byte_size": byte_size,
            "fits_udp": byte_size < 65000,
            "validation": validation,
            "spec": spec,
            "status": "valid" if validation["valid"] else "invalid",
            "timestamp": time.time(),
        }
        self.catalog[phash] = entry
        self.build_count += 1

        status = "valid" if validation["valid"] else "invalid"
        self.agent.log_event("architect_built",
                             f"hash={phash} status={status} size={byte_size}B")

        return entry

    async def validate_program(self, program):
        """Validate an externally provided NML program."""
        result = self._validate(program)
        result["byte_size"] = len(program.encode("utf-8"))
        return result

    def get_catalog(self):
        """Return all programs the architect has built."""
        return [
            {
                "hash": e["hash"],
                "intent": e["spec"].get("intent", ""),
                "status": e["status"],
                "byte_size": e["byte_size"],
                "fits_udp": e["fits_udp"],
                "timestamp": e["timestamp"],
            }
            for e in self.catalog.values()
        ]

    def _build_prompt(self, spec):
        """Build the LLM prompt from a program specification."""
        parts = [f"Write an NML program in symbolic syntax.\n"]

        if spec.get("intent"):
            parts.append(f"Purpose: {spec['intent']}")

        if spec.get("features"):
            parts.append(f"Input features: {spec['features']}")

        if spec.get("architecture"):
            parts.append(f"Architecture: {spec['architecture']}")

        if spec.get("data_bindings"):
            parts.append("Data bindings:")
            for name, desc in spec["data_bindings"].items():
                parts.append(f"  {name}: {desc}")

        if spec.get("based_on"):
            existing = self.agent.seen_programs.get(spec["based_on"])
            if existing:
                parts.append(f"\nAdapt this existing program:\n{existing}")

        if spec.get("training"):
            t = spec["training"]
            epochs = t.get("epochs", 1000)
            lr = t.get("learning_rate", 0.01)
            parts.append(f"Training: {epochs} epochs, learning rate {lr}")

        if spec.get("threshold"):
            parts.append(f"Decision threshold: {spec['threshold']}")

        parts.append("\nOutput ONLY the symbolic NML program. No explanation.")

        return "\n".join(parts)

    async def _generate(self, prompt, syntax="symbolic"):
        """Call the NML LLM to generate a program."""
        syntax_instruction = {
            "symbolic": "Generate in symbolic syntax (Unicode opcodes: ↓×⊕⌐σ◼, Greek registers: ικλμνξοπρςαβγδφψ)",
            "classic": "Generate in classic syntax (MMUL, MADD, LD, ST, R0-RF)",
            "verbose": "Generate in verbose syntax (MATRIX_MULTIPLY, ADD, LOAD, STORE)",
        }.get(syntax, "")

        messages = [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": f"{syntax_instruction}\n\n{prompt}"},
        ]

        try:
            async with ClientSession() as session:
                async with session.post(
                    f"{self.llm_url}/v1/chat/completions",
                    json={"messages": messages, "max_tokens": 1024, "mode": "nml",
                          "temperature": 0.2},
                    timeout=60,
                ) as resp:
                    data = await resp.json()
                    return data["choices"][0]["message"]["content"]
        except Exception as e:
            self.agent.log_event("architect_llm_error", str(e))
            return None

    def _clean_output(self, raw):
        """Strip markdown fences and non-code content from LLM output."""
        lines = raw.strip().split("\n")
        cleaned = []
        in_fence = False
        for line in lines:
            if line.strip().startswith("```"):
                in_fence = not in_fence
                continue
            if in_fence or not line.strip().startswith("```"):
                cleaned.append(line)
        result = "\n".join(cleaned).strip()
        if not result:
            result = raw.strip()
        return result

    def _validate(self, program):
        """Validate by attempting assembly with the NML binary (dry run)."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".nml", delete=False) as f:
            f.write(program)
            path = f.name

        try:
            result = subprocess.run(
                [str(NML_BINARY), path, "--max-cycles", "0"],
                capture_output=True, text=True, timeout=10,
            )
            output = result.stdout + result.stderr

            has_error = result.returncode != 0
            error_lines = [l for l in output.split("\n")
                           if "error" in l.lower() or "TRAP" in l or "unknown" in l.lower()]

            return {
                "valid": not has_error and not error_lines,
                "exit_code": result.returncode,
                "errors": error_lines[:5] if error_lines else [],
                "output_preview": output[:300],
                "classic_fallback": has_error,
            }
        except subprocess.TimeoutExpired:
            return {"valid": False, "exit_code": -1, "errors": ["Validation timeout"],
                    "classic_fallback": False}
        except FileNotFoundError:
            return {"valid": False, "exit_code": -1,
                    "errors": [f"NML binary not found at {NML_BINARY}"],
                    "classic_fallback": False}
        finally:
            Path(path).unlink(missing_ok=True)

    def _to_symbolic(self, classic_program):
        """Convert a classic-syntax program to symbolic using the NML builder mappings."""
        opmap = {
            "MMUL": "×", "MADD": "⊕", "MSUB": "⊖", "EMUL": "⊗", "EDIV": "⊘",
            "SDOT": "·", "SCLR": "∗", "SDIV": "÷",
            "RELU": "⌐", "SIGM": "σ", "TANH": "τ", "SOFT": "Σ",
            "LD": "↓", "ST": "↑", "MOV": "←", "ALLC": "□",
            "RSHP": "⊞", "TRNS": "⊤", "SPLT": "⊢", "MERG": "⊣",
            "LOOP": "↻", "ENDP": "↺",
            "SYNC": "⏸", "HALT": "◼",
            "CMPF": "⋈", "CMP": "≶", "CMPI": "≺",
            "LEAF": "∎", "TACC": "∑", "TNET": "⥁",
            "JMPT": "↗", "JMPF": "↘", "JUMP": "→",
            "CALL": "⇒", "RET": "⇐", "TRAP": "⚠",
            "CONV": "⊛", "POOL": "⊓", "UPSC": "⊔", "PADZ": "⊡",
            "ATTN": "⊙", "NORM": "‖", "EMBD": "⊏", "GELU": "ℊ",
            "RDUC": "⊥", "WHER": "⊻", "CLMP": "⊧", "CMPR": "⊜",
            "FFT": "∿", "FILT": "⋐",
            "META": "§", "FRAG": "◆", "ENDF": "◇", "LINK": "⊕",
            "PTCH": "⊿", "SIGN": "✦", "VRFY": "✓",
            "VOTE": "⚖", "PROJ": "⟐", "DIST": "⟂", "GATH": "⊃", "SCAT": "⊂",
            "BKWD": "∂", "WUPD": "∇", "LOSS": "𝓛",
        }
        regmap = {
            "R0": "ι", "R1": "κ", "R2": "λ", "R3": "μ", "R4": "ν",
            "R5": "ξ", "R6": "ο", "R7": "π", "R8": "ρ", "R9": "ς",
            "RA": "α", "RB": "β", "RC": "γ", "RD": "δ", "RE": "φ", "RF": "ψ",
        }

        lines = []
        for line in classic_program.split("\n"):
            stripped = line.split(";")[0].strip()
            if not stripped or stripped.startswith(";"):
                continue
            tokens = stripped.split()
            if not tokens:
                continue
            op = opmap.get(tokens[0], tokens[0])
            args = []
            for tok in tokens[1:]:
                args.append(regmap.get(tok.upper(), tok))
            lines.append(op + " " + " ".join(args) if args else op)
        return "\n".join(lines)

    def _to_compact_symbolic(self, program):
        """Convert program to pilcrow-delimited compact form (strips comments)."""
        lines = [l.strip() for l in program.split("\n")
                 if l.strip() and not l.strip().startswith(";")]
        return "\xb6".join(lines)
