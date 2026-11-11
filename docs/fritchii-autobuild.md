# Fritchii autobuild

`.github/workflows/fritchii-autobuild.yml` picks up new releases of Frizti's Fritchii (the emulator the co-op kit is built on) and builds them with our patches. It replaces the old "Fritchii watch" workflow, which only opened an issue.

## When it runs

- Every 6 hours (minute 23), and by hand from the Actions tab ("Run workflow").
- On a push that changes the workflow file, as a dry run (so a change to it is tested on its branch).

## What it does

1. **check** (Linux): one query for Fritchii's releases (drafts never count, pre-releases do), and the newest version is compared with `lichii/PIN` using `sort -V`. The tag's commit comes from `git ls-remote`, not the API. It stops here when:
   - nothing is newer;
   - the branch `fritchii-bump-<version>` already exists (its pull request is waiting);
   - an issue "Fritchii <version>: auto-build failed …" is open (a broken release is not rebuilt every 6 hours; close the issue to retry).
2. **build** (Windows, `windows-2022`):
   - Builds and runs our unit tests (`COOP_BUILD_TESTS=ON`, ctest).
   - Writes the new tag and commit into the workspace copy of `lichii/PIN` and runs `lichii/fetch.ps1`, which clones Fritchii and applies `lichii/patches` with `git am`.
   - Installs Qt 6.8.3 (`msvc2022_64`, qtcharts) and the Vulkan SDK the way Fritchii's own CI does.
   - Configures and builds `lichii.exe` with the flags in lichii/README.md "Build (Windows)" (`_CL_`, `LICHII_COOP`, `trim_paths.cmake`, the emulator's updater off).
   - Runs `windeployqt` and uploads `build/coop/bin` (with a `BUILD.txt` naming the Fritchii tag, our commit and the run) as the artifact `lichii-coop-<tag>-windows`, kept 14 days.
3. **pr** (after a green build):
   - Commits the PIN bump as daurosk (noreply address) and pushes it to `fritchii-bump-<version>` with the workflow token.
   - Opens a pull request into main. Its body has the run link and what is left: README version notes, autotest, kit.
   - If the repo does not let the workflow token open pull requests (Settings > Actions > General > "Allow GitHub Actions to create and approve pull requests"), it opens an issue with a compare link and the same text instead.
4. **failed** (after a red build): one API call finds the failed step. It then opens the issue "Fritchii <version>: auto-build failed at <step>", with a hint: a patch that does not apply means rebasing it on the new tag in the bump pull request.

## Things to know

- Nothing is merged, tagged or released by the workflow. The pull request is reviewed and integrated like any other (docs/workflow.md).
- A branch pushed with the workflow token starts no other workflows, so the Build checks do not run on the bump pull request by themselves. Close and reopen it once (or push a commit to it) to start them.
- A patch that needs changes for the new release is not fixed by the workflow: rebase the patch by hand in the bump branch (lichii/README.md).
- Windows only. The Linux build is on hold.
- Manual run inputs:
  - `tag` builds a given Fritchii tag, even one that is not newer.
  - `dry_run` only builds: no branch, pull request or issue. With nothing newer, a dry run builds the pinned tag. This is the way to check the workflow itself.
