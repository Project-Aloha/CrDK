#!/usr/bin/env python3
"""Compatibility entry point for the renamed ICB host test."""

import os
import sys
from pathlib import Path


target = Path(__file__).with_name("run_icb_tests.py")
os.execv(sys.executable, [sys.executable, str(target), *sys.argv[1:]])
