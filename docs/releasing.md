# Release process

This runbook builds and checks a ReRevved release candidate locally. Publishing
happens when the owner pushes a `v*` tag, which triggers the release workflow.

## Set the version

1. Update the version in the root `project(rerevved VERSION ...)` declaration
   in [`CMakeLists.txt`](../CMakeLists.txt).
2. After building and packaging, confirm that the archive name uses the project
   version. The window title and Windows executable tooltip intentionally omit
   it.

## Verify and build

1. Run the repository gate from the repository root:

   ```powershell
   .\scripts\verify.ps1
   ```

2. Start from a fresh `out/build/win-amd64-release/` build directory and run the
   Release pipeline:

   ```powershell
   .\scripts\rexglue.ps1 -SelfTest
   .\scripts\rexglue.ps1 -Stage Codegen
   .\scripts\rexglue.ps1 -Stage Build
   ```

3. Create the release archive:

   ```powershell
   python .\scripts\package.py
   ```

4. Inspect `out/rerevved-v<version>-windows-x64.zip`. Confirm that it contains
   the executable, runtime libraries, licenses, player README, and an empty
   `game/` directory, with no retail content or per-user files.

Linux packaging produces a `.tar.gz` archive from a Linux Release binary. Linux
support is experimental, so record the build environment.

## Accept the standalone archive

Use a new extraction directory and test user-data location. Complete every item
before publication:

- [ ] First run accepts a legally owned ISO and starts with the extracted
  content.
- [ ] First run accepts an already-extracted content folder and remembers it.
- [ ] Controller navigation reaches and operates the front end.
- [ ] Keyboard navigation reaches and operates the front end.
- [ ] A game can be saved, exited, launched again, and reloaded.
- [ ] One complete match reaches its natural end state.
- [ ] The archive and staged tree contain no retail content, saves, logs,
  caches, or machine-specific paths.

Record the version, archive checksum, verification result, build host, and
acceptance result in the release notes. After all checks pass, give the archive
and notes to the owner for publication.
