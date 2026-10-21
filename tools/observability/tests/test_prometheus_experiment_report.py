from __future__ import annotations

import runpy
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "prometheus_experiment_report.py"
REPORT_MODULE = runpy.run_path(str(MODULE_PATH))
evaluate_objectives = REPORT_MODULE["evaluate_objectives"]
render_markdown = REPORT_MODULE["render_markdown"]
build_queries = REPORT_MODULE["build_queries"]
collect_values = REPORT_MODULE["collect_values"]


class PrometheusExperimentReportTests(unittest.TestCase):
    def test_all_objectives_pass_with_sufficient_samples(self) -> None:
        values = {
            "availability_percent": 100.0,
            "availability_samples": 180.0,
            "join_attempts": 30.0,
            "join_success_percent": 100.0,
            "first_frame_samples": 12.0,
            "first_frame_p95_ms": 1800.0,
            "sync_samples": 150.0,
            "sync_good_percent": 98.0,
            "pair_skew_p95_ms": 180.0,
            "control_ack_samples": 24.0,
            "control_ack_p95_ms": 120.0,
            "correction_dispatch_attempts": 6.0,
            "correction_dispatch_failure_percent": 0.0,
            "correction_result_samples": 6.0,
            "correction_result_failure_percent": 0.0,
            "convergence_samples": 6.0,
            "convergence_p95_ms": 1400.0,
        }

        results = evaluate_objectives(values)

        self.assertEqual({result.status for result in results}, {"PASS"})

    def test_low_sample_volume_is_not_reported_as_pass(self) -> None:
        values = {
            "availability_percent": 100.0,
            "availability_samples": 180.0,
            "join_attempts": 1.0,
            "join_success_percent": 100.0,
        }

        results = {result.key: result for result in evaluate_objectives(values)}

        self.assertEqual(results["availability"].status, "PASS")
        self.assertEqual(results["join"].status, "INSUFFICIENT_DATA")
        self.assertEqual(results["first_frame"].status, "NO_DATA")

    def test_threshold_violation_is_reported_as_fail(self) -> None:
        values = {
            "availability_percent": 98.0,
            "availability_samples": 180.0,
            "sync_samples": 120.0,
            "sync_good_percent": 90.0,
            "pair_skew_p95_ms": 420.0,
        }

        results = {result.key: result for result in evaluate_objectives(values)}

        self.assertEqual(results["availability"].status, "FAIL")
        self.assertEqual(results["sync_good"].status, "FAIL")
        self.assertEqual(results["pair_skew"].status, "FAIL")

    def test_markdown_contains_status_summary(self) -> None:
        values = {
            "availability_percent": 100.0,
            "availability_samples": 180.0,
        }
        results = evaluate_objectives(values)

        markdown = render_markdown(
            "baseline",
            "15m",
            "http://127.0.0.1:9090",
            "2026-10-21T00:00:00+00:00",
            results,
            {},
        )

        self.assertIn("# SyncCinema Observability Experiment Report", markdown)
        self.assertIn("| Server availability | **PASS**", markdown)
        self.assertIn("- PASS: 1", markdown)
        self.assertIn("- NO_DATA: 8", markdown)

    def test_queries_use_requested_window(self) -> None:
        queries = build_queries("30m")

        self.assertTrue(queries)
        self.assertTrue(all("[30m]" in query for query in queries.values()))

    def test_unavailable_prometheus_fails_fast_before_metric_queries(self) -> None:
        class UnavailableClient:
            query_calls = 0

            def check_ready(self) -> None:
                raise OSError("collector unavailable")

            def query(self, _expression: str) -> float | None:
                self.query_calls += 1
                return 1.0

        client = UnavailableClient()
        values, errors = collect_values(client, {"a": "up", "b": "vector(1)"})

        self.assertEqual(client.query_calls, 0)
        self.assertEqual(values, {"a": None, "b": None})
        self.assertIn("prometheus_ready", errors)


if __name__ == "__main__":
    unittest.main()
