#pragma once

#include <Arduino.h>

bool isValidOtaUrl(const String &url);
bool parseVersionTriplet(const String &version, int &major, int &minor, int &patch);
bool isRemoteVersionNewer(const String &currentVersion, const String &remoteVersion);
String buildOtaReleaseSignBase(
	const String &channel,
	const String &deviceType,
	const String &board,
	const String &version,
	const String &url,
	const String &sha256,
	size_t size,
	const String &releasedAt);
bool verifyOtaReleaseSignature(
	const char *publicKeyPem,
	const String &payload,
	const String &signatureBase64);
bool otaSecureHexEquals(const String &expected, const String &actual);
