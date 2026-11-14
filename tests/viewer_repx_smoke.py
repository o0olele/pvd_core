"""Run the real host's strict and explicitly partial RepX export workflows."""
import json
from pathlib import Path
import subprocess
import sys
import uuid
import xml.etree.ElementTree as ET


def run(viewer: Path, capture: Path, fixtures: Path) -> None:
    directory = fixtures / ("viewer-repx-" + uuid.uuid4().hex[:12])
    directory.mkdir(parents=True)
    output = directory / "当前状态.repx"
    report = directory / "partial.json"
    screenshot = directory / "partial.png"
    command = [str(viewer), "--repx-smoke", str(capture), "--warp", "--hidden"]
    result = subprocess.run(command + ["--allow-partial", "--output", str(output),
        "--report", str(report), "--screenshot", str(screenshot)], cwd=directory, timeout=30)
    data = json.loads(report.read_text(encoding="utf-8"))
    assert result.returncode == 0 and data["passed"], data
    assert data["allow_partial"] and not data["complete"] and data["diagnostics"] > 0, data
    assert data["actors"] > 0 and data["shapes"] > 0 and data["event_count"] > 0, data
    assert not data["auto_settings_io"] and output.stat().st_size == data["xml_bytes"], data
    root = ET.parse(output).getroot()
    assert root.tag == "PhysX30Collection" and root.attrib["version"] == "3.4.2"
    assert "PARTIAL" in output.read_text(encoding="utf-8")
    assert screenshot.stat().st_size > 10000
    strict = directory / "strict.repx"
    failed = subprocess.run(command + ["--output", str(strict)], cwd=directory, timeout=30)
    assert failed.returncode != 0 and not strict.exists(), "Strict export silently published native losses"
    assert not (directory / "imgui.ini").exists()
    data.update(directory=str(directory), output=str(output), screenshot=str(screenshot), strict_rejected=True)
    (fixtures / "viewer-repx-latest.json").write_text(json.dumps(data, indent=2), encoding="utf-8")
    print(json.dumps(data))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("viewer_repx_smoke.py VIEWER CAPTURE FIXTURES")
    run(*(Path(x).resolve() for x in sys.argv[1:]))
