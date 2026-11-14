"""Exercise real asynchronous whole/cropped saves and D3D UI output."""
import json
from pathlib import Path
import subprocess
import sys
import uuid


def run(viewer: Path, capture: Path, fixtures: Path) -> None:
    directory = fixtures / ("viewer-export-" + uuid.uuid4().hex[:12])
    directory.mkdir(parents=True)
    results = {}
    for mode in ("whole", "crop"):
        output = directory / (mode + "-保存.pxd2")
        report = directory / (mode + ".json")
        screenshot = directory / (mode + ".png")
        command = [str(viewer), "--export-smoke", str(capture), "--output", str(output),
                   "--report", str(report), "--screenshot", str(screenshot), "--warp", "--hidden"]
        if mode == "crop":
            command += ["--crop", "3", "15"]
        completed = subprocess.run(command, cwd=directory, timeout=35)
        data = json.loads(report.read_text(encoding="utf-8"))
        assert completed.returncode == 0 and data["passed"] and data["state_verified"], data
        assert data["bytes"] == output.stat().st_size > 1000, data
        assert screenshot.stat().st_size > 10000
        results[mode] = {"file": str(output), "screenshot": str(screenshot), **data}
    assert results["crop"]["frames"] == 13, results
    assert results["whole"]["frames"] > results["crop"]["frames"], results
    (fixtures / "viewer-export-latest.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(json.dumps(results))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("viewer_export_smoke.py VIEWER CAPTURE FIXTURES")
    run(*(Path(x).resolve() for x in sys.argv[1:]))
