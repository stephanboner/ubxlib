/*
 * Copyright 2020 u-blox Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Only #includes of u_* and the C standard library are allowed here,
 * no platform stuff and no OS stuff.  Anything required from
 * the platform/OS must be brought in through u_port* to maintain
 * portability.
 */

/** @file
 * @brief Implementation of the "general" API for cellular.
 */

#ifdef U_CFG_OVERRIDE
# include "u_cfg_override.h" // For a customer's configuration override
#endif

#include "stdlib.h"    // malloc() and free()
#include "stddef.h"    // NULL, size_t etc.
#include "stdint.h"    // int32_t etc.
#include "stdbool.h"
#include "string.h"    // memset()

#include "u_cfg_sw.h"

#include "u_error_common.h"

#include "u_port_debug.h"
#include "u_port_os.h"
#include "u_port_gpio.h"

#include "u_at_client.h"

#include "u_cell_module_type.h"
#include "u_cell.h"         // Order is
#include "u_cell_net.h"     // important here
#include "u_cell_private.h" // don't change it

#include "u_network_handle.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * STATIC VARIABLES
 * -------------------------------------------------------------- */

/** The next instance handle to use.
 */
static int32_t gNextInstanceHandle = (int32_t) U_NETWORK_HANDLE_CELL_MIN;

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Find a cellular instance in the list by AT handle.
// gUCellPrivateMutex should be locked before this is called.
//lint -e{818} suppress "could be declared as pointing to const":
// atHandle is anonymous
static uCellPrivateInstance_t *pGetCellInstanceAtHandle(uAtClientHandle_t atHandle)
{
    uCellPrivateInstance_t *pInstance = gpUCellPrivateInstanceList;

    while ((pInstance != NULL) && (pInstance->atHandle != atHandle)) {
        pInstance = pInstance->pNext;
    }

    return pInstance;
}

// Add a cellular instance to the list.
// gUCellPrivateMutex should be locked before this is called.
// Note: doesn't copy it, just adds it.
static void addCellInstance(uCellPrivateInstance_t *pInstance)
{
    pInstance->pNext = gpUCellPrivateInstanceList;
    gpUCellPrivateInstanceList = pInstance;
}

// Remove a cell instance from the list.
// gUCellPrivateMutex should be locked before this is called.
// Note: doesn't free it, the caller must do that.
static void removeCellInstance(const uCellPrivateInstance_t *pInstance)
{
    uCellPrivateInstance_t *pCurrent;
    uCellPrivateInstance_t *pPrev = NULL;

    pCurrent = gpUCellPrivateInstanceList;
    while (pCurrent != NULL) {
        if (pInstance == pCurrent) {
            if (pPrev != NULL) {
                pPrev->pNext = pCurrent->pNext;
            } else {
                gpUCellPrivateInstanceList = pCurrent->pNext;
            }
            pCurrent = NULL;
        } else {
            pPrev = pCurrent;
            pCurrent = pPrev->pNext;
        }
    }
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialise the cellular driver.
int32_t uCellInit()
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;

    if (gUCellPrivateMutex == NULL) {
        // Create the mutex that protects the linked list
        errorCode = uPortMutexCreate(&gUCellPrivateMutex);
    }

    return errorCode;
}

// Shut-down the cellular driver.
void uCellDeinit()
{
    uCellPrivateInstance_t *pInstance;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        // Remove all cell instances
        while (gpUCellPrivateInstanceList != NULL) {
            pInstance = gpUCellPrivateInstanceList;
            removeCellInstance(pInstance);
            // Free any scan results
            uCellPrivateScanFree(&(pInstance->pScanResults));
            // Free any chip to chip security context
            uCellPrivateC2cRemoveContext(pInstance);
            free(pInstance);
        }

        // Unlock the mutex so that we can delete it
        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
        uPortMutexDelete(gUCellPrivateMutex);
        gUCellPrivateMutex = NULL;
    }
}

