"""CPU model of hybrid water bitonic sorting, NOT actual GPU validation.

Run: python tools/tests/WaterSortScheduleTests.py
"""

import random
import unittest


SENTINEL = (0xFFFFFFFF, 0xFFFFFFFF)


def schedule(capacity, tile):
    """Yield dispatches as (local, [(k, j), ...]) compare stages."""
    assert capacity > 0 and capacity & (capacity - 1) == 0
    assert tile > 0 and tile & (tile - 1) == 0
    prefix = []
    k = 2
    while k <= min(capacity, tile):
        j = k // 2
        while j:
            prefix.append((k, j))
            j //= 2
        k *= 2
    yield True, prefix
    while k <= capacity:
        j = k // 2
        while j >= tile:
            yield False, [(k, j)]
            j //= 2
        tail = []
        while j:
            tail.append((k, j))
            j //= 2
        yield True, tail
        k *= 2


def hybrid_sort(records, tile):
    result = records[:]
    capacity = len(result)
    for local, stages in schedule(capacity, tile):
        if not local:
            for k, j in stages:
                for i in range(capacity):
                    other = i ^ j
                    if other <= i:
                        continue
                    if (result[i] > result[other]) == ((i & k) == 0):
                        result[i], result[other] = result[other], result[i]
            continue
        for start in range(0, capacity, tile):
            # Model all B slots and B/2 lanes even when Capacity < B.
            shared = [SENTINEL] * tile
            for lane in range(tile // 2):
                for slot in (lane, lane + tile // 2):
                    if start + slot < capacity:
                        shared[slot] = result[start + slot]
            for k, j in stages:
                for lane in range(tile // 2):
                    left, right = pair_indices(lane, j)
                    if (shared[left] > shared[right]) == (((start + left) & k) == 0):
                        shared[left], shared[right] = shared[right], shared[left]
            for lane in range(tile // 2):
                for slot in (lane, lane + tile // 2):
                    if start + slot < capacity:
                        result[start + slot] = shared[slot]
    return result


def pair_indices(lane, j):
    left = (lane & (j - 1)) | ((lane & ~(j - 1)) << 1)
    return left, left | j


class WaterSortScheduleTests(unittest.TestCase):
    def test_pair_ownership(self):
        for tile in (256, 512, 1024, 2048):
            j = tile // 2
            while j:
                owned = []
                for lane in range(tile // 2):
                    left, right = pair_indices(lane, j)
                    self.assertEqual(left ^ j, right)
                    self.assertLess(left, right)
                    owned.extend((left, right))
                self.assertEqual(sorted(owned), list(range(tile)))
                j //= 2

    def check_keys(self, keys, tile):
        count = len(keys)
        capacity = 1 << (count - 1).bit_length()
        records = [(key, i) for i, key in enumerate(keys)]
        records += [SENTINEL] * (capacity - count)
        result = hybrid_sort(records, tile)
        self.assertEqual(result, sorted(records))
        offsets = [count] * capacity
        for i in range(count):
            if i == 0 or result[i - 1][0] != result[i][0]:
                offsets[result[i][0]] = i
        histogram = [0] * capacity
        for key in keys:
            histogram[key] += 1
        cursor = 0
        for key, size in enumerate(histogram):
            self.assertEqual(offsets[key], cursor if size else count)
            cursor += size

    def test_boundaries_and_distributions(self):
        rng = random.Random(98173)
        for tile in (32, 128, 256, 512, 1024, 2048):
            counts = sorted({1, 2, 3, 31, 32, 33, 127, 128, 129,
                             tile - 1, tile, tile + 1, 2 * tile - 1,
                             2 * tile, 2 * tile + 1})
            for count in counts:
                capacity = 1 << (count - 1).bit_length()
                distributions = (
                    [rng.randrange(capacity) for _ in range(count)],
                    [0] * count,
                    [capacity - 1] * count,
                    list(range(count)),
                    list(reversed(range(count))),
                )
                for mode, keys in enumerate(distributions):
                    with self.subTest(tile=tile, count=count, mode=mode):
                        self.check_keys(keys, tile)

    def test_representative_particle_count(self):
        rng = random.Random(51975)
        keys = [rng.randrange(1024) for _ in range(51975)]
        for tile in (256, 512, 1024, 2048):
            with self.subTest(tile=tile):
                self.check_keys(keys, tile)

    def test_dispatch_counts(self):
        # Sorting only: Hash/BuildOffsets/density/integration are excluded.
        for exponent, tile, expected in ((16, 512, 36), (16, 2048, 21),
                                         (20, 512, 78), (20, 2048, 55)):
            with self.subTest(exponent=exponent, tile=tile):
                remainder = max(0, exponent - (tile.bit_length() - 1))
                self.assertEqual(1 + remainder * (remainder + 3) // 2, expected)
                self.assertEqual(len(list(schedule(1 << exponent, tile))), expected)
        self.assertEqual(16 * 17 // 2, 136)
        self.assertEqual(20 * 21 // 2, 210)


if __name__ == "__main__":
    unittest.main(verbosity=2)
