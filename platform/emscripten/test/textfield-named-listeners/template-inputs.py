"""Verify already obtained templates without extracting archive paths."""
import hashlib
import json
import os
from pathlib import Path
from pathlib import PurePosixPath
import re
import sys
import zipfile


def inspect_template(filename, expected, candidate):
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise ValueError("Expected a lowercase SHA256")
    with open(filename, "rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != expected:
        raise ValueError("Template checksum mismatch")
    with zipfile.ZipFile(filename) as archive:
        names = {}
        for entry in archive.infolist():
            path = PurePosixPath(entry.filename)
            if path.is_absolute() or ".." in path.parts or "\\" in entry.filename:
                raise ValueError("Unsafe template entry")
            normalized = str(path)
            if normalized in names:
                raise ValueError("Duplicate template entry")
            names[normalized] = entry
        for required in ("html/coronaHtml5App.wasm", "html/coronaHtml5App.js",
                         "html/index.html", "html/index-debug.html", "html/index-nosplash.html"):
            if required not in names or names[required].file_size == 0:
                raise ValueError("Missing template component: " + required)
        wasm = archive.read(names["html/coronaHtml5App.wasm"])
        if wasm[:4] != b"\0asm":
            raise ValueError("Invalid WASM")
        provenance = None
        if candidate:
            provenance = json.loads(archive.read(names["webtemplate-provenance.json"]))
            if provenance["engine_version"] != "2026.3736":
                raise ValueError("Wrong candidate version")
            if not re.fullmatch(r"[0-9a-f]{40}", provenance["fork_sha"]):
                raise ValueError("Invalid fork revision")
        javascript = archive.read(names["html/coronaHtml5App.js"])
    return {"sha256": digest, "wasm_sha256": hashlib.sha256(wasm).hexdigest(),
            "js_sha256": hashlib.sha256(javascript).hexdigest(),
            "wasm_bytes": len(wasm), "provenance": provenance}


if __name__ == "__main__":
    if sys.argv[1] == "--packaged":
        root = Path(sys.argv[2])
        inputs = json.loads((root / "inputs.json").read_text())
        for arm in ("baseline", "candidate"):
            bundle = root / arm / "output/TextFieldRegression.html5/TextFieldRegression.bin"
            with zipfile.ZipFile(bundle) as archive:
                wasm = archive.read("TextFieldRegression.wasm")
                javascript = archive.read("TextFieldRegression.js")
            assert hashlib.sha256(wasm).hexdigest() == inputs[arm]["wasm_sha256"]
            # webPackageApp.lua rewrites preload metadata and directory creation
            # as well as application filenames. Record both JS hashes; only WASM
            # is byte-identical across this packaging step.
            assert b"TextFieldRegression.data" in javascript
            assert b"TextFieldRegression.wasm" in javascript
            assert b"resource.car" in javascript
            inputs[arm]["packaged_bundle_sha256"] = hashlib.sha256(bundle.read_bytes()).hexdigest()
            inputs[arm]["packaged_js_sha256"] = hashlib.sha256(javascript).hexdigest()
            inputs[arm]["js_packaging_transform"] = [
                "loadPackage file metadata", "FS_createPath directories",
                "application .data filename", "application .wasm filename"]
        (root / "inputs.json").write_text(json.dumps(inputs, indent=2))
        sys.exit(0)
    baseline, candidate, output, image = sys.argv[1:]
    data = {"engine_version": "2026.3736", "image": image}
    for arm, filename in (("baseline", baseline), ("candidate", candidate)):
        data[arm] = inspect_template(filename, os.environ[arm.upper() + "_TEMPLATE_SHA256"],
                                     arm == "candidate")
    if data["baseline"]["wasm_sha256"] == data["candidate"]["wasm_sha256"]:
        raise ValueError("The two runtimes must differ")
    with open(output, "w") as stream:
        json.dump(data, stream, indent=2)
