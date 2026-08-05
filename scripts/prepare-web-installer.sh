#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 3 ]]; then
	echo "Usage: $0 <base-merged.bin> <touch-merged.bin> <output-directory>" >&2
	exit 64
fi

readonly BASE_SOURCE_BINARY="$1"
readonly TOUCH_SOURCE_BINARY="$2"
readonly OUTPUT_DIRECTORY="$3"
readonly REPOSITORY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly BASE_VERSION="$(sed -n 's/^#define FW_VERSION "\([^"]*\)"/\1/p' "${REPOSITORY_ROOT}/models/notificator_base/notificator_base.ino")"
readonly TOUCH_VERSION="$(sed -n 's/^constexpr char FIRMWARE_VERSION\[\] = "\([^"]*\)";/\1/p' "${REPOSITORY_ROOT}/models/notificator_touch_349/notificator_touch_349.ino")"

if [[ -z "${BASE_VERSION}" || -z "${TOUCH_VERSION}" ]]; then
	echo "Unable to read one or more firmware versions from source." >&2
	exit 1
fi

node - "${REPOSITORY_ROOT}" "${BASE_VERSION}" "${TOUCH_VERSION}" <<'NODE'
const fs = require('fs');
const path = require('path');
const [root, baseVersion, touchVersion] = process.argv.slice(2);
const catalog = JSON.parse(fs.readFileSync(path.join(root, 'installer/firmware-catalog.json')));
const expected = {
  'notificator-base-stable': baseVersion,
  'notificator-touch-349-preview': touchVersion,
};
for (const [id, version] of Object.entries(expected)) {
  const entry = catalog.firmwares.find((item) => item.id === id);
  if (!entry || entry.version !== version) throw new Error(`Catalog mismatch for ${id}`);
  const manifest = JSON.parse(fs.readFileSync(path.join(root, 'installer', entry.manifest)));
  if (manifest.version !== version) throw new Error(`Manifest mismatch for ${id}`);
}
NODE

for source_binary in "${BASE_SOURCE_BINARY}" "${TOUCH_SOURCE_BINARY}"; do
	if [[ ! -f "${source_binary}" ]]; then
		echo "Merged firmware was not found: ${source_binary}" >&2
		exit 66
	fi
done

if [[ -z "${OUTPUT_DIRECTORY}" || "${OUTPUT_DIRECTORY}" == "/" || "${OUTPUT_DIRECTORY}" == "." || "${OUTPUT_DIRECTORY}" == ".." || "${OUTPUT_DIRECTORY}" == "${HOME}" || "${OUTPUT_DIRECTORY}" == "${REPOSITORY_ROOT}" ]]; then
	echo "Refusing unsafe output directory: ${OUTPUT_DIRECTORY}" >&2
	exit 64
fi

# A merged 4 MB ESP32 factory image should be substantially larger than the
# application-only OTA image. Rejecting small input prevents publishing a web
# installer that cannot initialize a blank device or update its partition table.
for source_binary in "${BASE_SOURCE_BINARY}" "${TOUCH_SOURCE_BINARY}"; do
	source_size="$(wc -c < "${source_binary}" | tr -d ' ')"
	if (( source_size < 3000000 )); then
		echo "Expected a merged factory image, but ${source_binary} is only ${source_size} bytes." >&2
		exit 65
	fi
done

rm -rf -- "${OUTPUT_DIRECTORY}"
mkdir -p "${OUTPUT_DIRECTORY}/firmware" "${OUTPUT_DIRECTORY}/manifests"
cp "${REPOSITORY_ROOT}/installer/index.html" "${OUTPUT_DIRECTORY}/index.html"
cp "${REPOSITORY_ROOT}/installer/app.js" "${OUTPUT_DIRECTORY}/app.js"
cp "${REPOSITORY_ROOT}/installer/styles.css" "${OUTPUT_DIRECTORY}/styles.css"
cp "${REPOSITORY_ROOT}/installer/firmware-catalog.json" "${OUTPUT_DIRECTORY}/firmware-catalog.json"
cp "${REPOSITORY_ROOT}/installer/manifests/notificator-base-stable.json" "${OUTPUT_DIRECTORY}/manifests/notificator-base-stable.json"
cp "${REPOSITORY_ROOT}/installer/manifests/notificator-touch-349-preview.json" "${OUTPUT_DIRECTORY}/manifests/notificator-touch-349-preview.json"
cp "${BASE_SOURCE_BINARY}" "${OUTPUT_DIRECTORY}/firmware/notificator-base-${BASE_VERSION}.factory.bin"
cp "${TOUCH_SOURCE_BINARY}" "${OUTPUT_DIRECTORY}/firmware/notificator-touch-349-${TOUCH_VERSION}.factory.bin"
touch "${OUTPUT_DIRECTORY}/.nojekyll"

echo "Prepared Base ${BASE_VERSION} and Touch ${TOUCH_VERSION} web installer in ${OUTPUT_DIRECTORY}."
