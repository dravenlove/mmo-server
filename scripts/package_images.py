#!/usr/bin/env python3
"""在构建机生成 Linux 镜像包；目标 VPS 只需 docker load，无需编译。"""
import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parent.parent


def capture(*command):
    return subprocess.check_output(command, cwd=ROOT, text=True).strip()


def run_logged(command, path):
    # 逐行显示进度，同时保存日志；失败立即停止，避免导出旧镜像。
    with path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True,
                                   encoding="utf-8", errors="replace")
        try:
            for line in process.stdout:
                print(line, end="", flush=True)
                log.write(line)
            code = process.wait()
        except BaseException:
            process.terminate()
            process.wait()
            raise
        finally:
            process.stdout.close()
        if code:
            raise RuntimeError(f"Command failed ({code}); log: {path}")


def export_images(output, name, references, platform):
    raw = output / (name + ".tar.partial")
    compressed = output / (name + ".tar.gz.partial")
    destination = output / (name + ".tar.gz")
    print(f"Exporting {', '.join(references)} -> {destination.name}", flush=True)
    # 用 -o 写二进制，避免 Windows PowerShell 的文本管道损坏 tar。
    subprocess.run(["docker", "image", "save", "-o", str(raw), *references],
                   cwd=ROOT, check=True)
    with raw.open("rb") as source, compressed.open("wb") as target:
        with gzip.GzipFile(filename="", mode="wb", fileobj=target,
                           compresslevel=6, mtime=0) as stream:
            shutil.copyfileobj(source, stream, length=1024 * 1024)
    # 校验归档中真实的标签和 Linux 架构，不能只根据文件名判断成功。
    with tarfile.open(compressed, "r:gz") as archive:
        manifest = json.load(archive.extractfile("manifest.json"))
        found = {tag for entry in manifest for tag in entry.get("RepoTags", [])}
        if found != set(references):
            raise RuntimeError(f"Unexpected archive tags: {found}")
        for entry in manifest:
            config = json.load(archive.extractfile(entry["Config"]))
            actual = f"{config['os']}/{config['architecture']}"
            if actual != platform:
                raise RuntimeError(f"Archive platform {actual} != {platform}")
    compressed.replace(destination)
    raw.unlink()  # 只删除本次创建的临时 tar，保留验证后的压缩包。
    digest = hashlib.sha256()
    with destination.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    checksum = digest.hexdigest()
    destination.with_suffix(destination.suffix + ".sha256").write_text(
        f"{checksum}  {destination.name}\n", encoding="ascii")
    return {"file": destination.name, "bytes": destination.stat().st_size,
            "sha256": checksum, "images": references}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True, help="本次镜像版本，如 mvp-20261104")
    parser.add_argument("--platform", choices=["linux/amd64", "linux/arm64"], default="linux/amd64")
    parser.add_argument("--debian-mirror", default="deb.debian.org", help="Debian 镜像域名，不带协议和路径")
    parser.add_argument("--goproxy", default="https://proxy.golang.org,direct")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}", args.tag):
        parser.error("invalid image tag")
    if not re.fullmatch(r"[A-Za-z0-9.-]+", args.debian_mirror):
        parser.error("--debian-mirror must be a hostname")
    if not shutil.which("docker"):
        raise RuntimeError("Docker CLI not found")
    if capture("docker", "info", "--format", "{{.OSType}}") != "linux":
        raise RuntimeError("Start Docker Desktop with Linux containers before packaging")
    capture("docker", "buildx", "version")

    output = ROOT / "dist" / "docker" / f"{args.tag}-{args.platform.replace('/', '-')}"
    output.mkdir(parents=True, exist_ok=True)
    if (output / "manifest.json").exists() or list(output.glob("*.tar.gz")):
        raise RuntimeError(f"Package already exists: {output}; choose a new --tag")
    logs = ROOT / "output" / ("image-build-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S"))
    logs.mkdir(parents=True, exist_ok=False)
    revision = capture("git", "rev-parse", "HEAD")
    dirty = bool(capture("git", "status", "--porcelain"))
    images = []
    for service in ("backend", "gateway", "client", "scene"):
        reference = f"mmo/{service}:{args.tag}"
        dockerfile = "Dockerfile.scene" if service == "scene" else "Dockerfile.golang"
        command = ["docker", "buildx", "build", "--platform", args.platform,
                   "--load", "--progress=plain", "-f", dockerfile,
                   "--build-arg", f"DEBIAN_MIRROR={args.debian_mirror}",
                   "--label", f"org.opencontainers.image.revision={revision}",
                   "--label", "org.opencontainers.image.source=https://github.com/dravenlove/mmo-server",
                   "-t", reference]
        if service != "scene":
            command.extend(["--build-arg", f"SERVICE={service}", "--build-arg", f"GOPROXY={args.goproxy}"])
        print(f"Building {reference} ({args.platform})", flush=True)
        run_logged([*command, "."], logs / f"{service}.log")
        info = json.loads(capture("docker", "image", "inspect", reference))[0]
        actual = f"{info['Os']}/{info['Architecture']}"
        if actual != args.platform:
            raise RuntimeError(f"Image {reference}: {actual} != {args.platform}")
        images.append({"reference": reference, "id": info["Id"], "platform": actual})

    groups = {"mmo-gateway": [f"mmo/{name}:{args.tag}" for name in ("backend", "gateway", "client")],
              "mmo-scene": [f"mmo/scene:{args.tag}"]}
    archives = [export_images(output, name, refs, args.platform) for name, refs in groups.items()]
    manifest = {"schema_version": 1, "tag": args.tag, "platform": args.platform,
                "source_revision": revision, "source_dirty": dirty,
                "created_utc": datetime.now(timezone.utc).isoformat(),
                "images": images, "archives": archives}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Packages ready: {output}", flush=True)
    print(f"Set MMO_IMAGE_TAG={args.tag} in both VPS env files. No runtime credentials are included.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Packaging failed: {error}", file=sys.stderr)
        sys.exit(1)
