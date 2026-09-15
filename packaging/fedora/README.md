# Release procedure (Fedora / Copr)

Copr builds from the moving tag `copr-build`, not `master`. Any new tag/branch push triggers a
rebuild from wherever `copr-build` currently points, regardless of which ref triggered it — so
move the tag *before* creating a new one, or a spurious trigger (e.g. pushing an unrelated
branch) would rebuild unreleased work.

## Steps

1. Bump version on `master`:
   - `Version:` + new `%changelog` entry in [ckb-next-ddg.spec](ckb-next-ddg.spec)
   - `pkgver`/`pkgrel` in [../aur/PKGBUILD](../aur/PKGBUILD), then regenerate `.SRCINFO`:
     ```bash
     podman run --rm -v "$PWD/packaging/aur:/pkg" -w /pkg archlinux:base-devel bash -c '
       useradd -m builder && chown -R builder /pkg &&
       su builder -c "makepkg --printsrcinfo > .SRCINFO"
     '
     podman unshare chown -R 0:0 packaging/aur
     ```
   - Commit, push `master`.

2. Move `copr-build` to the new commit (a force-push on an existing tag doesn't fire a `create`
   event, so this alone doesn't trigger anything):
   ```bash
   git tag -f copr-build master
   git push origin copr-build --force
   ```

3. Tag the release (`vMAJOR.MINOR.PATCH.ddg.N`, annotated):
   ```bash
   git tag -a vX.Y.Z.ddg.N -m "X.Y.Z.ddg.N"
   git push origin vX.Y.Z.ddg.N
   ```
   This fires the `create` webhook event that triggers Copr, already pointed at the right commit
   from step 2.

   Also matches `v*.ddg.*`, so it triggers `.github/workflows/aur-publish.yml` if enabled
   (independent of the Copr webhook).

4. Check the build at
   [copr.fedorainfracloud.org/coprs/davidedg/ckb-next-ddg/builds](https://copr.fedorainfracloud.org/coprs/davidedg/ckb-next-ddg/builds)
   or `copr-cli list-builds davidedg/ckb-next-ddg`.

GitHub's "create" webhook event can't be scoped to tags only (it covers branches too); "Releases"
was tried as a more selective alternative and doesn't work — Copr acks the delivery but never
starts a build. Hence the moving tag instead of a different GitHub event.

## Local validation before tagging (optional)

`Source0` can't be fetched before the tag exists on GitHub (404). To validate the bumped spec
without a remote tag, build against a locally generated archive instead:

```bash
git archive --format=tar.gz --prefix=ckb-next-ddg-X.Y.Z.ddg.N/ HEAD \
  -o /tmp/ckb-next-ddg-X.Y.Z.ddg.N.tar.gz

podman run --rm \
  -v "$PWD:/src:Z" \
  -v "/tmp/ckb-next-ddg-X.Y.Z.ddg.N.tar.gz:/src/packaging/fedora/ckb-next-ddg-X.Y.Z.ddg.N.tar.gz:Z,ro" \
  registry.fedoraproject.org/fedora:latest bash -c '
    dnf -y install rpkg rpm-build dnf-plugins-core &&
    mkdir -p /tmp/srpmout && cd /src/packaging/fedora &&
    rpkg srpm --outdir /tmp/srpmout &&
    dnf -y builddep /tmp/srpmout/*.src.rpm &&
    rpmbuild --rebuild /tmp/srpmout/*.src.rpm --define "_topdir /tmp/rpmbuild"'
```

Without `%_disable_source_fetch 0` set, `rpmbuild` uses the local file instead of trying to
download `Source0`.
