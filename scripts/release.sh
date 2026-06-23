#!/usr/bin/env bash
# scripts/release.sh — bump version, tag, create draft release, push.
# AppVeyor picks up the tag, builds all platforms, uploads artifacts.
#
#   scripts/release.sh patch       0.1.0 → 0.1.1
#   scripts/release.sh minor       0.1.0 → 0.2.0
#   scripts/release.sh major       0.1.0 → 1.0.0
#   scripts/release.sh 1.2.3       exact version
#   scripts/release.sh patch --dry preview only
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

DRY=0; [ "${2:-}" = "--dry" ] && DRY=1
BUMP="${1:?usage: release.sh patch|minor|major|<x.y.z> [--dry]}"

# read current version from Makefile or Cargo.toml-style
CURRENT=$(grep -m1 '^version\s*=' Makefile 2>/dev/null | sed 's/.*=\s*"\(.*\)"/\1/' | sed 's/.*=\s*\(.*\)/\1/' | tr -d ' ')
if [ -z "$CURRENT" ]; then
    # fallback: git tag
    CURRENT=$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//' || echo "0.1.0")
fi
IFS='.' read -r MAJ MIN PAT <<< "$CURRENT"

case "$BUMP" in
    patch)    NEW="$MAJ.$MIN.$((PAT + 1))" ;;
    minor)    NEW="$MAJ.$((MIN + 1)).0" ;;
    major)    NEW="$((MAJ + 1)).0.0" ;;
    [0-9]*.*) NEW="$BUMP" ;;
    *)        echo "error: unknown bump type '$BUMP'"; exit 1 ;;
esac

TAG="v$NEW"
REPO="Chidi09/VarianLang"

echo "  current: $CURRENT"
echo "      new: $NEW  ($TAG)"
[ "$DRY" -eq 1 ] && { echo "(dry run — no changes made)"; exit 0; }

if git rev-parse "$TAG" >/dev/null 2>&1; then
    echo "error: tag $TAG already exists"; exit 1
fi
if ! git diff --cached --quiet || ! git diff --quiet; then
    echo "error: uncommitted changes — commit or stash first"; exit 1
fi
if ! gh auth status &>/dev/null; then
    echo "error: gh not authenticated — run: gh auth login"; exit 1
fi

# bump version in Makefile and any other files
sed -i "s/^version\s*=\s*\"${CURRENT}\"/version = \"${NEW}\"/" Makefile 2>/dev/null || true

git add -A
git commit -m "chore: bump version to $NEW"
git tag -a "$TAG" -m "Release $TAG"
git push origin main "$TAG"

# create draft release — AppVeyor uploads artifacts + we publish
gh release create "$TAG" \
    --repo "$REPO" \
    --title "Varian $NEW" \
    --draft \
    --notes "Varian v${NEW} — compiled, concurrent, batteries-included language.

Downloads will appear once AppVeyor finishes building."

echo ""
echo "✅  $TAG dispatched"
echo "    Draft release: https://github.com/$REPO/releases/tag/$TAG"
echo "    AppVeyor builds + uploads artifacts, then publish the draft."
