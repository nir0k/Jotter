#!/usr/bin/env bash
# Builds the Flatpak bundle into app/packaging/flatpak/build from the working tree: tracked files
# and new files that .gitignore does not exclude. The version comes from the VERSION file. Needs
# the KDE SDK and runtime of the manifest and flatpak-builder, either installed on the host or as
# the org.flatpak.Builder Flatpak.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(git -C "$here" rev-parse --show-toplevel)"
version="$(tr -d '[:space:]' < "$repo/VERSION")"
app_id=io.github.nir0k.Jotter
out="$here/build"
bundle="$out/jotter-$version.$(uname -m).flatpak"

mkdir -p "$out"
rm -f "$out"/jotter-*.tar.gz "$out"/*.flatpak
git -C "$repo" ls-files -z --cached --others --exclude-standard |
    tar -C "$repo" --null -T - --transform "s,^,jotter-$version/," -czf "$out/jotter-$version.tar.gz"
sha256="$(sha256sum "$out/jotter-$version.tar.gz" | cut -d ' ' -f 1)"
sed -e "s/@VERSION@/$version/g" -e "s/@SHA256@/$sha256/g" "$here/$app_id.yml.in" > "$out/$app_id.yml"

if command -v flatpak-builder > /dev/null; then
    builder=(flatpak-builder)
else
    builder=(flatpak run org.flatpak.Builder)
fi
"${builder[@]}" --force-clean --state-dir="$out/state" --repo="$out/repo" \
    "$out/app" "$out/$app_id.yml"

desktop-file-validate "$out/app/files/share/applications/$app_id.desktop"
appstreamcli validate --no-net "$out/app/files/share/metainfo/$app_id.metainfo.xml"

# Installing the bundle fetches the KDE runtime from Flathub.
flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
    "$out/repo" "$bundle" "$app_id"
echo "The bundle is $bundle"
