#!/usr/bin/env bash
# scripts/bootstrap-wlroots.sh
#
# Bootstrap the SC7Labs-patched wlroots build required by SC7Labs Rack.
#
# This script:
#   1. Reads the pinned upstream wlroots base SHA from patches/WLROOTS_BASE_REVISION
#   2. Clones official wlroots if vendor/wlroots is absent or incomplete
#   3. Verifies HEAD matches the pinned SHA BEFORE patching
#   4. Applies patches/wlroots-sc7labs-rack.patch
#   5. Builds libwlroots.so.12 into vendor/wlroots/build/
#   6. Verifies the built library is present
#
# Safety guarantees:
#   - System wlroots (/usr/lib/libwlroots*) is NEVER modified
#   - /usr/bin/sway is NEVER modified
#   - vendor/wlroots/ stays local to the repo — never installed system-wide
#   - Script is safe to re-run: skips clone/patch/build if already done
#   - Fails loudly on any SHA mismatch, patch failure, or build failure
#   - Never silently falls back to an unpatched build
#
# Usage:
#   scripts/bootstrap-wlroots.sh [--force-rebuild]
#
#   --force-rebuild  Re-apply patch and rebuild even if library already exists
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

WLROOTS_DIR="$REPO_ROOT/vendor/wlroots"
BUILD_DIR="$WLROOTS_DIR/build"
PATCH_FILE="$REPO_ROOT/patches/wlroots-sc7labs-rack.patch"
BASE_REV_FILE="$REPO_ROOT/patches/WLROOTS_BASE_REVISION"
UPSTREAM_URL="https://gitlab.freedesktop.org/wlroots/wlroots.git"

FORCE_REBUILD=false
if [[ "${1:-}" == "--force-rebuild" ]]; then
    FORCE_REBUILD=true
fi

# ─── Helpers ─────────────────────────────────────────────────────────────────

die() {
    echo "" >&2
    echo "════════════════════════════════════════════════════════════" >&2
    echo "  BOOTSTRAP ERROR: $*" >&2
    echo "════════════════════════════════════════════════════════════" >&2
    echo "" >&2
    echo "  SC7Labs Rack DnD requires a patched local wlroots build." >&2
    echo "  The system wlroots was NOT modified." >&2
    echo "════════════════════════════════════════════════════════════" >&2
    exit 1
}

info() { echo "  [bootstrap-wlroots] $*"; }

# ─── Step 0: Validate source files ───────────────────────────────────────────

[[ -f "$BASE_REV_FILE" ]] || die "Missing: $BASE_REV_FILE"
[[ -f "$PATCH_FILE" ]]    || die "Missing: $PATCH_FILE"

PINNED_SHA="$(tr -d '[:space:]' < "$BASE_REV_FILE")"
[[ -n "$PINNED_SHA" ]] || die "WLROOTS_BASE_REVISION is empty"
[[ "${#PINNED_SHA}" -eq 40 ]] || die "WLROOTS_BASE_REVISION does not look like a full SHA-1: '$PINNED_SHA'"

info "Pinned wlroots base revision: $PINNED_SHA"

# ─── Step 1: Check build cache (skip if already done and not forced) ──────────

BUILT_LIB="$BUILD_DIR/libwlroots.so.12"
if [[ "$FORCE_REBUILD" == "false" && -f "$BUILT_LIB" ]]; then
    # Verify the existing build came from the right patched source
    if [[ -d "$WLROOTS_DIR/.git" ]]; then
        EXISTING_HEAD="$(git -C "$WLROOTS_DIR" rev-parse HEAD 2>/dev/null || true)"
        EXISTING_BRANCH="$(git -C "$WLROOTS_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
        info "Existing build found (HEAD=$EXISTING_HEAD branch=$EXISTING_BRANCH)"
        # Accept if the HEAD is not the bare base (i.e. patch has been applied)
        if [[ "$EXISTING_HEAD" != "$PINNED_SHA" ]]; then
            info "Patched build already present — skipping rebuild."
            info "  Built library: $BUILT_LIB"
            exit 0
        else
            info "Warning: HEAD is at base SHA (patch not applied). Will re-patch and rebuild."
        fi
    else
        info "Existing build found (no .git). Skipping rebuild."
        exit 0
    fi
fi

# ─── Step 2: Clone wlroots if not present ────────────────────────────────────

if [[ ! -d "$WLROOTS_DIR/.git" ]]; then
    info "vendor/wlroots not present — cloning from upstream..."
    info "  URL: $UPSTREAM_URL"
    info "  SHA: $PINNED_SHA"

    # Clone shallowly up to the pinned tag (0.17.4 = a2d2c38...)
    # We clone the 0.17.4 tag to get a shallow history containing the base SHA
    git clone \
        --filter=blob:none \
        --no-checkout \
        "$UPSTREAM_URL" \
        "$WLROOTS_DIR" \
        || die "git clone failed"

    # Fetch the exact commit
    git -C "$WLROOTS_DIR" fetch --depth=1 origin "$PINNED_SHA" \
        || die "Could not fetch pinned SHA $PINNED_SHA from upstream"

    git -C "$WLROOTS_DIR" checkout FETCH_HEAD \
        || die "git checkout of pinned SHA failed"

    git -C "$WLROOTS_DIR" checkout -b working-dnd-transport \
        || die "Could not create working-dnd-transport branch"

    info "Clone complete."
fi

# ─── Step 3: Verify HEAD matches pinned SHA before patching ──────────────────

