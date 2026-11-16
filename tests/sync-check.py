#!/usr/bin/env python3
"""Runs the sync scenarios of the desktop application against the SFTP test stand and writes
an HTML report.

The script builds the tst_sync test of the dev preset, starts the stand from this folder
(docker compose up -d --wait), runs the test with JOTTER_TEST_SFTP pointing at the stand and
renders reports/sync-report.html from its JUnit output. A key pair made for the run lets the
key sign-in be checked too. A stand the script started is stopped again unless --keep is
given. Standard library only.
"""

import argparse
import datetime
import html
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ElementTree
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
ROOT = TESTS_DIR.parent
APP_DIR = ROOT / "app"
TEST_SOURCE = APP_DIR / "tests" / "tst_sync.cpp"
REPORTS_DIR = TESTS_DIR / "reports"
# The stand's fixture account; it exists only in the disposable test container.
STAND_ADDRESS = "sftp://jotter:jotter@127.0.0.1:2222/library"


def run(command, **kwargs):
    """Runs a command, failing the check when it fails."""
    print("+ " + " ".join(str(part) for part in command), flush=True)
    return subprocess.run(command, check=True, **kwargs)


def stand_running():
    """Tells whether the stand's SFTP container is running."""
    result = subprocess.run(
        ["docker", "compose", "ps", "--status", "running", "--quiet", "sftp"],
        cwd=TESTS_DIR, capture_output=True, text=True, check=True)
    return bool(result.stdout.strip())


def make_key(folder):
    """Makes an Ed25519 key pair for the key sign-in, or returns None without ssh-keygen."""
    if shutil.which("ssh-keygen") is None:
        print("ssh-keygen is missing; the key sign-in is skipped", flush=True)
        return None
    key = Path(folder) / "id_ed25519"
    run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "jotter-sync-check",
         "-f", str(key)])
    return key


def test_descriptions():
    """Reads the comment above each test function of tst_sync.cpp, by function name."""
    source = TEST_SOURCE.read_text(encoding="utf-8")
    descriptions = {}
    pattern = re.compile(r"((?:^ {4}///.*\n)+)^ {4}void (\w+)\(\)", re.MULTILINE)
    for comment, name in pattern.findall(source):
        lines = [line.strip()[3:].strip() for line in comment.splitlines()]
        descriptions[name] = " ".join(line for line in lines if line)
    return descriptions


def git_revision():
    """Describes the checked-out commit and whether the working tree has changes."""
    try:
        commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                                capture_output=True, text=True, check=True).stdout.strip()
        branch = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=ROOT,
                                capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT,
                               capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"
    return f"{branch} @ {commit}" + (" with uncommitted changes" if dirty else "")


def read_results(junit):
    """Reads the test cases of a Qt Test JUnit file."""
    suite = ElementTree.parse(junit).getroot()
    cases = []
    for case in suite.iter("testcase"):
        name = case.get("name")
        # The set-up functions are listed only when they fail.
        if name in ("initTestCase", "cleanupTestCase") and case.find("failure") is None:
            continue
        failure = case.find("failure")
        error = case.find("error")
        skipped = case.find("skipped")
        if failure is not None or error is not None:
            status = "failed"
            problem = failure if failure is not None else error
            message = problem.get("message", "")
            if problem.text and problem.text.strip():
                message += "\n" + problem.text.strip()
        elif skipped is not None:
            status, message = "skipped", skipped.get("message", "")
        else:
            status, message = "passed", ""
        output = [line.strip() for text in case.itertext() for line in text.splitlines()
                  if line.strip()]
        notes = [line for line in output if line not in message]
        cases.append({"name": name, "status": status, "time": float(case.get("time", 0)),
                      "message": message, "notes": notes})
    return suite, cases


