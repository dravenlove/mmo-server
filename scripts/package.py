#!/usr/bin/env python3
"""只打包明确列出的源码和部署资料，不收集凭据、数据库或测试输出。"""
import hashlib
import json
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parent.parent
files = [root / name for name in (
    "README.md", "ARCHITECTURE.md", "go.mod", "go.sum", ".gitignore",
    ".dockerignore", ".gitattributes", ".env.example", "Dockerfile.golang", "Dockerfile.scene", "compose.yaml",
    ".vscode/extensions.json", ".vscode/launch.json", ".vscode/tasks.json",
    ".github/workflows/publish-images.yaml",
    "configs/combat.default.json", "configs/combat.demo.json",
)]
for directory in ("cmd", "internal", "scene/src", "scene/tests", "scripts", "docs", "third_party"):
    files.extend(p for p in (root / directory).rglob("*")
                 if p.is_file() and "__pycache__" not in p.parts and not p.name.startswith("__debug_bin"))
files.append(root / "scene/CMakeLists.txt")
files.extend(p for p in (root / "desktop").rglob("*")
             if p.is_file() and p.suffix in (".cs", ".csproj", ".xaml")
             and "bin" not in p.parts and "obj" not in p.parts)
files.extend(p for p in (root / "deploy").iterdir() if p.is_file() and p.name.endswith((".compose.yaml", ".env.example", ".service.example")))
files = sorted(set(files), key=lambda p: p.relative_to(root).as_posix())
output = root / "dist"
output.mkdir(exist_ok=True)
archive = output / "mmo-mvp-source.zip"
manifest = {}
with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
    for path in files:
        relative = path.relative_to(root).as_posix()
        data = path.read_bytes()
        manifest[relative] = hashlib.sha256(data).hexdigest()
        info = zipfile.ZipInfo("game_sever/" + relative)
        info.create_system = 3
        info.external_attr = (0o100755 if path.suffix in (".sh", ".py") else 0o100644) << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        bundle.writestr(info, data)
    bundle.writestr("game_sever/SOURCE_MANIFEST.json", json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
(output / "mmo-mvp-source.zip.sha256").write_text(digest + "  " + archive.name + "\n", encoding="utf-8")
print(f"Packaged {len(files)} source files: {archive} ({archive.stat().st_size} bytes)")
