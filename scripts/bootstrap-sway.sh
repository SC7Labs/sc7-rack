#!/usr/bin/env bash
# Build a pinned Sway 1.9 with the upstream popup lifecycle fix, using only
# Rack's private wlroots library. Neither system Sway nor system wlroots change.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="$ROOT/vendor/sway"
BUILD="$SOURCE/build"
SWAY="$BUILD/sway/sway"
PATCH="$ROOT/patches/sway-popup-lifecycle.patch"
BASE_FILE="$ROOT/patches/SWAY_BASE_REVISION"
WLROOTS_BUILD="$ROOT/vendor/wlroots/build"
WLROOTS_LIB="$WLROOTS_BUILD/libwlroots.so.12"
WLROOTS_STAMP="$WLROOTS_BUILD/.sc7-patch-id"
STAMP="$BUILD/.sc7-patch-id"
INSTALL_DEPS=false
FORCE_REBUILD=false
CHECK_ONLY=false

die() { echo "SC7 Rack Sway: $*" >&2; exit 1; }
info() { echo "SC7 Rack Sway: $*"; }

for arg in "$@"; do
    case "$arg" in
        --install-deps) INSTALL_DEPS=true ;;
        --force-rebuild) FORCE_REBUILD=true ;;
        --check) CHECK_ONLY=true ;;
        *) die "Unknown option: $arg" ;;
    esac
done
if [[ "$CHECK_ONLY" == true && ( "$INSTALL_DEPS" == true || "$FORCE_REBUILD" == true ) ]]; then
    die "--check cannot be combined with build options"
fi

[[ -f "$BASE_FILE" && -f "$PATCH" ]] || die "Pinned source or popup patch is missing"
[[ -f "$WLROOTS_LIB" && -f "$WLROOTS_STAMP" ]] ||
    die "Rack-local wlroots is missing; run ./scripts/bootstrap-wlroots.sh first"
BASE_SHA="$(tr -d '[:space:]' < "$BASE_FILE")"
[[ "$BASE_SHA" =~ ^[0-9a-f]{40}$ ]] || die "Invalid pinned Sway revision"
SWAY_PATCH_ID="$(git patch-id --stable < "$PATCH" | awk '{print $1}')"
# The first VM bootstrap committed the popup handlers but left the protocol
# version change unstaged. Identify that precise intermediate source state from
# the tracked full patch so it can be completed without accepting other edits.
PARTIAL_PATCH_ID="$(awk '/^diff --git a\/sway\/server\.c b\/sway\/server\.c$/ { exit } { print }' "$PATCH" |
    git patch-id --stable | awk '{print $1}')"
WLROOTS_PATCH_ID="$(git patch-id --stable < "$ROOT/patches/wlroots-sc7labs-rack.patch" | awk '{print $1}')"
[[ "$SWAY_PATCH_ID" =~ ^[0-9a-f]{40}$ && "$PARTIAL_PATCH_ID" =~ ^[0-9a-f]{40}$ &&
   "$SWAY_PATCH_ID" != "$PARTIAL_PATCH_ID" && "$WLROOTS_PATCH_ID" =~ ^[0-9a-f]{40}$ ]] ||
    die "Could not identify tracked patches"
[[ "$(tr -d '[:space:]' < "$WLROOTS_STAMP")" == "$WLROOTS_PATCH_ID" ]] ||
    die "Rack-local wlroots build is stale; rerun ./scripts/bootstrap-wlroots.sh"
EXPECTED_STAMP="$SWAY_PATCH_ID $WLROOTS_PATCH_ID"

