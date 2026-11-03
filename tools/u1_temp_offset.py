#!/usr/bin/env python3
"""Live per-head nozzle temperature offsets for the Snapmaker U1 (Klipper + Moonraker).

During a multi-head print the G-code re-sets each head's target at every tool change, so a
temperature changed on the screen only lasts until that head is used again. This watcher polls
the heater targets over the Moonraker HTTP API and, whenever the print sets a new printing target
for a head, re-applies your offset (or absolute temperature) with SET_HEATER_TEMPERATURE.

Examples:
    tools/u1_temp_offset.py --offset 3=+10              # head 3 prints 10 C hotter
    tools/u1_temp_offset.py --offset 1=-5 --set 4=235   # head 1 5 C cooler, head 4 always 235 C
    tools/u1_temp_offset.py --offset 2=+8 --dry-run     # show what would be sent

Heads are numbered 1-4 as on the printer (Klipper heaters extruder, extruder1..extruder3).
Targets below --min-active (idle/standby, e.g. 70 C) and 0 (off) are left alone unless
--include-idle is given. Stop with Ctrl-C; the print continues with whatever is set at that moment.

Limitations: polling only sees targets, not who set them, so with --offset a print target that
happens to equal the value this watcher last applied (e.g. print 220 -> watcher 230, later the
print itself asks for 230) is taken as already adjusted and not offset again. --set is unaffected.
Between the print's command and the correction the heater may briefly move toward the print's
value. Rewriting the temperatures in the G-code before upload avoids both.
"""

import argparse
import http.client
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HEATERS = {1: "extruder", 2: "extruder1", 3: "extruder2", 4: "extruder3"}


def parse_pairs(values, kind):
    out = {}
    for v in values or []:
        try:
            head, amount = v.split("=", 1)
            head = int(head)
            amount = float(amount)
        except ValueError:
            sys.exit(f"bad --{kind} value {v!r}; expected HEAD=NUMBER, e.g. 3=+10")
        if head not in HEATERS:
            sys.exit(f"bad head {head} in --{kind}; heads are 1-4")
        out[head] = amount
    return out


class Moonraker:
    def __init__(self, host, port, timeout=5.0):
        self.base = f"http://{host}:{port}"
        self.timeout = timeout

    def query_targets(self):
        objs = "&".join(f"{name}=target,temperature" for name in HEATERS.values())
        url = f"{self.base}/printer/objects/query?print_stats=state&{objs}"
        with urllib.request.urlopen(url, timeout=self.timeout) as r:
            status = json.load(r)["result"]["status"]
        state = status.get("print_stats", {}).get("state", "unknown")
        targets = {h: float(status[name]["target"]) for h, name in HEATERS.items() if name in status}
        temps = {h: float(status[name]["temperature"]) for h, name in HEATERS.items() if name in status}
        return state, targets, temps

    def gcode(self, script):
        data = json.dumps({"script": script}).encode()
        req = urllib.request.Request(f"{self.base}/printer/gcode/script", data=data,
                                     headers={"Content-Type": "application/json"}, method="POST")
        # Klipper runs API G-code between print commands, so this can wait a few seconds.
        with urllib.request.urlopen(req, timeout=30.0) as r:
            return json.load(r).get("result")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.68.81", help="printer IP (default %(default)s)")
    ap.add_argument("--port", type=int, default=7125, help="Moonraker port (default %(default)s)")
    ap.add_argument("--offset", action="append", metavar="HEAD=DELTA", help="add DELTA C to HEAD's printing target")
    ap.add_argument("--set", action="append", metavar="HEAD=TEMP", help="use TEMP C as HEAD's printing target")
    ap.add_argument("--min-active", type=float, default=150.0,
                    help="targets below this are idle/standby and left alone (default %(default)s)")
    ap.add_argument("--include-idle", action="store_true", help="also offset idle/standby targets (never 0/off)")
    ap.add_argument("--max-temp", type=float, default=300.0, help="never set above this (default %(default)s)")
    ap.add_argument("--interval", type=float, default=0.25, help="poll interval seconds (default %(default)s)")
    ap.add_argument("--exit-when-done", action="store_true", help="exit when the print is no longer printing/paused")
    ap.add_argument("--dry-run", action="store_true", help="log what would be sent without sending")
    args = ap.parse_args()

    offsets = parse_pairs(args.offset, "offset")
    absolutes = parse_pairs(args.set, "set")
    if not offsets and not absolutes:
        sys.exit("nothing to do: give at least one --offset or --set")
    overlap = set(offsets) & set(absolutes)
    if overlap:
        sys.exit(f"head(s) {sorted(overlap)} have both --offset and --set; pick one")

    mr = Moonraker(args.host, args.port)
    base = {}        # last target set by the print (not by us) per head
    ours = {}        # last target this script set per head
    last_sent = {}   # time of last send per head (rate limit)
    dry_logged = {}  # --dry-run: last print target reported per head (nothing is sent, so it never changes)

    def desired_for(head, target):
        if target <= 0:
            return None
        if target < args.min_active and not args.include_idle:
            return None
        if head in absolutes:
            value = absolutes[head] if target >= args.min_active else target + offsets.get(head, 0.0)
        elif head in offsets:
            value = target + offsets[head]
        else:
            return None
        return max(0.0, min(args.max_temp, round(value)))

    rules = ", ".join([f"head {h} {d:+g} C" for h, d in sorted(offsets.items())] +
                      [f"head {h} = {t:g} C" for h, t in sorted(absolutes.items())])
    print(f"[u1-temp] watching {mr.base}: {rules}{' (dry run)' if args.dry_run else ''}", flush=True)

    while True:
        try:
            state, targets, temps = mr.query_targets()
        except (urllib.error.URLError, http.client.HTTPException, TimeoutError, OSError, KeyError, ValueError) as e:
            print(f"[u1-temp] query failed: {e}; retrying", flush=True)
            time.sleep(max(args.interval, 2.0))
            continue

        if args.exit_when_done and state not in ("printing", "paused"):
            print(f"[u1-temp] print state is {state!r}; exiting", flush=True)
            return

        now = time.monotonic()
        for head, target in targets.items():
            if head in ours and abs(target - ours[head]) < 0.5:
                continue  # still our value
            # Anything else was set by the print (possibly the same value again after a tool change).
            base[head] = target
            want = desired_for(head, target)
            if want is None or abs(want - target) < 0.5:
                ours.pop(head, None)
                continue
            if now - last_sent.get(head, -1e9) < 0.5:
                continue  # sent very recently; give the printer a moment to apply it
            if args.dry_run:
                if dry_logged.get(head) == target:
                    continue
                dry_logged[head] = target
            cmd = f"SET_HEATER_TEMPERATURE HEATER={HEATERS[head]} TARGET={want:g}"
            print(f"[u1-temp] head {head}: print set {target:g} C -> {want:g} C "
                  f"(now {temps.get(head, 0):.0f} C){' [dry run]' if args.dry_run else ''}", flush=True)
            last_sent[head] = now
            if not args.dry_run:
                # Record first: if the reply times out after the printer applied it, the new target
                # must still be recognised as ours, or an --offset would be applied on top of itself.
                ours[head] = want
                try:
                    mr.gcode(cmd)
                except (urllib.error.URLError, http.client.HTTPException, TimeoutError, OSError) as e:
                    print(f"[u1-temp] send failed: {e}", flush=True)
        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[u1-temp] stopped", flush=True)
