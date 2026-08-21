# Scaleform/GFx contract

This is the durable host/guest contract for the Scaleform/GFx front end. The
supported runtime boundary is in
[`rexglue-runtime.md`](rexglue-runtime.md).

## Movie invokes are functional behavior

The guest-to-movie path is functional code, not a logging helper.
`GfxInvokeStrArgs` at `0x82C4AA98` supplies string arguments to
`GfxInvokeImpl` at `0x82C796B0`. The worker dispatches to `GfxMethodInvoke` at
`0x821FC928`, which resolves the method name and executes the ActionScript
function. Calls such as `SetText`, `AnimateEnter`, `AnimateExit`, and completion
callbacks control movie state, visibility, and text population.

An instrumentation wrapper must call the original body by default and may only
log arguments, callers, or bounded state snapshots around it. A no-op shortcut
can make the screen appear stable while silently preventing every movie's
state machine from advancing.

## Font-library loading

The exported movies may request the shared font library as
`gfxfontlib.swf`, while the converted movie files use `.gfx` names. The
`sub_821E1AB0` open boundary retries a failed `.gfx` request as `.swf` and then
restores the guest string; it does not rewrite successful opens or apply a
global extension rule.

Keep the fallback at the open boundary so the guest's deferred loader and
movie registration remain authoritative. A missing font library can look like
a renderer defect because `SetText` may produce no tessellated glyph geometry.

## Glyph-texture invalidation

GFx retains its stock 512-entry vector-glyph cache. ReRevved does not alter the
guest cache capacity.

Glyph-atlas textures rely on ReXGlue's shared-memory invalidation. ReXGlue must
install each texture write watch before requesting or uploading its guest-memory
range. A write during preparation or upload then marks the texture outdated for
the next request. A failed load removes its provisional watches and restores the
outdated state so the texture can be retried. This prevents stale host textures
from appearing as missing or mangled glyphs.

## Removed GFx trace hooks

The removed GFx trace hooks were observational only. They did not write guest
memory, change a `PPCRegister`, or alter guest control flow. Their only side
effect was host timing, so they are not compatibility repairs and must remain
removed.

| Removed hook | Verdict | Reason |
|---|---|---|
| `ReRevvedTraceGfxObjectIconsBuilderDirect` | Diagnostic only | Environment-gated logging and guest reads only. |
| `ReRevvedTraceGfxObjectIconsBuilderAlternate` | Diagnostic only | Environment-gated logging and guest reads only. |
| `ReRevvedTraceGfxObjectIconsNtCreateFileContext` | Diagnostic only | Captured private trace state only. |
| `ReRevvedTraceGfxObjectIconsNtCreateFileContext143B0` | Diagnostic only | Captured private trace state only. |
| `ReRevvedTraceGfxObjectIconsNtCreateFileContext1A694` | Diagnostic only | Captured private trace state only. |
| `ReRevvedTraceGfxObjectIconsNtCreateFileContext229F4` | Diagnostic only | Captured private trace state only. |
| `ReRevvedTraceGfxObjectIconsNtCreateFileContext22B50` | Diagnostic only | Captured private trace state only. |
| `ReRevvedTraceGfxObjectIconsNtCreateFileContext22C44` | Diagnostic only | Captured private trace state only. |
| `ReRevvedTraceGfxObjectIconsNtCreateFileContext23794` | Diagnostic only | Captured private trace state only. |
| `ReRevvedTraceGfxObjectIconsNtOpenFileContext25510` | Diagnostic only | Captured private trace state only. |
| `ReRevvedTraceGfxObjectIconsNtOpenFileContext0F2F4` | Diagnostic only | Captured private trace state only. |
| `ReRevvedTraceGfxObjectIconsNtOpenFileContext14890` | Diagnostic only | Captured private trace state only. |
| `ReRevvedTraceGfxObjectIconsNtOpenFileContext1B0AC` | Diagnostic only | Captured private trace state only. |
| `ReRevvedTraceGfxObjectIconsNtOpenFileContext1B66C` | Diagnostic only | Captured private trace state only. |
| `ReRevvedTraceGfxObjectIconsNtOpenFileContext22F34` | Diagnostic only | Captured private trace state only. |
| `ReRevvedTraceGfxImageShapeRegister` | Diagnostic only | Updated private atomic registration records only. |
| `ReRevvedTraceGfxImageShapeDisplayBegin` | Diagnostic only | Updated private trace state and read guest state only. |
| `ReRevvedTraceGfxImageShapeCoupledBuild` | Diagnostic only | Counted calls and sampled guest state only. |
| `ReRevvedTraceGfxImageShapeSimpleWriter` | Diagnostic only | Incremented a private trace counter only. |
| `ReRevvedTraceGfxImageShapeComplexWriter` | Diagnostic only | Incremented a private trace counter only. |
| `ReRevvedTraceGfxImageShapeResourceResult` | Diagnostic only | Counted results and sampled guest state only. |
| `ReRevvedTraceGfxImageShapeFillDispatch` | Diagnostic only | Recorded registers in private trace state only. |
| `ReRevvedTraceGfxImageShapeDraw` | Diagnostic only | Recorded draw counts in private trace state only. |
| `ReRevvedTraceGfxImageShapeDisplayEnd` | Diagnostic only | Read and logged accumulated diagnostic state only. |

`rerevved_functions.toml` had no uncertain trace-only entries during the
audit.

## Input

Input must flow through ReXGlue's runtime input path, the game's action
predicates, and its movie handlers. Diagnostic hooks may observe those calls but
must not replace the original predicate or synthesize a permanently held input.

## Render config and render caps

GFx owns the render-config object and the render-caps query output. ReRevved
hooks the guest boundaries at 0x82245050, 0x82302E90, and 0x82302F0C. The
hooks retain bounded per-thread query state plus process-wide pending-config
atomics and apply one compatibility repair; they do not select a renderer or
synthesize a caps result.

ReRevvedRememberGfxRenderConfig at 0x82245050 remembers the candidate config
pointer and renderer from the guest call. The render-caps begin hook captures
the query renderer, output identity, and caller. A nested begin is
ignored. The end hook clears the query state before evaluating the result.

The restoration contract is narrow:

1. A failed query is retained only when the caller is 0x82245130, the result is
   zero, the remembered renderer matches, and the candidate 0x14-based
   0x18-byte range is readable.
2. A later result is eligible only when it is nonzero, its output pointer is
   the captured output, its 16-byte output range is readable, and its renderer
   matches the retained renderer.
3. For that valid same-renderer result, the hook copies the first two output
   words to the retained config at offsets 0x24 and 0x28, after checked guest
   range validation, then clears the pending state.

Mismatched callers, renderers, output pointers, unreadable ranges, and nested
queries do not enter the repair path. Each destination word uses a checked
write; if the second write fails after the first succeeds, the pending config
remains eligible for a later matching result. The repair is active whenever
the hooks are installed and has no diagnostic mode.

This boundary does not prove that GFx pixels were displayed; visual success
still requires a captured frame or an independent owner-observed result.

## Timing and state ownership

Movie registration, asset loading, ActionScript invokes, and front-end state
transitions are separate stages. When content is absent, trace the load and
invoke boundaries before changing PM4 decoding, forcing a screen object, or
adding a renderer fallback. A bounded timing aid may keep a state visible while
content arrives, but it must preserve the guest transition path and have an
explicit escape when content never publishes.

When changing a front-end hook, verify the natural interactive path. A forced
transition does not prove that the title's own input or invoke path works.
