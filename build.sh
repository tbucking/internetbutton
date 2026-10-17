#!/usr/bin/env bash
#
# Build one of the firmware projects, and optionally flash it.
#
#   ./build.sh simon                 build
#   ./build.sh simon --flash         build, verify, flash
#   ./build.sh --list                show projects
#
# WHY THIS SCRIPT EXISTS. Two things are easy to get wrong here, and one of them
# is dangerous:
#
#   1. Omitting --target. The Particle CLI's default compile target for a Photon
#      is Device OS 3.3.1. Flashing a binary built for that makes the cloud
#      Safe Mode healer replace the device's Device OS over the air, with no USB
#      activity, so it looks like the firmware corrupted itself. docs/INIT.md §6
#      reproduces it. Everything here is built for 2.3.1 and this script refuses
#      to produce a binary that fails the dependency check.
#
#   2. Forgetting to sync the vendored library. The InternetButton library lives
#      once in vendor/ and is copied into each project, because `particle
#      compile` does not follow symlinks unless --followSymlinks is passed.
#      Copying is done here, every time, so it cannot drift.
#
# It does NOT use `particle library add`, which hangs on this setup (no output,
# exit code 124) and creates nothing.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE="$REPO/firmware"
VENDOR="$REPO/vendor/InternetButton"
TARGET="2.3.1"
# "system module number 2" is Device OS; 2302 == 2.3.1
WANT_MODULE="2302"
DEVICE="${PARTICLE_DEVICE:-1f0034000e47343432313031}"

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
warn() { printf '\033[33mwarn:\033[0m %s\n'  "$*" >&2; }
info() { printf '  %s\n' "$*"; }

# --------------------------------------------------------------------- arg parsing

FLASH=0
PROJECT=""
for arg in "$@"; do
  case "$arg" in
    --flash) FLASH=1 ;;
    --list)  ls -1 "$FIRMWARE" | sed 's/^/  /'; exit 0 ;;
    -h|--help) sed -n '2,10p' "$0" | sed 's/^# \?//'; exit 0 ;;
    -*)      die "unknown option: $arg (try --help)" ;;
    *)       [ -z "$PROJECT" ] || die "only one project at a time" ; PROJECT="$arg" ;;
  esac
done

[ -n "$PROJECT" ] || { printf 'usage: %s <project> [--flash]\n\nprojects:\n' "$0"; ls -1 "$FIRMWARE" | sed 's/^/  /'; exit 1; }

PROJDIR="$FIRMWARE/$PROJECT"

# ------------------------------------------------------------------ preflight

# `particle` is installed to ~/bin by the installer but that is not always on PATH
if ! command -v particle >/dev/null 2>&1; then
  if [ -x "$HOME/bin/particle" ]; then
    PATH="$HOME/bin:$PATH"
  else
    die "particle CLI not found (looked on PATH and in ~/bin)"
  fi
fi

[ -d "$PROJDIR" ]      || die "no such project: $PROJECT (try --list)"
[ -f "$PROJDIR/project.properties" ] || die "$PROJECT has no project.properties"
[ -d "$PROJDIR/src" ]  || die "$PROJECT has no src/ directory"
[ -d "$VENDOR" ]       || die "vendor/InternetButton is missing"

# ------------------------------------------------------- sync the vendored library

echo "==> $PROJECT"
LIBDEST="$PROJDIR/lib/InternetButton"

# Checksums rather than file counts: a count is identical for a complete copy and
# a truncated one, so it cannot tell those apart. This can.
checksum() { find "$1" -type f -print0 | sort -z | xargs -0 cat | md5sum | cut -d' ' -f1; }

want=$(checksum "$VENDOR")

if [ -e "$LIBDEST" ] && ! [ -d "$LIBDEST" ]; then
  rm -f "$LIBDEST"                      # in case an older symlink is present
fi
rm -rf "$LIBDEST"
mkdir -p "$(dirname "$LIBDEST")"
cp -r "$VENDOR" "$LIBDEST"

got=$(checksum "$LIBDEST")
if [ "$want" != "$got" ]; then
  die "library sync mismatch: vendor=$want dest=$got"
fi
info "library: $(find "$LIBDEST" -type f | wc -l) files synced from vendor/ (checksum verified)"

# -------------------------------------------------------------------- compile

OUT="${TMPDIR:-/tmp}/$PROJECT.bin"
info "compiling for Device OS $TARGET ..."

set +e
particle compile photon "$PROJDIR" --target "$TARGET" --saveTo "$OUT" >/tmp/build-$$.log 2>&1
rc=$?
set -e

if [ $rc -ne 0 ]; then
  echo "--- compiler output (tail) ---" >&2
  tail -25 /tmp/build-$$.log >&2
  rm -f /tmp/build-$$.log
  die "compile failed"
fi

if ! grep -q "Targeting version: $TARGET" /tmp/build-$$.log; then
  # the flag not taking effect is exactly the failure this script exists to prevent
  rm -f /tmp/build-$$.log
  die "build did not report 'Targeting version: $TARGET' — refusing to continue"
fi

grep -E 'Memory use:|Flash|RAM|^ +[0-9]+' /tmp/build-$$.log | tail -3 | sed 's/^/  /'
rm -f /tmp/build-$$.log

[ -f "$OUT" ] || die "compile reported success but produced no binary"

# --------------------------------------------- verify the dependency before it can ship

inspect=$(particle binary inspect "$OUT" 2>&1 || true)
module=$(printf '%s\n' "$inspect" | grep -oE 'module number 2 at version [0-9]+' | grep -oE '[0-9]+$' | head -1)

if [ -z "$module" ]; then
  printf '%s\n' "$inspect" >&2
  die "could not read the Device OS dependency from the binary"
fi

if [ "$module" != "$WANT_MODULE" ]; then
  die "binary depends on Device OS module $module, expected $WANT_MODULE — flashing it would trigger the Safe Mode healer and upgrade the device. NOT flashing."
fi

info "verified: depends on Device OS $TARGET (module $module)"
info "binary: $OUT"

# ------------------------------------------------------------------- optional flash

if [ "$FLASH" = "1" ]; then
  echo "==> flashing $DEVICE"
  particle flash "$DEVICE" "$OUT"
else
  info "not flashing (pass --flash to flash $DEVICE)"
fi
