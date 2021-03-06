/*
 * Copyright 2020 u-blox Cambourne Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _U_NETWORK_CONFIG_WIFI_H_
#define _U_NETWORK_CONFIG_WIFI_H_

/* No #includes allowed here */

/* This header file defines the configuration structure for the
 * network API for Wifi.
 */

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* Note: try, wherever possible, to use only basic types in this
 * structure (i.e. int32_t, const char, bool, etc.) since otherwise
 * your Wifi headers will have to be brought in to all files that
 * need this config type, so into all network examples, etc.
 * irrespective of whether Wifi is used there.
 */

/** The network configuration for Wifi.
 */
typedef struct {
    uNetworkType_t type; /**< All uNetworkConfigurationXxx structures
                              must begin with this for error checking
                              purposes. */
    // TODO
} uNetworkConfigurationWifi_t;

#endif // _U_NETWORK_CONFIG_WIFI_H_

// End of file
