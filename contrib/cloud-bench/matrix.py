#!/usr/bin/env python3

import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid


LAYOUTS = ("group",)
FAILPOINTS = (
    "before-artifact-build",
    "before-artifact-upload",
    "after-artifact-upload",
    "before-cas",
    "after-cas",
)


def run(command, *, cwd=None, env=None, check=True, timeout=300):
    return subprocess.run(
        command,
        cwd=cwd,
        env=env,
        check=check,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
    )


def git(repo, *args, env=None, check=True, timeout=300):
    command = ["git"]
    if repo is not None:
        command.extend(("-C", str(repo)))
    command.extend(args)
    return run(command, env=env, check=check, timeout=timeout)


def classify(stderr):
    text = stderr.lower()
    if "manifest cas retry limit" in text:
        return "manifest-cas-retry-limit"
    if "unable to migrate objects" in text:
        return "odb-commit-failed"
    if "remote end hung up" in text or "empty reply" in text:
        return "backend-crash"
    if "failed to connect" in text:
        return "transport-connect"
    if "timed out" in text:
        return "timeout"
    if "rejected" in text:
        return "ref-rejected"
    return "none" if not stderr.strip() else "other"


def percentile(values, percent):
    if not values:
        return None
    ordered = sorted(values)
    rank = max(0, math.ceil(percent * len(ordered)) - 1)
    return round(ordered[rank], 3)


def metric_sum(records, name):
    return sum(record.get(name, 0) for record in records)


class MetricsCursor:
    def __init__(self, path):
        self.path = pathlib.Path(path)

    def mark(self):
        try:
            return self.path.stat().st_size
        except FileNotFoundError:
            return 0

    def read(self, offset):
        try:
            with self.path.open("rb") as stream:
                stream.seek(offset)
                raw = stream.read()
        except FileNotFoundError:
            return []
        records = []
        for line in raw.splitlines():
            if line.strip():
                records.append(json.loads(line))
        return records


def fixed_env(ordinal):
    timestamp = f"2001-01-{1 + ordinal % 27:02d}T00:00:00+0000"
    env = os.environ.copy()
    env.update(
        {
            "GIT_AUTHOR_NAME": "Cloud Matrix",
            "GIT_AUTHOR_EMAIL": "cloud-matrix@example.com",
            "GIT_AUTHOR_DATE": timestamp,
            "GIT_COMMITTER_NAME": "Cloud Matrix",
            "GIT_COMMITTER_EMAIL": "cloud-matrix@example.com",
            "GIT_COMMITTER_DATE": timestamp,
        }
    )
    return env


