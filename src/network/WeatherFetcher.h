#pragma once

#include "WeatherStore.h"

namespace weather {

// Refreshes `forecast` on the way to sleep when weather is on, the clock is
// set, a Wi-Fi network is saved and the forecast is older than three hours:
// joins the last-used saved network, finds the place (the weather location
// setting, or the network address when empty), downloads the forecast, saves
// it and turns Wi-Fi off. Blocks for at most about 25 s. Returns true when the
// forecast changed.
bool refreshBeforeSleep(Forecast& forecast);

}  // namespace weather
