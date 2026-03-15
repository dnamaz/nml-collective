#!/usr/bin/env python3
"""
NML Relay Server — WebSocket relay for cross-network agent communication.

Agents on different LANs connect outbound to this relay. The relay forwards
messages between all connected agents, enabling WAN-scale collectives.

Agents connect with: --relay ws://relay-host:7777

Usage:
    python3 nml_relay.py --port 7777
"""

import argparse
import asyncio
import json
import time
from aiohttp import web

connected_agents = {}  # ws -> {name, port, connected_at}


async def handle_ws(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    agent_name = "unknown"

    try:
        async for msg in ws:
            if msg.type == web.WSMsgType.TEXT:
                try:
                    data = json.loads(msg.data)
                except json.JSONDecodeError:
                    continue

                if data.get("type") == "register":
                    agent_name = data.get("name", "unknown")
                    connected_agents[ws] = {
                        "name": agent_name,
                        "port": data.get("port", 0),
                        "connected_at": time.time(),
                    }
                    print(f"  [+] {agent_name} connected ({len(connected_agents)} agents)")

                    peers = [{"name": i["name"], "port": i["port"]}
                             for w, i in connected_agents.items() if w != ws]
                    await ws.send_json({"type": "peers", "peers": peers})
                    continue

                for other_ws, info in list(connected_agents.items()):
                    if other_ws != ws and not other_ws.closed:
                        try:
                            await other_ws.send_json(data)
                        except Exception:
                            pass

            elif msg.type == web.WSMsgType.BINARY:
                for other_ws in list(connected_agents.keys()):
                    if other_ws != ws and not other_ws.closed:
                        try:
                            await other_ws.send_bytes(msg.data)
                        except Exception:
                            pass

            elif msg.type in (web.WSMsgType.ERROR, web.WSMsgType.CLOSE):
                break
    finally:
        if ws in connected_agents:
            print(f"  [-] {connected_agents[ws]['name']} disconnected ({len(connected_agents)-1} agents)")
            del connected_agents[ws]

    return ws


async def handle_health(request):
    agents = [{"name": i["name"], "port": i["port"]} for i in connected_agents.values()]
    return web.json_response({
        "status": "healthy",
        "service": "nml-relay",
        "agents": len(connected_agents),
        "agent_list": agents,
    })


def main():
    parser = argparse.ArgumentParser(description="NML Relay — WebSocket relay for WAN agents")
    parser.add_argument("--port", type=int, default=7777, help="Relay port (default: 7777)")
    args = parser.parse_args()

    app = web.Application()
    app.router.add_get("/ws", handle_ws)
    app.router.add_get("/health", handle_health)

    print(f"NML Relay Server on :{args.port}")
    print(f"  Agents connect with: --relay ws://host:{args.port}/ws")
    web.run_app(app, port=args.port, print=None)


if __name__ == "__main__":
    main()