class Corpus:
    def __init__(self, root, run_id, payload_bytes):
        self.repo = pathlib.Path(root) / "corpus"
        git(None, "init", "--quiet", "--initial-branch=main", str(self.repo))
        git(self.repo, "config", "user.name", "Cloud Matrix")
        git(self.repo, "config", "user.email", "cloud-matrix@example.com")
        empty_tree = git(self.repo, "mktree", env=fixed_env(0)).stdout.strip()
        self.root_commit = git(
            self.repo,
            "commit-tree",
            "-m",
            "root",
            empty_tree,
            env=fixed_env(0),
        ).stdout.strip()
        self.run_id = run_id
        self.payload_bytes = payload_bytes
        self._commits = {}

    def commit(self, sample_ordinal, writer_ordinal, kind="sample"):
        key = (sample_ordinal, writer_ordinal, kind)
        if key in self._commits:
            return self._commits[key]
        seed = hashlib.sha256(
            f"cloud-odb-v1:{self.run_id}:{kind}:{sample_ordinal}:{writer_ordinal}".encode()
        ).digest()
        payload = (seed * ((self.payload_bytes + len(seed) - 1) // len(seed)))[
            : self.payload_bytes
        ]
        blob_path = self.repo / ".git" / "cloud-matrix-payload"
        blob_path.write_bytes(payload)
        blob = git(self.repo, "hash-object", "-w", str(blob_path)).stdout.strip()
        blob_path.unlink()
        tree_input = f"100644 blob {blob}\tpayload-{writer_ordinal:03d}\n"
        tree = subprocess.run(
            ["git", "-C", str(self.repo), "mktree"],
            input=tree_input,
            text=True,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=fixed_env(sample_ordinal + writer_ordinal + 1),
        ).stdout.strip()
        commit = subprocess.run(
            [
                "git",
                "-C",
                str(self.repo),
                "commit-tree",
                tree,
                "-p",
                self.root_commit,
            ],
            input=f"{kind} {sample_ordinal} writer {writer_ordinal}\n",
            text=True,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=fixed_env(sample_ordinal + writer_ordinal + 1),
        ).stdout.strip()
        self._commits[key] = commit
        return commit


def push_one(barrier, repo, url, commit, refname, failpoint=None):
    barrier.wait()
    command = ["git", "-C", str(repo)]
    if failpoint:
        command.extend(("-c", f"http.extraHeader=X-Cloud-Odb-Failpoint: {failpoint}"))
    command.extend(("push", "--porcelain", url, f"{commit}:{refname}"))
    started = time.perf_counter()
    try:
        result = run(command, check=False, timeout=600)
        return {
            "ref": refname,
            "latencyMs": round((time.perf_counter() - started) * 1000, 3),
            "exitCode": result.returncode,
            "success": result.returncode == 0,
            "stderrClass": classify(result.stderr),
            "stderr": result.stderr[-2000:],
        }
    except subprocess.TimeoutExpired as error:
        return {
            "ref": refname,
            "latencyMs": round((time.perf_counter() - started) * 1000, 3),
            "exitCode": None,
            "success": False,
            "stderrClass": "timeout",
            "stderr": str(error),
        }


def visible_refs(url):
    try:
        result = git(None, "ls-remote", "--refs", url, check=False, timeout=90)
    except subprocess.TimeoutExpired:
        return None, {}, "timeout"
    refs = {}
    if result.returncode == 0:
        for line in result.stdout.splitlines():
            oid, refname = line.split("\t", 1)
            refs[refname] = oid
    return result.returncode, refs, classify(result.stderr)


def verify_repository(url, destination, cursor, cache_state):
    attempts = []
    metrics = []
    clone = None
    started = time.perf_counter()
    for attempt in range(3):
        shutil.rmtree(destination, ignore_errors=True)
        mark = cursor.mark()
        attempt_started = time.perf_counter()
        try:
            clone = git(
                None,
                "clone",
                "--quiet",
                "--mirror",
                url,
                str(destination),
                check=False,
                timeout=120,
            )
        except subprocess.TimeoutExpired as error:
            clone = subprocess.CompletedProcess([], 124, "", str(error))
        attempt_metrics = cursor.read(mark)
        metrics.extend(attempt_metrics)
        attempts.append(
            {
                "ordinal": attempt,
                "success": clone.returncode == 0,
                "latencyMs": round((time.perf_counter() - attempt_started) * 1000, 3),
                "stderrClass": classify(clone.stderr),
                "storage": summarize_metrics(attempt_metrics),
            }
        )
        if clone.returncode == 0:
            break
        time.sleep(1)
    latency_ms = round((time.perf_counter() - started) * 1000, 3)
    if clone.returncode:
        return {
            "clone": False,
            "fsck": False,
            "cacheState": cache_state,
            "latencyMs": latency_ms,
            "storage": summarize_metrics(metrics),
            "stderrClass": classify(clone.stderr),
            "attempts": attempts,
        }
    fsck = git(destination, "fsck", "--strict", check=False, timeout=900)
    return {
        "clone": True,
        "fsck": fsck.returncode == 0,
        "cacheState": cache_state,
        "latencyMs": latency_ms,
        "storage": summarize_metrics(metrics),
        "stderrClass": classify(fsck.stderr),
        "attempts": attempts,
    }


def summarize_metrics(records):
    fields = (
        "requests",
        "gets",
        "heads",
        "puts",
        "rangeRequests",
        "rangeRequestedBytes",
        "uploadedBytes",
        "downloadedBytes",
        "conflicts",
        "casRetries",
        "publishes",
        "objectReads",
        "usefulBytes",
        "groupCacheHits",
    )
    result = {name: metric_sum(records, name) for name in fields}
    result["processes"] = len({record.get("pid") for record in records})
    result["rawEventCount"] = len(records)
    result["rawEvents"] = records
    return result


def run_sample(corpus, cursor, url, run_id, sample_ordinal, writers, warmup):
    barrier = threading.Barrier(writers)
    jobs = []
    for writer in range(writers):
        commit = corpus.commit(sample_ordinal, writer)
        refname = f"refs/heads/matrix/{run_id}/s{sample_ordinal:03d}/w{writer:03d}"
        jobs.append((commit, refname))
    mark = cursor.mark()
    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=writers) as executor:
        futures = [
            executor.submit(push_one, barrier, corpus.repo, url, commit, refname)
            for commit, refname in jobs
        ]
        raw = [future.result() for future in futures]
    wall_ms = round((time.perf_counter() - started) * 1000, 3)
    metrics = cursor.read(mark)
    _, refs, _ = visible_refs(url)
    expected = {item["ref"] for item in raw if item["success"]}
    failed = {item["ref"] for item in raw if not item["success"]}
    successful_latencies = [item["latencyMs"] for item in raw if item["success"]]
    return {
        "ordinal": sample_ordinal,
        "warmup": warmup,
        "cacheState": "warmup" if warmup else "measured",
        "writers": writers,
        "wallMs": wall_ms,
        "successes": len(successful_latencies),
        "failures": writers - len(successful_latencies),
        "visibleSuccessfulRefs": sum(refname in refs for refname in expected),
        "visibleFailedRefs": sum(refname in refs for refname in failed),
        "p50Ms": percentile(successful_latencies, 0.50),
        "p95Ms": percentile(successful_latencies, 0.95),
        "p99Ms": percentile(successful_latencies, 0.99),
        "storage": summarize_metrics(metrics),
        "writersRaw": raw,
    }


