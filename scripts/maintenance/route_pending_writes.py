#!/usr/bin/env python3
"""Route a legacy global Axon queue into registered project queues safely.

Default mode is read-only.  --apply first archives the original queue, then
writes deduplicated project queues and leaves only unmatched paths active.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import time
from pathlib import Path


def read_lines(path: Path) -> list[str]:
    if not path.exists():
        return []
    return [line.strip() for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def registry_roots(path: Path) -> list[Path]:
    data = json.loads(path.read_text(encoding="utf-8"))
    roots = []
    for repo in data.get("repos", []):
        root = repo.get("root")
        if isinstance(root, str) and root:
            candidate = Path(root).resolve()
            if (candidate / ".axon").is_dir():
                roots.append(candidate)
    return sorted(set(roots), key=lambda item: len(str(item)), reverse=True)


def route(entries: list[str], roots: list[Path]) -> tuple[dict[Path, list[str]], list[str]]:
    routed: dict[Path, list[str]] = {root: [] for root in roots}
    unmatched: list[str] = []
    for entry in entries:
        resolved = Path(os.path.realpath(entry))
        owner = next((root for root in roots if resolved == root or root in resolved.parents), None)
        if owner is None:
            unmatched.append(entry)
        else:
            routed[owner].append(entry)
    return {root: values for root, values in routed.items() if values}, unmatched


def write_unique(path: Path, entries: list[str]) -> None:
    existing = read_lines(path)
    merged = list(dict.fromkeys(existing + entries))
    path.write_text("\n".join(merged) + ("\n" if merged else ""), encoding="utf-8")


def recover_quarantined(queue: Path, roots: list[Path], apply: bool) -> dict:
    """Reencaminha somente evidências que voltaram a existir sob root registrado.

    Entradas de scratchpad, paths apagados ou destinos não registrados permanecem
    na quarentena: ausência de match nunca é motivo para reativá-las.
    """
    files = sorted(queue.parent.glob("pending-writes.unroutable-*.txt"))
    recovered: dict[Path, list[str]] = {}
    retained = 0
    changed: list[str] = []
    for artifact in files:
        entries = read_lines(artifact)
        eligible = [entry for entry in entries if Path(entry).exists()]
        routed, unmatched = route(eligible, roots)
        for root, values in routed.items():
            recovered.setdefault(root, []).extend(values)
        retained_entries = [entry for entry in entries if entry not in eligible] + unmatched
        retained += len(retained_entries)
        if apply and len(retained_entries) != len(entries):
            backup = artifact.with_name(f"{artifact.stem}.recovered-{int(time.time())}.bak")
            shutil.copy2(artifact, backup)
            artifact.write_text("\n".join(dict.fromkeys(retained_entries)) + ("\n" if retained_entries else ""), encoding="utf-8")
            changed.append(str(artifact))
    if apply:
        for root, values in recovered.items():
            write_unique(root / ".axon" / "pending-writes.txt", values)
    return {
        "artifacts": len(files),
        "recovered": {str(root): len(values) for root, values in recovered.items()},
        "recovered_entries": sum(len(values) for values in recovered.values()),
        "retained_entries": retained,
        "changed_artifacts": changed,
    }


def prune_quarantined(queue: Path, retention_days: float, apply: bool) -> dict:
    """Expira apenas referências inexistentes após a janela de retenção.

    Artefatos e caminhos ainda existentes são preservados. Toda alteração deixa
    um backup datado ao lado da evidência original.
    """
    now = time.time()
    age_seconds = retention_days * 86_400
    candidates = sorted(queue.parent.glob("pending-writes.unroutable-*.txt"))
    expired = 0
    changed: list[str] = []
    for artifact in candidates:
        if now - artifact.stat().st_mtime < age_seconds:
            continue
        entries = read_lines(artifact)
        retained = [entry for entry in entries if Path(entry).exists()]
        removed = len(entries) - len(retained)
        expired += removed
        if apply and removed:
            backup = artifact.with_name(f"{artifact.stem}.pruned-{int(now)}.bak")
            shutil.copy2(artifact, backup)
            if retained:
                artifact.write_text("\n".join(dict.fromkeys(retained)) + "\n", encoding="utf-8")
            else:
                artifact.unlink()
            changed.append(str(artifact))
    return {
        "retention_days": retention_days,
        "expired_entries": expired,
        "changed_artifacts": changed,
    }


def main() -> int:
    home = Path.home()
    parser = argparse.ArgumentParser()
    parser.add_argument("--queue", type=Path, default=home / ".axon" / "pending-writes.txt")
    parser.add_argument("--registry", type=Path, default=home / ".axon" / "registry.json")
    parser.add_argument("--apply", action="store_true",
                        help="route entries, recover eligible quarantine entries, and archive the original queue")
    parser.add_argument("--quarantine-unmatched", action="store_true", help="move unmatched entries to a dated evidence file")
    parser.add_argument("--recover-quarantine", action="store_true",
                        help="recover only entries that now exist under a registered project root")
    parser.add_argument("--quarantine-retention-days", type=float, default=30,
                        help="expire nonexistent quarantine entries after this many days on --apply (default: 30)")
    args = parser.parse_args()

    queue = args.queue.resolve()
    registry = args.registry.resolve()
    if not registry.exists():
        print(json.dumps({"status": "unavailable", "reason": "registry_missing", "registry": str(registry)}))
        return 2
    if args.quarantine_retention_days < 0:
        parser.error("--quarantine-retention-days must be non-negative")
    entries = read_lines(queue)
    roots = registry_roots(registry)
    routed, unmatched = route(entries, roots)
    result = {
        "status": "ready" if not unmatched else "degraded",
        "mode": "apply" if args.apply else "dry_run",
        "queue": str(queue),
        "entries": len(entries),
        "registered_roots": len(roots),
        "routed": {str(root): len(values) for root, values in routed.items()},
        "routed_entries": sum(len(values) for values in routed.values()),
        "unmatched_entries": len(unmatched),
        "unmatched_sample": unmatched[:10],
    }
    if args.recover_quarantine or args.apply:
        result["quarantine_recovery"] = recover_quarantined(queue, roots, args.apply)
        result["quarantine_retention"] = prune_quarantined(queue, args.quarantine_retention_days, args.apply)
    if not args.apply:
        print(json.dumps(result, indent=2))
        return 0

    if not queue.exists():
        print(json.dumps(result, indent=2))
        return 0
    backup = queue.with_name(f"pending-writes.routed-{int(time.time())}.bak")
    shutil.copy2(queue, backup)
    remaining = [] if args.quarantine_unmatched else list(unmatched)
    for root, values in routed.items():
        target = root / ".axon" / "pending-writes.txt"
        if target.resolve() == queue:
            remaining.extend(values)
        else:
            write_unique(target, values)
    queue.write_text("\n".join(dict.fromkeys(remaining)) + ("\n" if remaining else ""), encoding="utf-8")
    result["backup"] = str(backup)
    if args.quarantine_unmatched and unmatched:
        quarantine = queue.with_name(f"pending-writes.unroutable-{int(time.time())}.txt")
        quarantine.write_text("\n".join(dict.fromkeys(unmatched)) + "\n", encoding="utf-8")
        result["quarantine"] = str(quarantine)
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