// Add a cellular instance.
int32_t uCellAdd(uCellModuleType_t moduleType,
                 uAtClientHandle_t atHandle,
                 int32_t pinEnablePower, int32_t pinPwrOn,
                 int32_t pinVInt, bool leavePowerAlone)
{
    int32_t handleOrErrorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    int32_t platformError = 0;
    uCellPrivateInstance_t *pInstance = NULL;
    uPortGpioConfig_t gpioConfig = U_PORT_GPIO_CONFIG_DEFAULT;
    int32_t enablePowerAtStart;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        // Check parameters
        handleOrErrorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if (((size_t) moduleType < gUCellPrivateModuleListSize) &&
            (atHandle != NULL) &&
            (pGetCellInstanceAtHandle(atHandle) == NULL)) {
            handleOrErrorCode = (int32_t) U_ERROR_COMMON_NO_MEMORY;
            // Allocate memory for the instance
            pInstance = (uCellPrivateInstance_t *) malloc(sizeof(uCellPrivateInstance_t));
            if (pInstance != NULL) {
                handleOrErrorCode = (int32_t) U_ERROR_COMMON_PLATFORM;
                // Fill the values in
                memset(pInstance, 0, sizeof(*pInstance));
                // Find a free handle
                do {
                    pInstance->handle = gNextInstanceHandle;
                    gNextInstanceHandle++;
                    if (gNextInstanceHandle > (int32_t) U_NETWORK_HANDLE_CELL_MAX) {
                        gNextInstanceHandle = (int32_t) U_NETWORK_HANDLE_CELL_MIN;
                    }
                } while (pUCellPrivateGetInstance(pInstance->handle) != NULL);
                pInstance->atHandle = atHandle;
                pInstance->pinEnablePower = pinEnablePower;
                pInstance->pinPwrOn = pinPwrOn;
                pInstance->pinVInt = pinVInt;
                for (size_t x = 0;
                     x < sizeof(pInstance->networkStatus) / sizeof(pInstance->networkStatus[0]);
                     x++) {
                    pInstance->networkStatus[x] = U_CELL_NET_STATUS_UNKNOWN;
                }
                uCellPrivateClearRadioParameters(&(pInstance->radioParameters));
                pInstance->pModule = &(gUCellPrivateModuleList[moduleType]);
                pInstance->pSecurityC2cContext = NULL;
                pInstance->pNext = NULL;

                // Now set up the pins
                uPortLog("U_CELL: initialising with enable power pin ");
                if (pinEnablePower >= 0) {
                    uPortLog("%d (0x%02x), ", pinEnablePower, pinEnablePower);
                } else {
                    uPortLog("not connected, ");
                }
                uPortLog("PWR_ON pin ", pinPwrOn, pinPwrOn);
                if (pinPwrOn >= 0) {
                    uPortLog("%d (0x%02x)", pinPwrOn, pinPwrOn);
                } else {
                    uPortLog("not connected");
                }
                if (leavePowerAlone) {
                    uPortLog(", leaving the level of both those pins alone");
                }
                uPortLog(" and VInt pin ");
                if (pinVInt >= 0) {
                    uPortLog("%d (0x%02x).\n", pinVInt, pinVInt);
                } else {
                    uPortLog("not connected.\n");
                }
                // Sort PWR_ON pin if there is one
                if (pinPwrOn >= 0) {
                    gpioConfig.pin = pinPwrOn;
                    if (!leavePowerAlone) {
                        // Set PWR_ON high so that we can pull it low
                        platformError = uPortGpioSet(pinPwrOn, 1);
                    }
                    if (platformError == 0) {
                        // PWR_ON open drain so that we can pull it low and then let it
                        // float afterwards since it is pulled-up by the cellular module
                        // TODO: the u-blox C030-R412M board requires a pull-up here.
                        gpioConfig.pullMode = U_PORT_GPIO_PULL_MODE_PULL_UP;
                        gpioConfig.driveMode = U_PORT_GPIO_DRIVE_MODE_OPEN_DRAIN;
                        gpioConfig.direction = U_PORT_GPIO_DIRECTION_OUTPUT;
                        platformError = uPortGpioConfig(&gpioConfig);
                        if (platformError != 0) {
                            uPortLog("U_CELL: uPortGpioConfig() for PWR_ON pin %d"
                                     " (0x%02x) returned error code %d.\n",
                                     pinPwrOn, pinPwrOn, platformError);
                        }
                    } else {
                        uPortLog("U_CELL: uPortGpioSet() for PWR_ON pin %d (0x%02x)"
                                 " returned error code %d.\n",
                                 pinPwrOn, pinPwrOn, platformError);
                    }
                }
                // Sort the enable power pin, if there is one
                if ((platformError == 0) && (pinEnablePower >= 0)) {
                    gpioConfig.pin = pinEnablePower;
                    gpioConfig.pullMode = U_PORT_GPIO_PULL_MODE_NONE;
                    gpioConfig.driveMode = U_PORT_GPIO_DRIVE_MODE_NORMAL;
                    // Input/output so we can read it as well
                    gpioConfig.direction = U_PORT_GPIO_DIRECTION_INPUT_OUTPUT;
                    platformError = uPortGpioConfig(&gpioConfig);
                    if (platformError == 0) {
                        enablePowerAtStart = uPortGpioGet(pinEnablePower);
                        if (!leavePowerAlone) {
                            // Make sure the default is off.
                            enablePowerAtStart = 0;
                        }
                        platformError = uPortGpioSet(pinEnablePower, enablePowerAtStart);
                        if (platformError != 0) {
                            uPortLog("U_CELL: uPortGpioSet() for enable power pin %d"
                                     " (0x%02x) returned error code %d.\n",
                                     pinEnablePower, pinEnablePower, platformError);
                        }
                    } else {
                        uPortLog("U_CELL: uPortGpioConfig() for enable power pin %d"
                                 " (0x%02x) returned error code %d.\n",
                                 pinEnablePower, pinEnablePower, platformError);
                    }
                }
                // Finally, sort the VINT pin if there is one
                if ((platformError == 0) && (pinVInt >= 0)) {
                    // Set pin that monitors VINT as input
                    gpioConfig.pin = pinVInt;
                    gpioConfig.direction = U_PORT_GPIO_DIRECTION_INPUT;
                    platformError = uPortGpioConfig(&gpioConfig);
                    if (platformError != 0) {
                        uPortLog("U_CELL: uPortGpioConfig() for VInt pin %d"
                                 " (0x%02x) returned error code %d.\n",
                                 pinVInt, pinVInt, platformError);
                    }
                }
                // With that done, set up the AT client for this module
                if (platformError == 0) {
                    uAtClientTimeoutSet(atHandle,
                                        pInstance->pModule->atTimeoutSeconds * 1000);
                    uAtClientDelaySet(atHandle,
                                      pInstance->pModule->commandDelayMs);
                    // ...and finally add it to the list
                    addCellInstance(pInstance);
                    handleOrErrorCode = pInstance->handle;
                } else {
                    // If we hit a platform error, free memory again
                    free(pInstance);
                }
            }
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return (int32_t) handleOrErrorCode;
}

// Remove a cellular instance.
void uCellRemove(int32_t cellHandle)
{
    uCellPrivateInstance_t *pInstance;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        if (pInstance != NULL) {
            removeCellInstance(pInstance);
            // Free any scan results
            uCellPrivateScanFree(&(pInstance->pScanResults));
            // Free any chip to chip security context
            uCellPrivateC2cRemoveContext(pInstance);
            free(pInstance);
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }
}

// Get the handle of the AT client.
int32_t uCellAtClientHandleGet(int32_t cellHandle,
                               uAtClientHandle_t *pAtHandle)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uCellPrivateInstance_t *pInstance;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        pInstance = pUCellPrivateGetInstance(cellHandle);
        if ((pInstance != NULL) && (pAtHandle != NULL)) {
            *pAtHandle = pInstance->atHandle;
            errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCode;
}

// End of file
