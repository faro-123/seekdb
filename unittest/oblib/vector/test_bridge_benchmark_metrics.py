import unittest
import tempfile
import time
from pathlib import Path
from benchmark_cuvs_bridge import recall_at_k, summarize_vram, VramSampler
from prepare_cuvs_gpu_diagnostic import single_gpu_bus


class RecallTests(unittest.TestCase):
    def test_exact_and_partial_recall(self):
        self.assertEqual(1.0, recall_at_k([[2, 1], [4, 3]], [[1, 2, 0], [3, 4, 5]], 6, 2))
        self.assertEqual(0.75, recall_at_k([[0, 1], [4, 3]], [[1, 2, 0], [3, 4, 5]], 6, 2))

    def test_duplicate_or_missing_ids_are_not_counted_as_success(self):
        for result in ([[1, 1]], [[1]], [[]]):
            with self.assertRaises(ValueError):
                recall_at_k(result, [[1, 2]], 5, 2)


class VramTests(unittest.TestCase):
    def test_constant_counter_is_inconclusive_instead_of_one_times(self):
        constant = [{"vram_peak_sampled_bytes": 624242688, "vram_min_sampled_bytes": 624242688}]
        active = [{"vram_peak_sampled_bytes": 900000000, "vram_min_sampled_bytes": 28028928}]
        for runs in ({"baseline": constant, "candidate": constant},
                     {"baseline": active, "candidate": constant},
                     {"baseline": constant, "candidate": active}):
            activity, ratio = summarize_vram(runs)
            self.assertIsNone(ratio)
            self.assertFalse(all(activity.values()))

    def test_active_counters_use_maximum_sampled_peaks(self):
        activity, ratio = summarize_vram({
            "baseline": [{"vram_peak_sampled_bytes": 100, "vram_min_sampled_bytes": 1},
                         {"vram_peak_sampled_bytes": 200, "vram_min_sampled_bytes": 1}],
            "candidate": [{"vram_peak_sampled_bytes": 300, "vram_min_sampled_bytes": 1}],
        })
        self.assertTrue(all(activity.values()))
        self.assertEqual(1.5, ratio)

    def test_background_sampler_observes_higher_usage(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "vram"
            path.write_text("1")
            sampler = VramSampler(path)
            path.write_text("4096")
            with sampler:
                deadline = time.monotonic() + 2
                while sampler.samples < 2 and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertGreaterEqual(sampler.samples, 2)
            self.assertEqual(1, sampler.initial)
            self.assertEqual(4096, sampler.peak)
            self.assertEqual(1, sampler.minimum)
            self.assertFalse(sampler.thread.is_alive())

    def test_records_initial_usage_and_stops_thread(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "vram"
            path.write_text("123456\n")
            with VramSampler(path) as sampler:
                self.assertEqual(123456, sampler.initial)
                self.assertEqual(123456, sampler.peak)
                self.assertEqual(123456, sampler.minimum)
            self.assertFalse(sampler.thread.is_alive())

    def test_invalid_initial_counter_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "vram"
            path.write_text("unavailable")
            with self.assertRaises(ValueError):
                VramSampler(path)

    def test_sampling_error_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "vram"
            path.write_text("1")
            sampler = VramSampler(path)
            sampler.error = OSError("counter disappeared")
            with self.assertRaises(RuntimeError):
                with sampler:
                    pass
            self.assertFalse(sampler.thread.is_alive())

    def test_out_of_range_and_query_count(self):
        for result in ([[1, 5]], [[-1, 2]], [], [[1, 2], [1, 2]]):
            with self.assertRaises(ValueError):
                recall_at_k(result, [[1, 2]], 5, 2)


class GpuSelectionTests(unittest.TestCase):
    def test_uses_the_current_bus_instead_of_a_previous_instance_address(self):
        for bus in ("0000:E3:00.0", "0000:43:00.0"):
            self.assertEqual(bus.lower(), single_gpu_bus(f"GPU[0]          : PCI Bus: {bus}\n"))

    def test_missing_or_multiple_devices_are_not_guessed(self):
        for output in ("", "GPU[1] : PCI Bus: 0000:43:00.0",
                       "GPU[0] : PCI Bus: 0000:43:00.0\nGPU[1] : PCI Bus: 0000:e3:00.0"):
            with self.assertRaises(RuntimeError):
                single_gpu_bus(output)


if __name__ == "__main__":
    unittest.main()
