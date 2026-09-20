#!/usr/bin/env python3
"""Run the enabled persistent RPC lifecycle probe."""
from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import tempfile
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from persistent_subagent_rpc_lifecycle_probe import main as lifecycle_main


def main() -> int:
    return lifecycle_main()


if __name__ == "__main__":
    raise SystemExit(main())
