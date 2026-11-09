import importlib.util
import unittest
from pathlib import Path
spec = importlib.util.spec_from_file_location("measure_sync", Path(__file__).resolve().parents[1] / "tools" / "measure-sync.py")
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)

class SyncTests(unittest.TestCase):
    def test_good_sync(self):
        self.assertTrue(sync.assess_sync([1,2,3,4], [1.02,2.02,3.02,4.02], 5)["pass_40ms"])
    def test_partial_severe_offset(self):
        result = sync.assess_sync([1,2,3,4], [1.01,2.01,3.4,4.4], 5)
        self.assertFalse(result["pass_40ms"])
        self.assertEqual(result["unmatched_video"], 2)
    def test_missing_audio(self):
        self.assertFalse(sync.assess_sync([1,2,3,4], [1,2,3], 5)["pass_40ms"])
    def test_insufficient_evidence(self):
        self.assertFalse(sync.assess_sync([1], [1], 5)["pass_40ms"])
    def test_pairing_is_one_to_one(self):
        self.assertFalse(sync.assess_sync([1,1.01,2,3], [1,2,3], 5)["pass_40ms"])
    def test_whole_second_shift_nonperiodic_source(self):
        flashes = [1.15,2.4,3.2,4.45,5.15,6.3,7.2]
        self.assertFalse(sync.assess_sync(flashes, [t+1 for t in flashes], 9)["pass_40ms"])
    def test_common_outage(self):
        self.assertTrue(sync.assess_sync([1,2,6,7], [1,2,6,7], 8)["pass_40ms"])

if __name__ == "__main__":
    unittest.main()
