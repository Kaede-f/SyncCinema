#!/usr/bin/env python3
"""Generate a repeatable SyncCinema acceptance report from Prometheus."""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable


WINDOW_PATTERN = re.compile(r"^[1-9][0-9]*[smhdw]$")


@dataclass(frozen=True)
class Objective:
    key: str
    title: str
    value_key: str
    operator: str
    threshold: float
    unit: str
    sample_key: str | None = None
    minimum_samples: int = 0


@dataclass(frozen=True)
class ObjectiveResult:
    key: str
    title: str
    status: str
    value: float | None
    threshold: float
    operator: str
    unit: str
    samples: float | None
    minimum_samples: int
    reason: str


class PrometheusClient:
    def __init__(self, base_url: str, timeout_seconds: float = 5.0) -> None:
        parsed = urllib.parse.urlparse(base_url)
        if parsed.scheme not in {"http", "https"} or not parsed.netloc:
            raise ValueError("Prometheus URL must use http or https")
        self.base_url = base_url.rstrip("/")
        self.timeout_seconds = timeout_seconds

    def check_ready(self) -> None:
        request = urllib.request.Request(
            f"{self.base_url}/-/ready",
            headers={"Accept": "text/plain"},
        )
        with urllib.request.urlopen(request, timeout=self.timeout_seconds) as response:
            if response.status != 200:
                raise RuntimeError(
                    f"Prometheus readiness returned HTTP {response.status}"
                )

    def query(self, expression: str) -> float | None:
        query_string = urllib.parse.urlencode({"query": expression})
        url = f"{self.base_url}/api/v1/query?{query_string}"
        request = urllib.request.Request(url, headers={"Accept": "application/json"})

        with urllib.request.urlopen(request, timeout=self.timeout_seconds) as response:
            payload = json.load(response)

        if payload.get("status") != "success":
            raise RuntimeError(payload.get("error", "Prometheus query failed"))

        data = payload.get("data", {})
        result_type = data.get("resultType")
        result = data.get("result")
        if result_type == "scalar":
            return _parse_number(result[1])
        if result_type == "vector":
            if not result:
                return None
            if len(result) != 1:
                raise RuntimeError("query returned more than one time series")
            return _parse_number(result[0]["value"][1])
        raise RuntimeError(f"unsupported Prometheus result type: {result_type}")


def _parse_number(raw_value: object) -> float | None:
    value = float(raw_value)
    return value if math.isfinite(value) else None


def build_queries(window: str) -> dict[str, str]:
    return {
        "availability_percent": (
            f'100 * avg_over_time(up{{job="synccinema"}}[{window}])'
        ),
        "availability_samples": (
            f'count_over_time(up{{job="synccinema"}}[{window}])'
        ),
        "join_attempts": (
            f"sum(increase(synccinema_join_results_total[{window}]))"
        ),
        "join_success_percent": (
            "100 * sum(increase(synccinema_join_results_total"
            f'{{outcome="accepted"}}[{window}])) '
            "/ clamp_min(sum(increase(synccinema_join_results_total"
            f"[{window}])), 1)"
        ),
        "first_frame_samples": (
            f"sum(increase(synccinema_time_to_first_frame_ms_count[{window}]))"
        ),
        "first_frame_p95_ms": (
            "histogram_quantile(0.95, sum by (le) "
            f"(rate(synccinema_time_to_first_frame_ms_bucket[{window}])))"
        ),
        "sync_samples": (
            f"sum(increase(synccinema_sync_pair_samples_total[{window}]))"
        ),
        "sync_good_percent": (
            "100 * sum(increase(synccinema_sync_pair_samples_total"
            f'{{quality="good"}}[{window}])) '
            "/ clamp_min(sum(increase(synccinema_sync_pair_samples_total"
            f"[{window}])), 1)"
        ),
        "pair_skew_p95_ms": (
            "histogram_quantile(0.95, sum by (le) "
            f"(rate(synccinema_pair_skew_ms_bucket[{window}])))"
        ),
        "control_ack_samples": (
            f"sum(increase(synccinema_control_ack_latency_ms_count[{window}]))"
        ),
        "control_ack_p95_ms": (
            "histogram_quantile(0.95, sum by (le) "
            f"(rate(synccinema_control_ack_latency_ms_bucket[{window}])))"
        ),
        "correction_dispatch_attempts": (
            "sum(increase(synccinema_corrections_total"
            f'{{status=~"dispatched|dispatch_failed"}}[{window}]))'
        ),
        "correction_dispatch_failure_percent": (
            "100 * sum(increase(synccinema_corrections_total"
            f'{{status="dispatch_failed"}}[{window}])) '
            "/ clamp_min(sum(increase(synccinema_corrections_total"
            f'{{status=~"dispatched|dispatch_failed"}}[{window}])), 1)'
        ),
        "correction_result_samples": (
            "sum(increase(synccinema_corrections_total"
            f'{{status=~"applied|rejected|unmatched|missing_result"}}[{window}]))'
        ),
        "correction_result_failure_percent": (
            "100 * sum(increase(synccinema_corrections_total"
            f'{{status=~"rejected|unmatched|missing_result"}}[{window}])) '
            "/ clamp_min(sum(increase(synccinema_corrections_total"
            f'{{status=~"applied|rejected|unmatched|missing_result"}}[{window}])), 1)'
        ),
        "convergence_samples": (
            f"sum(increase(synccinema_correction_convergence_ms_count[{window}]))"
        ),
        "convergence_p95_ms": (
            "histogram_quantile(0.95, sum by (le) "
            f"(rate(synccinema_correction_convergence_ms_bucket[{window}])))"
        ),
    }


