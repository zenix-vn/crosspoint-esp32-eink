#include "ReleaseJsonParser.h"

#include <cstdlib>
#include <cstring>

namespace {

void safeCopy(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  size_t n = srcLen < dstSize - 1 ? srcLen : dstSize - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// Keeps a SHA-256 only when it is exactly 64 hex digits, lowercased, so a
// malformed feed value reads as "no checksum" rather than a certain mismatch.
void copySha256(char* dst, const char* src, const size_t len) {
  dst[0] = '\0';
  if (len != 64) return;
  for (size_t i = 0; i < 64; i++) {
    const char c = src[i];
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
      dst[i] = c;
    } else if (c >= 'A' && c <= 'F') {
      dst[i] = static_cast<char>(c - 'A' + 'a');
    } else {
      dst[0] = '\0';
      return;
    }
  }
  dst[64] = '\0';
}

// Cuts a byte-limited UTF-8 copy back to its last whole code point.
void trimPartialUtf8(char* s) {
  const size_t n = strlen(s);
  size_t i = n;
  while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) i--;
  if (i == 0) return;
  const unsigned char lead = static_cast<unsigned char>(s[i - 1]);
  const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  if (n - (i - 1) < need) s[i - 1] = '\0';
}

}  // namespace

ReleaseJsonParser::ReleaseJsonParser()
    : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd}) {
  safeCopy(firmwareAssetName, sizeof(firmwareAssetName), "firmware.bin", sizeof("firmware.bin") - 1);
  reset();
}

void ReleaseJsonParser::setFirmwareAssetName(const char* name) {
  safeCopy(firmwareAssetName, sizeof(firmwareAssetName), name, strlen(name));
}

void ReleaseJsonParser::reset() {
  parser.reset();
  position = Position::TOP_LEVEL;
  lastKey = LastKey::NONE;
  depth = 0;
  assetDepth = 0;
  tagName[0] = '\0';
  body[0] = '\0';
  firmwareUrl[0] = '\0';
  firmwareSha256[0] = '\0';
  firmwareSize = 0;
  tagFound = false;
  firmwareFound = false;
  currentAssetName[0] = '\0';
  currentAssetUrl[0] = '\0';
  currentAssetSize = 0;
  currentAssetSha256[0] = '\0';
}

void ReleaseJsonParser::feed(const char* data, size_t len) { parser.feed(data, len); }

bool ReleaseJsonParser::foundTag() const { return tagFound; }
bool ReleaseJsonParser::foundFirmware() const { return firmwareFound; }
const char* ReleaseJsonParser::getTagName() const { return tagName; }
const char* ReleaseJsonParser::getFirmwareUrl() const { return firmwareUrl; }
size_t ReleaseJsonParser::getFirmwareSize() const { return firmwareSize; }
const char* ReleaseJsonParser::getBody() const { return body; }
const char* ReleaseJsonParser::getFirmwareSha256() const { return firmwareSha256; }

void ReleaseJsonParser::commitAsset() {
  if (strcmp(currentAssetName, firmwareAssetName) == 0) {
    memcpy(firmwareUrl, currentAssetUrl, sizeof(firmwareUrl));
    firmwareSize = currentAssetSize;
    memcpy(firmwareSha256, currentAssetSha256, sizeof(firmwareSha256));
    firmwareFound = true;
  }
  currentAssetName[0] = '\0';
  currentAssetUrl[0] = '\0';
  currentAssetSize = 0;
  currentAssetSha256[0] = '\0';
}

// -- SAX callbacks (static trampolines) -------------------------------------

void ReleaseJsonParser::sOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth == 1) {
        if (len == 8 && memcmp(key, "tag_name", 8) == 0)
          self->lastKey = LastKey::TAG_NAME;
        else if (len == 4 && memcmp(key, "body", 4) == 0)
          self->lastKey = LastKey::BODY;
        else if (len == 6 && memcmp(key, "assets", 6) == 0)
          self->lastKey = LastKey::ASSETS;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    case Position::IN_ASSET_OBJECT:
      if (self->assetDepth == 1) {
        if (len == 4 && memcmp(key, "name", 4) == 0)
          self->lastKey = LastKey::ASSET_NAME;
        else if (len == 20 && memcmp(key, "browser_download_url", 20) == 0)
          self->lastKey = LastKey::ASSET_URL;
        else if (len == 4 && memcmp(key, "size", 4) == 0)
          self->lastKey = LastKey::ASSET_SIZE;
        else if (len == 6 && memcmp(key, "sha256", 6) == 0)
          self->lastKey = LastKey::ASSET_SHA256;
        else if (len == 6 && memcmp(key, "digest", 6) == 0)
          self->lastKey = LastKey::ASSET_DIGEST;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->lastKey) {
    case LastKey::TAG_NAME:
      if (self->position == Position::TOP_LEVEL && self->depth == 1) {
        safeCopy(self->tagName, sizeof(self->tagName), value, len);
        self->tagFound = true;
      }
      break;
    case LastKey::BODY:
      if (self->position == Position::TOP_LEVEL && self->depth == 1) {
        safeCopy(self->body, sizeof(self->body), value, len);
        trimPartialUtf8(self->body);
      }
      break;
    case LastKey::ASSET_SHA256:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1)
        copySha256(self->currentAssetSha256, value, len);
      break;
    case LastKey::ASSET_DIGEST:
      // GitHub's asset "digest" is "sha256:<hex>".
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1 && len == 71 &&
          memcmp(value, "sha256:", 7) == 0)
        copySha256(self->currentAssetSha256, value + 7, 64);
      break;
    case LastKey::ASSET_NAME:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1)
        safeCopy(self->currentAssetName, sizeof(self->currentAssetName), value, len);
      break;
    case LastKey::ASSET_URL:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1)
        safeCopy(self->currentAssetUrl, sizeof(self->currentAssetUrl), value, len);
      break;
    default:
      break;
  }
  self->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnNumber(void* ctx, const char* value, size_t /*len*/) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  if (self->lastKey == LastKey::ASSET_SIZE && self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1) {
    self->currentAssetSize = static_cast<size_t>(strtoul(value, nullptr, 10));
  }
  self->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnBool(void* ctx, bool /*value*/) {
  static_cast<ReleaseJsonParser*>(ctx)->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnNull(void* ctx) { static_cast<ReleaseJsonParser*>(ctx)->lastKey = LastKey::NONE; }

void ReleaseJsonParser::sOnObjectStart(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      self->depth++;
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSETS_ARRAY:
      self->position = Position::IN_ASSET_OBJECT;
      self->assetDepth = 1;
      self->currentAssetName[0] = '\0';
      self->currentAssetUrl[0] = '\0';
      self->currentAssetSize = 0;
      self->currentAssetSha256[0] = '\0';
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth++;
      self->lastKey = LastKey::NONE;
      break;
  }
}

void ReleaseJsonParser::sOnObjectEnd(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth--;
      if (self->assetDepth == 0) {
        self->commitAsset();
        self->position = Position::IN_ASSETS_ARRAY;
      }
      self->lastKey = LastKey::NONE;
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnArrayStart(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->lastKey == LastKey::ASSETS && self->depth == 1) {
        self->position = Position::IN_ASSETS_ARRAY;
      } else {
        self->depth++;
      }
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth++;
      self->lastKey = LastKey::NONE;
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnArrayEnd(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_ASSETS_ARRAY:
      self->position = Position::TOP_LEVEL;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth--;
      self->lastKey = LastKey::NONE;
      break;
  }
}
