#pragma once

#include "barton-core-client.h"
#include <rbus/rbus.h>
#include <stdbool.h>

#define BARTON_RBUS_COMPONENT_NAME "BartonRbusProvider"

/* Properties */
#define BARTON_RBUS_PROP_STATUS        "Barton.Status"
#define BARTON_RBUS_PROP_DEVICE_COUNT  "Barton.DeviceCount"

/* Methods */
#define BARTON_RBUS_METHOD_DISCOVER_START    "Barton.DiscoverStart()"
#define BARTON_RBUS_METHOD_DISCOVER_STOP     "Barton.DiscoverStop()"
#define BARTON_RBUS_METHOD_GET_DEVICES       "Barton.GetDevices()"
#define BARTON_RBUS_METHOD_GET_DEVICE        "Barton.GetDevice()"
#define BARTON_RBUS_METHOD_REMOVE_DEVICE     "Barton.RemoveDevice()"
#define BARTON_RBUS_METHOD_READ_RESOURCE     "Barton.ReadResource()"
#define BARTON_RBUS_METHOD_WRITE_RESOURCE    "Barton.WriteResource()"
#define BARTON_RBUS_METHOD_COMMISSION_DEVICE "Barton.CommissionDevice()"

/* Events */
#define BARTON_RBUS_EVT_DEVICE_ADDED     "Barton.DeviceAdded!"
#define BARTON_RBUS_EVT_DEVICE_REMOVED   "Barton.DeviceRemoved!"
#define BARTON_RBUS_EVT_RESOURCE_UPDATED "Barton.ResourceUpdated!"
#define BARTON_RBUS_EVT_DISC_STARTED     "Barton.DiscoveryStarted!"
#define BARTON_RBUS_EVT_DISC_STOPPED     "Barton.DiscoveryStopped!"

typedef struct BartonRbusContext
{
    rbusHandle_t  handle;
    BCoreClient  *client;
} BartonRbusContext;

bool barton_rbus_provider_init(BartonRbusContext *ctx, BCoreClient *client);
void barton_rbus_provider_cleanup(BartonRbusContext *ctx);
