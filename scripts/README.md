# Scripts

The supported runtime driver is `rexglue.ps1`:

| Command | Purpose and output |
|---|---|
| `.\scripts\rexglue.ps1 -SelfTest` | Validate required paths and print the configure, codegen, build, and launch commands. It does not run CMake or the game. |
| `.\scripts\rexglue.ps1 -Stage Configure` | Configure against the installed sibling SDK. |
| `.\scripts\rexglue.ps1 -Stage Codegen` | Regenerate ignored guest C++ under `generated/default/`. |
| `.\scripts\rexglue.ps1 -Stage Build` | Build the Release title under `out/build/win-amd64-release/`. |
| `.\scripts\rexglue.ps1 -Stage Launch` | Launch the existing executable with `xenos` and D3D12 ROV, writing the log to `out/rexglue_boot.log`. |
| `.\scripts\rexglue.ps1 -Stage All` | Configure, generate, configure again, build, and run the bounded launch check. |

`-ProbeSeconds <n>` bounds `Launch` and `All` (the default is 20 seconds); a
timeout stops the process started by the driver. `-Interactive` waits for normal
game exit and removes that timeout. `-LaunchArgument --name=value` appends one
validated game argument. Launch creates the ignored `out/rexglue-user/` and
`out/rexglue-cache/` directories.

## Release packaging

`package.py` stages release binaries, runtime libraries, the player README,
licenses, the Windows desktop-shortcut helper, and an empty `game/` directory.
It rejects retail and writable runtime file types. The root CMake `project()`
declaration supplies the version.

After a Release build, package the native platform from the repository root:

```powershell
python .\scripts\package.py
```

Windows produces `out/rerevved-v<version>-windows-x64.zip`. A Linux build host
produces `out/rerevved-v<version>-linux-x64.tar.gz`; Linux remains experimental.
Use `--platform`, `--build-dir`, `--sdk-dir`, or `--out-dir` for non-default
layouts. See the [release process](../docs/releasing.md) for the checklist.

## Content manifest generation

`gen-content-manifest.ps1` generates `src/content_manifest.inc` for maintainers.
It records sorted relative paths and exact sizes from `Resource/Common` without
copying retail content. The default input is the ignored `game/` root.

```powershell
.\scripts\gen-content-manifest.ps1
.\scripts\gen-content-manifest.ps1 -ContentRoot <content-root> -OutputPath <path>
```

Regenerate the manifest only when the supported content set intentionally
changes, and review the resulting source diff before verification.

## Trace analyzer

ReXGlue frame traces use `<title-id>_<counter>.xtr`; stream traces use
`<title-id>_stream.xtr`. Use the analyzer on an existing .xtr file:

    python .\scripts\analyze-rexglue-trace.py .\545407E5_123.xtr --summary-only
    python .\scripts\analyze-rexglue-trace.py .\545407E5_123.xtr --minimum-read 0x2000 --frame 3

The analyzer prints trace metadata, draw inventories, texture fetches, and
memory reads at least `0x1000` bytes by default. `--summary-only` suppresses
per-draw records; `--minimum-read` and `--frame` narrow the report. It rejects
unsupported versions, truncated packets, and invalid compressed payloads.

Capture the current foreground window as an ignored BMP. The script temporarily
sets the window topmost and removes that temporary state even when capture fails:

    .\scripts\capture-window.ps1 -Out .\out\window_topmost_capture.bmp

Compare two BMPs without changing them. Crops are x,y,w,h and must have equal
dimensions:

    .\scripts\compare-frames.ps1 .\baseline.bmp .\out\window_topmost_capture.bmp -MeanAbsMax 0.01 -Ssim 0.99

The command reports `mean_abs` and `ssim`; thresholds apply only to the supplied
images and crop regions.

## Cleanup

`clean-logs.ps1` deletes only top-level repository `*.log`, `out/*.log`, and the
known `out/*.txt` dump names. It does not recurse and does not remove `.xtr` or
BMP files. Review the targets first, then delete only stale files:

    .\scripts\clean-logs.ps1 -WhatIf
    .\scripts\clean-logs.ps1 -Days 7

Do not replace the dry run with an unrestricted recursive delete. The analyzer,
captures, and comparisons provide bounded evidence; none by itself proves that
the title rendered missing GFx content or that its natural state transition
succeeded. See the
[ReXGlue runtime contract](../docs/rexglue-runtime.md).
