#pragma once

#include <Arduino.h>

/**
 * Validate an OTA download URL.
 *
 * Firmware updates are deliberately restricted to HTTPS. The signed manifest
 * authenticates the selected binary URL.
 */
bool isValidOtaUrl(const String &url);

/** Parse a strict major.minor.patch version into integer components. */
bool parseVersionTriplet(const String &version, int &major, int &minor, int &patch);

/** Return true only when remoteVersion is a valid, newer semantic version. */
bool isRemoteVersionNewer(const String &currentVersion, const String &remoteVersion);

/**
 * Build the canonical payload signed by the firmware release workflow.
 *
 * Field order and line endings are part of the OTA manifest protocol.
 */
String buildOtaReleaseSignBase(
	const String &channel,
	const String &deviceType,
	const String &board,
	const String &version,
	const String &url,
	const String &sha256,
	size_t size,
	const String &releasedAt);

/**
 * Verify a base64 DER-encoded ECDSA P-256 signature over a canonical release.
 */
bool verifyOtaReleaseSignature(
	const char *publicKeyPem,
	const String &payload,
	const String &signatureBase64);

/** Compare two lowercase SHA-256 hex strings without early exit. */
bool otaSecureHexEquals(const String &expected, const String &actual);
