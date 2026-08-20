# Contributing

ReRevved accepts focused changes that improve the ReXGlue title build,
runtime correctness, documentation, or supporting analysis tools. Do not submit
copyrighted game files, extracted assets, generated guest code, build output, or
machine-local evidence.

## Before a change

Read [`docs/ai_agents/README.md`](docs/ai_agents/README.md) for the contribution
and evidence policies. Base runtime work on the SDK commit pinned by
[`rexglue-sdk.lock.json`](rexglue-sdk.lock.json). Keep
the sibling SDK checkout read-only unless the change is explicitly scoped to
that repository.

## Verification

Run the repository gate from Windows PowerShell:

```powershell
cd <repo>; .\scripts\verify.ps1
```

The gate checks first-party C/C++ with the root `.clang-format`. Run
`clang-format -i <file>` on changed C/C++ files before committing.

For a title build, also run:

```powershell
cd <repo>; .\scripts\rexglue.ps1 -SelfTest
cd <repo>; .\scripts\rexglue.ps1 -Stage Codegen
cd <repo>; .\scripts\rexglue.ps1 -Stage Build
```

State what changed and which checks passed. A successful build does not prove a
runtime claim. Runtime changes need bounded trace, capture, or owner-observed
evidence appropriate to the claim.

## Change boundaries

- Preserve the title's original control flow unless the documented contract
  requires a compatibility hook.
- Keep diagnostics disabled by default and preserve the original behavior when
  enabled.
- Put settled public facts in the canonical tracked guide. Keep machine paths,
  experiments, and private evidence out of tracked files.
- Do not edit generated files directly.
