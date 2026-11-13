#!/usr/bin/env python3
"""创建独立本机联调配置；已存在时不覆盖，不输出服务密钥。"""
import argparse
import os
from pathlib import Path
import secrets
from urllib.parse import urlsplit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--origin", default="http://localhost:8088")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent.parent / "deploy/native-local.env")
    args = parser.parse_args()
    value = urlsplit(args.origin)
    if (value.scheme not in ("http", "https") or not value.hostname or value.username or value.password
            or value.path or value.query or value.fragment or any(c in args.origin for c in "\r\n,;*")):
        parser.error("origin 必须是完整且精确的网页 Origin，例如 http://localhost:8088")
    contents = ("# 本机独立 C++ 测试，不连接真实业务库\n"
                f"INTERNAL_TOKEN={secrets.token_hex(32)}\nNATIVE_ALLOWED_ORIGINS={args.origin}\n"
                "MMO_IMAGE_PREFIX=mmo\nMMO_NATIVE_IMAGE_TAG=dev\n")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        print("配置已存在，保留原内容：", args.output)
        return
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(contents)
    print("已生成独立测试配置：", args.output, "（密钥不输出）")


if __name__ == "__main__":
    main()
