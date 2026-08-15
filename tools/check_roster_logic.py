"""Validate Roster's ordering and lookup algorithms.

Roster keeps records sorted by hash and finds them with a binary search. If
insertion ever puts a record out of order, the search silently misses it and a
valid card is DENIED at the door -- a failure that looks like a hardware or
reader fault and would be miserable to trace. Cheaper to prove the algorithm
here than to debug it on a ladder.

Mirrors Roster::add / Roster::remove / Roster::indexOfHash / replaceAll's
insertion sort exactly. Pure algorithm: no ESP32, no filesystem.
"""
import random

MAX_ENTRIES = 512


def index_of_hash(entries, h):
    """Mirror of Roster::indexOfHash - binary search over hash-sorted records."""
    lo, hi = 0, len(entries)
    while lo < hi:
        mid = lo + (hi - lo) // 2
        if entries[mid][0] == h:
            return mid
        if entries[mid][0] < h:
            lo = mid + 1
        else:
            hi = mid
    return -1


def add(entries, h, name):
    """Mirror of Roster::add - reject duplicates/overflow, insert in hash order."""
    if len(entries) >= MAX_ENTRIES or index_of_hash(entries, h) >= 0:
        return False
    pos = 0
    while pos < len(entries) and entries[pos][0] < h:
        pos += 1
    entries.insert(pos, (h, name))
    return True


def remove(entries, h):
    """Mirror of Roster::remove."""
    i = index_of_hash(entries, h)
    if i < 0:
        return False
    entries.pop(i)
    return True


def replace_all_sort(staged):
    """Mirror of replaceAll's insertion sort."""
    s = list(staged)
    for i in range(1, len(s)):
        key = s[i]
        j = i
        while j > 0 and s[j - 1][0] > key[0]:
            s[j] = s[j - 1]
            j -= 1
        s[j] = key
    return s


def sorted_ok(entries):
    return all(entries[i][0] <= entries[i + 1][0] for i in range(len(entries) - 1))


random.seed(20260814)
fails = 0

# 1. Random insert order -> every key findable, array stays sorted.
print("=== insert / find round-trip ===")
for trial in range(200):
    keys = random.sample(range(1, 2**63), random.randint(1, 60))
    entries = []
    for k in keys:
        add(entries, k, "n%d" % k)
    if not sorted_ok(entries):
        print("  FAIL trial %d: array not sorted after inserts" % trial); fails += 1; break
    missing = [k for k in keys if index_of_hash(entries, k) < 0]
    if missing:
        print("  FAIL trial %d: %d inserted key(s) not findable" % (trial, len(missing))); fails += 1; break
else:
    print("  200 trials, up to 60 random keys each: all findable, order held  PASS")

# 2. Interleaved add/remove keeps the invariant.
print("=== interleaved add / remove ===")
entries, live = [], set()
for _ in range(4000):
    if live and random.random() < 0.4:
        k = random.choice(sorted(live))
        remove(entries, k); live.discard(k)
    else:
        k = random.randrange(1, 2**63)
        if add(entries, k, "x"): live.add(k)
    if not sorted_ok(entries):
        print("  FAIL: order broken"); fails += 1; break
else:
    bad = [k for k in live if index_of_hash(entries, k) < 0]
    ghost = [h for h, _ in entries if h not in live]
    if bad or ghost:
        print("  FAIL: %d missing, %d ghost entries" % (len(bad), len(ghost))); fails += 1
    else:
        print("  4000 ops, %d live entries: no misses, no ghosts  PASS" % len(entries))

# 3. Duplicate rejection - a re-enrolled card must not create a second record,
#    which would make remove() leave one behind and the card keep working.
print("=== duplicate rejection ===")
entries = []
add(entries, 42, "first")
dup = add(entries, 42, "second")
print("  second add of same hash rejected:", "PASS" if not dup and len(entries) == 1 else "FAIL")
if dup or len(entries) != 1:
    fails += 1

# 4. replaceAll's insertion sort (the cloud-sync path).
print("=== replaceAll insertion sort ===")
ok = True
for _ in range(200):
    staged = [(random.randrange(1, 2**63), "n") for _ in range(random.randint(0, 80))]
    s = replace_all_sort(staged)
    if not sorted_ok(s) or len(s) != len(staged):
        ok = False; break
    for h, _ in staged:
        if index_of_hash(s, h) < 0:
            ok = False; break
print("  200 random rosters sorted and searchable:", "PASS" if ok else "FAIL")
if not ok:
    fails += 1

# 5. Boundary: full roster rejects further adds without corrupting itself.
print("=== capacity ceiling ===")
entries = []
for i in range(MAX_ENTRIES):
    add(entries, i + 1, "n")
over = add(entries, 10**18, "overflow")
print("  at MAX_ENTRIES: add rejected =", not over, "| size =", len(entries),
      "| sorted =", sorted_ok(entries),
      "->", "PASS" if (not over and len(entries) == MAX_ENTRIES and sorted_ok(entries)) else "FAIL")
if over or len(entries) != MAX_ENTRIES:
    fails += 1

print()
print("RESULT:", "ALL PASS" if fails == 0 else "%d FAILURE(S)" % fails)
