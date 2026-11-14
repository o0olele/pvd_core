"""Fetch the exact Dear ImGui sources used by the optional Windows viewer."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DESTINATION = ROOT / "third_party/imgui"
REVISION = "f5befd2d29e66809cd1110a152e375a7f1981f06"  # v1.91.9b
FILES = ["LICENSE.txt", "imgui.h", "imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp",
         "imgui_widgets.cpp", "imgui_internal.h", "imconfig.h", "imstb_rectpack.h",
         "imstb_textedit.h", "imstb_truetype.h", "backends/imgui_impl_win32.h",
         "backends/imgui_impl_win32.cpp", "backends/imgui_impl_dx11.h", "backends/imgui_impl_dx11.cpp"]

def fetch(path):
    url = f"https://raw.githubusercontent.com/ocornut/imgui/{REVISION}/{path}"
    request = urllib.request.Request(url, headers={"User-Agent": "pvd-viewer-build"})
    with urllib.request.urlopen(request, timeout=30) as response:
        data = response.read()
    target = DESTINATION / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return {"path": path, "url": url, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}

def main():
    with ThreadPoolExecutor(max_workers=4) as pool:
        files = list(pool.map(fetch, FILES))
    manifest = {"name": "Dear ImGui", "version": "v1.91.9b", "revision": REVISION,
                "repository": "https://github.com/ocornut/imgui", "license": "MIT", "files": files}
    (DESTINATION / "pvd-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps({"revision": REVISION, "files": len(files), "bytes": sum(x["bytes"] for x in files)}))

if __name__ == "__main__":
    main()
