/*
 * Transfer app: runs the web interface (components/webui) while it's open, so books can be
 * uploaded and saved words viewed from a browser. Shows the address to open, the pairing
 * code when a browser asks for one, and upload progress. Leaving the app stops the server.
 * See components/app_transfer/README.md.
 */
#pragma once

#include "app.h"

extern const app_t transfer_app;
