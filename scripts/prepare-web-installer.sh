#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
	echo "Usage: $0 <merged-firmware.bin> <output-directory>" >&2
	exit 64
fi

readonly SOURCE_BINARY="$1"
readonly OUTPUT_DIRECTORY="$2"
readonly REPOSITORY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly VERSION="$(sed -n 's/^#define FW_VERSION "\([^"]*\)"/\1/p' "${REPOSITORY_ROOT}/models/notificator_base/notificator_base.ino")"
readonly CATALOG_VERSION="$(sed -n '/"id": "notificator-base-stable"/,/^[[:space:]]*}/ s/^[[:space:]]*"version": "\([^"]*\)",/\1/p' "${REPOSITORY_ROOT}/installer/firmware-catalog.json")"
readonly MANIFEST_VERSION="$(sed -n 's/^[[:space:]]*"version": "\([^"]*\)",/\1/p' "${REPOSITORY_ROOT}/installer/manifests/notificator-base-stable.json")"

if [[ -z "${VERSION}" ]]; then
	echo "Unable to read FW_VERSION from the Notificator Base sketch." >&2
	exit 1
fi

if [[ "${CATALOG_VERSION}" != "${VERSION}" || "${MANIFEST_VERSION}" != "${VERSION}" ]]; then
	echo "Firmware version mismatch: source=${VERSION}, catalog=${CATALOG_VERSION}, manifest=${MANIFEST_VERSION}." >&2
	exit 65
fi

if [[ ! -f "${SOURCE_BINARY}" ]]; then
	echo "Merged firmware was not found: ${SOURCE_BINARY}" >&2
	exit 66
fi

if [[ -z "${OUTPUT_DIRECTORY}" || "${OUTPUT_DIRECTORY}" == "/" || "${OUTPUT_DIRECTORY}" == "." || "${OUTPUT_DIRECTORY}" == ".." || "${OUTPUT_DIRECTORY}" == "${HOME}" || "${OUTPUT_DIRECTORY}" == "${REPOSITORY_ROOT}" ]]; then
	echo "Refusing unsafe output directory: ${OUTPUT_DIRECTORY}" >&2
	exit 64
fi

# A merged 4 MB ESP32 factory image should be substantially larger than the
# application-only OTA image. Rejecting small input prevents publishing a web
# installer that cannot initialize a blank device or update its partition table.
readonly SOURCE_SIZE="$(wc -c < "${SOURCE_BINARY}" | tr -d ' ')"
if (( SOURCE_SIZE < 3000000 )); then
	echo "Expected a merged factory image, but ${SOURCE_BINARY} is only ${SOURCE_SIZE} bytes." >&2
	exit 65
fi

rm -rf -- "${OUTPUT_DIRECTORY}"
mkdir -p "${OUTPUT_DIRECTORY}/firmware" "${OUTPUT_DIRECTORY}/manifests"
cp "${REPOSITORY_ROOT}/installer/index.html" "${OUTPUT_DIRECTORY}/index.html"
cp "${REPOSITORY_ROOT}/installer/app.js" "${OUTPUT_DIRECTORY}/app.js"
cp "${REPOSITORY_ROOT}/installer/styles.css" "${OUTPUT_DIRECTORY}/styles.css"
cp "${REPOSITORY_ROOT}/installer/firmware-catalog.json" "${OUTPUT_DIRECTORY}/firmware-catalog.json"
cp "${REPOSITORY_ROOT}/installer/manifests/notificator-base-stable.json" "${OUTPUT_DIRECTORY}/manifests/notificator-base-stable.json"
cp "${SOURCE_BINARY}" "${OUTPUT_DIRECTORY}/firmware/notificator-base-${VERSION}.factory.bin"
touch "${OUTPUT_DIRECTORY}/.nojekyll"

echo "Prepared Notificator Base ${VERSION} web installer in ${OUTPUT_DIRECTORY}."
