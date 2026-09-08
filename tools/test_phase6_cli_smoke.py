#!/usr/bin/env python3
import argparse
import json
import shlex
import subprocess
import tempfile
from pathlib import Path


def to_msys_path(path: Path) -> str:
    s = str(path)
    if len(s) >= 3 and s[1] == ":" and (s[2] == "\\" or s[2] == "/"):
        drive = s[0].lower()
        rest = s[2:].replace("\\", "/")
        return f"/{drive}{rest}"
    return s.replace("\\", "/")


def run_msys(msys_bash: Path, cmd_parts) -> subprocess.CompletedProcess:
    shell_cmd = "export MSYSTEM=UCRT64; source /etc/profile; " + " ".join(cmd_parts)
    return subprocess.run([str(msys_bash), "-lc", shell_cmd], check=False, capture_output=True, text=True)


def assert_true(cond: bool, msg: str):
    if not cond:
        raise RuntimeError(msg)


def main() -> int:
    p = argparse.ArgumentParser(description="Phase 6 CLI smoke tests")
    p.add_argument("--decoder", required=True)
    p.add_argument("--wav", required=True)
    p.add_argument("--msys-bash", default=r"C:\msys64\usr\bin\bash.exe")
    p.add_argument("--out-dir", default="")
    args = p.parse_args()

    decoder = Path(args.decoder).resolve()
    wav = Path(args.wav).resolve()
    msys_bash = Path(args.msys_bash)

    if args.out_dir:
        out_root = Path(args.out_dir).resolve()
        out_root.mkdir(parents=True, exist_ok=True)
        temp_ctx = None
    else:
        temp_ctx = tempfile.TemporaryDirectory(prefix="phase6_smoke_")
        out_root = Path(temp_ctx.name)

    cfg_path = out_root / "phase6_config.json"
    csv_path = out_root / "phase6_out.csv"
    json_path = out_root / "phase6_out.json"
    met_path = out_root / "phase6_metrics.json"
    log_path = out_root / "phase6_diag.log"

    cfg = {
        "profile": "balanced",
        "decode_threads": 2,
        "seed": 42,
        "stream": True,
        "stream_iterations": 2,
        "stream_poll_ms": 80,
        "stream_tail_seconds": 5,
        "output_json": str(json_path),
        "diag_log": str(log_path),
    }
    cfg_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")

    cmd_parts = [
        shlex.quote(to_msys_path(decoder)),
        shlex.quote(to_msys_path(wav)),
        shlex.quote(to_msys_path(csv_path)),
        "--config",
        shlex.quote(to_msys_path(cfg_path)),
        "--metrics",
        shlex.quote(to_msys_path(met_path)),
        "--profile",
        "fast",
        "--seed",
        "7",
        "--decode-threads",
        "2",
        "--stream",
        "--stream-iterations",
        "2",
        "--stream-poll-ms",
        "80",
        "--stream-tail-seconds",
        "5",
        "--output-json",
        shlex.quote(to_msys_path(json_path)),
        "--diag-log",
        shlex.quote(to_msys_path(log_path)),
        "--max-seconds",
        "8",
        "--no-progress",
        "--quiet",
    ]
    cp = run_msys(msys_bash, cmd_parts)
    assert_true(cp.returncode == 0, f"decoder failed rc={cp.returncode}\nstdout={cp.stdout}\nstderr={cp.stderr}")

    assert_true(csv_path.exists(), "CSV output missing")
    assert_true(json_path.exists(), "JSON output missing")
    assert_true(met_path.exists(), "Metrics output missing")
    assert_true(log_path.exists(), "Diagnostic log missing")

    j = json.loads(json_path.read_text(encoding="utf-8"))
    assert_true(j.get("schema_version") == "jndb.decode.v1", "JSON schema_version mismatch")
    assert_true(isinstance(j.get("results"), list), "JSON results must be a list")
    stats = j.get("stats") or {}
    assert_true("quality_score" in stats and "track_count" in stats, "JSON stats missing keys")

    diag_lines = [x.strip() for x in log_path.read_text(encoding="utf-8").splitlines() if x.strip()]
    assert_true(any("startup decode_threads=" in x for x in diag_lines), "diag startup line missing")
    assert_true(sum(1 for x in diag_lines if x.startswith("iter=")) >= 2, "diag iter lines missing")

    m = json.loads(met_path.read_text(encoding="utf-8"))
    assert_true("quality_score" in m and "decoded_count" in m, "metrics JSON missing expected fields")

    assert_true(csv_path.read_text(encoding="utf-8").startswith("track_id,"), "CSV header mismatch")

    sess_dir = out_root / "session_evidence"
    cmd_parts2 = [
        shlex.quote(to_msys_path(decoder)),
        shlex.quote(to_msys_path(wav)),
        shlex.quote(to_msys_path(out_root / "phase6_out_session.csv")),
        "--mode",
        "urban-noise",
        "--dashboard",
        "rich",
        "--session-export",
        shlex.quote(to_msys_path(sess_dir)),
        "--max-seconds",
        "8",
        "--no-progress",
        "--quiet",
    ]
    cp2 = run_msys(msys_bash, cmd_parts2)
    assert_true(cp2.returncode == 0, f"session export decode failed rc={cp2.returncode}\nstdout={cp2.stdout}\nstderr={cp2.stderr}")

    session_summary = sess_dir / "session_summary.json"
    snippet_index = sess_dir / "snippets" / "snippet_index.csv"
    assert_true(session_summary.exists(), "session_summary.json missing")
    assert_true(snippet_index.exists(), "snippet_index.csv missing")
    lines = [x for x in snippet_index.read_text(encoding="utf-8").splitlines() if x.strip()]
    assert_true(len(lines) >= 2, "snippet_index must contain at least one snippet row")
    first = lines[1].split(",")
    assert_true(len(first) >= 7, "snippet_index columns mismatch")
    wav_name = first[6]
    assert_true((sess_dir / "snippets" / wav_name).exists(), "snippet WAV referenced in index is missing")

    print("Phase 6/7 CLI smoke: PASSED")
    print(f"- out_dir: {out_root}")
    if temp_ctx is not None:
        temp_ctx.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
