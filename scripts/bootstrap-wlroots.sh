#!/usr/bin/env bash
# scripts/bootstrap-wlroots.sh
#
# Bootstrap the SC7Labs-patched wlroots build required by SC7Labs Rack.
#
# This script:
#   1. Reads the pinned upstream wlroots base SHA from patches/WLROOTS_BASE_REVISION
#   2. Clones official wlroots if vendor/wlroots is absent or incomplete
#   3. Verifies a pinned-base, previous-patch, or current-patch checkout
#   4. Applies the full patch on a fresh checkout, or the incremental DnD fix
#   5. Builds libwlroots.so.12 into vendor/wlroots/build/
#   6. Verifies the built library has the DnD, Wayland, Pixman, EGL, GLES2,
#      and GBM capabilities Rack needs
#
# Safety guarantees:
#   - System wlroots (/usr/lib/libwlroots*) is NEVER modified
#   - /usr/bin/sway is NEVER modified
#   - vendor/wlroots/ stays local to the repo — never installed system-wide
#   - Script is safe to re-run: a current source and matching stamped build
#     skip clone/patch/build; an interrupted upgrade rebuilds the library
#   - Fails loudly on any SHA mismatch, patch failure, or build failure
#   - Never silently falls back to an unpatched build
#
# Usage:
#   scripts/bootstrap-wlroots.sh [--force-rebuild] [--install-deps]
#
#   --force-rebuild  Rebuild even if the verified library already exists
#   --install-deps    Install missing build inputs on supported apt systems
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

WLROOTS_DIR="$REPO_ROOT/vendor/wlroots"
BUILD_DIR="$WLROOTS_DIR/build"
PATCH_FILE="$REPO_ROOT/patches/wlroots-sc7labs-rack.patch"
LEGACY_PATCH_ID_FILE="$REPO_ROOT/patches/WLROOTS_LEGACY_PATCH_ID"
MIGRATION_PATCH_FILE="$REPO_ROOT/patches/wlroots-dnd-lifetime-fix.patch"
BASE_REV_FILE="$REPO_ROOT/patches/WLROOTS_BASE_REVISION"
UPSTREAM_URL="https://gitlab.freedesktop.org/wlroots/wlroots.git"

FORCE_REBUILD=false
INSTALL_DEPS=false
for arg in "$@"; do
    case "$arg" in
        --force-rebuild) FORCE_REBUILD=true ;;
        --install-deps) INSTALL_DEPS=true ;;
        *) echo "Unknown bootstrap option: $arg" >&2; exit 2 ;;
    esac
done

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

verify_patched_source() {
    local current_head actual_patch_id
    current_head="$(git -C "$WLROOTS_DIR" rev-parse HEAD 2>/dev/null)" \
        || die "Could not read wlroots HEAD"
    git -C "$WLROOTS_DIR" merge-base --is-ancestor "$PINNED_SHA" "$current_head" \
        || die "wlroots HEAD is not descended from the pinned base $PINNED_SHA"
    [[ "$current_head" != "$PINNED_SHA" ]] \
        || die "wlroots is still at the unpatched pinned base"
    git -C "$WLROOTS_DIR" diff --quiet && git -C "$WLROOTS_DIR" diff --cached --quiet \
        || die "wlroots has uncommitted tracked source changes"
    actual_patch_id="$(git -C "$WLROOTS_DIR" diff --binary "$PINNED_SHA" "$current_head" |
        git patch-id --stable | awk '{print $1}')"
    SOURCE_PATCH_ID="$actual_patch_id"
    [[ -n "$SOURCE_PATCH_ID" &&
       ( "$SOURCE_PATCH_ID" == "$EXPECTED_PATCH_ID" || "$SOURCE_PATCH_ID" == "$LEGACY_PATCH_ID" ) ]] \
        || die "wlroots changes do not match the current or supported legacy SC7Labs patch"
}

verify_library_features() {
    local symbols symbol
    symbols="$(nm -D "$BUILT_LIB" 2>/dev/null)" || return 1
    # These exported functions come from distinct wlroots source units. Their
    # presence proves the configured renderer/backend code reached the library,
    # rather than merely finding an old .so with the DnD patch.
    for symbol in \
        wlr_wl_backend_find_by_display \
        wlr_wl_backend_create \
        wlr_pixman_renderer_create \
        wlr_egl_create_with_context \
        wlr_gles2_renderer_create \
        wlr_gbm_allocator_create; do
        if ! grep -E "[[:space:]][TtWw][[:space:]]${symbol}$" <<< "$symbols" >/dev/null; then
            info "Missing required wlroots capability: $symbol"
            return 1
        fi
    done
}

# ─── Step 0: Validate source files ───────────────────────────────────────────

