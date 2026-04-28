#pragma once

#include "provider/barton-core-network-credentials-provider.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define B_RBUS_NETWORK_CREDENTIALS_PROVIDER_TYPE \
    (b_rbus_network_credentials_provider_get_type())

G_DECLARE_FINAL_TYPE(BRbusNetworkCredentialsProvider,
                     b_rbus_network_credentials_provider,
                     B_RBUS, NETWORK_CREDENTIALS_PROVIDER,
                     GObject)

/**
 * Set Wi-Fi credentials that will be returned to BartonCore when requested.
 */
void b_rbus_network_credentials_provider_set_wifi(const gchar *ssid,
                                                   const gchar *password);

BRbusNetworkCredentialsProvider *b_rbus_network_credentials_provider_new(void);

G_END_DECLS
