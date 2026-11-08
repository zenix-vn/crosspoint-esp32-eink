#pragma once

#include <string>

class OtaUpdater {
  bool updateAvailable = false;
  bool betaChannel = false;
  std::string latestVersion;
  std::string releaseNotes;
  std::string expectedSha256;  // lowercase hex, empty when the feed gives none
  std::string otaUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;

 public:
  using ProgressCallback = void (*)(void* ctx);

  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
    WRONG_DEVICE_ERROR,
    CHECKSUM_ERROR,
  };

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  OtaUpdater() = default;
  // Beta reads the feed's beta.json sibling instead of latest.json. Only
  // meaningful for a self-hosted feed; the GitHub default has no beta file.
  void setBetaChannel(const bool beta) { betaChannel = beta; }
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  const std::string& getReleaseNotes() const { return releaseNotes; }
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr);
};
