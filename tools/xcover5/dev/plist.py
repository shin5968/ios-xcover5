#!/usr/bin/env python3
"""Print a (binary or XML) plist as JSON.   plist.py FILE"""
import json, plistlib, sys

with open(sys.argv[1], 'rb') as f:
    print(json.dumps(plistlib.load(f), indent=1, default=str))
