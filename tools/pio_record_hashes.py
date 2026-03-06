from datetime import datetime, timezone
from pathlib import Path
import hashlib

Import("env")


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _record_hashes(target, source, env):
    project_dir = Path(env.subst("$PROJECT_DIR"))
    pio_env = env.subst("$PIOENV")
    build_dir = Path(env.subst("$BUILD_DIR"))
    progname = env.subst("${PROGNAME}")

    elf_path = build_dir / f"{progname}.elf"
    bin_path = build_dir / f"{progname}.bin"
    if not elf_path.exists() or not bin_path.exists():
        print("[hash] skipped (missing firmware outputs)")
        return

    ts = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    elf_hash = _sha256(elf_path)
    bin_hash = _sha256(bin_path)

    lines = [
        f"timestamp_utc={ts}",
        f"pio_env={pio_env}",
        f"elf={elf_path}",
        f"elf_sha256={elf_hash}",
        f"bin={bin_path}",
        f"bin_sha256={bin_hash}",
        "",
    ]
    payload = "\n".join(lines)

    (build_dir / "build_hashes.txt").write_text(payload, encoding="utf-8")
    (project_dir / ".pio" / "last_build_hashes.txt").write_text(payload, encoding="utf-8")

    print(f"[hash] elf_sha256={elf_hash}")
    print(f"[hash] bin_sha256={bin_hash}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _record_hashes)