prepare_expected_trees() {
    [[ -d "$SOURCE/.git" ]] || die "Sway source is not a Git checkout: $SOURCE"
    local temporary index
    temporary="$(mktemp -d "${TMPDIR:-/tmp}/sc7-sway-index.XXXXXXXX")" ||
        die "Could not create a temporary Sway index"
    index="$temporary/index"
    GIT_INDEX_FILE="$index" git -C "$SOURCE" read-tree "$BASE_SHA" ||
        die "Could not read the pinned Sway tree"
    awk '/^diff --git a\/sway\/server\.c b\/sway\/server\.c$/ { exit } { print }' "$PATCH" |
        GIT_INDEX_FILE="$index" git -C "$SOURCE" apply --cached - ||
        die "Could not reconstruct the previous partial popup patch"
    EXPECTED_PARTIAL_TREE="$(GIT_INDEX_FILE="$index" git -C "$SOURCE" write-tree)" ||
        die "Could not identify the previous partial Sway tree"
    GIT_INDEX_FILE="$index" git -C "$SOURCE" read-tree "$BASE_SHA" ||
        die "Could not reset the temporary Sway index"
    GIT_INDEX_FILE="$index" git -C "$SOURCE" apply --cached "$PATCH" ||
        die "Could not reconstruct the tracked popup patch"
    EXPECTED_FULL_TREE="$(GIT_INDEX_FILE="$index" git -C "$SOURCE" write-tree)" ||
        die "Could not identify the expected Sway tree"
    rm -f -- "$index"
    rmdir -- "$temporary"
}

staged_patch_id() {
    git -C "$SOURCE" diff --cached --binary "$BASE_SHA" |
        git patch-id --stable | awk '{print $1}'
}

recover_partial_popup_patch() {
    [[ -d "$SOURCE/.git" ]] || return 0
    git -C "$SOURCE" merge-base --is-ancestor "$BASE_SHA" HEAD || return 0
    local current_head actual_id
    current_head="$(git -C "$SOURCE" rev-parse HEAD)"
    [[ "$current_head" != "$BASE_SHA" ]] || return 0
    actual_id="$(git -C "$SOURCE" diff --binary "$BASE_SHA" HEAD |
        git patch-id --stable | awk '{print $1}')"
    [[ "$actual_id" == "$PARTIAL_PATCH_ID" &&
       "$(git -C "$SOURCE" rev-parse 'HEAD^{tree}')" == "$EXPECTED_PARTIAL_TREE" ]] || return 0
    git -C "$SOURCE" diff --cached --quiet || return 0
    [[ "$(git -C "$SOURCE" diff --name-only)" == sway/server.c ]] || return 0
    [[ "$(git -C "$SOURCE" show HEAD:sway/server.c |
        grep -c '^#define SWAY_XDG_SHELL_VERSION 2$')" == 1 ]] || return 0
    cmp -s "$SOURCE/sway/server.c" <(
        git -C "$SOURCE" show HEAD:sway/server.c |
            sed 's/^#define SWAY_XDG_SHELL_VERSION 2$/#define SWAY_XDG_SHELL_VERSION 3/'
    ) || return 0

    info "Completing the interrupted authenticated popup patch..."
    git -C "$SOURCE" add -- sway/server.c
    [[ "$(staged_patch_id)" == "$SWAY_PATCH_ID" ]] ||
        die "Recovered popup patch does not match the tracked patch"
    [[ "$(git -C "$SOURCE" write-tree)" == "$EXPECTED_FULL_TREE" ]] ||
        die "Recovered popup tree does not match the tracked patch"
    git -C "$SOURCE" diff --quiet ||
        die "Recovered popup patch left unstaged tracked changes"
    git -C "$SOURCE" -c user.name='SC7 Rack build' \
        -c user.email='build@sc7.invalid' commit --no-gpg-sign \
        -m 'fix(xdg-shell): expose popup reposition protocol' >/dev/null ||
        die "Could not commit recovered popup patch"
}

