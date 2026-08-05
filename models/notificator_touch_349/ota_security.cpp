#include "ota_security.h"

#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

bool isValidOtaUrl(const String &url)
{
	String normalized = url;
	normalized.trim();
	return normalized.startsWith("https://");
}

bool parseVersionTriplet(const String &version, int &major, int &minor, int &patch)
{
	major = 0;
	minor = 0;
	patch = 0;
	const int firstDot = version.indexOf('.');
	const int secondDot = version.indexOf('.', firstDot + 1);
	if (firstDot < 1 || secondDot <= firstDot + 1)
		return false;
	const String parts[] = {
		version.substring(0, firstDot),
		version.substring(firstDot + 1, secondDot),
		version.substring(secondDot + 1),
	};
	for (const String &part : parts)
	{
		if (!part.length())
			return false;
		for (size_t index = 0; index < part.length(); ++index)
			if (!isDigit(part[index]))
				return false;
	}
	major = parts[0].toInt();
	minor = parts[1].toInt();
	patch = parts[2].toInt();
	return true;
}

bool isRemoteVersionNewer(const String &currentVersion, const String &remoteVersion)
{
	int currentMajor = 0;
	int currentMinor = 0;
	int currentPatch = 0;
	int remoteMajor = 0;
	int remoteMinor = 0;
	int remotePatch = 0;
	if (!parseVersionTriplet(currentVersion, currentMajor, currentMinor, currentPatch) ||
		!parseVersionTriplet(remoteVersion, remoteMajor, remoteMinor, remotePatch))
		return false;
	if (remoteMajor != currentMajor)
		return remoteMajor > currentMajor;
	if (remoteMinor != currentMinor)
		return remoteMinor > currentMinor;
	return remotePatch > currentPatch;
}

String buildOtaReleaseSignBase(
	const String &channel,
	const String &deviceType,
	const String &board,
	const String &version,
	const String &url,
	const String &sha256,
	size_t size,
	const String &releasedAt)
{
	String payload;
	payload.reserve(channel.length() + deviceType.length() + board.length() +
		version.length() + url.length() + sha256.length() + releasedAt.length() + 96);
	payload += "NOTIFICATOR-OTA-V1\n";
	payload += channel + "\n" + deviceType + "\n" + board + "\n" + version + "\n";
	payload += url + "\n" + sha256 + "\n" + String(size) + "\n" + releasedAt;
	return payload;
}

bool verifyOtaReleaseSignature(
	const char *publicKeyPem,
	const String &payload,
	const String &signatureBase64)
{
	if (!publicKeyPem || !publicKeyPem[0] || !payload.length() || !signatureBase64.length())
		return false;
	unsigned char signature[96];
	size_t signatureLength = 0;
	if (mbedtls_base64_decode(
			signature,
			sizeof(signature),
			&signatureLength,
			reinterpret_cast<const unsigned char *>(signatureBase64.c_str()),
			signatureBase64.length()) != 0 ||
		!signatureLength)
		return false;
	unsigned char digest[32];
	if (mbedtls_sha256(
			reinterpret_cast<const unsigned char *>(payload.c_str()),
			payload.length(),
			digest,
			0) != 0)
		return false;
	mbedtls_pk_context key;
	mbedtls_pk_init(&key);
	if (mbedtls_pk_parse_public_key(
			&key,
			reinterpret_cast<const unsigned char *>(publicKeyPem),
			strlen(publicKeyPem) + 1) != 0)
	{
		mbedtls_pk_free(&key);
		return false;
	}
	const int result = mbedtls_pk_verify(
		&key, MBEDTLS_MD_SHA256, digest, sizeof(digest), signature, signatureLength);
	mbedtls_pk_free(&key);
	return result == 0;
}

bool otaSecureHexEquals(const String &expected, const String &actual)
{
	if (expected.length() != 64 || actual.length() != 64)
		return false;
	unsigned char difference = 0;
	for (size_t index = 0; index < 64; ++index)
		difference |= static_cast<unsigned char>(expected[index] ^ actual[index]);
	return difference == 0;
}
