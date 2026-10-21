# Versioned 3736 HTML5 template

The separate webtemplate.yml workflow is manual-only. It mirrors upstream
3736's source stamping and macos-26 webtemplate job with Xcode 26.4, the official
coronalabs emsdk e2.0.34 distribution, and unchanged Release flags. The date
is fixed to 2026-10-12. It runs one clean runtime build with at most three jobs.
It never invokes the full Daily Build workflow.

Job timeouts are based on the successful upstream 3736
[Daily Build run 38284435270](https://github.com/coronalabs/corona/actions/runs/38284435270)
at d74923057d3fc3abad1903b61de5a459d9566363 (2026-10-12).
The [source job](https://github.com/coronalabs/corona/actions/runs/38284435270/job/114929295081)
took 1m54s; source-code allows 10 minutes for recursive checkout, input freezing,
stamping and artifact transfer (8m06s margin).
The comparable macos-26/Xcode 26.4
[webtemplate job](https://github.com/coronalabs/corona/actions/runs/38284435270/job/114930054153)
took 19m12s including SDK setup (17m50s in the build step); webtemplate allows
60 minutes (40m48s margin) for the three-job build cap and added evidence work.
The upstream [release job](https://github.com/coronalabs/corona/actions/runs/38284435270/job/114958848342)
took 57s; publish allows 10 minutes (9m03s margin) for checksum verification,
artifact transfer and release upload. These are elapsed job times, excluding
the queue, and margins are allowances rather than measured fork timings.

Review the exact source revision before dispatch. The full source SHA must equal
the workflow's selected commit. Supply a reviewed SHA256 of the official SDK
archive: its release metadata provides no digest. Obtain and review those bytes
before compiling; the workflow verifies them before extraction. Also supply the
matching image-tooling revision and the verified official MSI template hash.
There is no checksum default and no stock-SDK or Linux fallback.

The source job freezes recursive gitlinks and source inputs, uses the upstream
process_sources.sh, and transfers a checksummed processed source archive. The
template job records hosted runner/OS/architecture, Xcode/SDK, bundled helper
architecture/hashes, emcc/clang/node/Python/Binaryen identities, flags and job
count. SDK and object caches are cold; no compile cache is restored.

The build log retains compiler/archive/link commands, JPEG archive members and
link evidence. Resource evidence includes timestamps, setup download bytes,
aggregate sampled process RSS, time -l statistics, vm_stat, swap, disk counters
and coarse disk/source/SDK/object/archive sizes. Host counters include other
runner processes. Logs remain available after a failed build; a failed run
produces no release asset. Queue/setup time is separate from compilation.

Outputs are webtemplate-3736-<zip-sha256>.zip, a source-versioned manifest,
an evidence archive, and SHA256SUMS. The embedded webtemplate-provenance.json
identifies source/tool inputs without a self-referential final zip hash.
The external manifest binds every zip entry and the WASM/JS to the final hash.
The compiled version string is checked statically; actual runtime version,
browser baseline/candidate results and JPEG decode remain acceptance gates.

After a successful manually dispatched build, the publish job creates a new
webtemplate-3736-<full-source-sha> tag at that exact commit and publishes a
prerelease. Existing refs/assets are never moved or overwritten. Actions
artifacts transfer data inside the run; durable prerelease assets serve later
consumers. Consumers must verify checksums even for a content-addressed name.
The Daily Build tag trigger accepts only four-digit numeric release tags so
these template tags do not start the native build matrix.

Adding or reviewing this workflow does not dispatch it, create a tag, build an
image, or establish browser/native acceptance.
