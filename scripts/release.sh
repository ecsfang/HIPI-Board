#!/bin/bash
#
# HIPI: make a release
#
#   scripts/release.sh v1.4.0 [-m "commit message"] [--no-test] [--dry-run]
#
# Before: write documents/release-notes/<version>.md.
#
# What it does:
#   1. checks: version format, release notes exist, the tag isn't taken,
#      the branch is up to date with origin
#   2. runs the PC tests (make test in tools/hipiview) -- unless --no-test
#   3. commits what's not committed yet (asks first; -m gives the message)
#   4. makes the annotated tag and pushes the branch and the tag
#
# The push of the tag starts the GitHub workflows: build-hipi.yml builds
# the firmware and the SD card zip and makes the release, with the release
# notes as its text; build-hipiview.yml adds hipiview for Windows.
#
# --dry-run: only checks and says what it would do.

set -u

die() { echo "FEL: $*" >&2; exit 1; }

# ── Arguments ─────────────────────────────────────────────────────────────
VERSION=""
MESSAGE=""
RUN_TESTS=true
DRY_RUN=false
while [ $# -gt 0 ]; do
    case "$1" in
        -m)        shift; [ $# -gt 0 ] || die "-m behöver ett meddelande"; MESSAGE="$1" ;;
        --no-test) RUN_TESTS=false ;;
        --dry-run) DRY_RUN=true ;;
        -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        v*)        VERSION="$1" ;;
        *)         die "okänt argument: $1 (se --help)" ;;
    esac
    shift
done
[ -n "$VERSION" ] || die "ange versionen, t.ex.: scripts/release.sh v1.4.0"
# v<major>.<minor>.<patch>, optionally -rc<n> (a pre-release)
[[ "$VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-rc[0-9]+)?$ ]] || die "versionen ska se ut som v1.4.0 (eller v1.4.0-rc1)"

# Run from the repository's top folder, wherever it was started
ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || die "inte i ett git-repo"
cd "$ROOT" || exit 1

NOTES="documents/release-notes/${VERSION}.md"
BRANCH="$(git rev-parse --abbrev-ref HEAD)"

# ── 1. Checks ─────────────────────────────────────────────────────────────
echo "== Kontrollerar"
[ -f "$NOTES" ] || die "$NOTES saknas -- skriv release-texten först"
[ "$BRANCH" != "HEAD" ] || die "ingen gren utcheckad (detached HEAD)"
git rev-parse -q --verify "refs/tags/${VERSION}" >/dev/null && die "taggen ${VERSION} finns redan lokalt"
git fetch --quiet origin || die "git fetch misslyckades"
git ls-remote --exit-code --tags origin "refs/tags/${VERSION}" >/dev/null 2>&1 && die "taggen ${VERSION} finns redan på origin"
if git rev-parse -q --verify "origin/${BRANCH}" >/dev/null; then
    BEHIND="$(git rev-list --count "HEAD..origin/${BRANCH}")"
    [ "$BEHIND" -eq 0 ] || die "${BRANCH} ligger ${BEHIND} commit(s) efter origin -- gör git pull först"
fi
LAST_TAG="$(git describe --tags --abbrev=0 --match 'v[0-9]*' 2>/dev/null || echo '-')"
echo "   version:      ${VERSION} (förra: ${LAST_TAG})"
echo "   gren:         ${BRANCH}"
echo "   release-text: ${NOTES}"
[ "$BRANCH" = "main" ] || echo "   OBS: grenen är inte main"

# ── 2. Tests ──────────────────────────────────────────────────────────────
if $RUN_TESTS; then
    echo "== Kör testerna (tools/hipiview: make test)"
    if ! make -C tools/hipiview test > /tmp/hipi_release_test.log 2>&1; then
        tail -30 /tmp/hipi_release_test.log
        die "testerna gick inte igenom (hela loggen: /tmp/hipi_release_test.log)"
    fi
    echo "   alla tester gick igenom"
fi

# ── 3. Uncommitted changes ────────────────────────────────────────────────
if [ -n "$(git status --porcelain)" ]; then
    echo "== Ej incheckat:"
    git status --short
    [ -n "$MESSAGE" ] || MESSAGE="Release ${VERSION}"
    if $DRY_RUN; then
        echo "   (dry-run) skulle checka in allt ovan: \"${MESSAGE}\""
    else
        read -r -p "Checka in allt ovan med meddelandet \"${MESSAGE}\"? [j/N] " ok
        [[ "$ok" =~ ^[jJyY]$ ]] || die "avbrutet -- inget gjort"
        git add -A && git commit -q -m "$MESSAGE" || die "commit misslyckades"
        echo "   incheckat: $(git log -1 --oneline)"
    fi
fi
# The release notes must be in what's tagged (the workflow reads them from there)
if ! $DRY_RUN; then
    git ls-files --error-unmatch "$NOTES" >/dev/null 2>&1 || die "$NOTES är inte incheckad"
fi

# ── 4. Tag and push ───────────────────────────────────────────────────────
TITLE="HIPI ${VERSION#v}"
echo "== Taggar och pushar"
echo "   commit: $(git log -1 --oneline)"
echo "   tagg:   ${VERSION} (\"${TITLE}\")"
echo "   push:   ${BRANCH} och ${VERSION} till origin"
if $DRY_RUN; then
    echo "   (dry-run) inget taggat eller pushat"
    exit 0
fi
read -r -p "Tagga och pusha nu? [j/N] " ok
[[ "$ok" =~ ^[jJyY]$ ]] || die "avbrutet -- inget taggat eller pushat"
git tag -a "$VERSION" -m "$TITLE" || die "kunde inte skapa taggen"
git push origin "$BRANCH" || die "push av ${BRANCH} misslyckades (taggen finns lokalt: git tag -d ${VERSION})"
git push origin "$VERSION" || die "push av taggen misslyckades (försök igen: git push origin ${VERSION})"

# ── Done ──────────────────────────────────────────────────────────────────
URL="$(git remote get-url origin | sed -e 's#^git@github.com:#https://github.com/#' -e 's#\.git$##')"
echo
echo "Klart: ${VERSION} är pushad. GitHub bygger och skapar releasen nu:"
echo "   ${URL}/actions"
echo "   ${URL}/releases/tag/${VERSION}"
if command -v gh >/dev/null 2>&1; then
    echo "   (gh run watch -- följ bygget härifrån)"
fi
