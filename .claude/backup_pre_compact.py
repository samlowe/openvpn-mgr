#!/usr/bin/env python3
import json
import shutil
import sys
from datetime import datetime
from pathlib import Path

input_data = json.load(sys.stdin)
transcript_path = input_data.get("transcript_path", "")
trigger = input_data.get("tool_input", {}).get(
    "trigger", input_data.get("trigger", "unknown")
)

if transcript_path and Path(transcript_path).exists():
    backup_dir = Path(".claude/backups")
    backup_dir.mkdir(parents=True, exist_ok=True)

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    backup_name = f"transcript_{trigger}_{timestamp}.jsonl"
    shutil.copy2(transcript_path, backup_dir / backup_name)

    # Keep only last 10 backups
    backups = sorted(backup_dir.glob("transcript_*.jsonl"))
    for old_backup in backups[:-10]:
        old_backup.unlink()

sys.exit(0)