def render(cases, suite, exit_code, started, duration):
    """Renders the report page."""
    descriptions = test_descriptions()
    counts = {status: sum(1 for case in cases if case["status"] == status)
              for status in ("passed", "failed", "skipped")}
    passed = exit_code == 0 and counts["failed"] == 0
    properties = {item.get("name"): item.get("value") for item in suite.iter("property")}
    rows = []
    for case in cases:
        details = []
        if case["message"]:
            details.append(f'<pre class="message">{html.escape(case["message"])}</pre>')
        if case["notes"]:
            items = "".join(f"<li>{html.escape(note)}</li>" for note in case["notes"])
            details.append(f'<ul class="notes">{items}</ul>')
        rows.append(
            f'<tr class="{case["status"]}">'
            f'<td><span class="badge {case["status"]}">{case["status"]}</span></td>'
            f'<td><code>{html.escape(case["name"])}</code>'
            f'<p>{html.escape(descriptions.get(case["name"], ""))}</p>{"".join(details)}</td>'
            f'<td class="time">{case["time"]:.2f} s</td></tr>')
    measurements = [(case["name"], note) for case in cases for note in case["notes"]
                    if re.search(r"\btook\b", note)]
    measurement_rows = "".join(
        f"<tr><td><code>{html.escape(name)}</code></td><td>{html.escape(note)}</td></tr>"
        for name, note in measurements)
    verdict = "All checks passed" if passed else "Checks failed"
    facts = [
        ("Started", started.strftime("%Y-%m-%d %H:%M:%S")),
        ("Duration", f"{duration:.1f} s"),
        ("Storage", "SFTP test stand, " + STAND_ADDRESS.split("@")[1]),
        ("Revision", git_revision()),
        ("Qt", properties.get("QtVersion", "unknown")),
    ]
    fact_items = "".join(f"<div><dt>{html.escape(label)}</dt><dd>{html.escape(value)}</dd></div>"
                         for label, value in facts)
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Jotter sync check</title>
<style>
:root {{
  color-scheme: light dark;
  --bg: #f7f7f8; --surface: #ffffff; --text: #1d1d21; --muted: #62626b; --line: #e2e2e6;
  --passed: #1a7f37; --failed: #c62828; --skipped: #8a6d00;
  --passed-bg: #e6f4ea; --failed-bg: #fdecea; --skipped-bg: #fff6d6;
}}
@media (prefers-color-scheme: dark) {{
  :root {{
    --bg: #17171a; --surface: #202024; --text: #ececf0; --muted: #a0a0aa; --line: #34343a;
    --passed: #5bd27a; --failed: #ff7b72; --skipped: #e3c04d;
    --passed-bg: #1d3324; --failed-bg: #3d1f1f; --skipped-bg: #3a3218;
  }}
}}
* {{ box-sizing: border-box; }}
body {{ margin: 0; background: var(--bg); color: var(--text);
  font: 15px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }}
main {{ max-width: 1100px; margin: 0 auto; padding: 32px 16px 48px; }}
h1 {{ font-size: 24px; margin: 0 0 4px; }}
h2 {{ font-size: 18px; margin: 32px 0 12px; }}
.verdict {{ font-size: 18px; font-weight: 600; color: var(--{"passed" if passed else "failed"}); }}
.cards {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 12px;
  margin: 20px 0; }}
.card {{ background: var(--surface); border: 1px solid var(--line); border-radius: 12px;
  padding: 14px 16px; }}
.card strong {{ display: block; font-size: 28px; }}
.card.passed strong {{ color: var(--passed); }}
.card.failed strong {{ color: var(--failed); }}
.card.skipped strong {{ color: var(--skipped); }}
dl {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 8px 24px;
  margin: 0; }}
dt {{ color: var(--muted); font-size: 13px; }}
dd {{ margin: 0; overflow-wrap: anywhere; }}
.table {{ overflow-x: auto; background: var(--surface); border: 1px solid var(--line);
  border-radius: 12px; }}
table {{ width: 100%; border-collapse: collapse; }}
th, td {{ text-align: left; vertical-align: top; padding: 10px 14px;
  border-bottom: 1px solid var(--line); }}
tr:last-child td {{ border-bottom: none; }}
th {{ color: var(--muted); font-weight: 500; font-size: 13px; }}
td p {{ margin: 4px 0 0; color: var(--muted); }}
td.time {{ white-space: nowrap; text-align: right; }}
.badge {{ display: inline-block; border-radius: 999px; padding: 1px 10px; font-size: 13px;
  font-weight: 600; }}