verify_source() {
    [[ -d "$SOURCE/.git" ]] || die "Sway source is not a Git checkout: $SOURCE"
    git -C "$SOURCE" diff --quiet && git -C "$SOURCE" diff --cached --quiet ||
        die "Sway has uncommitted tracked source changes"
    git -C "$SOURCE" merge-base --is-ancestor "$BASE_SHA" HEAD ||
        die "Sway source does not descend from the pinned revision"
    local current_head actual_id
    current_head="$(git -C "$SOURCE" rev-parse HEAD)"
    if [[ "$current_head" == "$BASE_SHA" ]]; then
        SOURCE_PATCH_ID=""
        return
    fi
    actual_id="$(git -C "$SOURCE" diff --binary "$BASE_SHA" HEAD | git patch-id --stable | awk '{print $1}')"
    [[ "$actual_id" == "$SWAY_PATCH_ID" &&
       "$(git -C "$SOURCE" rev-parse 'HEAD^{tree}')" == "$EXPECTED_FULL_TREE" ]] ||
        die "Sway source does not match the tracked popup patch"
    SOURCE_PATCH_ID="$actual_id"
}

verify_binary() {
    [[ -x "$SWAY" && -f "$STAMP" ]] || return 1
    [[ "$(cat "$STAMP")" == "$EXPECTED_STAMP" ]] || return 1
    local linkage
    linkage="$(LD_LIBRARY_PATH="$WLROOTS_BUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$SWAY")" || return 1
    [[ "$linkage" == *"$WLROOTS_LIB"* && "$linkage" != *"not found"* ]] || return 1
    LD_LIBRARY_PATH="$WLROOTS_BUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$SWAY" --version | grep -q '^sway version 1\.9-'
}

if [[ "$CHECK_ONLY" == true ]]; then
    prepare_expected_trees
    verify_source
    [[ "$SOURCE_PATCH_ID" == "$SWAY_PATCH_ID" ]] && verify_binary ||
        die "Rack-local Sway source or build is missing or stale"
    info "Verified pinned Sway, popup patch, and Rack-local wlroots linkage."
    exit 0
fi

if [[ ! -d "$SOURCE/.git" ]]; then
    [[ ! -e "$SOURCE" ]] || die "Unverified existing Sway directory: $SOURCE"
    mkdir -p "$ROOT/vendor"
    info "Cloning pinned Sway 1.9 into vendor/sway..."
    git clone --depth 1 --branch 1.9 https://github.com/swaywm/sway.git "$SOURCE" ||
        die "Sway clone failed"
    [[ "$(git -C "$SOURCE" rev-parse HEAD)" == "$BASE_SHA" ]] ||
        die "Sway 1.9 tag does not match pinned revision"
fi

prepare_expected_trees
recover_partial_popup_patch
verify_source
if [[ -z "$SOURCE_PATCH_ID" ]]; then
    git -C "$SOURCE" apply --check "$PATCH" || die "Popup patch does not apply"
    git -C "$SOURCE" apply "$PATCH"
    git -C "$SOURCE" add -- include/sway/tree/view.h sway/desktop/xdg_shell.c sway/server.c
    [[ "$(staged_patch_id)" == "$SWAY_PATCH_ID" ]] ||
        die "Staged popup patch does not match the tracked patch"
    [[ "$(git -C "$SOURCE" write-tree)" == "$EXPECTED_FULL_TREE" ]] ||
        die "Staged popup tree does not match the tracked patch"
    git -C "$SOURCE" diff --quiet ||
        die "Popup patch left unstaged tracked changes"
    git -C "$SOURCE" -c user.name='SC7 Rack build' \
        -c user.email='build@sc7.invalid' commit --no-gpg-sign \
        -m 'fix(xdg-shell): defer popup unconstrain until initial commit' >/dev/null ||
        die "Could not commit local popup patch"
    verify_source
    [[ "$SOURCE_PATCH_ID" == "$SWAY_PATCH_ID" ]] || die "Popup patch verification failed"
fi

if [[ "$FORCE_REBUILD" == false ]] && verify_binary; then
    info "Verified local Sway build already present."
    exit 0
fi

for tool in meson ninja pkg-config cc wayland-scanner ldd; do
    command -v "$tool" >/dev/null 2>&1 || die "Missing build tool: $tool"
