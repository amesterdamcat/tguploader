#pragma once

namespace httplib { class Server; }

// Registers the video search/browse API (the C++ port of web/app.py) onto the
// embedded server. All routes are under /api/* and back onto scanner.db
// (read-only). index.html is served by nginx; this only provides the JSON API.
void register_search_routes(httplib::Server& svr);