OBJECTIVES = (
    Objective("availability", "Server availability", "availability_percent", ">=", 99.5, "%", "availability_samples", 20),
    Objective("join", "JOIN success ratio", "join_success_percent", ">=", 99.0, "%", "join_attempts", 20),
    Objective("first_frame", "First-frame P95", "first_frame_p95_ms", "<=", 3000.0, "ms", "first_frame_samples", 10),
    Objective("sync_good", "Sync-good ratio", "sync_good_percent", ">=", 95.0, "%", "sync_samples", 100),
    Objective("pair_skew", "Pair skew P95", "pair_skew_p95_ms", "<=", 250.0, "ms", "sync_samples", 100),
    Objective("control_ack", "Control ACK latency P95", "control_ack_p95_ms", "<=", 500.0, "ms", "control_ack_samples", 20),
    Objective("correction_dispatch", "Correction dispatch failure ratio", "correction_dispatch_failure_percent", "<=", 1.0, "%", "correction_dispatch_attempts", 5),
    Objective("correction_result", "Correction result failure ratio", "correction_result_failure_percent", "<=", 1.0, "%", "correction_result_samples", 5),
    Objective("convergence", "Correction convergence P95", "convergence_p95_ms", "<=", 2000.0, "ms", "convergence_samples", 5),
)


def evaluate_objectives(
    values: dict[str, float | None],
    objectives: tuple[Objective, ...] = OBJECTIVES,
) -> list[ObjectiveResult]:
    results: list[ObjectiveResult] = []
    comparisons: dict[str, Callable[[float, float], bool]] = {
        ">=": lambda value, threshold: value >= threshold,
        "<=": lambda value, threshold: value <= threshold,
    }

    for objective in objectives:
        value = values.get(objective.value_key)
        samples = values.get(objective.sample_key) if objective.sample_key else None

        if value is None:
            status = "NO_DATA"
            reason = "Prometheus returned no finite value"
        elif objective.sample_key and (
            samples is None or samples < objective.minimum_samples
        ):
            status = "INSUFFICIENT_DATA"
            reason = (
                f"requires {objective.minimum_samples} samples; "
                f"observed {_format_number(samples)}"
            )
        elif comparisons[objective.operator](value, objective.threshold):
            status = "PASS"
            reason = "objective satisfied"
        else:
            status = "FAIL"
            reason = "objective violated"

        results.append(
            ObjectiveResult(
                key=objective.key,
                title=objective.title,
                status=status,
                value=value,
                threshold=objective.threshold,
                operator=objective.operator,
                unit=objective.unit,
                samples=samples,
                minimum_samples=objective.minimum_samples,
                reason=reason,
            )
        )

    return results


