#pragma once

namespace httplib { class Server; }

// LeQu (乐趣Live) recorder backend. Mirrors the ctbrec integration
// (recorder_api) but targets the reverse-engineered LeQu gateway directly:
//   - SMS login → sessionId (EL-AUTH), persisted under .account_configs/
//   - hot / following / recommended live lists + search
//   - auto-tracking: poll lists, auto-record tracked anchors with ffmpeg when
//     they go live, remux FLV → MP4 losslessly on stop
//
// All routes are JWT-guarded (same scheme as the rest of the panel) and live
// under /api/lequ/*. Recordings land in an independent media dir (default
// /mnt/storage/lequ/media), NOT the ctbrec/Telegram pipeline.
void register_lequ_routes(httplib::Server& svr);

// Start the background auto-tracking poll loop. Call once at server startup.
void lequ_start_scheduler();