CURRENT_HEAD="$(git -C "$WLROOTS_DIR" rev-parse HEAD)"
CURRENT_BRANCH="$(git -C "$WLROOTS_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "detached")"

info "Current wlroots HEAD:   $CURRENT_HEAD (branch: $CURRENT_BRANCH)"

if [[ "$CURRENT_HEAD" != "$PINNED_SHA" ]]; then
    # HEAD is not the base — check if the base is an ancestor (patch already applied)
    if git -C "$WLROOTS_DIR" merge-base --is-ancestor "$PINNED_SHA" HEAD 2>/dev/null; then
        info "Base SHA is ancestor of HEAD — patch appears already applied."
        # Still need to build if library missing
    else
        die "HEAD ($CURRENT_HEAD) is not the pinned base ($PINNED_SHA) and is not a descendant. Refusing to patch."
    fi
else
    # HEAD == base SHA: apply the patch
    info "Applying SC7Labs patch..."

    # Dry-run first
    git -C "$WLROOTS_DIR" apply --check "$PATCH_FILE" \
        || die "Patch does not apply cleanly to $PINNED_SHA — patch mismatch."

    git -C "$WLROOTS_DIR" apply "$PATCH_FILE" \
        || die "Patch application failed."

    git -C "$WLROOTS_DIR" add -A \
        || die "git add after patch failed"

    git -C "$WLROOTS_DIR" commit \
        --no-gpg-sign \
        -m "build(wlroots): apply SC7Labs Rack patches" \
        || die "git commit after patch failed"

    info "Patch applied and committed."
fi

# ─── Step 4: Check build dependencies ────────────────────────────────────────

MISSING_BUILD_DEPS=()
for dep in meson ninja gcc pkg-config; do
    if ! command -v "$dep" >/dev/null 2>&1; then
        MISSING_BUILD_DEPS+=("$dep")
    fi
done

# Check pkg-config dependencies
for pc_dep in wayland-server wayland-client wlroots; do
    # wlroots itself won't exist yet, skip its own pc check
    [[ "$pc_dep" == "wlroots" ]] && continue
    if ! pkg-config --exists "$pc_dep" 2>/dev/null; then
        MISSING_BUILD_DEPS+=("pkg:$pc_dep")
    fi
done

# Check for wayland-protocols and xkbcommon
for pc_dep in wayland-protocols xkbcommon; do
    if ! pkg-config --exists "$pc_dep" 2>/dev/null; then
        MISSING_BUILD_DEPS+=("pkg:$pc_dep")
    fi
done

if [[ ${#MISSING_BUILD_DEPS[@]} -gt 0 ]]; then
    echo "" >&2
    echo "  Missing build dependencies:" >&2
    for dep in "${MISSING_BUILD_DEPS[@]}"; do
        echo "    ✗ $dep" >&2
    done
    echo "" >&2
    echo "  On Ubuntu/Debian, install with:" >&2
    echo "    sudo apt install meson ninja-build gcc pkg-config \\" >&2
    echo "      libwayland-dev libxkbcommon-dev wayland-protocols \\" >&2
    echo "      libegl-dev libgles2-mesa-dev libgbm-dev libdrm-dev \\" >&2
    echo "      libinput-dev libudev-dev libpixman-1-dev libseat-dev \\" >&2
    echo "      libxcb1-dev libxcb-composite0-dev libxcb-icccm4-dev \\" >&2
    echo "      libxcb-render0-dev libxcb-res0-dev libxcb-xfixes0-dev \\" >&2
    echo "      libxcb-xinput-dev libx11-dev libx11-xcb-dev hwdata" >&2
    die "Missing build dependencies — cannot build patched wlroots"
fi

# ─── Step 5: Configure with meson ────────────────────────────────────────────

if [[ ! -f "$BUILD_DIR/build.ninja" ]] || [[ "$FORCE_REBUILD" == "true" ]]; then
    info "Configuring wlroots build with meson..."
    # wlroots 0.17.4: only -Dexamples=false is needed.
    # The wayland backend is auto-detected when wayland-client is present.
    # Do NOT pass -Dbackends=wayland — that value is not valid in 0.17.4.
    # Do NOT pass --prefix=/usr — build stays local, never installed system-wide.
    meson setup \
        --buildtype=release \
        -Dexamples=false \
        "$BUILD_DIR" \
        "$WLROOTS_DIR" \
        || die "meson setup failed"
    info "Meson configuration complete."
fi

# ─── Step 6: Build ───────────────────────────────────────────────────────────

info "Building patched wlroots..."
ninja -C "$BUILD_DIR" \
    || die "ninja build failed"

# ─── Step 7: Verify output ───────────────────────────────────────────────────

if [[ ! -f "$BUILT_LIB" ]]; then
    die "Build completed but $BUILT_LIB not found — unexpected build output"
fi

LIB_SIZE="$(stat -c%s "$BUILT_LIB")"
info "Build successful."
info "  Library: $BUILT_LIB (${LIB_SIZE} bytes)"

# Verify the DnD symbol is present in the built library
if ! nm -D "$BUILT_LIB" 2>/dev/null | grep -q "wlr_wl_backend_find_by_display"; then
    die "Built library missing SC7Labs DnD symbol 'wlr_wl_backend_find_by_display' — patch may not have been applied"
fi

info "  DnD symbol verified in built library."
info ""
info "SC7Labs patched wlroots is ready."
info "Launcher uses: LD_LIBRARY_PATH=$BUILD_DIR"
