# HTML5 native text named-listener regression

This fixture uses real native fields and the registry DisplayObject bridge.
It is not a Lua lifecycle model. A successful comparison requires an observed
official 2026.3736 **FAIL** and patched 2026.3736 **PASS**.

After build authorization, use an immutable candidate builder image and two
already obtained, checksum-verified templates. Obtain the baseline from the
official 3736 MSI; do not compile another baseline runtime. The script packages
two scratch projects with the same builder and its existing --webtemplate option.

~~~sh
export BASELINE_TEMPLATE_SHA256=<verified-official-template-sha256>
export CANDIDATE_TEMPLATE_SHA256=<verified-patched-template-sha256>
export CHROMIUM_PATH=<existing-pinned-chromium-executable>
export CHROMIUM_SHA256=<verified-executable-sha256>
./run.sh --image <image-at-sha256-digest> \
  --baseline-template <official-3736-template.zip> \
  --candidate-template <patched-3736-template.zip> \
  --output <new-scratch-directory>
~~~

Requires Docker, Python 3.11+, Node 20+, an existing playwright-core **1.63.0**
installation (resolvable through NODE_PATH), and its matching Chromium. It does
not install dependencies. Browser flags select software WebGL; the executable
checksum and actual browser version are recorded. Use a new directory per run.

The real viewport starts at 640x960 and changes through 800x1200 and 960x1440.
Every requested resize must produce an engine resize and a changed content scale.
The removed proxy must lose its size, while the live field must keep scaling
within one pixel and preserve its font-scaling mode. Missing preconditions,
unexpected baseline success, candidate runtime errors, failed assertions and
timeouts return nonzero. Baseline errors stay observable throughout the burst.

An independent stage checks finalize function/table listeners, removed listeners,
duplicate insertion/removal, table and implicit-self named listeners, native
listener masks, original delegate arguments, multiple return values, and repeated
Lua-error exits. It temporarily intercepts and restores the registry class method;
it does not modify private upvalues. Repeated field/text-box and parent removal
cycles must permit later resizes.

Real DOM focus, typing, Enter and blur check all four userInput phases, function
and table registrations, last-wins behavior, invalid-listener preservation, named
registration interleaving and the existing removeEventListener mismatch.
Editing-only payload assertions preserve current indexing and zero placeholders.
Weak-value tokens check replacement, callback self-replacement, callback removal,
and registry-reference release after bounded frames and controlled collection.
Page disposal is recorded separately: it cannot prove destruction ordering after
the Lua state dies; source review must also retain the valid-state guard.

Evidence includes separate package logs, console/page/Module/Lua error records,
numeric assertions, template hashes and the bytes passed to WebAssembly (including
blob URLs unpacked from the app bundle). A fresh context blocks service workers
and the server disables caching. The executed WASM must equal the verified zip
entry before resize testing proceeds.
The app bundle's WASM is verified byte-for-byte before serving. JS hashes are
recorded before and after the existing packager rewrites preload metadata,
directories and application data/WASM filenames; JS byte equality is not claimed.

No compilation, browser outcome, JPEG decode, or native compatibility is implied
by adding this fixture. Progressive JPEG acceptance is a separate shipping-WASM
decoder test; this fixture does not replace it.