[[ -f "$BASE_REV_FILE" ]] || die "Missing: $BASE_REV_FILE"
[[ -f "$PATCH_FILE" ]]    || die "Missing: $PATCH_FILE"
[[ -f "$LEGACY_PATCH_ID_FILE" ]] || die "Missing: $LEGACY_PATCH_ID_FILE"
[[ -f "$MIGRATION_PATCH_FILE" ]] || die "Missing: $MIGRATION_PATCH_FILE"
DEPS_HELPER="$SCRIPT_DIR/wlroots_build_deps.py"
[[ -x "$DEPS_HELPER" ]] || die "Missing build dependency checker: $DEPS_HELPER"

# Git is needed to authenticate an existing build before the normal cache
# check. A fresh GitHub clone already has it, but archive installs may not.
if ! command -v git >/dev/null 2>&1; then
    if [[ "$INSTALL_DEPS" == "true" ]]; then
        "$DEPS_HELPER" --ensure || die "Required wlroots build dependencies are unavailable"
    else
        "$DEPS_HELPER" --check || die "Required wlroots build dependencies are unavailable"
    fi
fi

PINNED_SHA="$(tr -d '[:space:]' < "$BASE_REV_FILE")"
[[ -n "$PINNED_SHA" ]] || die "WLROOTS_BASE_REVISION is empty"
[[ "${#PINNED_SHA}" -eq 40 ]] || die "WLROOTS_BASE_REVISION does not look like a full SHA-1: '$PINNED_SHA'"

info "Pinned wlroots base revision: $PINNED_SHA"
EXPECTED_PATCH_ID="$(git patch-id --stable < "$PATCH_FILE" | awk '{print $1}')"
[[ -n "$EXPECTED_PATCH_ID" ]] || die "Could not identify the tracked wlroots patch"
LEGACY_PATCH_ID="$(tr -d '[:space:]' < "$LEGACY_PATCH_ID_FILE")"
[[ "$LEGACY_PATCH_ID" =~ ^[0-9a-f]{40}$ ]] || die "Legacy wlroots patch ID is invalid"

# ─── Step 1: Check build cache (skip if already done and not forced) ──────────

BUILT_LIB="$BUILD_DIR/libwlroots.so.12"
BUILT_PATCH_ID_FILE="$BUILD_DIR/.sc7-patch-id"
if [[ "$FORCE_REBUILD" == "false" && -f "$BUILT_LIB" ]]; then
    [[ -d "$WLROOTS_DIR/.git" ]] \
        || die "Built library exists without a verifiable wlroots Git checkout"
    verify_patched_source
    built_patch_id=""
    if [[ -f "$BUILT_PATCH_ID_FILE" ]]; then
        built_patch_id="$(tr -d '[:space:]' < "$BUILT_PATCH_ID_FILE")"
    fi
    if [[ "$SOURCE_PATCH_ID" == "$EXPECTED_PATCH_ID" &&
          "$built_patch_id" == "$EXPECTED_PATCH_ID" ]] &&
       command -v nm >/dev/null 2>&1 && verify_library_features; then
        info "Verified patched build already present — skipping rebuild."
        info "  Built library: $BUILT_LIB"
        exit 0
    fi
    info "Existing library lacks required features or cannot be verified — reconfiguring and rebuilding."
fi

# Check/install build inputs before cloning or changing the wlroots checkout.
if [[ "$INSTALL_DEPS" == "true" ]]; then
    "$DEPS_HELPER" --ensure || die "Required wlroots build dependencies are unavailable"
else
    "$DEPS_HELPER" --check || die "Required wlroots build dependencies are unavailable"
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
    verify_patched_source
    if [[ "$SOURCE_PATCH_ID" == "$LEGACY_PATCH_ID" ]]; then
        info "Upgrading the authentic previous SC7Labs DnD patch in place..."
        git -C "$WLROOTS_DIR" apply --check "$MIGRATION_PATCH_FILE" \
            || die "Legacy DnD lifetime patch does not apply cleanly"
        git -C "$WLROOTS_DIR" apply --index "$MIGRATION_PATCH_FILE" \
            || die "Applying DnD lifetime fix failed"
        upgraded_patch_id="$(git -C "$WLROOTS_DIR" diff --cached --binary "$PINNED_SHA" |
            git patch-id --stable | awk '{print $1}')"
        if [[ "$upgraded_patch_id" != "$EXPECTED_PATCH_ID" ]]; then
            git -C "$WLROOTS_DIR" apply --reverse --index "$MIGRATION_PATCH_FILE" \
                || die "Upgraded source mismatch and rollback of staged DnD fix failed"
            die "Upgraded wlroots source does not match the current SC7Labs patch"
        fi
        git -C "$WLROOTS_DIR" commit --no-gpg-sign \
            -m "fix(wlroots): release DnD state after selection and drag" \
            || die "Committing DnD lifetime fix failed"
        verify_patched_source
        [[ "$SOURCE_PATCH_ID" == "$EXPECTED_PATCH_ID" ]] \
            || die "Upgraded wlroots source does not match the current SC7Labs patch"
        info "Legacy DnD patch upgraded without recloning or reapplying the base patch."
    else
        info "Authentic SC7Labs patch already committed — continuing to build."
    fi
