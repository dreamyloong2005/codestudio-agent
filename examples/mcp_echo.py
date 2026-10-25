"""Minimal stdio MCP server: Python standard library only; no filesystem tools.

Configure an absolute python.exe path and this script's absolute path in your
manifest. stdout is reserved for JSON-RPC; diagnostics belong on stderr.
"""
import json
import sys

if hasattr(sys.stdin, "reconfigure"):
    sys.stdin.reconfigure(encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")

initialized = False
for line in sys.stdin:
    request = json.loads(line)
    method = request.get("method")
    if "id" not in request:
        if method == "notifications/initialized":
            initialized = True
        continue
    response = {"jsonrpc": "2.0", "id": request["id"]}
    if method == "initialize":
        response["result"] = {"protocolVersion": "2025-11-25", "capabilities": {"tools": {}},
                              "serverInfo": {"name": "codestudio-echo-example", "version": "1.0.0"}}
    elif method == "ping":
        response["result"] = {}
    elif method == "tools/list" and initialized:
        response["result"] = {"tools": [{"name": "echo", "description": "Return the supplied text.",
            "inputSchema": {"type": "object", "properties": {"text": {"type": "string", "maxLength": 4096}},
                            "required": ["text"], "additionalProperties": False}}]}
    elif method == "tools/call" and initialized:
        params = request.get("params", {})
        arguments = params.get("arguments", {})
        if (params.get("name") == "echo" and isinstance(arguments, dict) and set(arguments) == {"text"}
                and isinstance(arguments["text"], str) and len(arguments["text"]) <= 4096):
            response["result"] = {"content": [{"type": "text", "text": arguments["text"]}], "isError": False}
        else:
            response["result"] = {"content": [{"type": "text", "text": "Invalid echo arguments"}], "isError": True}
    else:
        response["error"] = {"code": -32601, "message": "Method not found"}
    print(json.dumps(response, ensure_ascii=False), flush=True)
