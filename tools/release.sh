#!/usr/bin/env bash
# Release the main commit that passed every required CI job, in two steps:
#
#   release.sh prepare   (release job) when a release is due, commits the new
#                        version and changelog onto main, tags it v<version>
#                        and writes version=<version> to $GITHUB_OUTPUT;
#                        nothing when no release is due.
#   release.sh publish   (release-publish job) publishes the GitHub release of
#                        $VERSION with the .debs and Alpine tarballs in dist/.
#
# Between the two, ci.yml builds the packages from the tag (package.yml).
set -euo pipefail
cd "$(dirname "$0")/.."

output() { echo "$1" >> "${GITHUB_OUTPUT:-/dev/null}"; }

prepare() {
  : "${GITHUB_SHA:?}" "${GITHUB_EVENT_NAME:?}"
  git config user.name 'github-actions[bot]'
  git config user.email '41898282+github-actions[bot]@users.noreply.github.com'
  git fetch origin main --tags
  local remote version
  remote=$(git rev-parse origin/main)
  if [[ "$remote" == "$GITHUB_SHA" ]]; then
    # Weekly, or at once for a security fix; see due() in release.py.
    if ! python3 tools/release.py --due "$GITHUB_EVENT_NAME"; then
      echo "${GITHUB_EVENT_NAME}: no release due (nothing to release, or not due before the weekly run)."
      output version=
      return 0
    fi
    version=$(python3 tools/release.py)
    git add CMakeLists.txt CHANGELOG.md
    # Pushed with the deploy key, which triggers workflows: [skip ci] stops
    # this commit from releasing again.
    git commit -m "chore(release): ${version} after green CI" -m "[skip ci]" -m "Blockclonia-Release-For: ${GITHUB_SHA}"
    git tag "v${version}"
    # Atomic publication plus a lease closes the race between the fetch and
    # the push: this is a fast-forward, and the lease rejects a newer main.
    if ! git push --atomic --force-with-lease="refs/heads/main:${GITHUB_SHA}" origin HEAD:main "refs/tags/v${version}"; then
      # Losing the race to a merge is a skip: that commit's own run releases.
      git fetch origin main
      if [[ "$(git rev-parse origin/main)" != "$GITHUB_SHA" ]]; then
        echo 'main moved while releasing; skipping release.'
        output version=
        return 0
      fi
      return 1
    fi
  elif [[ "$(git log -1 --format=%B origin/main)" == *"Blockclonia-Release-For: ${GITHUB_SHA}"* ]]; then
    # A rerun after a packaging or publishing failure resumes this release,
    # without another bump.
    git checkout --detach origin/main
    version=$(python3 tools/release.py --version)
    [[ "$(git rev-parse "v${version}^{commit}")" == "$(git rev-parse HEAD)" ]]
  else
    echo 'main moved since this CI run; skipping release.'
    output version=
    return 0
  fi
  echo "Releasing ${version}."
  output "version=${version}"
}

publish() {
  : "${VERSION:?}" "${GH_REPO:?}" "${GH_TOKEN:?}"
  shopt -s nullglob
  local debs=(dist/blockclonia_"${VERSION}"_*.deb) tarballs=(dist/blockclonia-"${VERSION}"-alpine-*.tar.gz)
  # A .deb for each Ubuntu architecture and a tarball for each Alpine one,
  # all from the tag.
  if ((${#debs[@]} != 2 || ${#tarballs[@]} != 2)); then
    echo "expected the amd64 and arm64 .debs and the x86_64 and aarch64 Alpine tarballs of ${VERSION} in dist/," \
      "found: ${debs[*]:-} ${tarballs[*]:-}" >&2
    return 1
  fi
  local work=${RUNNER_TEMP:-$(mktemp -d)}
  # Look the release up first, drafts included (the tag endpoint omits them);
  # API failures must stop publication.
  gh api "repos/${GH_REPO}/releases?per_page=100" > "$work/releases.json"
  local state
  state=$(python3 - "v${VERSION}" "$work/releases.json" <<'PY'
import json, sys
tag, path = sys.argv[1:]
release = next((r for r in json.load(open(path)) if r['tag_name'] == tag), None)
print('none' if release is None else 'draft' if release['draft'] else 'published')
PY
)
  if [[ $state == published ]]; then
    echo "v${VERSION} is already published."
    return 0
  fi
  # Releases start as drafts and are published once the packages are
  # attached, since immutable releases refuse new assets after publication.
  if [[ $state == none ]]; then
    python3 tools/release.py --notes "$VERSION" > "$work/notes.md"
    gh release create "v${VERSION}" --draft --verify-tag --title "blockclonia ${VERSION}" --notes-file "$work/notes.md"
  fi
  gh release upload "v${VERSION}" "${debs[@]}" "${tarballs[@]}" --clobber
  gh release edit "v${VERSION}" --draft=false --latest
}

case "${1:-}" in
  prepare) prepare ;;
  publish) publish ;;
  *) echo "usage: $0 prepare|publish" >&2; exit 2 ;;
esac