def collect_values(
    client: PrometheusClient,
    queries: dict[str, str],
) -> tuple[dict[str, float | None], dict[str, str]]:
    values = {key: None for key in queries}
    errors: dict[str, str] = {}

    try:
        client.check_ready()
    except (OSError, ValueError, RuntimeError, urllib.error.URLError) as error:
        # Prometheus 整体不可用时不再重复等待每条查询，直接生成 NO_DATA 报告。
        errors["prometheus_ready"] = str(error)
        return values, errors

    for key, expression in queries.items():
        try:
            values[key] = client.query(expression)
        except (OSError, ValueError, RuntimeError, urllib.error.URLError) as error:
            errors[key] = str(error)
    return values, errors


def _format_number(value: float | None) -> str:
    if value is None:
        return "N/A"
    if value.is_integer():
        return str(int(value))
    return f"{value:.2f}"


def render_markdown(
    scenario: str,
    window: str,
    prometheus_url: str,
    generated_at: str,
    results: list[ObjectiveResult],
    errors: dict[str, str],
) -> str:
    lines = [
        "# SyncCinema Observability Experiment Report",
        "",
        f"- Scenario: `{scenario}`",
        f"- Window: `{window}`",
        f"- Generated at: `{generated_at}`",
        f"- Prometheus: `{prometheus_url}`",
        "",
        "## Objective Results",
        "",
        "| Objective | Status | Observed | Target | Samples |",
        "|---|---:|---:|---:|---:|",
    ]

    for result in results:
        observed = _format_number(result.value)
        if result.value is not None:
            observed += f" {result.unit}"
        target = f"{result.operator} {_format_number(result.threshold)} {result.unit}"
        samples = _format_number(result.samples)
        lines.append(
            f"| {result.title} | **{result.status}** | {observed} | "
            f"{target} | {samples} |"
        )

    counts = {status: 0 for status in ("PASS", "FAIL", "INSUFFICIENT_DATA", "NO_DATA")}
    for result in results:
        counts[result.status] += 1
    lines.extend(
        [
            "",
            "## Summary",
            "",
            f"- PASS: {counts['PASS']}",
            f"- FAIL: {counts['FAIL']}",
            f"- INSUFFICIENT_DATA: {counts['INSUFFICIENT_DATA']}",
            f"- NO_DATA: {counts['NO_DATA']}",
        ]
    )

    if errors:
        lines.extend(["", "## Query Errors", ""])
        for key, error in sorted(errors.items()):
            lines.append(f"- `{key}`: {error}")

    lines.extend(
        [
            "",
            "## Interpretation",
            "",
            "`FAIL` means the measured window violated an objective. "
            "`INSUFFICIENT_DATA` means the metric existed but the experiment "
            "did not produce enough samples for a reliable conclusion. "
            "`NO_DATA` means collection or querying must be checked before "
            "judging product quality.",
            "",
        ]
    )
    return "\n".join(lines)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prometheus-url", default="http://127.0.0.1:9090")
    parser.add_argument("--window", default="15m", help="Prometheus range, for example 15m or 1h")
    parser.add_argument("--scenario", default="manual-test")
    parser.add_argument("--output", type=Path, required=True, help="Markdown report path")
    parser.add_argument("--timeout-seconds", type=float, default=5.0)
    parser.add_argument("--fail-on-objective", action="store_true")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if not WINDOW_PATTERN.fullmatch(arguments.window):
        print("invalid --window; use a positive integer followed by s, m, h, d, or w", file=sys.stderr)
        return 2
    if arguments.timeout_seconds <= 0:
        print("--timeout-seconds must be positive", file=sys.stderr)
        return 2

    client = PrometheusClient(
        arguments.prometheus_url,
        timeout_seconds=arguments.timeout_seconds,
    )
    values, errors = collect_values(client, build_queries(arguments.window))

    results = evaluate_objectives(values)
    generated_at = datetime.now(timezone.utc).isoformat(timespec="seconds")
    markdown = render_markdown(
        arguments.scenario,
        arguments.window,
        arguments.prometheus_url,
        generated_at,
        results,
        errors,
    )

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(markdown, encoding="utf-8")
    json_output = arguments.output.with_suffix(".json")
    json_output.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "scenario": arguments.scenario,
                "window": arguments.window,
                "generated_at": generated_at,
                "prometheus_url": arguments.prometheus_url,
                "measurements": values,
                "objectives": [asdict(result) for result in results],
                "query_errors": errors,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    print(f"Markdown report: {arguments.output}")
    print(f"JSON report: {json_output}")

    if arguments.fail_on_objective and any(
        result.status != "PASS" for result in results
    ):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
