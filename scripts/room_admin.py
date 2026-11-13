#!/usr/bin/env python3
"""房间状态、可信脚本重新加载及安全排空；只从私有 env 读取内部凭据。"""
import argparse
import json
from pathlib import Path
import urllib.error
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("status", "reload", "drain"))
    parser.add_argument("--env-file", type=Path, required=True)
    parser.add_argument("--worker-url", default="http://127.0.0.1:9006")
    parser.add_argument("--backend-url", default="http://127.0.0.1:8081")
    parser.add_argument("--room-id")
    args = parser.parse_args()
    token = ""
    for line in args.env_file.read_text(encoding="utf-8-sig").splitlines():
        key, separator, value = line.strip().partition("=")
        if separator and key == "INTERNAL_TOKEN":
            token = value.strip()
            if len(token) >= 2 and token[0] == token[-1] and token[0] in "\"'": token = token[1:-1]
    if len(token) < 32 or "\n" in token or "\r" in token:
        parser.error("私有env缺少有效INTERNAL_TOKEN；不要把密钥作为命令参数")
    url = args.backend_url if args.command == "drain" else args.worker_url
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
        parser.error("管理地址必须是可信服务的HTTP(S)地址，不含凭据或query")
    body = {}
    if args.room_id:
        if args.command == "reload": parser.error("reload固定读取服务器配置的manifest，不接收room_id或脚本路径")
        body["room_id"] = args.room_id
    path = "/v1/room_drain" if args.command == "drain" else "/v1/rooms/scripts/reload" if args.command == "reload" else "/v1/rooms/status" if args.room_id else "/v1/rooms/list"
    request = urllib.request.Request(url.rstrip("/") + path, data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json", "X-Internal-Token": token})
    opener = urllib.request.build_opener(NoRedirect(), urllib.request.ProxyHandler({}))
    try:
        with opener.open(request, timeout=18) as response:
            payload = json.load(response)
        print(json.dumps(payload, ensure_ascii=False))
        if args.command == "drain" and not payload.get("complete", False):
            raise SystemExit(2)
    except urllib.error.HTTPError as error:
        print(json.dumps({"ok": False, "status": error.code, "command": args.command}, ensure_ascii=False))
        raise SystemExit(1)
    except (OSError, ValueError):
        print(json.dumps({"ok": False, "command": args.command, "error": "管理服务不可达或响应无效；未更改本机配置"}, ensure_ascii=False))
        raise SystemExit(1)


if __name__ == "__main__":
    main()