done

# Meson's uninstalled pkg-config file points at the private headers/library,
# while its installed file carries the feature variables Sway requires. Merge
# those into a private file; no system pkg-config files are changed.
LOCAL_PC_DIR="$SOURCE/.sc7-pkgconfig"
mkdir -p "$LOCAL_PC_DIR"
[[ -f "$WLROOTS_BUILD/meson-uninstalled/wlroots-uninstalled.pc" &&
   -f "$WLROOTS_BUILD/meson-private/wlroots.pc" ]] ||
    die "Rack-local wlroots pkg-config metadata is missing"
pc_tmp="$(mktemp "$LOCAL_PC_DIR/wlroots.pc.XXXXXXXX")" || die "Could not create private pkg-config metadata"
cat "$WLROOTS_BUILD/meson-uninstalled/wlroots-uninstalled.pc" > "$pc_tmp"
grep '^have_' "$WLROOTS_BUILD/meson-private/wlroots.pc" >> "$pc_tmp"
mv -f -- "$pc_tmp" "$LOCAL_PC_DIR/wlroots.pc"

# An optional dependency prefix supports builds without root access; a normal
# installer uses the system development packages instead.
export PKG_CONFIG_PATH="$LOCAL_PC_DIR${SC7_SWAY_DEP_PREFIX:+:$SC7_SWAY_DEP_PREFIX/usr/lib/$(uname -m)-linux-gnu/pkgconfig}${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$WLROOTS_BUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# System compiler launchers may use a cache outside the workspace. Keep this
# private build independent of that cache and its permissions.
export CCACHE_DISABLE=1

declare -A APT_PACKAGES=(
    [json-c]=libjson-c-dev [libevdev]=libevdev-dev
    [libpcre2-8]=libpcre2-dev [wayland-server]=libwayland-dev
    [wayland-client]=libwayland-dev [wayland-cursor]=libwayland-dev
    [wayland-protocols]=wayland-protocols [xkbcommon]=libxkbcommon-dev
    [cairo]=libcairo2-dev [pango]=libpango1.0-dev
    [pangocairo]=libpango1.0-dev [pixman-1]=libpixman-1-dev
    [libdrm]=libdrm-dev
)
REQUIRED_PC=(json-c libevdev libpcre2-8 wayland-server wayland-client
    wayland-cursor wayland-protocols xkbcommon cairo pango pangocairo pixman-1 libdrm wlroots)
missing=()
packages=()
for dependency in "${REQUIRED_PC[@]}"; do
    if ! pkg-config --exists "$dependency"; then
        missing+=("$dependency")
        if [[ -n "${APT_PACKAGES[$dependency]:-}" ]]; then
            packages+=("${APT_PACKAGES[$dependency]}")
        fi
    fi
done
# Sway 1.9 includes this header even with wlroots' libinput backend disabled.
if [[ ! -f /usr/include/libinput.h &&
      ( -z "${SC7_SWAY_DEP_PREFIX:-}" || ! -f "$SC7_SWAY_DEP_PREFIX/usr/include/libinput.h" ) ]]; then
    missing+=(libinput-header)
    packages+=(libinput-dev)
