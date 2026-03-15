#!/usr/bin/env python3
"""
NML Agent — receives signed programs from a hub, verifies, and executes.

Usage:
    python3 nml_agent.py --name agent_1 --hub http://localhost:8082 --port 9001
    python3 nml_agent.py --name agent_2 --hub http://localhost:8082 --port 9002 --data local_data.nml.data
"""

import argparse
import asyncio
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from aiohttp import web, ClientSession

PROJECT_ROOT = Path(__file__).parent.parent

def _find_nml():
    candidates = [
        PROJECT_ROOT / "nml-crypto", PROJECT_ROOT / "nml",
        PROJECT_ROOT.parent / "nml" / "nml-crypto", PROJECT_ROOT.parent / "nml" / "nml",
    ]
    for c in candidates:
        if c.exists(): return c
    import shutil
    found = shutil.which("nml-crypto") or shutil.which("nml")
    return Path(found) if found else Path("nml")

NML_BINARY = _find_nml()


async def execute_signed(program: str, data: str = None) -> dict:
    """Execute a signed NML program using nml-crypto."""
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
                key = parts[0].strip()
                val = parts[1].strip()
                memory[key] = val

        return {
            "success": result.returncode == 0,
            "output": output,
            "memory": memory,
            "exit_code": result.returncode,
        }
    except subprocess.TimeoutExpired:
        return {"success": False, "output": "Execution timed out", "memory": {}, "exit_code": -1}
    finally:
        Path(prog_path).unlink(missing_ok=True)
        if data_path:
            Path(data_path).unlink(missing_ok=True)


def create_agent_app(name: str, hub_url: str, data_path: str = None):
    """Create an agent HTTP server."""

    local_data = None
    if data_path and Path(data_path).exists():
        local_data = Path(data_path).read_text()

    results_store = []

    def _cors():
        return {
            "Access-Control-Allow-Origin": "*",
            "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
            "Access-Control-Allow-Headers": "Content-Type",
        }

    def _json(data, status=200):
        return web.Response(text=json.dumps(data), content_type="application/json",
                            status=status, headers=_cors())

    async def handle_health(request):
        return _json({
            "status": "healthy",
            "agent": name,
            "hub": hub_url,
            "has_local_data": local_data is not None,
        })

    async def handle_execute(request):
        body = await request.json()
        program = body.get("program", "")
        data = body.get("data", local_data)

        if not program:
            return _json({"error": "No program provided"}, 400)

        result = await execute_signed(program, data)
        result["agent"] = name
        results_store.append(result)
        return _json(result)

    async def handle_results(request):
        return _json({"agent": name, "results": results_store})

    async def register_with_hub(app):
        """Register this agent with the hub on startup."""
        try:
            async with ClientSession() as session:
                await session.post(f"{hub_url}/agent/register", json={
                    "name": name,
                    "url": f"http://localhost:{app['port']}",
                    "capabilities": ["TNET", "NML-V", "NML-T", "NML-R"],
                })
                print(f"  Registered with hub at {hub_url}")
        except Exception as e:
            print(f"  WARNING: Could not register with hub: {e}")

    app = web.Application()
    app.router.add_get("/health", handle_health)
    app.router.add_post("/execute", handle_execute)
    app.router.add_get("/results", handle_results)
    app.router.add_route("OPTIONS", "/{path:.*}",
                         lambda r: web.Response(status=204, headers=_cors()))
    app.on_startup.append(register_with_hub)

    return app


def main():
    parser = argparse.ArgumentParser(description="NML Agent — execute signed programs")
    parser.add_argument("--name", type=str, required=True, help="Agent name")
    parser.add_argument("--hub", type=str, default="http://localhost:8082", help="Hub URL")
    parser.add_argument("--port", type=int, default=9001, help="Agent port")
    parser.add_argument("--data", type=str, default=None, help="Path to local .nml.data file")
    args = parser.parse_args()

    app = create_agent_app(args.name, args.hub, args.data)
    app['port'] = args.port
    print(f"NML Agent '{args.name}' on :{args.port}")
    print(f"  Hub: {args.hub}")
    if args.data:
        print(f"  Data: {args.data}")
    web.run_app(app, port=args.port, print=None)


if __name__ == "__main__":
    main()
