#pragma once

#include "barton-core-client.h"
#include <rbus/rbus.h>
#include <stdbool.h>

#define BARTON_RBUS_COMPONENT_NAME   "BartonIoTProvider"
#define BARTON_RBUS_PREFIX           "Device.IoT."

/* ---- Scalar Properties (dmcli/rbuscli getv) ---- */
#define BARTON_RBUS_PROP_STATUS         "Device.IoT.Status"
#define BARTON_RBUS_PROP_DEVICE_COUNT   "Device.IoT.DeviceCount"
#define BARTON_RBUS_PROP_DEVICES_JSON   "Device.IoT.Devices"
#define BARTON_RBUS_PROP_DISC_ACTIVE    "Device.IoT.Discovery.Active"

/* ---- Methods ---- */
#define BARTON_RBUS_METHOD_DISC_START       "Device.IoT.Discovery.Start()"
#define BARTON_RBUS_METHOD_DISC_STOP        "Device.IoT.Discovery.Stop()"
#define BARTON_RBUS_METHOD_GET_DEVICE       "Device.IoT.Device.Get()"
#define BARTON_RBUS_METHOD_REMOVE_DEVICE    "Device.IoT.Device.Remove()"
#define BARTON_RBUS_METHOD_READ_RESOURCE    "Device.IoT.Resource.Read()"
#define BARTON_RBUS_METHOD_WRITE_RESOURCE   "Device.IoT.Resource.Write()"
#define BARTON_RBUS_METHOD_EXEC_RESOURCE    "Device.IoT.Resource.Execute()"
#define BARTON_RBUS_METHOD_COMMISSION       "Device.IoT.Matter.Commission()"
#define BARTON_RBUS_METHOD_OPEN_COMM_WIN    "Device.IoT.Matter.OpenCommissioningWindow()"
#define BARTON_RBUS_METHOD_GET_STATUS       "Device.IoT.GetStatus()"
#define BARTON_RBUS_METHOD_SET_PROPERTY     "Device.IoT.SetProperty()"
#define BARTON_RBUS_METHOD_GET_PROPERTY     "Device.IoT.GetProperty()"

/* ---- Events ---- */
#define BARTON_RBUS_EVT_DEVICE_ADDED        "Device.IoT.DeviceAdded!"
#define BARTON_RBUS_EVT_DEVICE_REMOVED      "Device.IoT.DeviceRemoved!"
#define BARTON_RBUS_EVT_RESOURCE_UPDATED    "Device.IoT.ResourceUpdated!"
#define BARTON_RBUS_EVT_DISC_STARTED        "Device.IoT.DiscoveryStarted!"
#define BARTON_RBUS_EVT_DISC_STOPPED        "Device.IoT.DiscoveryStopped!"

typedef struct BartonRbusContext
{
    rbusHandle_t  handle;
    BCoreClient  *client;
    bool          discoveryActive;
} BartonRbusContext;

bool barton_rbus_provider_init(BartonRbusContext *ctx, BCoreClient *client);
void barton_rbus_provider_cleanup(BartonRbusContext *ctx);
