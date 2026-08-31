#!/usr/bin/env python3
"""
Minimal MCP-over-HTTP client for the Saleae Logic 2 automation server.

Logic 2 exposes an MCP server at 127.0.0.1:10530 once it is enabled under
Settings -> Automation -> MCP Server. This module talks to it directly over
HTTP so the capture scripts work from a plain shell, with no agent or SDK
in the loop.

Usage as a library:
    import saleae_mcp
    saleae_mcp.init()
    print(saleae_mcp.tool("get_devices"))

Usage from the shell:
    python3 saleae_mcp.py get_devices
    python3 saleae_mcp.py export_raw_data_csv '{"captureId":1, ...}'
"""

import json
import sys
import urllib.error
import urllib.request

URL = "http://127.0.0.1:10530"
TIMEOUT = 900  # captures block; wait_capture can run for minutes

_next_id = [0]


class LogicError(RuntimeError):
    """An error returned by the Logic 2 MCP server."""


def call(method, params=None):
    """Issue a JSON-RPC call and return the `result` object."""
    _next_id[0] += 1
    body = {"jsonrpc": "2.0", "id": _next_id[0], "method": method}
    if params is not None:
        body["params"] = params
    req = urllib.request.Request(
        URL,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json",
                 "Accept": "application/json, text/event-stream"},
    )
    try:
        raw = urllib.request.urlopen(req, timeout=TIMEOUT).read().decode()
    except urllib.error.URLError as e:
        raise LogicError(
            f"cannot reach Logic 2 at {URL}: {e}. Is Logic 2 running with "
            f"Settings -> Automation -> MCP Server enabled?") from e
    # Responses may arrive as plain JSON or as a text/event-stream frame.
    for line in raw.splitlines():
        line = line[6:] if line.startswith("data: ") else line
        if line.strip().startswith("{"):
            msg = json.loads(line)
            if "error" in msg:
                raise LogicError(msg["error"])
            return msg.get("result")
    raise LogicError(f"no JSON-RPC payload in response: {raw[:400]}")


def tool(name, args=None):
    """Call a tool and return its concatenated text content."""
    result = call("tools/call", {"name": name, "arguments": args or {}})
    return "\n".join(c["text"] for c in result.get("content", [])
                     if c.get("type") == "text")


def tool_json(name, args=None):
    """Call a tool whose text content is a JSON object, and parse it."""
    text = tool(name, args)
    try:
        return json.loads(text)
    except json.JSONDecodeError as e:
        raise LogicError(f"{name} did not return JSON: {text[:300]}") from e


def init():
    """Perform the MCP handshake. Call once before any tool()."""
    call("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                        "clientInfo": {"name": "caesium", "version": "1"}})


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip())
        return 2
    init()
    args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    print(tool(sys.argv[1], args))
    return 0


if __name__ == "__main__":
    sys.exit(main())
