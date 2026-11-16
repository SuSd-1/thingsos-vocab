#!/bin/bash
# Import a ThingsOS release source archive as a new upstream commit/tag.
# Usage: import-upstream.sh <version>   e.g. import-upstream.sh 0.2.0
set -euo pipefail

VERSION=${1:?usage: import-upstream.sh <version>}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BASE="https://thingsos.scholarcn.com/metalio"
ZIP_URL="$BASE/ThingsOS-metalio-${VERSION}-source.zip"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "==> downloading $ZIP_URL"
curl -fsSL "$ZIP_URL" -o "$WORK/src.zip"
shasum -a 256 "$WORK/src.zip"

echo "==> extracting"
python3 "$ROOT/vocab-kit/tools/extract_zip.py" "$WORK/src.zip" "$WORK/tree"

cd "$ROOT"
if git rev-parse --verify -q main >/dev/null; then
  git checkout -q main
else
  git checkout -q -b main
fi

echo "==> syncing upstream tree (vocab-kit/ and build/ preserved)"
rsync -a --delete \
  --exclude '.git' --exclude '.gitignore' --exclude 'vocab-kit' --exclude 'build' \
  "$WORK/tree/" "$ROOT/"

git add -A
if git diff --cached --quiet; then
  echo "==> tree unchanged for $VERSION"
else
  git -c user.name="vocab-kit" -c user.email="local@localhost" \
      commit -q -m "upstream: ThingsOS ${VERSION} source (Metalio E-Ink4-Plus)"
fi
git tag -f "upstream/${VERSION}" >/dev/null
echo "==> tagged upstream/${VERSION}"
git log --oneline -1