fi
if (( ${#packages[@]} > 0 )); then
    mapfile -t packages < <(printf '%s\n' "${packages[@]}" | sort -u)
fi
if (( ${#packages[@]} > 0 )) && [[ "$INSTALL_DEPS" == true ]]; then
    "$ROOT/scripts/install-packages.sh" "${packages[@]}" ||
        die "Could not install Sway build dependencies; rerun ./install.sh after fixing the package error"
    missing=()
    for dependency in "${REQUIRED_PC[@]}"; do
        pkg-config --exists "$dependency" || missing+=("$dependency")
    done
    [[ -f /usr/include/libinput.h ||
       ( -n "${SC7_SWAY_DEP_PREFIX:-}" && -f "$SC7_SWAY_DEP_PREFIX/usr/include/libinput.h" ) ]] ||
        missing+=(libinput-header)
fi
if (( ${#missing[@]} > 0 )); then
    die "Missing Sway build dependencies: ${missing[*]}. Run: sudo apt-get install ${packages[*]}; then rerun ./install.sh"
fi

if [[ -n "${SC7_SWAY_DEP_PREFIX:-}" ]]; then
    local_lib="$SC7_SWAY_DEP_PREFIX/usr/lib/$(uname -m)-linux-gnu"
    # Meson parses these strings as shell words. Escape spaces in the repo
    # path so each -I/-L argument remains a single path.
    escaped_prefix="${SC7_SWAY_DEP_PREFIX// /\\ }"
    escaped_lib="${local_lib// /\\ }"
    export CFLAGS="-I${escaped_prefix}/usr/include -I${escaped_prefix}/usr/include/json-c -I${escaped_prefix}/usr/include/libevdev-1.0 ${CFLAGS:-}"
    export LDFLAGS="-L${escaped_lib} ${LDFLAGS:-}"
fi
meson_extra=()
if [[ -n "${SC7_SWAY_DEP_PREFIX:-}" ]]; then
    # Explicit options also refresh a build directory configured before the
    # local dependency prefix was available.
    meson_extra+=("-Dc_args=$CFLAGS" "-Dc_link_args=$LDFLAGS")
fi

if [[ -f "$BUILD/build.ninja" ]]; then
    meson setup --reconfigure "$BUILD" "$SOURCE" \
        -Dman-pages=disabled -Dxwayland=disabled -Dtray=disabled \
        -Dswaybar=false -Dswaynag=false -Dgdk-pixbuf=disabled "${meson_extra[@]}"
    if [[ "$FORCE_REBUILD" == true ]]; then
        ninja -C "$BUILD" -t clean sway/sway
    fi
else
    if ! meson setup "$BUILD" "$SOURCE" --buildtype=release \
        -Dman-pages=disabled -Dxwayland=disabled -Dtray=disabled \
        -Dswaybar=false -Dswaynag=false -Dgdk-pixbuf=disabled "${meson_extra[@]}"; then
        [[ -d "$BUILD/meson-private" ]] || die "Sway Meson configuration failed"
    fi
    if [[ ! -f "$BUILD/build.ninja" ]]; then
        [[ -d "$BUILD/meson-private" ]] || die "Sway Meson did not create build.ninja"
        info "Recovering incomplete Sway Meson configuration..."
        meson setup --wipe "$BUILD" "$SOURCE" --buildtype=release \
            -Dman-pages=disabled -Dxwayland=disabled -Dtray=disabled \
            -Dswaybar=false -Dswaynag=false -Dgdk-pixbuf=disabled "${meson_extra[@]}" ||
            die "Sway Meson configuration failed after recovery"
    fi
    [[ -f "$BUILD/build.ninja" ]] || die "Sway Meson did not create build.ninja"
fi
ninja -C "$BUILD" sway/sway

# Publish the stamp only after the binary is built and resolves private
# wlroots. This prevents an interrupted rebuild from passing --check.
[[ -x "$SWAY" ]] || die "Local Sway binary was not built"
linkage="$(ldd "$SWAY")" || die "Could not inspect local Sway linkage"
[[ "$linkage" == *"$WLROOTS_LIB"* && "$linkage" != *"not found"* ]] ||
    die "Local Sway does not resolve Rack-local wlroots"
"$SWAY" --version | grep -q '^sway version 1\.9-' || die "Unexpected local Sway version"
stamp_tmp="$(mktemp "$BUILD/.sc7-patch-id.XXXXXXXX")" || die "Could not create build stamp"
printf '%s\n' "$EXPECTED_STAMP" > "$stamp_tmp"
mv -f -- "$stamp_tmp" "$STAMP"
verify_binary || die "Built Sway did not pass final validation"
info "Pinned local Sway is ready: $SWAY"
