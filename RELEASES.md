# Releases

vellum is released as prebuilt archives on GitHub Releases, built and published by `.github/workflows/publish.yml` from a `v*` tag.

## Targets

| Target | Zig target | Archive |
|---|---|---|
| Linux x86-64, glibc 2.27 or newer | `x86_64-linux-gnu.2.27` | `vellum-vX.Y.Z-x86_64-linux.tar.gz` |
| Windows x86-64 | `x86_64-windows-gnu` | `vellum-vX.Y.Z-x86_64-windows.zip` |

Both are built on Linux with Zig 0.17.0, against the gesso commit `build.zig.zon` pins. The Linux floor is gesso's: glibc 2.27 (Ubuntu 18.04). macOS is not published yet.

Each archive holds the executable, `README.md`, `LICENSE`, on Windows `register-windows.ps1`, and a `licenses/` folder with the license texts of everything compiled in: md4c, gesso, kb_text_shape and stb (both inside gesso), and SDL3 with its REUSE license set, on Linux with SDL's Wayland protocol code. `release/stage.sh` builds one target and packs it:

```sh
release/stage.sh linux dist
release/stage.sh windows dist
```

## Checks

`validation.yml` runs the parser's tests, builds both archives, starts the Linux build and draws `tests/sample.md` without a window (`--shot`), then does the same with the Windows build on a Windows runner. Its gate job, `validate`, runs even when a build is skipped and passes only when both succeeded (`wakamex/release-actions/validate-gate`); it is the required check `release-eligible / validate`, produced on every push to `main` by `release-eligibility.yml`.

## Releasing vX.Y.Z

1. Set `.version` in `build.zig.zon` to `X.Y.Z` and write `release-notes/vX.Y.Z.md`, ending it with a "Verifying a download" section that gives both commands below for that version.
2. Commit both as `Release vX.Y.Z` and push `main` without tags.
3. Wait for `release-eligible / validate` to pass on that commit, and check that remote `main` still points to it.
4. Tag it with an annotated `vX.Y.Z` and push only the tag.

`publish.yml` then reruns validation, checks that the tag is annotated, matches `build.zig.zon` and has release notes (`wakamex/release-actions/verify-release-tag`), and rebuilds both archives from the tag. The shared `wakamex/release-actions` binary release workflow writes `SHA256SUMS`, attests every asset and verifies the attestations, and creates the Release from the notes file. Rerunning it on a published tag verifies the existing Release instead of replacing it. A broken release is fixed by a new version; tags and published assets are never moved or replaced.

The attestations are signed by the shared workflow, so verify a downloaded asset with:

```sh
gh attestation verify ASSET --repo wakamex/vellum \
  --signer-workflow wakamex/release-actions/.github/workflows/binary-release.yml
gh release verify-asset vX.Y.Z ASSET --repo wakamex/vellum
```

## One-time setup

- Turn on immutable releases in the repository settings.
- Once `release-eligible / validate` has passed on `main`, add a tag ruleset named `Validated release tags` for `refs/tags/v*` that requires that check (from the GitHub Actions app that produced it), with no bypass actors, deletion restricted and non-fast-forward updates blocked.
