#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>

class ReleaseJsonParser {
 public:
  ReleaseJsonParser();

  ReleaseJsonParser(const ReleaseJsonParser&) = delete;
  ReleaseJsonParser& operator=(const ReleaseJsonParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);

  // Release-asset filename to match (default "firmware.bin").
  void setFirmwareAssetName(const char* name);

  bool foundTag() const;
  bool foundFirmware() const;
  const char* getTagName() const;
  const char* getFirmwareUrl() const;
  size_t getFirmwareSize() const;
  // Release notes ("body"), truncated to fit; empty when the release has none.
  const char* getBody() const;
  // Lowercase hex SHA-256 of the firmware asset, from "sha256" or a
  // "digest": "sha256:<hex>" field; empty when the feed gives none.
  const char* getFirmwareSha256() const;

 private:
  enum class Position : uint8_t {
    TOP_LEVEL,
    IN_ASSETS_ARRAY,
    IN_ASSET_OBJECT,
  };

  enum class LastKey : uint8_t {
    NONE,
    TAG_NAME,
    BODY,
    ASSETS,
    ASSET_NAME,
    ASSET_URL,
    ASSET_SIZE,
    ASSET_SHA256,
    ASSET_DIGEST,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void commitAsset();

  StreamingJsonParser parser;

  Position position;
  LastKey lastKey;
  uint8_t depth;
  uint8_t assetDepth;

  char tagName[32];
  char body[600];
  char firmwareUrl[512];
  char firmwareSha256[65];
  size_t firmwareSize;
  bool tagFound;
  bool firmwareFound;

  char currentAssetName[48];
  char currentAssetUrl[512];
  size_t currentAssetSize;
  char currentAssetSha256[65];

  char firmwareAssetName[48];
};
