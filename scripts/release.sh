#!/usr/bin/env bash
#
# Cut a release: bump include/trtc_asr/version.h, stamp CHANGELOG, commit, tag.
# CMake project version and package.sh artifacts follow version.h.
#
# Usage:
#   scripts/release.sh 1.1.0
#   scripts/release.sh 1.1.0 --dry-run

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VERSION="${1:-}"
DRY_RUN=0
shift || true
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "usage: scripts/release.sh <major.minor.patch> [--dry-run]" >&2
  exit 2
fi

DATE="$(date +%F)"
TAG="v${VERSION}"

if git rev-parse "$TAG" >/dev/null 2>&1; then
  echo "tag $TAG already exists" >&2
  exit 1
fi

CURRENT="$(sed -n 's/^#define TRTC_ASR_VERSION_STRING "\(.*\)"$/\1/p' include/trtc_asr/version.h)"
if [[ -z "$CURRENT" ]]; then
  echo "cannot read TRTC_ASR_VERSION_STRING from include/trtc_asr/version.h" >&2
  exit 1
fi

IFS=. read -r MAJOR MINOR PATCH <<<"$VERSION"

stamp_changelog() {
  python3 - "$VERSION" "$DATE" <<'PY'
import sys
from pathlib import Path
version, date = sys.argv[1], sys.argv[2]
path = Path("CHANGELOG.md")
text = path.read_text(encoding="utf-8")
heading = f"## [{version}] - {date}"
if heading in text:
    sys.exit(0)
old = "## [未发布]"
if old not in text:
    raise SystemExit("CHANGELOG.md has no '## [未发布]' section to stamp")
replacement = f"## [未发布]\n\n## [{version}] - {date}"
path.write_text(text.replace(old, replacement, 1), encoding="utf-8")
PY
}

echo "==> $CURRENT -> $VERSION"

if [[ $DRY_RUN -eq 1 ]]; then
  echo "would update include/trtc_asr/version.h"
  echo "would stamp CHANGELOG.md as ## [$VERSION] - $DATE"
  echo "would commit and tag $TAG"
  exit 0
fi

perl -i -pe "s/#define TRTC_ASR_VERSION_MAJOR .*/#define TRTC_ASR_VERSION_MAJOR $MAJOR/" include/trtc_asr/version.h
perl -i -pe "s/#define TRTC_ASR_VERSION_MINOR .*/#define TRTC_ASR_VERSION_MINOR $MINOR/" include/trtc_asr/version.h
perl -i -pe "s/#define TRTC_ASR_VERSION_PATCH .*/#define TRTC_ASR_VERSION_PATCH $PATCH/" include/trtc_asr/version.h
perl -i -pe "s/#define TRTC_ASR_VERSION_STRING \".*\"/#define TRTC_ASR_VERSION_STRING \"$VERSION\"/" include/trtc_asr/version.h
stamp_changelog

git add include/trtc_asr/version.h CHANGELOG.md
git commit -m "chore: release $VERSION"
git tag -a "$TAG" -m "Release $VERSION"

echo "tagged $TAG. push with:"
echo "  git push origin HEAD && git push origin $TAG"
