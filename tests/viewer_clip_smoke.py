"""Exercise named clips through the real host, async exporter and saved workspace."""
import json
from pathlib import Path
import subprocess
import sys
import uuid


def run(viewer: Path, capture: Path, fixtures: Path) -> None:
    directory = fixtures / ("viewer-clips-" + uuid.uuid4().hex[:12])
    directory.mkdir(parents=True)
    state, output = directory / "剪辑.pvdws", directory / "片段.pxd2"
    report, screenshot = directory / "clips.json", directory / "clips.png"
    completed = subprocess.run([
        str(viewer), "--clip-smoke", str(capture), "--output", str(output),
        "--workspace-state", str(state), "--report", str(report),
        "--screenshot", str(screenshot), "--warp", "--hidden"],
        cwd=directory, timeout=40)
    assert completed.returncode == 0, completed.returncode
    result = json.loads(report.read_text(encoding="utf-8"))
    for flag in ("passed", "split_trim", "stable_source", "export_verified", "workspace_verified"):
        assert result[flag], (flag, result)
    assert result["visible_rows"] == 2 and result["native_frames"] == 6, result
    assert not result["auto_settings_io"] and not (directory / "imgui.ini").exists()
    assert state.stat().st_size > 100 and output.stat().st_size > 1000
    assert screenshot.stat().st_size > 10000
    result.update(directory=str(directory), screenshot=str(screenshot))
    (fixtures / "viewer-clips-latest.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("viewer_clip_smoke.py VIEWER CAPTURE FIXTURES")
    run(*(Path(x).resolve() for x in sys.argv[1:]))
