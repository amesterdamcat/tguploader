#pragma once

namespace httplib { class Server; }

// Registers the ctbrec recorder control API onto the embedded server.
//
// ctbrec ships its own web UI, but it collapses once the recordings list grows
// large (tens of thousands of entries). These routes provide a lean control
// surface: add a model by name, list tracked/online/currently-recording models,
// and start/stop/suspend recordings. All routes are under /api/rec/* and proxy
// to the local ctbrec server (https://127.0.0.1:8443) with HMAC-signed bodies;
// the ctbrec credentials and HMAC key never reach the browser.
void register_recorder_routes(httplib::Server& svr);