.badge.passed {{ background: var(--passed-bg); color: var(--passed); }}
.badge.failed {{ background: var(--failed-bg); color: var(--failed); }}
.badge.skipped {{ background: var(--skipped-bg); color: var(--skipped); }}
pre.message {{ white-space: pre-wrap; margin: 8px 0 0; padding: 8px 10px; border-radius: 8px;
  background: var(--bg); font-size: 13px; }}
ul.notes {{ margin: 6px 0 0; padding-left: 18px; font-size: 13px; }}
code {{ font-family: ui-monospace, "JetBrains Mono", monospace; font-size: 13px; }}
</style>
</head>
<body>
<main>
<h1>Jotter sync check</h1>
<div class="verdict">{verdict}</div>
<div class="cards">
  <div class="card passed"><strong>{counts["passed"]}</strong>passed</div>
  <div class="card failed"><strong>{counts["failed"]}</strong>failed</div>
  <div class="card skipped"><strong>{counts["skipped"]}</strong>skipped</div>
</div>
<dl>{fact_items}</dl>
<h2>Scenarios</h2>
<div class="table"><table>
<thead><tr><th>Result</th><th>Scenario</th><th>Time</th></tr></thead>
<tbody>{"".join(rows)}</tbody>
</table></div>
<h2>Measurements</h2>
<div class="table"><table>
<thead><tr><th>Scenario</th><th>Measurement</th></tr></thead>
<tbody>{measurement_rows or '<tr><td colspan="2">None recorded.</td></tr>'}</tbody>
</table></div>
</main>
</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=Path, default=APP_DIR / "build" / "dev",
                        help="build folder of the dev preset (default: app/build/dev)")
    parser.add_argument("--no-build", action="store_true",
                        help="run the test as built, without building it first")
    parser.add_argument("--keep", action="store_true",
                        help="leave the stand running when the script started it")
    parser.add_argument("--filter", nargs="*", default=[],
                        help="test functions to run (default: all)")
    arguments = parser.parse_args()

    if not arguments.no_build:
        run(["cmake", "--build", "--preset", "dev", "--target", "tst_sync"], cwd=APP_DIR)
    binary = arguments.build_dir / "tests" / "tst_sync"
    if not binary.exists():
        sys.exit(f"{binary} is missing: configure the dev preset in app/ first")

    REPORTS_DIR.mkdir(exist_ok=True)
    junit = REPORTS_DIR / "sync-junit.xml"
    report = REPORTS_DIR / "sync-report.html"
    was_running = stand_running()
    with tempfile.TemporaryDirectory(prefix="jotter-sync-check-") as keys:
        key = make_key(keys)
        stand_env = dict(os.environ)
        test_env = dict(os.environ, JOTTER_TEST_SFTP=STAND_ADDRESS)
        if key is not None:
            stand_env["JOTTER_SFTP_AUTHORIZED_KEY"] = key.with_suffix(".pub").read_text().strip()
            test_env["JOTTER_TEST_SFTP_KEY"] = str(key)
        # A new key changes the container's environment, so compose recreates it.
        run(["docker", "compose", "up", "-d", "--wait"], cwd=TESTS_DIR, env=stand_env)
        started = datetime.datetime.now()
        clock = datetime.datetime.now().timestamp()
        try:
            result = subprocess.run(
                [str(binary), *arguments.filter, "-o", f"{junit},junitxml", "-o", "-,txt"],
                env=test_env)
        finally:
            if not was_running and not arguments.keep:
                run(["docker", "compose", "down"], cwd=TESTS_DIR)
        duration = datetime.datetime.now().timestamp() - clock

    if not junit.exists():
        sys.exit(f"The test wrote no results (exit code {result.returncode})")
    suite, cases = read_results(junit)
    report.write_text(render(cases, suite, result.returncode, started, duration),
                      encoding="utf-8")
    print(f"Report: {report}")
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
