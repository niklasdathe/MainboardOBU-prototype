#!/usr/bin/env python3
"""Validate one BicycleOBU hardware YAML profile for exclusive pin collisions."""
from __future__ import annotations
import argparse
from collections import defaultdict
from pathlib import Path
import sys
import yaml

def flatten_pin_map(prefix, value, out):
    if isinstance(value, dict):
        for key, child in value.items():
            flatten_pin_map(f"{prefix}.{key}" if prefix else key, child, out)
    elif isinstance(value, str) and value.startswith("D") and value[1:].isdigit():
        out.append((value, prefix))

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("profile", type=Path)
    args = ap.parse_args()
    data = yaml.safe_load(args.profile.read_text())
    shared_bus_pins = set()
    for bus_name, bus in (data.get("buses") or {}).items():
        if bus.get("shared"):
            pins=[]
            flatten_pin_map(f"bus:{bus_name}", bus.get("pins", {}), pins)
            shared_bus_pins.update(pin for pin, _ in pins)
    uses=defaultdict(list)
    for bus_name, bus in (data.get("buses") or {}).items():
        pins=[]; flatten_pin_map(f"bus:{bus_name}", bus.get("pins", {}), pins)
        for pin, owner in pins: uses[pin].append(owner)
    for dev_name, dev in (data.get("devices") or {}).items():
        pins=[]; flatten_pin_map(f"device:{dev_name}", dev.get("pins", {}), pins)
        for pin, owner in pins: uses[pin].append(owner)
    errors=[]
    for pin, owners in sorted(uses.items()):
        explicit=[o for o in owners if o.startswith("device:")]
        if len(explicit) > 1:
            errors.append(f"{pin}: exclusive device signals collide: {', '.join(explicit)}")
        elif len(owners) > 1 and pin not in shared_bus_pins:
            errors.append(f"{pin}: non-shared pin collision: {', '.join(owners)}")
    print(f"profile: {data.get('name', args.profile.stem)}")
    for pin, owners in sorted(uses.items()):
        print(f"  {pin:>3}: {', '.join(owners)}")
    for name, item in (data.get("reserved_conflicts") or {}).items():
        print(f"  reserved conflict {name}: {item.get('reason','')}")
    if errors:
        print("ERRORS:", file=sys.stderr)
        for error in errors: print(f"  - {error}", file=sys.stderr)
        return 2
    print("pin plan: OK")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