def run_failpoints(corpus, cursor, url, run_id, ordinal_base):
    results = []
    for offset, failpoint in enumerate(FAILPOINTS):
        ordinal = ordinal_base + offset
        commit = corpus.commit(ordinal, 0, kind=failpoint)
        refname = f"refs/heads/failpoint/{run_id}/{failpoint}"
        mark = cursor.mark()
        barrier = threading.Barrier(1)
        raw = push_one(barrier, corpus.repo, url, commit, refname, failpoint)
        metrics = cursor.read(mark)
        _, refs, _ = visible_refs(url)
        results.append(
            {
                "name": failpoint,
                "pushAcknowledged": raw["success"],
                "refVisible": refname in refs,
                "stderrClass": raw["stderrClass"],
                "storage": summarize_metrics(metrics),
                "passed": not raw["success"] and refname not in refs,
            }
        )
    return results


def parse_writer_counts(value):
    counts = [int(item) for item in value.split(",")]
    if not counts or any(count <= 0 or count > 100 for count in counts):
        raise argparse.ArgumentTypeError("writers must be between 1 and 100")
    return counts


def main():
    parser = argparse.ArgumentParser(prog="cloud-bench matrix")
    parser.add_argument("--url", default="http://127.0.0.1:8080")
    parser.add_argument("--writers", type=parse_writer_counts, default=[1, 10, 50])
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--payload-bytes", type=int, default=20 * 1024)
    parser.add_argument("--metrics", default="/results/process.ndjson")
    parser.add_argument("--output", default="/results/latest.json")
    parser.add_argument("--skip-failpoints", action="store_true")
    args = parser.parse_args()
    if args.warmups < 0 or args.samples <= 0 or args.payload_bytes <= 0:
        parser.error("warmups, samples, and payload bytes must be positive")

    run_id = uuid.uuid4().hex
    with tempfile.TemporaryDirectory(prefix="cloud-odb-matrix-") as temporary:
        corpus = Corpus(temporary, run_id, args.payload_bytes)
        cursor = MetricsCursor(args.metrics)
        result = {
            "schema": "git-cloud-odb-matrix/v1",
            "measurementScope": "smart-http-git-workload-over-s3-odb",
            "cloudMeasured": True,
            "runId": run_id,
            "deploymentId": os.environ.get("RAILWAY_DEPLOYMENT_ID"),
            "gitVersion": git(None, "version").stdout.strip(),
            "baseUrl": args.url,
            "corpus": {
                "generator": "deterministic-commit-tree-v1",
                "rootCommit": corpus.root_commit,
                "payloadBytesPerWriter": args.payload_bytes,
            },
            "configuration": {
                "writers": args.writers,
                "layouts": list(LAYOUTS),
                "warmups": args.warmups,
                "samples": args.samples,
                "failpoints": not args.skip_failpoints,
            },
            "layouts": [],
        }
        overall_ok = True
        ordinal = 0
        for layout in LAYOUTS:
            url = f"{args.url.rstrip('/')}/bench.git"
            seed_ref = f"refs/heads/matrix/{run_id}/seed"
            seed_mark = cursor.mark()
            seed = push_one(
                threading.Barrier(1), corpus.repo, url, corpus.root_commit, seed_ref
            )
            seed["storage"] = summarize_metrics(cursor.read(seed_mark))
            layout_result = {
                "id": "C",
                "layout": layout,
                "url": url,
                "seed": seed,
                "writerLevels": [],
            }
            overall_ok = overall_ok and seed["success"]
            layout_result["coldClone"] = verify_repository(
                url, pathlib.Path(temporary) / f"cold-{layout}", cursor, "cold"
            )
            overall_ok = overall_ok and layout_result["coldClone"]["clone"]
            overall_ok = overall_ok and layout_result["coldClone"]["fsck"]
            for writers in args.writers:
                samples = []
                for warmup in range(args.warmups):
                    samples.append(
                        run_sample(
                            corpus,
                            cursor,
                            url,
                            run_id,
                            ordinal,
                            writers,
                            True,
                        )
                    )
                    ordinal += 1
                for sample in range(args.samples):
                    samples.append(
                        run_sample(
                            corpus,
                            cursor,
                            url,
                            run_id,
                            ordinal,
                            writers,
                            False,
                        )
                    )
                    ordinal += 1
                verification = verify_repository(
                    url,
                    pathlib.Path(temporary) / f"verify-{layout}-{writers}",
                    cursor,
                    "post-writes",
                )
                measured = [sample for sample in samples if not sample["warmup"]]
                gate_ok = verification["clone"] and verification["fsck"] and all(
                    sample["successes"] == sample["writers"]
                    and sample["failures"] == 0
                    and sample["visibleSuccessfulRefs"] == sample["successes"]
                    and sample["visibleFailedRefs"] == 0
                    for sample in samples
                )
                overall_ok = overall_ok and gate_ok
                layout_result["writerLevels"].append(
                    {
                        "writers": writers,
                        "correctnessPassed": gate_ok,
                        "verification": verification,
                        "summary": {
                            "p50Ms": percentile(
                                [sample["p50Ms"] for sample in measured if sample["p50Ms"] is not None],
                                0.50,
                            ),
                            "p95Ms": percentile(
                                [sample["p95Ms"] for sample in measured if sample["p95Ms"] is not None],
                                0.95,
                            ),
                            "p99Ms": percentile(
                                [sample["p99Ms"] for sample in measured if sample["p99Ms"] is not None],
                                0.99,
                            ),
                            "failures": sum(sample["failures"] for sample in measured),
                        },
                        "samples": samples,
                    }
                )
            if not args.skip_failpoints:
                layout_result["failpoints"] = run_failpoints(
                    corpus, cursor, url, run_id, ordinal
                )
                ordinal += len(FAILPOINTS)
                failpoints_ok = all(item["passed"] for item in layout_result["failpoints"])
                overall_ok = overall_ok and failpoints_ok
            layout_result["restartVerification"] = verify_repository(
                url,
                pathlib.Path(temporary) / f"restart-{layout}",
                cursor,
                "fresh-process-restart",
            )
            overall_ok = overall_ok and layout_result["restartVerification"]["clone"]
            overall_ok = overall_ok and layout_result["restartVerification"]["fsck"]
            result["layouts"].append(layout_result)
        result["ok"] = overall_ok
        encoded = json.dumps(result, sort_keys=True, separators=(",", ":")) + "\n"
        output = pathlib.Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary_output = output.with_suffix(output.suffix + ".tmp")
        temporary_output.write_text(encoded)
        temporary_output.replace(output)
        sys.stdout.write(encoded)
        return 0 if overall_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