else
    # HEAD == base SHA: apply the patch
    info "Applying SC7Labs patch..."
    git -C "$WLROOTS_DIR" diff --quiet && git -C "$WLROOTS_DIR" diff --cached --quiet \
        || die "wlroots has uncommitted tracked source changes"

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
    verify_patched_source
    [[ "$SOURCE_PATCH_ID" == "$EXPECTED_PATCH_ID" ]] \
        || die "Freshly patched wlroots source does not match the current SC7Labs patch"
fi

# ─── Step 4: Configure with Meson ─────────────────────────────────────────────

if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    info "Configuring wlroots build with meson..."
    # wlroots 0.17.4 builds the nested Wayland backend and Pixman renderer
    # unconditionally. Require the GLES2 renderer and GBM allocator so Meson
    # fails instead of silently producing a software-only library.
    # Do NOT pass -Dbackends=wayland — that value is not valid in 0.17.4.
    # Do NOT pass --prefix=/usr — build stays local, never installed system-wide.
    if ! meson setup \
        --buildtype=release \
        -Dexamples=false \
        -Drenderers=gles2 \
        -Dallocators=gbm \
        "$BUILD_DIR" \
        "$WLROOTS_DIR"; then
        # A prior interrupted setup may leave Meson metadata without a build
        # graph. Wipe only this generated build directory and retry setup.
        [[ -d "$BUILD_DIR/meson-private" ]] \
            || die "meson setup failed"
    fi
    # Meson can return success for an already-configured directory even when
    # an interrupted build left build.ninja missing.
    if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
        [[ -d "$BUILD_DIR/meson-private" ]] \
            || die "meson setup did not create build.ninja"
        info "Recovering incomplete Meson configuration..."
        meson setup --wipe --buildtype=release -Dexamples=false \
            -Drenderers=gles2 -Dallocators=gbm \
            "$BUILD_DIR" "$WLROOTS_DIR" \
            || die "meson setup failed after recovering incomplete build"
    fi
    [[ -f "$BUILD_DIR/build.ninja" ]] || die "meson setup did not create build.ninja"
    info "Meson configuration complete."
else
    # An old build graph can exist with an incomplete renderer even when
    # libwlroots.so.12 is absent (an interrupted Ninja build). Wipe only
    # generated Meson output for every rebuild so newly installed EGL/GBM/
    # GLES2 packages are probed afresh. The patched source is preserved.
    info "Refreshing wlroots Meson configuration with fresh dependency probes..."
    meson setup --wipe --buildtype=release -Dexamples=false \
        -Drenderers=gles2 -Dallocators=gbm "$BUILD_DIR" "$WLROOTS_DIR" \
        || die "meson setup failed while refreshing renderer features"
fi

# ─── Step 5: Build ───────────────────────────────────────────────────────────

info "Building patched wlroots..."
ninja -C "$BUILD_DIR" \
    || die "ninja build failed"

# ─── Step 6: Verify output ───────────────────────────────────────────────────

if [[ ! -f "$BUILT_LIB" ]]; then
    die "Build completed but $BUILT_LIB not found — unexpected build output"
fi

LIB_SIZE="$(stat -c%s "$BUILT_LIB")"
info "Build successful."
info "  Library: $BUILT_LIB (${LIB_SIZE} bytes)"

# Verify all runtime-critical renderer/backend functions in the final library.
if ! verify_library_features; then
    die "Built library is missing required Wayland, Pixman, EGL, GLES2, GBM, or SC7Labs DnD features"
fi

# A previous libwlroots can have every exported symbol while still containing
# old DnD code. Mark the build only after Ninja and feature verification, so
# an interrupted source upgrade can never be accepted as a cached build.
stamp_tmp="$(mktemp "$BUILD_DIR/.sc7-patch-id.XXXXXXXX")" \
    || die "Could not create wlroots build verification stamp"
printf '%s\n' "$EXPECTED_PATCH_ID" > "$stamp_tmp" \
    || die "Could not write wlroots build verification stamp"
mv -f -- "$stamp_tmp" "$BUILT_PATCH_ID_FILE" \
    || die "Could not publish wlroots build verification stamp"

info "  DnD, Wayland, Pixman, EGL, GLES2, and GBM capabilities verified."
info ""
info "SC7Labs patched wlroots is ready."
info "Launcher uses: LD_LIBRARY_PATH=$BUILD_DIR"
