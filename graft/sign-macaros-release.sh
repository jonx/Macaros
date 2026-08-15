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
MODE="${1:---notarize}"

case "$MODE" in
    --sign-only|--notarize) ;;
    *) echo "usage: $0 [--sign-only|--notarize]" >&2; exit 2 ;;
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

echo ">> signing application bundle"
codesign --force --options runtime --timestamp --sign "$IDENTITY" "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"

if [ "$MODE" = "--sign-only" ]; then
    echo ">> signed app: $APP"
    exit 0
fi

# Fail before uploading if the named keychain profile has not been configured.
xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null

ZIP="$ROOT/build/Macaros-notary.zip"
rm -f "$ZIP"
/usr/bin/ditto -c -k --keepParent "$APP" "$ZIP"

echo ">> submitting app to Apple notarization"
xcrun notarytool submit "$ZIP" --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple "$APP"
xcrun stapler validate "$APP"
rm -f "$ZIP"

echo ">> creating DMG from the signed and stapled app"
"$HERE/make-aros-release.sh" --dmg-only
codesign --force --timestamp --sign "$IDENTITY" "$DMG"

echo ">> submitting DMG to Apple notarization"
xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple "$DMG"
xcrun stapler validate "$DMG"

spctl --assess --type execute --verbose=2 "$APP"
spctl --assess --type open --context context:primary-signature --verbose=2 "$DMG"
echo ">> notarized release ready: $DMG"
