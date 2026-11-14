"""End-to-end SDK -> TCP -> live model -> real D3D11 -> recording/reopen test."""
import json
from pathlib import Path
import subprocess
import sys
import time
import uuid


def run(viewer: Path, producer: Path, fixtures: Path) -> None:
    directory = fixtures / ("viewer-live-" + uuid.uuid4().hex[:12])
    directory.mkdir(parents=True)
    ready = directory / "ready.json"
    report = directory / "report.json"
    screenshot = directory / "live.png"
    capture = directory / "received.pxd2"
    command = [str(viewer), "--listen", "0", "--record", str(capture),
               "--ready", str(ready), "--live-smoke", "--warp", "--hidden",
               "--report", str(report), "--screenshot", str(screenshot)]
    with subprocess.Popen(command, cwd=directory) as application:
        try:
            deadline = time.monotonic() + 12
            while not ready.exists():
                if application.poll() is not None:
                    raise RuntimeError("Viewer exited before listening: " +
                                       (report.read_text() if report.exists() else str(application.returncode)))
                if time.monotonic() > deadline:
                    raise TimeoutError("Viewer did not announce its listening port")
                time.sleep(0.025)
            port = json.loads(ready.read_text())["port"]
            sdk = subprocess.run([str(producer), "--tcp", "127.0.0.1", str(port),
                                  str(directory / "sdk"), "120", "8"],
                                 cwd=directory, capture_output=True, timeout=25)
            if sdk.returncode:
                raise RuntimeError(sdk.stdout.decode(errors="replace") + sdk.stderr.decode(errors="replace"))
            code = application.wait(timeout=12)
            result = json.loads(report.read_text())
            assert code == 0 and result["passed"], result
            assert result["frames"] == 240, result
            assert result["recorded_bytes"] == capture.stat().st_size == result["wire_bytes"], result
            assert result["reopened"] and result["online_render_frames"] >= 3, result
            assert screenshot.stat().st_size > 10000
            # Stable pointers to the latest unique output; recordings remain exclusive.
            (fixtures / "viewer-live-latest.json").write_text(json.dumps({
                "directory": str(directory), "screenshot": str(screenshot),
                "capture": str(capture), "report": str(report), **result
            }, indent=2), encoding="utf-8")
            print(json.dumps(result))
        finally:
            if application.poll() is None:
                application.kill()
                application.wait(timeout=5)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("viewer_live_smoke.py VIEWER PRODUCER FIXTURES")
    run(*(Path(x).resolve() for x in sys.argv[1:]))
