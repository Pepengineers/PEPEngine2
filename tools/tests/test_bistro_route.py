import runpy
import unittest
from pathlib import Path


route = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'validate-bistro-route.py'))


def points(xz):
    return route['np'].array([[x, 0, z] for x, z in xz], dtype=float)


class BistroRouteTests(unittest.TestCase):
    def test_only_major_objects_block_the_route(self):
        for name in ('Paris_SidewalkBarrier_01A', 'Paris_StreetPivot_01', 'FireHydrant',
                     'Paris_Bollard_01A', 'Paris_Streetlight_01a', 'Paris_Table_01', 'Paris_Chair_01'):
            with self.subTest(name=name):
                self.assertFalse(route['is_route_obstacle'](name))
        for name in ('paris_building_02_3', 'bux_hedge_box_long_low',
                     'Linde_Tree_Large_linde_tree_large', 'Paris_Street_38'):
            with self.subTest(name=name):
                self.assertTrue(route['is_route_obstacle'](name))

    def test_simple_loop_has_separate_branches(self):
        spacing = route['check_loop_shape'](points([(0, 0), (3000, 0), (3000, 2000), (0, 2000), (0, 0)]))
        self.assertGreaterEqual(spacing, 150)

    def test_self_intersection_is_rejected(self):
        with self.assertRaisesRegex(AssertionError, 'self-intersections'):
            route['check_loop_shape'](points([(0, 0), (3000, 3000), (0, 3000), (3000, 0), (0, 0)]))

    def test_shared_segment_is_rejected(self):
        with self.assertRaisesRegex(AssertionError, 'shared segments'):
            route['check_loop_shape'](points([(0, 0), (3000, 0), (3000, 2000), (1500, 2000), (1500, 0), (0, 0)]))

    def test_narrow_common_passage_is_rejected(self):
        with self.assertRaisesRegex(AssertionError, 'narrow passage'):
            route['check_loop_shape'](points([(0, 0), (3000, 0), (3000, 100), (0, 100), (0, 0)]))


if __name__ == '__main__':
    unittest.main()
