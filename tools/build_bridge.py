#!/usr/bin/env python3
"""Build only durable repo-owned helper source, with explicit external SDK roots."""
import subprocess, sys
from pathlib import Path
root=Path(__file__).resolve().parents[1]; bridge=root/'native/bridge'
entry=bridge/'native/build_helper.py'
if not entry.is_file() or not (bridge/'source-provenance.json').is_file():
    raise SystemExit('Bridge source is not frozen/consolidated yet. Stage a reviewed exact source plan first; no mutable research-source fallback is allowed.')
raise SystemExit(subprocess.run([sys.executable,str(entry),'--native-model-root',str(bridge/'legacy'),*sys.argv[1:]]).returncode)
