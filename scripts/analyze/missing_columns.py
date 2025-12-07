import re
import csv

LOG_FILE = "liquidity_timeseries_log.txt"

pattern = re.compile(r"NEW SLICE 100ms: \[(\d+)-(\d+)\]")

timestamps = []

with open(LOG_FILE, "r") as f:
    for line in f:
        m = pattern.search(line)
        if m:
            start = int(m.group(1))
            end = int(m.group(2))
            timestamps.append((start, end))

# Sort by start time
timestamps.sort(key=lambda x: x[0])

# Extract only the END timestamps since those represent the column boundaries
ends = [end for _, end in timestamps]

print(f"Found {len(ends)} total 100ms slices")
print("---- Checking for gaps ----")

missing = []
duplicates = []

for i in range(len(ends) - 1):
    cur = ends[i]
    nxt = ends[i + 1]

    diff = nxt - cur

    if diff == 0:
        duplicates.append(cur)
    elif diff != 100:
        # record all missing slices
        needed = list(range(cur + 100, nxt, 100))
        missing.extend(needed)

if duplicates:
    print("\nDUPLICATE TIMESTAMPS (BAD):")
    for d in duplicates[:20]:
        print(d)
    if len(duplicates) > 20:
        print(f"...and {len(duplicates)-20} more")

if missing:
    print("\nMISSING 100ms SLICES:")
    for m in missing[:50]:
        print(m)
    if len(missing) > 50:
        print(f"...and {len(missing)-50} more")

csv_filename = "slices_report.csv"

# Write only counts to CSV: columns ["duplicates", "missing intervals"]
with open(csv_filename, "w", newline="") as csvfile:
    writer = csv.writer(csvfile)
    writer.writerow(["duplicates", "missing intervals"])
    writer.writerow([len(duplicates), len(missing)])

print(f"\nReport written to {csv_filename}")

print("\nSUMMARY")
print("-------")
print(f"Total duplicates: {len(duplicates)}")
print(f"Total missing intervals: {len(missing)}")
