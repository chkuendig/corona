"""Frozen source, hosted build measurements, and content-addressed template."""
import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import time
import zipfile

UPSTREAM = "d74923057d3fc3abad1903b61de5a459d9566363"
VERSION = "2026.3736"


def command(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()


def digest(path):
    checksum = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(chunk)
    return checksum.hexdigest()


def write(path, data):
    Path(path).write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def tree_bytes(path):
    return sum(item.stat().st_size for item in Path(path).rglob("*")
               if item.is_file() and not item.is_symlink())


def freeze():
    started = utc()
    revision = os.environ["SOURCE_SHA"]
    assert command("git", "rev-parse", "HEAD") == revision
    assert command("git", "rev-parse", "3736^{commit}") == UPSTREAM
    subprocess.run(["git", "merge-base", "--is-ancestor", UPSTREAM, revision], check=True)
    assert not command("git", "status", "--porcelain", "--untracked-files=no")
    submodules = {}
    for line in subprocess.check_output(
            ["git", "submodule", "status", "--recursive"], text=True).splitlines():
        assert line.startswith(" "), "Uninitialized or mismatched submodule"
        sha, name = line[1:].split()[:2]
        submodules[name] = sha
    tree = command("git", "ls-tree", "-r", revision)
    seed = tree + json.dumps(submodules, sort_keys=True)
    data = {
        "engine_version": VERSION, "upstream_sha": UPSTREAM, "fork_sha": revision,
        "tooling_sha": revision, "image_tooling_sha": os.environ["IMAGE_TOOLING_SHA"],
        "source_tree": command("git", "rev-parse", "HEAD^{tree}"),
        "source_input_sha256": hashlib.sha256(seed.encode()).hexdigest(),
        "submodule_gitlinks": submodules,
        "stamp": {"year": 2026, "build": 3736, "month": 10, "day": 12},
        "emsdk": {"url": os.environ["EMSDK_URL"], "sha256": os.environ["EMSDK_SHA256"],
                  "cache_hit": False},
        "baseline_msi_template_sha256": os.environ["BASELINE_TEMPLATE_SHA256"],
        "flags": {"configuration": "Release", "makeflags": "-j3",
                  "compile_optimization": "-O2", "link_optimization": "-O3",
                  "debug_info": False},
        "cache": {"objects": "clean", "sdk": "cold",
                  "key": hashlib.sha256((seed + os.environ["EMSDK_SHA256"]
                                         + VERSION + "Release-j3").encode()).hexdigest()},
        "source_freeze_started": started, "source_freeze_ended": utc(),
        "source_preparation_started": os.environ["SOURCE_PREP_STARTED"],
        "run_url": os.environ["GITHUB_SERVER_URL"] + "/" + os.environ["GITHUB_REPOSITORY"]
                   + "/actions/runs/" + os.environ["GITHUB_RUN_ID"],
    }
    write("webtemplate-inputs.json", data)


def host_snapshot():
    return {"utc": utc(), "disk_free_bytes": shutil.disk_usage(".").free,
            "vm_stat": command("vm_stat"), "swap": command("sysctl", "vm.swapusage"),
            "host_disk_io": command("iostat", "-Id")}


def process_tree_rss(root_pid):
    rows = [line.split() for line in command("ps", "-axo", "pid=,ppid=,rss=").splitlines()]
    descendants = {root_pid}
    while True:
        children = {int(pid) for pid, parent, _ in rows if int(parent) in descendants}
        expanded = descendants | children
        if expanded == descendants:
            break
        descendants = expanded
    return sum(int(rss) * 1024 for pid, _, rss in rows if int(pid) in descendants)


def build():
    evidence = Path(os.environ["WEBTEMPLATE_EVIDENCE"])
    sdk = Path(os.environ["WEBTEMPLATE_SDK"])
    header = Path("librtt/Core/Rtt_Version.h").read_text()
    for macro, value in (("YEAR", "2026"), ("REVISION", "3736"),
                         ("MONTH", "10"), ("DAY", "12")):
        assert re.search(r"#define\s+Rtt_BUILD_" + macro + r"\s+" + value + r"\b", header)
    data = json.loads(Path("webtemplate-inputs.json").read_text())
    assert data["fork_sha"] == os.environ["SOURCE_SHA"]
    versions = {
        "runner_image_os": os.environ.get("ImageOS"),
        "runner_image_version": os.environ.get("ImageVersion"),
        "runner_arch": os.environ.get("RUNNER_ARCH"),
        "os": platform.platform(), "architecture": platform.machine(),
        "cpu_count": os.cpu_count(), "ram_bytes": int(command("sysctl", "-n", "hw.memsize")),
        "xcode": command("xcodebuild", "-version"),
        "macos_sdk": command("xcrun", "--sdk", "macosx", "--show-sdk-version"),
        "emcc": command("emcc", "--version"), "clang": command("clang", "--version"),
        "node": command("node", "--version"), "python": sys.version,
        "make": command("make", "--version"), "zip": command("zip", "-v"),
    }
    wasm_opt = next((item for item in (sdk / "upstream/bin/wasm-opt",
                                      sdk / "upstream/bin/binaryen/wasm-opt")
                     if item.exists()), None)
    if wasm_opt is None:
        wasm_opt = shutil.which("wasm-opt")
    assert wasm_opt, "Missing Binaryen identity"
    versions["wasm_opt"] = command(str(wasm_opt), "--version")
    versions["binaryen_sha256"] = digest(wasm_opt)
    versions["emscripten_clang"] = command(str(sdk / "upstream/bin/clang"), "--version")
    if os.environ.get("EMSDK_PYTHON"):
        versions["emsdk_python"] = command(os.environ["EMSDK_PYTHON"], "--version")
    for name in ("lua", "luac", "lua2c.sh", "CopyResources.sh", "car"):
        executable = Path("bin/mac") / name
        assert executable.is_file() and os.access(executable, os.X_OK), name
        versions[name] = {"sha256": digest(executable), "architecture": command("file", str(executable))}
    versions["bundled_lua"] = command("bin/mac/lua", "-v")
    versions["bundled_luac"] = command("bin/mac/luac", "-v")
    write(evidence / "tools.json", versions)
    write(evidence / "before-build.json", host_snapshot() | {
        "source_bytes": tree_bytes("."), "sdk_bytes": tree_bytes(sdk)})
    started, monotonic = utc(), time.monotonic()
    peak = 0
    with open(evidence / "compile.log", "w") as log, open(evidence / "samples.jsonl", "w") as samples:
        process = subprocess.Popen(
            ["/usr/bin/time", "-l", "./build_template.sh"], cwd="platform/emscripten/gmake",
            stdout=log, stderr=subprocess.STDOUT)
        try:
            while process.poll() is None:
                rss = process_tree_rss(process.pid)
                peak = max(peak, rss)
                samples.write(json.dumps(host_snapshot() | {
                    "process_tree_rss_bytes": rss, "elapsed_seconds": time.monotonic() - monotonic}) + "\n")
                samples.flush()
                time.sleep(5)
            status = process.wait()
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait()
    write(evidence / "build-timing.json", {
        "started": started, "ended": utc(), "elapsed_seconds": time.monotonic() - monotonic,
        "exit_code": status, "sampled_process_tree_peak_rss_bytes": peak,
        "measurement_notes": "ps RSS is an aggregate sample; time -l maxrss is a single-process maximum in bytes. "
                             "vm_stat reports its page size. iostat and swap include other runner processes.",
        "jobs": 3,
    })
    write(evidence / "after-build.json", host_snapshot() | {
        "object_bytes": tree_bytes("platform/emscripten/gmake/obj"),
        "generated_lua_bytes": tree_bytes("platform/emscripten/lua"),
        "template_bytes": Path("platform/emscripten/webtemplate.zip").stat().st_size
                          if Path("platform/emscripten/webtemplate.zip").exists() else 0})
    if status:
        raise RuntimeError("Template build failed; see compile.log")


def archive():
    started, monotonic = utc(), time.monotonic()
    evidence = Path(os.environ["WEBTEMPLATE_EVIDENCE"])
    output = Path(os.environ["WEBTEMPLATE_OUTPUT"])
    inputs = json.loads(Path("webtemplate-inputs.json").read_text())
    tools = json.loads((evidence / "tools.json").read_text())
    temporary = output / "template.tmp"
    provenance = inputs | {"tools": tools, "runner": tools["runner_image_version"]}
    original = Path("platform/emscripten/webtemplate.zip")
    with zipfile.ZipFile(original) as source, zipfile.ZipFile(
            temporary, "w", compression=zipfile.ZIP_DEFLATED) as target:
        seen = set()
        for entry in source.infolist():
            name = str(PurePosixPath(entry.filename))
            assert not name.startswith("/") and ".." not in PurePosixPath(name).parts and "\\" not in name
            if name == ".":
                continue
            assert name not in seen
            seen.add(name)
            target.writestr(name + ("/" if entry.is_dir() else ""), source.read(entry))
        target.writestr("webtemplate-provenance.json", json.dumps(provenance, sort_keys=True, indent=2))
    entries = {}
    with zipfile.ZipFile(temporary) as archive_file:
        for entry in archive_file.infolist():
            if not entry.is_dir():
                content = archive_file.read(entry)
                entries[entry.filename] = {"sha256": hashlib.sha256(content).hexdigest(), "bytes": len(content)}
        for name in ("html/coronaHtml5App.wasm", "html/coronaHtml5App.js",
                     "html/index.html", "html/index-debug.html", "html/index-nosplash.html"):
            assert entries[name]["bytes"] > 0
        assert any(name.startswith("res_widget/") for name in entries)
        assert any(name.startswith("fbinstant/") for name in entries)
        wasm = archive_file.read("html/coronaHtml5App.wasm")
        assert wasm.startswith(b"\0asm") and b"2026.3736" in wasm
    template_hash = digest(temporary)
    template = output / ("webtemplate-3736-" + template_hash + ".zip")
    temporary.rename(template)
    revision = inputs["fork_sha"]
    write(evidence / "archive-timing.json", {
        "started": started, "ended": utc(), "elapsed_seconds": time.monotonic() - monotonic,
        "zip_bytes": template.stat().st_size, "disk_free_bytes": shutil.disk_usage(".").free})
    evidence_archive = output / ("evidence-3736-" + revision + ".tgz")
    with tarfile.open(evidence_archive, "w:gz") as bundle:
        bundle.add(evidence, arcname="evidence")
    source_hash = (evidence / "source-SHA256SUMS").read_text().split()[0]
    manifest = inputs | {
        "tools": tools, "source_snapshot_sha256": source_hash,
        "template": {"name": template.name, "sha256": template_hash, "bytes": template.stat().st_size},
        "entries": entries, "wasm": entries["html/coronaHtml5App.wasm"],
        "js": entries["html/coronaHtml5App.js"],
        "compiled_version_string_present": True,
        "runtime_version_verified": False,
        "runtime_acceptance": "Pending actual browser getter, regression and JPEG decode",
        "jpeg_archive_evidence_sha256": digest(evidence / "jpeg-archive.txt"),
        "jpeg_link_evidence_sha256": digest(evidence / "jpeg-link.txt"),
        "evidence": {"name": evidence_archive.name, "sha256": digest(evidence_archive)},
        "build": json.loads((evidence / "build-timing.json").read_text()),
    }
    manifest_path = output / ("manifest-3736-" + revision + ".json")
    write(manifest_path, manifest)
    with open(output / "SHA256SUMS", "w") as checksums:
        for file in (template, manifest_path, evidence_archive):
            checksums.write(digest(file) + "  " + file.name + "\n")


if __name__ == "__main__":
    {"freeze": freeze, "build": build, "archive": archive}[sys.argv[1]]()
