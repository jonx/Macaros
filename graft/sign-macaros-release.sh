#!/bin/sh
# Sign, notarize, staple, and package an already-built Macaros release app.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
APP="${AROS_APP:-$ROOT/build/Macaros.app}"
DMG="${AROS_DMG:-$ROOT/build/Macaros.dmg}"
ENTITLEMENTS="${MACAROS_ENTITLEMENTS:-$HERE/aros-host-release.entitlements.plist}"
IDENTITY="${MACAROS_SIGN_IDENTITY:-}"
NOTARY_PROFILE="${MACAROS_NOTARY_PROFILE:-macaros}"
SIGN_SCOPE="${MACAROS_SIGN_SCOPE:-outer}"
MODE="${1:---notarize}"

case "$MODE" in
    --sign-only|--package-test|--notarize) ;;
    *) echo "usage: $0 [--sign-only|--package-test|--notarize]" >&2; exit 2 ;;
esac
case "$SIGN_SCOPE" in
    full|outer) ;;
    *) echo "MACAROS_SIGN_SCOPE must be 'full' or 'outer'" >&2; exit 2 ;;
esac

[ -n "$IDENTITY" ] || {
    echo "set MACAROS_SIGN_IDENTITY to a Developer ID Application identity" >&2
    echo "available identities:" >&2
    security find-identity -v -p codesigning >&2 || true
    exit 2
}
[ -d "$APP" ] || { echo "missing release app: $APP" >&2; exit 1; }
[ -f "$ENTITLEMENTS" ] || { echo "missing release entitlements: $ENTITLEMENTS" >&2; exit 1; }
security find-identity -v -p codesigning | grep -Fq "\"$IDENTITY\"" || {
    echo "codesigning identity is not available: $IDENTITY" >&2
    exit 1
}

"$HERE/make-aros-release.sh" --check

if [ "$SIGN_SCOPE" = full ]; then
    echo ">> signing nested host libraries"
    for binary in "$APP"/Contents/Frameworks/*.dylib; do
        [ -f "$binary" ] || continue
        codesign --force --options runtime --timestamp --sign "$IDENTITY" "$binary"
    done

    echo ">> signing hosted AROS runtime"
    for binary in \
        "$APP/Contents/Resources/AROS/boot/darwin/AROSBootstrap" \
        "$APP/Contents/Resources/AROS/boot/darwin/Macaros"; do
        [ -f "$binary" ] || { echo "missing hosted runtime: $binary" >&2; exit 1; }
        codesign --force --options runtime --timestamp \
            --entitlements "$ENTITLEMENTS" --sign "$IDENTITY" "$binary"
    done
else
    echo ">> retaining ad-hoc signatures on embedded host code"
    for binary in \
        "$APP"/Contents/Frameworks/*.dylib \
        "$APP/Contents/Resources/AROS/boot/darwin/AROSBootstrap" \
        "$APP/Contents/Resources/AROS/boot/darwin/Macaros"; do
        [ -f "$binary" ] || { echo "missing embedded host code: $binary" >&2; exit 1; }
        details=$(codesign -dvv "$binary" 2>&1 || true)
        printf '%s\n' "$details" | grep -q 'Signature=adhoc' || {
            echo "outer-only signing requires a fresh ad-hoc input: $binary" >&2
            exit 1
        }
        if printf '%s\n' "$details" | grep -q 'flags=.*runtime'; then
            echo "outer-only signing found hardened embedded code: $binary" >&2
            exit 1
        fi
    done
fi

echo ">> signing application bundle"
codesign --force --options runtime --timestamp --sign "$IDENTITY" "$APP"
if [ "$SIGN_SCOPE" = full ]; then
    codesign --verify --deep --strict --verbose=2 "$APP"
else
    codesign --verify --strict --verbose=2 "$APP"
fi

if [ "$MODE" = "--sign-only" ]; then
    echo ">> signed app: $APP"
    exit 0
fi

if [ "$MODE" = "--package-test" ]; then
    echo ">> creating signed, unnotarized test DMG"
    "$HERE/make-aros-release.sh" --dmg-only
    codesign --force --timestamp --sign "$IDENTITY" "$DMG"
    echo ">> signed test release ready: $DMG"
    exit 0
fi

# Fail before uploading if the named keychain profile has not been configured.
xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null

if [ "$SIGN_SCOPE" = full ]; then
    ZIP="$ROOT/build/Macaros-notary.zip"
    rm -f "$ZIP"
    /usr/bin/ditto -c -k --keepParent "$APP" "$ZIP"

    echo ">> submitting app to Apple notarization"
    xcrun notarytool submit "$ZIP" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$APP"
    xcrun stapler validate "$APP"
    rm -f "$ZIP"
else
    echo ">> outer-only scope: notarizing the signed delivery DMG, not the app separately"
fi

echo ">> creating DMG from the signed and stapled app"
"$HERE/make-aros-release.sh" --dmg-only
codesign --force --timestamp --sign "$IDENTITY" "$DMG"

echo ">> submitting DMG to Apple notarization"
xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple "$DMG"
xcrun stapler validate "$DMG"

if [ "$SIGN_SCOPE" = full ]; then
    spctl --assess --type execute --verbose=2 "$APP"
else
    codesign --verify --strict --verbose=2 "$APP"
fi
spctl --assess --type open --context context:primary-signature --verbose=2 "$DMG"
echo ">> notarized release ready: $DMG"
