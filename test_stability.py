#!/usr/bin/env python3
import subprocess
import re
import statistics
import sys
import time

def print_stats(offsets, delays):
    if not offsets:
        print("\nNo data collected.")
        return

    print("\n--- Statistics ---")
    print(f"Samples: {len(offsets)}")
    
    # Offset Stats
    print(f"Offset (s):")
    print(f"  Mean:   {statistics.mean(offsets):.6f}")
    print(f"  Median: {statistics.median(offsets):.6f}")
    print(f"  StDev:  {statistics.stdev(offsets) if len(offsets) > 1 else 0:.6f}")
    print(f"  Min/Max:{min(offsets):.6f} / {max(offsets):.6f}")
    
    # Delay/Jitter Stats
    print(f"Delay/Err (s):")
    print(f"  Mean:   {statistics.mean(delays):.6f}")
    print(f"  Max:    {max(delays):.6f}")

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <server_address>")
        sys.exit(1)

    server = sys.argv[1]
    offsets = []
    delays = []

    print(f"Polling {server} (Ctrl+C to stop)...")

    try:
        while True:
            # Run sntp. Capture stdout/stderr.
            result = subprocess.run(['sntp', server], capture_output=True, text=True)
            
            # Match format: +0.036149 +/- 0.011154
            match = re.search(r'([+-]?\d+\.\d+)\s+\+/-\s+(\d+\.\d+)', result.stdout + result.stderr)
            
            if match:
                offset = float(match.group(1))
                delay = float(match.group(2))
                offsets.append(offset)
                delays.append(delay)
                print(f"Offset: {offset:+.6f} | Delay: {delay:.6f}")
            else:
                print("x", end="", flush=True) # visual indicator for drop/parse fail
            
            time.sleep(1)

    except KeyboardInterrupt:
        print_stats(offsets, delays)
        sys.exit(0)

if __name__ == "__main__":
    main()
