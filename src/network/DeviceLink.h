#pragma once

#include <string>
#include <vector>

#include "network/HttpDownloader.h"

// This reader's identity on the self-hosted server (CROSSPOINT_INBOX_URL): the
// chip's factory MAC plus a random secret made on first use and kept in NVS,
// so it survives a card swap. Sent with an account's link key (SETTINGS.inboxKey,
// typed under More > Receive books), it joins that account on the server; books
// and word lists sent to it then arrive under More > Receive books.
namespace device_link {

// Whether this build talks to a server at all.
bool available();
const char* serverUrl();

// Request headers naming and authenticating the device, with its name,
// board, firmware and battery for the Devices page.
std::vector<HttpDownloader::Header> headers();
// The id and secret as form fields, for POST requests that carry no headers.
std::string formFields();

// Tells the server the device is alive (and registers it the first time).
// Needs Wi-Fi up; best effort.
void hello();

}  // namespace device_link
