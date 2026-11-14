"""Exercise asynchronous multi-session ownership, save and workspace restoration."""
import json
from pathlib import Path
import subprocess
import sys
import uuid


def run(viewer: Path, capture: Path, fixtures: Path) -> None:
    directory = fixtures / ("viewer-workspace-" + uuid.uuid4().hex[:12])
    directory.mkdir(parents=True)
    state = directory / "工作区.pvdws"
    report = directory / "workspace.json"
    screenshot = directory / "workspace.png"
    completed = subprocess.run([
        str(viewer), "--workspace-smoke", str(capture), "--workspace-state", str(state),
        "--report", str(report), "--screenshot", str(screenshot), "--warp", "--hidden"],
        cwd=directory, timeout=40)
    result = json.loads(report.read_text(encoding="utf-8"))
    assert completed.returncode == 0 and result["passed"], result
    for flag in ("stable_loading_target", "independent_state", "locks_protected",
                 "inactive_export_verified", "persistence", "identity_verified", "curves_retained"):
        assert result[flag], (flag, result)
    assert not result["auto_settings_io"] and result["slots"] == 2 and result["bookmarks"] == 1, result
    assert state.stat().st_size > 100 and Path(str(state) + ".capture.pxd2").stat().st_size > 1000
    assert screenshot.stat().st_size > 10000
    assert not (directory / "imgui.ini").exists()
    result.update(directory=str(directory), state=str(state), screenshot=str(screenshot))
    (fixtures / "viewer-workspace-latest.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("viewer_workspace_smoke.py VIEWER CAPTURE FIXTURES")
    run(*(Path(x).resolve() for x in sys.argv[1:]))
