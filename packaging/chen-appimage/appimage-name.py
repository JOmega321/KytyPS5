#!/usr/bin/env python3
"""Choose a local-date AppImage name from previously published dated artifacts."""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
from urllib.request import Request, urlopen
from zoneinfo import ZoneInfo

TIMEZONE = "America/New_York"
ARTIFACT_PREFIX = "KytyPS5-chenxiao07-AppImage-x86_64-"


def select_names(run, artifacts):
    started = datetime.fromisoformat(run["created_at"].replace("Z", "+00:00"))
    stamp = started.astimezone(ZoneInfo(TIMEZONE)).strftime("%m%d%y")
    pattern = re.compile(re.escape(ARTIFACT_PREFIX + stamp) + r"(?:v([1-9][0-9]*))?$")
    published = []
    for artifact in artifacts:
        match = pattern.fullmatch(artifact["name"])
        if match:
            published.append((int(match[1] or 0), artifact["workflow_run"]["id"]))
    own = {revision for revision, run_id in published if run_id == run["id"]}
    if len(own) > 1:
        raise ValueError("This run already has conflicting dated artifact names")
    revision = next(iter(own)) if own else max((rev for rev, _ in published), default=-1) + 1
    suffix = stamp + (f"v{revision}" if revision else "")
    return {
        "date": stamp,
        "revision": revision,
        "timezone": TIMEZONE,
        "run_id": run["id"],
        "source_commit": run["head_sha"],
        "appimage_name": f"KytyPS5-chenxiao07-{run['head_sha'][:8]}-x86_64-{suffix}.AppImage",
        "artifact_name": ARTIFACT_PREFIX + suffix,
    }


def api_json(repository, endpoint):
    request = Request(
        f"https://api.github.com/repos/{repository}/{endpoint}",
        headers={"Accept": "application/vnd.github+json",
                 "Authorization": "Bearer " + os.environ["GH_TOKEN"],
                 "X-GitHub-Api-Version": "2022-11-28"},
    )
    with urlopen(request, timeout=30) as response:
        return json.load(response)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--github-output", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    args = parser.parse_args()
    repository = os.environ["GITHUB_REPOSITORY"]
    run = api_json(repository, "actions/runs/" + str(int(os.environ["GITHUB_RUN_ID"])))
    if run["head_sha"] != os.environ["GITHUB_SHA"]:
        raise ValueError("Run source does not match the checked-out workflow revision")
    artifacts = []
    page = 1
    while True:
        batch = api_json(repository, f"actions/artifacts?per_page=100&page={page}")["artifacts"]
        artifacts.extend(batch)
        if len(batch) < 100:
            break
        page += 1
    names = select_names(run, artifacts)
    args.metadata.parent.mkdir(parents=True, exist_ok=True)
    args.metadata.write_text(json.dumps(names, indent=2) + "\n")
    with args.github_output.open("a") as output:
        for key in ("appimage_name", "artifact_name"):
            output.write(f"{key}={names[key]}\n")
    print(names["appimage_name"])


if __name__ == "__main__":
    main()
