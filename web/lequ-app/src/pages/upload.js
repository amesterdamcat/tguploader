
import { createApp, ref, computed, onMounted, onBeforeUnmount, watch, nextTick } from 'vue';
import '../styles/pages/upload.css';

/* ===================================================================
   AUTH + TOKEN
   =================================================================== */
const TOKEN_KEY = 'token';
const getToken = () => localStorage.getItem(TOKEN_KEY) || '';
const dropToken = () => localStorage.removeItem(TOKEN_KEY);

(function adoptUrlToken() {
  const u = new URL(location.href);
  let tok = u.searchParams.get('token');
  if (!tok && u.hash) {
    const m = u.hash.match(/(?:^#|&)token=([^&]+)/);
    if (m) tok = decodeURIComponent(m[1]);
  }
  if (tok) {
    localStorage.setItem(TOKEN_KEY, tok);
    u.searchParams.delete('token');
    u.hash = '';
    history.replaceState({}, '', u.pathname + (u.search ? u.search : ''));
  }
})();

// Module-level token-reset callback — set by Vue setup() so 401 handlers
// can drop the app back to the login overlay without forcing a page reload.
let onUnauthorized = () => {};

async function api(path, opts = {}) {
  const headers = {
    'Authorization': 'Bearer ' + getToken(),
    ...(opts.body ? { 'Content-Type': 'application/json' } : {}),
    ...opts.headers,
  };
  const res = await fetch(path, { ...opts, headers });
  if (res.status === 401) {
    dropToken();
    onUnauthorized();
    throw new Error('Unauthorized');
  }
  if (!res.ok) {
    let d = {};
    try { d = await res.json(); } catch {}
    throw new Error(d.detail || `HTTP ${res.status}`);
  }
  return res.json();
}

/* ===================================================================
   HELPERS
   =================================================================== */
function fmtBytesPerSec(bps) {
  if (!bps || bps < 100) return ['—', ''];
  const units = ['B/s', 'KB/s', 'MB/s', 'GB/s'];
  let u = 0;
  let v = bps;
  while (v >= 1024 && u < units.length - 1) { v /= 1024; u++; }
  return [v.toFixed(v >= 100 ? 0 : v >= 10 ? 1 : 2), units[u]];
}
function fmtBytes(bytes) {
  if (!bytes) return '0';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let u = 0;
  let v = bytes;
  while (v >= 1024 && u < units.length - 1) { v /= 1024; u++; }
  return v.toFixed(v >= 100 ? 0 : 1) + ' ' + units[u];
}
function fmtBytesPair(bytes) {
  if (!bytes) return ['0', 'B'];
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let u = 0;
  let v = bytes;
  while (v >= 1024 && u < units.length - 1) { v /= 1024; u++; }
  return [v.toFixed(v >= 100 ? 0 : 1), units[u]];
}
function elapsed(secs) {
  const s = Math.max(0, Math.floor(secs));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h > 0) return `${h}:${String(m).padStart(2,'0')}:${String(sec).padStart(2,'0')}`;
  return `${String(m).padStart(2,'0')}:${String(sec).padStart(2,'0')}`;
}

/* ===================================================================
   LOG CLASSIFIER
   =================================================================== */
const TAG_CLASS = {
  'INFO':    't-info',
  'DB':      't-db',
  'JOB':     't-job',
  'FIX-BIG': 't-fix',
  'WEB':     't-web',
  'SCAN':    't-scan',
  'OK':      't-ok',
  'SUCCESS': 't-ok',
  'WARN':    't-warn',
  'WARNING': 't-warn',
  'ERROR':   't-err',
  'SKIP':    't-skip',
};

const PROGRESS_RE = /↑\s*(\d+(?:\.\d+)?)\s*([KMG]?i?B?)\s*\/\s*(\d+(?:\.\d+)?)\s*([KMG]?i?B?)\s*\((\d+)%\)/;

/**
 * Parse a raw log line into {ts, tag, tagClass, rest, progress}.
 * Lines from server are prefixed with "HH:MM:SS.mmm ".
 */
function parseLine(raw) {
  const isProgressSentinel = raw.startsWith('\r');
  if (isProgressSentinel) raw = raw.slice(1);

  let ts = '';
  const tsMatch = raw.match(/^(\d{2}:\d{2}:\d{2})(?:\.\d{3})?\s+/);
  if (tsMatch) {
    ts = tsMatch[1];
    raw = raw.slice(tsMatch[0].length);
  }

  let tag = '';
  let tagClass = 't-other';
  const tagMatch = raw.match(/^\[([A-Za-z][A-Za-z0-9_\-]*)\]\s*/);
  if (tagMatch) {
    const t = tagMatch[1];
    raw = raw.slice(tagMatch[0].length);
    tag = t;
    const u = t.toUpperCase();
    if (TAG_CLASS[u]) {
      tagClass = TAG_CLASS[u];
    } else if (u.includes('BOT') || u.includes('UPLOAD')) {
      tagClass = 't-bot';
    } else {
      tagClass = 't-other';
    }
  }

  const pm = raw.match(PROGRESS_RE);
  let progress = null;
  if (pm) {
    progress = {
      pct: parseInt(pm[5], 10),
      current: pm[1] + ' ' + (pm[2] || 'B'),
      total: pm[3] + ' ' + (pm[4] || 'B'),
    };
  }

  return { ts, tag, tagClass, rest: raw, progress };
}

/* Inline highlight numbers / paths inside the rest */
function highlightContent(text) {
  // escape
  const esc = text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  // numbers + sizes + durations
  let out = esc.replace(
    /(\d+(?:\.\d+)?\s*(?:GB|MB|KB|GiB|MiB|KiB|B|s|ms|%)|\d+\/\d+)/g,
    '<span class="num">$1</span>'
  );
  // arrows
  out = out.replace(/(→|->|↑|↓)/g, '<span class="arrow">$1</span>');
  // file extensions are kept plain — too many false positives otherwise
  return out;
}

/* ===================================================================
   THE APP
   =================================================================== */
const App = {
  template: `
    <div v-if="!token" class="login-overlay">
      <form class="login-card" @submit.prevent="doLogin">
        <div class="brand-logo"></div>
        <h1>tg<span class="brand-dot">.</span>uploader</h1>
        <p>Sign in to access upload controls.</p>
        <div class="login-field">
          <label>username</label>
          <input class="input" v-model="loginForm.username" autocomplete="username"
                 autofocus required />
        </div>
        <div class="login-field">
          <label>password</label>
          <input class="input" type="password" v-model="loginForm.password"
                 autocomplete="current-password" required />
        </div>
        <div class="login-err" v-if="loginErr">{{ loginErr }}</div>
        <button type="submit" class="btn primary" :disabled="loginForm.loading">
          {{ loginForm.loading ? 'signing in…' : 'Sign in →' }}
        </button>
      </form>
    </div>

    <template v-else>
      <header>
        <div class="brand">
          <div class="brand-logo"></div>
          tg<span class="brand-dot">.</span>uploader
          <span style="color: var(--text-4); margin-left: 6px; font-variation-settings: 'wght' 380;">/ control</span>
        </div>

        <div class="head-center">
          <span class="pill" :class="{ live: sseConnected, beat: heartbeat }">
            <span class="dot"></span>
            <span>{{ sseConnected ? 'STREAM' : 'OFFLINE' }}</span>
          </span>
          <span class="pill" v-if="anyRunning">
            <span class="dot" style="background: var(--brand); box-shadow: 0 0 6px var(--brand-glow);"></span>
            <span class="num-tab">{{ runningSummary }}</span>
          </span>
        </div>

        <div class="head-right">
          <button class="btn primary" @click="startJob('bot-upload')" :disabled="botRunning" title="Start bot-upload (s)">
            <span class="kbd">s</span>BOT
          </button>
          <button class="btn primary" @click="startJob('fix-big')" :disabled="fixRunning" title="Start fix-big (f)">
            <span class="kbd">f</span>FIX
          </button>
          <button class="btn danger" @click="stopAll" :disabled="!anyRunning"
                  :class="{ 'force-armed': anyStopArmed }"
                  :title="anyStopArmed ? 'Click again to FORCE-KILL running curl/ffmpeg' : 'Stop all (X)'">
            <span class="kbd">X</span>{{ anyStopArmed ? 'FORCE' : 'STOP' }}
          </button>
          <button class="btn ghost" @click="openSettings" title="Settings"
                  style="padding: 6px 8px;" aria-label="settings">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor"
                 stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <circle cx="12" cy="12" r="3"/>
              <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"/>
            </svg>
          </button>
          <button class="btn ghost" @click="openKeymap" title="Keymap (?)">
            <span class="kbd">?</span>
          </button>
          <a class="btn ghost" :href="'/backup/?token=' + encodeURIComponent(token)" title="Open backup console">Backup</a>
          <button class="btn ghost" @click="logout">Sign out</button>
        </div>
      </header>

      <main>
        <div class="col-left">
          <!-- STATUS card -->
          <section class="card" :class="{ running: anyRunning }">
            <div class="card-head">
              <span>Status</span>
              <span class="spacer"></span>
              <span class="badge" v-if="activePathCount > 0"><tween-n :value="activePathCount" /> locked</span>
            </div>
            <div class="card-body">
              <div class="job-row" :class="{ running: fixRunning }">
                <div class="job-indicator"></div>
                <div class="job-info">
                  <div class="job-name">fix-big <span class="kbd-hint">f</span></div>
                  <div class="job-meta num-tab">{{ fixMeta }}</div>
                </div>
                <div class="job-controls">
                  <button class="btn ghost" v-if="fixRunning" @click="stopJob('fix-big')"
                          :title="anyStopArmed ? 'Click again to force-kill ffmpeg' : 'graceful stop'">
                    {{ anyStopArmed ? 'force' : 'stop' }}
                  </button>
                </div>
              </div>

              <div class="job-row" :class="{ running: botRunning }">
                <div class="job-indicator"></div>
                <div class="job-info">
                  <div class="job-name">bot-upload <span class="kbd-hint">s</span></div>
                  <div class="job-meta num-tab">{{ botMeta }}</div>
                </div>
                <div class="job-controls">
                  <button class="btn ghost" v-if="botRunning" @click="stopJob('bot-upload')"
                          :title="anyStopArmed ? 'Click again to abort curl mid-upload' : 'graceful stop'">
                    {{ anyStopArmed ? 'force' : 'stop' }}
                  </button>
                </div>
              </div>
            </div>
          </section>

          <!-- MODELS card -->
          <section class="card">
            <div class="card-head">
              <span>Models Workbench</span>
              <span class="spacer"></span>
              <button class="btn ghost" style="padding: 3px 8px" @click="refreshFolders" title="Refresh (r)">
                <span class="kbd">r</span>refresh
              </button>
            </div>

            <div class="models-summary">
              <div class="stat-cell brand">
                <div class="label">total</div>
                <div class="value num-tab">
                  <tween-n :value="totalBytes" :fmt="v => fmtBytesPair(v)[0]" />
                  <span class="unit">{{ folderTotalStr[1] }}</span>
                </div>
              </div>
              <div class="stat-cell">
                <div class="label">models</div>
                <div class="value num-tab"><tween-n :value="folders.length" /></div>
              </div>
              <div class="stat-cell fix">
                <div class="label">.big</div>
                <div class="value num-tab"><tween-n :value="totalBig" /></div>
              </div>
            </div>

            <div class="disk-meter" v-if="disk.total">
              <span>disk</span>
              <div class="meter">
                <div class="fill" :style="'--used-p:' + diskUsedPct + '%'"></div>
              </div>
              <span class="num-tab">{{ diskUsedStr }} / {{ diskTotalStr }}</span>
              <span class="pct num-tab"><tween-n :value="diskUsedPct" :fmt="v => v.toFixed(1) + '%'" /></span>
            </div>

            <div class="folder-search">
              <input v-model="folderQuery" type="text"
                     placeholder="filter — type to search by name"
                     />
            </div>

            <div class="bulk-bar" v-if="selected.size > 0">
              <span class="bulk-count num-tab">{{ selected.size }}</span>
              <span class="bulk-label">selected</span>
              <span class="spacer"></span>
              <button class="btn ghost" style="padding: 4px 9px;" @click="clearSelection">clear</button>
              <button class="btn" style="padding: 4px 9px;" @click="bulkFixBig" :disabled="fixRunning">⚒ fix-big all</button>
              <button class="btn primary" style="padding: 4px 9px;" @click="bulkUpload" :disabled="botRunning">▶ upload all</button>
            </div>

            <div class="folder-list">
              <div v-if="visibleFolders.length === 0" class="empty-state">
                <span v-if="folderQuery">No model matches "<code>{{ folderQuery }}</code>".</span>
                <span v-else>No model folders found.</span>
              </div>
              <div v-for="f in visibleFolders" :key="f.name" class="folder-row"
                   :class="{ selected: selected.has(f.name) }">
                <div class="frow-bar" :style="'--w:' + (f.size_bytes / maxFolderSize * 100) + '%'"></div>
                <label class="folder-check" @click.stop>
                  <input type="checkbox" :checked="selected.has(f.name)"
                         @change="toggleSelect(f.name)" />
                  <span class="folder-check-box"></span>
                </label>
                <div class="fmain-wrap" style="min-width: 0;">
                  <div class="fmain">
                    <span class="fname">{{ f.name }}</span>
                    <span class="fsize num-tab">{{ fmtBytes(f.size_bytes) }}</span>
                  </div>
                  <div class="fmeta">
                    <span class="chip" :class="f.available > 0 ? 'avail' : 'up'">
                      <span class="v num-tab">{{ f.video_count }}</span>vid
                    </span>
                    <span class="chip avail" v-if="f.available > 0">
                      <span class="v num-tab">{{ f.available }}</span>ready
                    </span>
                    <span class="chip big" v-if="f.pending_big > 0">
                      <span class="v num-tab">{{ f.pending_big }}</span>.big
                    </span>
                    <span class="chip up" v-if="f.uploaded > 0">
                      <span class="v num-tab">{{ f.uploaded }}</span>up
                    </span>
                  </div>
                </div>
                <div class="factions">
                  <button class="mini-btn upload"
                          :disabled="botRunning || f.available === 0"
                          @click="startJob('bot-upload', f.name)"
                          :title="'Upload ' + f.available + ' video(s) from ' + f.name">▶</button>
                  <button class="mini-btn fix"
                          :disabled="fixRunning || f.pending_big === 0"
                          @click="startJob('fix-big', f.name)"
                          :title="'Fix ' + f.pending_big + ' .big file(s) in ' + f.name">⚒</button>
                </div>
              </div>
            </div>
          </section>
        </div>

        <div class="col-right">
          <!-- NETWORK card -->
          <section class="card net-card" :class="{ 'compact': currentBot && currentFix }">
            <div class="card-head">
              <span>Throughput</span>
              <span class="spacer"></span>
              <span class="badge dim">/proc/net/dev · 60s</span>
            </div>
            <div class="card-body net-card-body no-pad">
              <div class="gauges">
                <gauge :value="latestTx" label="↑ upload" accent="var(--brand)"></gauge>
                <gauge :value="latestRx" label="↓ download" accent="var(--c-info)"></gauge>
              </div>
            </div>
          </section>

          <!-- NOW PLAYING card -->
          <section class="card np-card" :class="{ running: anyRunning }">
            <div class="card-head">
              <span>Active Transfer</span>
              <span class="spacer"></span>
              <span class="badge" v-if="botRunning && botEtaStr">
                <span style="color: var(--text-4); margin-right: 4px;">bot ETA</span>
                <span class="num-tab" style="color: var(--c-ok);">~{{ botEtaStr }}</span>
              </span>
              <span class="badge" v-if="fixRunning && fixEtaStr">
                <span style="color: var(--text-4); margin-right: 4px;">fix ETA</span>
                <span class="num-tab" style="color: var(--c-fix);">~{{ fixEtaStr }}</span>
              </span>
            </div>

            <div v-if="!currentBot && !currentFix" class="np-empty">
              <div>No active transfer</div>
              <div class="hint">Press <kbd style="background:var(--bg-2);border:1px solid var(--line);padding:0 5px;border-radius:3px;color:var(--brand);font-family:var(--mono);">s</kbd> for bot-upload · <kbd style="background:var(--bg-2);border:1px solid var(--line);padding:0 5px;border-radius:3px;color:var(--c-fix);font-family:var(--mono);">f</kbd> for fix-big</div>
            </div>

            <div v-else class="np-stack">
              <!-- Bot-upload panel -->
              <div v-if="currentBot" class="np-panel np-bot" :class="{ forwarding: currentBot.forwarding }">
                <div class="np-top">
                  <div class="np-file">
                    <div class="np-kind-row">
                      <span class="np-kind-tag bot">↑ bot-upload</span>
                      <span v-if="currentBot.forwarding" class="np-fwd-tag">⤴ relay → telegram</span>
                      <span v-if="currentBot.model" class="np-model">⊙ {{ currentBot.model }}</span>
                    </div>
                    <div class="name">{{ currentBot.name }}</div>
                  </div>
                  <div v-if="!currentBot.forwarding" class="np-pct num-tab bot"><tween-n :value="currentBot.pct" :fmt="v => Math.round(v) + '%'" /></div>
                  <div v-else class="np-pct num-tab bot fwd-speed">
                    <span class="arrow">↑</span><tween-n :value="latestTx" :duration="700" :fmt="v => fmtBytesPerSec(v)[0]" /><span class="unit">{{ txStr[1] }}</span>
                  </div>
                </div>
                <div class="np-bar bot" :class="{ forwarding: currentBot.forwarding }">
                  <div class="fill" :style="'--p:' + currentBot.pct + '%'"></div>
                  <div class="shine"></div>
                </div>
                <div class="np-stats">
                  <div class="np-stat">
                    <div class="label">transferred</div>
                    <div class="value num-tab">{{ currentBot.current || '—' }}</div>
                  </div>
                  <div class="np-stat">
                    <div class="label">total</div>
                    <div class="value num-tab dim">{{ currentBot.total }}</div>
                  </div>
                  <div class="np-stat">
                    <div class="label">uploaded</div>
                    <div class="value num-tab brand"><tween-n :value="sessionStats.bot_done" /></div>
                  </div>
                  <div class="np-stat">
                    <div class="label">failed</div>
                    <div class="value num-tab"
                         :style="{ color: sessionStats.bot_failed > 0 ? 'var(--c-err)' : 'var(--text-3)' }">
                      <tween-n :value="sessionStats.bot_failed" />
                    </div>
                  </div>
                </div>
              </div>

              <!-- Fix-big panel -->
              <div v-if="currentFix" class="np-panel np-fix">
                <div class="np-top">
                  <div class="np-file">
                    <div class="np-kind-row">
                      <span class="np-kind-tag fix">⚒ fix-big</span>
                      <span class="np-model" style="color: var(--c-fix);">{{ currentFix.current }} / {{ currentFix.total }}</span>
                    </div>
                    <div class="name">{{ currentFix.name }}</div>
                  </div>
                  <div class="np-pct num-tab fix"><tween-n :value="currentFix.pct" :fmt="v => Math.round(v) + '%'" /></div>
                </div>
                <div class="np-bar fix">
                  <div class="fill" :style="'--p:' + currentFix.pct + '%'"></div>
                  <div class="shine"></div>
                </div>
                <div class="np-stats">
                  <div class="np-stat">
                    <div class="label">file</div>
                    <div class="value num-tab">{{ currentFix.current }}/{{ currentFix.total }}</div>
                  </div>
                  <div class="np-stat">
                    <div class="label">fixed</div>
                    <div class="value num-tab" style="color: var(--c-fix);"><tween-n :value="sessionStats.fix_done" /></div>
                  </div>
                  <div class="np-stat">
                    <div class="label">remaining</div>
                    <div class="value num-tab dim">
                      <tween-n :value="Math.max(0, sessionStats.fix_total - sessionStats.fix_done)" />
                    </div>
                  </div>
                  <div class="np-stat">
                    <div class="label">failed</div>
                    <div class="value num-tab"
                         :style="{ color: sessionStats.fix_failed > 0 ? 'var(--c-err)' : 'var(--text-3)' }">
                      <tween-n :value="sessionStats.fix_failed" />
                    </div>
                  </div>
                </div>
              </div>
            </div>
          </section>

          <!-- FAILED LIST card (only visible when there are failures) -->
          <section v-if="failedFiles.length > 0" class="card fail-card">
            <div class="card-head">
              <span style="color: var(--c-err);">Failed · {{ failedFiles.length }}</span>
              <span class="spacer"></span>
              <button class="btn ghost" style="padding: 3px 8px;" @click="clearFailed">clear</button>
            </div>
            <div class="fail-list">
              <div v-for="(f, i) in failedFiles" :key="i" class="fail-row">
                <div style="min-width: 0;">
                  <div class="fname">{{ f.name }}</div>
                  <div class="fmeta">
                    <span v-if="f.model" class="model">⊙ {{ f.model }}</span>
                    · {{ f.kind }} · {{ new Date(f.at).toLocaleTimeString() }}
                  </div>
                </div>
                <button class="btn" style="padding: 4px 10px;"
                        :disabled="botRunning || !f.model"
                        @click="rerunFolderForFailed(f)"
                        :title="f.model ? ('rerun bot-upload on ' + f.model) : 'no model info'">
                  rerun
                </button>
              </div>
            </div>
          </section>

          <!-- TERMINAL card -->
          <section class="card terminal-card" :class="{ running: anyRunning }">
            <div class="card-head term-head">
              <span class="term-title">Terminal</span>
              <div class="ch-tabs">
                <button v-for="ch in CHANNELS" :key="ch"
                        class="ch-tab" :class="['t-' + ch.replace('-', ''), { active: activeChannel === ch }]"
                        @click="switchChannel(ch)">
                  <span class="ch-dot" :class="'t-' + ch.replace('-', '')"></span>
                  <span class="ch-name">{{ ch }}</span>
                  <span class="ch-count num-tab"><tween-n :value="channelLineCounts[ch]" /></span>
                </button>
              </div>
              <span class="spacer"></span>
              <span class="badge" v-if="lineRate > 0">
                <span style="color: var(--text-4); margin-right: 4px;">lps</span>
                <span class="num-tab" style="color: var(--brand-strong);"><tween-n :value="lineRate" :fmt="v => v.toFixed(1)" /></span>
              </span>
            </div>

            <div ref="termEl" class="terminal" @scroll="onTermScroll">
              <div v-for="line in displayLines" :key="line.id" class="log-line"
                   :class="{ fresh: line.fresh, progress: !!line.progress }">
                <span class="lno num-tab">{{ line.lno }}</span>
                <span class="ts num-tab">{{ line.ts }}</span>
                <span class="tag" :class="line.tagClass" v-if="line.tag">[{{ line.tag }}]</span>
                <span class="tag" v-else></span>
                <span class="content" v-if="!line.progress" v-html="line.html"></span>
                <span class="content" v-else>
                  <span class="num num-tab">{{ line.progress.current }}</span>
                  <span class="arrow">/</span>
                  <span class="num num-tab">{{ line.progress.total }}</span>
                  <span class="bar"><span class="fill" :style="'--p:' + line.progress.pct + '%'"></span><span class="shine"></span></span>
                  <span class="num num-tab" style="min-width: 36px">{{ line.progress.pct }}%</span>
                </span>
              </div>
              <div class="cursor-line">
                <span class="lno num-tab">{{ logLines.length + 1 }}</span>
                <span class="ts"></span>
                <span class="tag"></span>
                <span class="content"><span class="blink"></span></span>
              </div>
            </div>

            <div class="terminal-foot">
              <button class="btn ghost" style="padding: 4px 10px;" @click="clearLogs"><span class="kbd">c</span>clear</button>
              <button class="btn ghost" style="padding: 4px 10px;" @click="toggleAutoScroll">
                <span class="kbd">p</span>{{ autoScroll ? 'auto-scroll on' : 'paused' }}
              </button>
              <div class="spacer"></div>
              <span class="stat" v-if="anyRunning">
                <span style="color: var(--text-4); margin-right: 4px;">running</span>
                <span class="v num-tab">{{ runningSummary }}</span>
              </span>
            </div>
          </section>
        </div>
      </main>

      <div class="toast" v-if="lastErr">{{ lastErr }}</div>

      <dialog ref="settingsDlg" class="settings" @close="onSettingsClose"
              @click="onDialogBackdropClick">
        <div class="set-head">
          <span class="label">Settings</span>
          <span>bots.json</span>
          <span class="spacer"></span>
          <span class="meta" v-if="bots.path">{{ bots.path }}</span>
          <button class="btn ghost" style="padding: 4px 8px;" @click="closeSettings">esc</button>
        </div>

        <div class="set-body">
          <div class="set-section">
            <h3>Account</h3>
            <div class="set-field">
              <label>Current password</label>
              <input class="input" type="password" v-model="pwForm.current"
                     autocomplete="current-password" />
            </div>
            <div class="set-field">
              <label>New password</label>
              <input class="input" type="password" v-model="pwForm.next"
                     autocomplete="new-password" />
            </div>
            <div class="set-field">
              <label>Confirm new password</label>
              <input class="input" type="password" v-model="pwForm.confirm"
                     autocomplete="new-password" />
            </div>
            <div style="display: flex; gap: 8px; align-items: center;">
              <button class="btn" @click="changePassword" :disabled="pwForm.saving">
                {{ pwForm.saving ? 'changing…' : 'Change password' }}
              </button>
              <span :class="['pw-status', pwForm.statusKind]"
                    style="font-size: 11.5px; font-family: var(--mono);">{{ pwForm.status }}</span>
            </div>
          </div>

          <div class="set-section">
            <h3>Uploader behavior</h3>
            <div class="set-field">
              <label>Default upload directory</label>
              <input class="input" v-model="env.DEFAULT_UPLOAD_DIR"
                     placeholder="/mnt/storage/ctbrec/media" />
            </div>
            <div class="set-field">
              <label>Exempt folders (comma-separated full paths)</label>
              <input class="input" v-model="env.EXEMPT_FOLDERS"
                     placeholder="/mnt/storage/.../alice_kosmos,..." />
            </div>
            <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 10px;">
              <label class="env-toggle">
                <input type="checkbox" v-model="env.DELETE_AFTER_UPLOAD_b" />
                <span>delete source after upload</span>
              </label>
              <label class="env-toggle">
                <input type="checkbox" v-model="env.MARK_UPLOADED_FILES_b" />
                <span>mark with .uploaded suffix</span>
              </label>
            </div>
            <div class="set-field" style="margin-top: 10px;">
              <label>Uploaded suffix</label>
              <input class="input" v-model="env.UPLOADED_SUFFIX"
                     placeholder=".uploaded" />
            </div>
            <div style="display: flex; gap: 8px; align-items: center;">
              <button class="btn" @click="saveEnv" :disabled="env.saving">
                {{ env.saving ? 'saving…' : 'Save uploader config' }}
              </button>
              <span :class="['pw-status', env.statusKind]"
                    style="font-size: 11.5px; font-family: var(--mono);">{{ env.status }}</span>
            </div>
          </div>

          <div class="set-section">
            <h3>Endpoint</h3>
            <div class="set-field">
              <label>Bot API URL</label>
              <input class="input" v-model="bots.api_url" placeholder="http://127.0.0.1:8081"
                     />
            </div>
            <div class="set-field">
              <label>Channel ID</label>
              <input class="input" v-model="bots.channel_id" placeholder="-1001810923743 or @channel"
                     />
            </div>
          </div>

          <div class="set-section">
            <h3>Schedules ({{ schedules.list.length }})</h3>
            <p class="set-hint">Cron format: <code>min hour dom mon dow</code> · examples: <code>0 2 * * *</code> nightly 2am, <code>*/30 * * * *</code> every 30 min, <code>0 4 * * 1</code> 4am every Monday.</p>
            <div v-for="(s, i) in schedules.list" :key="i" class="sched-row">
              <input v-model="s.name" placeholder="name" />
              <input v-model="s.cron" placeholder="0 2 * * *" class="mono-input" />
              <select v-model="s.type">
                <option value="fix-big">fix-big</option>
                <option value="bot-upload">bot-upload</option>
              </select>
              <label class="env-toggle" style="padding: 6px 10px; gap: 6px;">
                <input type="checkbox" v-model="s.enabled" />
                <span style="font-size: 11px;">on</span>
              </label>
              <button class="icon-btn" @click="saveSchedule(i)" :disabled="s.saving"
                      :title="s.id ? 'save' : 'create'">{{ s.id ? '✓' : '+' }}</button>
              <button class="icon-btn del" v-if="s.id" @click="removeSchedule(i)" title="delete">×</button>
            </div>
            <button class="add-bot" @click="addSchedule">＋ add schedule</button>
            <div v-if="schedules.status"
                 :class="['pw-status', schedules.statusKind]"
                 style="margin-top: 8px; font-size: 11.5px;">{{ schedules.status }}</div>
          </div>

          <div class="set-section">
            <h3>Recent Runs</h3>
            <div v-if="history.items.length === 0" style="color: var(--text-4); font-size: 12px;">
              No history yet.
            </div>
            <div v-for="r in history.items.slice(0, 10)" :key="r.id" class="hist-row">
              <span class="hist-type" :class="'t-' + r.type.replace('-', '')">{{ r.type }}</span>
              <span class="hist-status" :class="'s-' + r.status">{{ r.status }}</span>
              <span class="hist-trigger">{{ r.trigger }}</span>
              <span class="hist-time num-tab">{{ new Date(r.started_at * 1000).toLocaleString() }}</span>
              <span class="hist-dur num-tab" v-if="r.duration > 0">{{ formatDur(r.duration) }}</span>
            </div>
            <button class="btn ghost" style="margin-top: 8px;" @click="reloadHistory">refresh</button>
          </div>

          <div class="set-section">
            <h3>Bots ({{ bots.list.length }})</h3>
            <div v-for="(b, i) in bots.list" :key="i" class="bot-row">
              <input v-model="b.name" placeholder="bot_name" />
              <input v-model="b.token" :class="['token', { hidden: !bots.showTokens }]"
                     placeholder="123456:AAA-bbb..." />
              <button class="icon-btn" @click="bots.showTokens = !bots.showTokens"
                      :title="bots.showTokens ? 'hide tokens' : 'show tokens'">
                {{ bots.showTokens ? '◉' : '◌' }}
              </button>
              <button class="icon-btn del" @click="removeBot(i)" title="remove">×</button>
            </div>
            <button class="add-bot" @click="addBot">＋ add bot</button>
          </div>
        </div>

        <div class="set-foot">
          <span class="status" :class="bots.statusKind">{{ bots.status }}</span>
          <button class="btn ghost" @click="reloadBots">reload</button>
          <button class="btn" @click="closeSettings">cancel</button>
          <button class="btn primary" @click="saveBots" :disabled="bots.saving">
            {{ bots.saving ? 'saving…' : 'save' }}
          </button>
        </div>
      </dialog>
    </template>
  `,
  setup() {
    const token = ref(getToken());
    const sseConnected = ref(false);
    const heartbeat = ref(false);

    // Per-job state from /api/job/status
    const fixState = ref({ running: false, started_at: 0, finished_at: 0, dir: '' });
    const botState = ref({ running: false, started_at: 0, finished_at: 0, dir: '' });
    const activePathCount = ref(0);
    const fixRunning = computed(() => fixState.value.running);
    const botRunning = computed(() => botState.value.running);
    const anyRunning = computed(() => fixRunning.value || botRunning.value);

    const tick = ref(0);
    setInterval(() => { tick.value++; }, 1000);
    const fixMeta = computed(() => {
      tick.value;
      if (!fixState.value.running) {
        if (fixState.value.finished_at) {
          const secs = Math.floor(Date.now() / 1000) - fixState.value.finished_at;
          if (secs < 60) return `idle · finished ${secs}s ago`;
          if (secs < 3600) return `idle · finished ${Math.floor(secs/60)}m ago`;
          return 'idle';
        }
        return 'idle';
      }
      return 'running · ' + elapsed(Date.now()/1000 - fixState.value.started_at);
    });
    const botMeta = computed(() => {
      tick.value;
      if (!botState.value.running) {
        if (botState.value.finished_at) {
          const secs = Math.floor(Date.now() / 1000) - botState.value.finished_at;
          if (secs < 60) return `idle · finished ${secs}s ago`;
          if (secs < 3600) return `idle · finished ${Math.floor(secs/60)}m ago`;
          return 'idle';
        }
        return 'idle';
      }
      return 'running · ' + elapsed(Date.now()/1000 - botState.value.started_at);
    });
    const runningSummary = computed(() => {
      const parts = [];
      if (fixRunning.value) parts.push('fix ' + elapsed(Date.now()/1000 - fixState.value.started_at));
      if (botRunning.value) parts.push('bot ' + elapsed(Date.now()/1000 - botState.value.started_at));
      return parts.join('  ·  ');
    });

    // Pending (just the count; the rich data lives in folders[])
    const pendingFolders = ref([]);
    const pendingTotal = computed(() => pendingFolders.value.reduce((s, f) => s + f.count, 0));
    const uploadDir = ref('');

    // All folders + disk
    const folders = ref([]);
    const folderQuery = ref('');
    const selected = ref(new Set());      // model names checked
    function toggleSelect(name) {
      const s = new Set(selected.value);
      if (s.has(name)) s.delete(name); else s.add(name);
      selected.value = s;
    }
    function selectAllVisible() {
      const s = new Set(selected.value);
      visibleFolders.value.forEach(f => s.add(f.name));
      selected.value = s;
    }
    function clearSelection() { selected.value = new Set(); }
    async function bulkUpload() {
      const names = Array.from(selected.value);
      if (names.length === 0) return;
      const dirs = names.map(n => uploadDir.value + '/' + n);
      try {
        await api('/api/job/start', {
          method: 'POST',
          body: JSON.stringify({ type: 'bot-upload', dirs }),
        });
        clearSelection();
        await refreshStatus();
      } catch (e) { setErr(e.message || String(e)); }
    }
    async function bulkFixBig() {
      const names = Array.from(selected.value);
      if (names.length === 0) return;
      const dirs = names.map(n => uploadDir.value + '/' + n);
      try {
        await api('/api/job/start', {
          method: 'POST',
          body: JSON.stringify({ type: 'fix-big', dirs }),
        });
        clearSelection();
        await refreshStatus();
      } catch (e) { setErr(e.message || String(e)); }
    }
    const totalBytes = ref(0);
    const disk = ref({ total: 0, used: 0, free: 0 });
    const maxFolderSize = computed(() =>
        Math.max(1, ...folders.value.map(f => f.size_bytes)));
    const totalBig = computed(() =>
        folders.value.reduce((s, f) => s + (f.pending_big || 0), 0));
    const folderTotalStr = computed(() => {
        const [v, u] = fmtBytesPair(totalBytes.value);
        return [v, u];
    });
    const diskUsedPct = computed(() =>
        disk.value.total ? (disk.value.used / disk.value.total * 100) : 0);
    const diskUsedStr = computed(() => fmtBytes(disk.value.used));
    const diskTotalStr = computed(() => fmtBytes(disk.value.total));
    const visibleFolders = computed(() => {
        const q = folderQuery.value.toLowerCase().trim();
        if (!q) return folders.value;
        return folders.value.filter(f => f.name.toLowerCase().includes(q));
    });

    // Network
    const netSamples = ref([]);
    const latestTx = ref(0);
    const latestRx = ref(0);
    const txStr = computed(() => fmtBytesPerSec(latestTx.value));
    const rxStr = computed(() => fmtBytesPerSec(latestRx.value));
    const sparkW = 400;
    const sparkH = 110;
    const sparkPathTx = computed(() => buildSparkLine(netSamples.value, 'tx', sparkW, sparkH));
    const sparkPathRx = computed(() => buildSparkLine(netSamples.value, 'rx', sparkW, sparkH));
    const sparkPathTxArea = computed(() => buildSparkArea(netSamples.value, 'tx', sparkW, sparkH));
    const sparkPathRxArea = computed(() => buildSparkArea(netSamples.value, 'rx', sparkW, sparkH));
    const lastTxPoint = computed(() => buildLastPoint(netSamples.value, 'tx', sparkW, sparkH));

    // Logs — kept per-channel; the UI tab decides what to render
    const CHANNELS = ['fix-big', 'bot-upload', 'web'];
    const logsByChannel = ref({ 'fix-big': [], 'bot-upload': [], 'web': [] });
    const MAX_LINES = 5000;
    let nextId = 1;
    const channelLnoCounters = { 'fix-big': 0, 'bot-upload': 0, 'web': 0 };
    const activeChannel = ref('fix-big');     // which tab is shown
    const logLines = computed(() => logsByChannel.value[activeChannel.value] || []);
    const displayLines = computed(() => logLines.value);
    const channelLineCounts = computed(() => ({
        'fix-big': logsByChannel.value['fix-big'].length,
        'bot-upload': logsByChannel.value['bot-upload'].length,
        'web': logsByChannel.value['web'].length,
    }));
    const termEl = ref(null);
    const autoScroll = ref(true);

    // -- Current upload + ETA tracking --
    // Two independent slots so fix-big and bot-upload can be tracked in
    // parallel (Now Playing card shows both when running).
    const currentBot = ref(null);    // { name, size, model, pct, current, total, startedAt }
    const currentFix = ref(null);    // same shape, current/total are item index, not bytes
    const currentUpload = computed(() => currentBot.value || currentFix.value);
    const sessionStats = ref({
      // Bot upload session
      bot_done: 0,
      bot_failed: 0,
      bot_first_at: 0,    // time of first Sending: line
      // Fix-big session
      fix_total: 0,
      fix_done: 0,
      fix_failed: 0,
      fix_first_at: 0,
    });
    const failedFiles = ref([]); // [{name, model, kind, at}]

    // Patterns
    const RE_BOT_SEND   = /^Sending:\s+([^\(]+?)\s*\(([^)]+)\)\s*\[([^\]]+)\]\s*$/;
    const RE_BOT_OK     = /^Success:\s+(\S+)\s*\(([^)]+)\)\s*$/;
    const RE_BOT_FAIL   = /^Upload failed:\s+(\S+)/;
    const RE_BOT_SKIP   = /^\s*\S+\s+exceeds Bot API limit/i;
    const RE_FIX_FOUND  = /^Found\s+(\d+)\s+oversized files/;
    const RE_FIX_ITEM   = /^\[(\d+)\/(\d+)\]\s+(\S+)/;
    const RE_FIX_SAVED  = /^Saved:\s+(\S+)/;
    const RE_PROGRESS   = /↑\s*([\d.]+)\s*([KMG]?i?B?)\s*\/\s*([\d.]+)\s*([KMG]?i?B?)\s*\((\d+)%\)/;

    function pushLogParse(parsedLine) {
      // parsedLine: { ts, tag, tagClass, rest, progress }
      const tag = (parsedLine.tag || '').toUpperCase();
      const r = parsedLine.rest || '';

      if (tag === 'INFO' || (parsedLine.tag || '').toLowerCase().includes('bot') ||
          (parsedLine.tag || '').toLowerCase().includes('upload')) {
        let m;
        if ((m = r.match(RE_BOT_SEND))) {
          currentBot.value = {
            name: m[1].trim(),
            size: m[2].trim(),
            model: m[3].trim(),
            pct: 0,
            current: '',
            total: m[2].trim(),
            startedAt: Date.now(),
            forwarding: false,
            kind: 'bot',
          };
          if (!sessionStats.value.bot_first_at) {
            sessionStats.value.bot_first_at = Date.now();
          }
        } else if ((m = r.match(RE_BOT_OK))) {
          sessionStats.value.bot_done++;
          currentBot.value = null;
        } else if ((m = r.match(RE_BOT_FAIL))) {
          sessionStats.value.bot_failed++;
          failedFiles.value.unshift({
            name: m[1],
            model: currentBot.value?.model || '',
            kind: 'bot',
            at: Date.now(),
          });
          if (failedFiles.value.length > 50) failedFiles.value.length = 50;
          currentBot.value = null;
        } else if (parsedLine.progress && currentBot.value) {
          currentBot.value.pct = parsedLine.progress.pct;
          currentBot.value.current = parsedLine.progress.current;
          currentBot.value.total = parsedLine.progress.total;
          // 100% only means "fully buffered to the local relay". The real
          // upload to Telegram starts now and its progress is unobservable.
          if (parsedLine.progress.pct >= 100) currentBot.value.forwarding = true;
        } else if (/forwarding to Telegram/i.test(r) && currentBot.value) {
          currentBot.value.forwarding = true;
          currentBot.value.pct = 100;
        }
      }
      if (tag === 'FIX-BIG') {
        let m;
        if ((m = r.match(RE_FIX_FOUND))) {
          sessionStats.value.fix_total = parseInt(m[1], 10);
          sessionStats.value.fix_done = 0;
          sessionStats.value.fix_failed = 0;
          sessionStats.value.fix_first_at = Date.now();
        } else if ((m = r.match(RE_FIX_ITEM))) {
          const done = parseInt(m[1], 10) - 1;
          const total = parseInt(m[2], 10);
          currentFix.value = {
            name: m[3],
            size: '',
            model: '',
            pct: total > 0 ? Math.round(done / total * 100) : 0,
            current: m[1],
            total: m[2],
            startedAt: Date.now(),
            kind: 'fix',
          };
        } else if ((m = r.match(RE_FIX_SAVED))) {
          sessionStats.value.fix_done++;
          // Bump pct on the in-flight card so the bar visibly advances.
          if (currentFix.value) {
            const t = parseInt(currentFix.value.total, 10) || 1;
            currentFix.value.pct = Math.min(100, Math.round(sessionStats.value.fix_done / t * 100));
          }
        }
      }
      // Job-done line clears the corresponding slot
      if (tag === 'JOB' && r.includes('done')) {
        if (r.includes('fix-big')) currentFix.value = null;
        if (r.includes('bot-upload')) currentBot.value = null;
      }
      if (tag === 'SKIP' || tag === 'WARN' || tag === 'WARNING') {
        if (RE_BOT_SKIP.test(r)) {
          // Counts as a renamed-to-.big skip; not a hard failure
        }
      }
    }

    // ETA + speed
    const bot_done = computed(() => sessionStats.value.bot_done);
    const fix_done = computed(() => sessionStats.value.fix_done);
    const bot_avg_sec = computed(() => {
      const stat = sessionStats.value;
      if (stat.bot_done === 0 || !stat.bot_first_at) return 0;
      const elapsed = (Date.now() - stat.bot_first_at) / 1000;
      return elapsed / stat.bot_done;
    });
    const fix_avg_sec = computed(() => {
      const stat = sessionStats.value;
      if (stat.fix_done === 0 || !stat.fix_first_at) return 0;
      const elapsed = (Date.now() - stat.fix_first_at) / 1000;
      return elapsed / stat.fix_done;
    });
    const totalAvailable = computed(() =>
      folders.value.reduce((s, f) => s + (f.available || 0), 0));
    const botEtaStr = computed(() => {
      if (!botRunning.value) return '';
      const remaining = totalAvailable.value - sessionStats.value.bot_done;
      if (remaining <= 0 || bot_avg_sec.value <= 0) return '';
      const secs = remaining * bot_avg_sec.value;
      return formatDur(secs);
    });
    const fixEtaStr = computed(() => {
      if (!fixRunning.value) return '';
      const remaining = sessionStats.value.fix_total - sessionStats.value.fix_done;
      if (remaining <= 0 || fix_avg_sec.value <= 0) return '';
      return formatDur(remaining * fix_avg_sec.value);
    });
    function formatDur(secs) {
      secs = Math.max(0, Math.floor(secs));
      if (secs < 60) return secs + 's';
      if (secs < 3600) return Math.floor(secs/60) + 'm ' + (secs%60) + 's';
      const h = Math.floor(secs/3600);
      const m = Math.floor((secs%3600)/60);
      return h + 'h ' + m + 'm';
    }
    function clearFailed() { failedFiles.value = []; }
    function rerunFolderForFailed(file) {
      // Try to extract model from filename: parent path is unknown but
      // failed-list also has model. Trigger bot-upload on that folder.
      if (file.model) startJob('bot-upload', file.model);
    }

    // Rate
    let recentLineCount = 0;
    const lineRate = ref(0);

    const lastErr = ref('');
    let errTimer = null;
    function setErr(msg) {
      lastErr.value = msg;
      if (errTimer) clearTimeout(errTimer);
      errTimer = setTimeout(() => { lastErr.value = ''; }, 4000);
    }

    // -- Settings: .env --
    const env = ref({
      DEFAULT_UPLOAD_DIR: '',
      EXEMPT_FOLDERS: '',
      UPLOADED_SUFFIX: '.uploaded',
      DELETE_AFTER_UPLOAD: 'true',
      MARK_UPLOADED_FILES: 'true',
      DELETE_AFTER_UPLOAD_b: true,
      MARK_UPLOADED_FILES_b: true,
      saving: false, status: '', statusKind: '',
    });
    async function reloadEnv() {
      try {
        const d = await api('/api/env');
        env.value.DEFAULT_UPLOAD_DIR = d.DEFAULT_UPLOAD_DIR || '';
        env.value.EXEMPT_FOLDERS     = d.EXEMPT_FOLDERS || '';
        env.value.UPLOADED_SUFFIX    = d.UPLOADED_SUFFIX || '.uploaded';
        env.value.DELETE_AFTER_UPLOAD   = d.DELETE_AFTER_UPLOAD  || 'false';
        env.value.MARK_UPLOADED_FILES   = d.MARK_UPLOADED_FILES  || 'false';
        env.value.DELETE_AFTER_UPLOAD_b = env.value.DELETE_AFTER_UPLOAD.toLowerCase() === 'true';
        env.value.MARK_UPLOADED_FILES_b = env.value.MARK_UPLOADED_FILES.toLowerCase() === 'true';
        env.value.status = '';
        env.value.statusKind = '';
      } catch (e) {
        env.value.status = e.message || 'load failed';
        env.value.statusKind = 'err';
      }
    }
    async function saveEnv() {
      env.value.saving = true;
      env.value.status = '';
      env.value.statusKind = '';
      const payload = {
        DEFAULT_UPLOAD_DIR: env.value.DEFAULT_UPLOAD_DIR.trim(),
        EXEMPT_FOLDERS:     env.value.EXEMPT_FOLDERS.trim(),
        UPLOADED_SUFFIX:    env.value.UPLOADED_SUFFIX.trim(),
        DELETE_AFTER_UPLOAD: env.value.DELETE_AFTER_UPLOAD_b ? 'true' : 'false',
        MARK_UPLOADED_FILES: env.value.MARK_UPLOADED_FILES_b ? 'true' : 'false',
      };
      try {
        const r = await api('/api/env', { method: 'POST', body: JSON.stringify(payload) });
        env.value.status = `saved · ${r.updated} key(s) updated`;
        env.value.statusKind = 'ok';
      } catch (e) {
        env.value.status = e.message || 'save failed';
        env.value.statusKind = 'err';
      } finally {
        env.value.saving = false;
      }
    }

    // -- Settings: change password --
    const pwForm = ref({
      current: '', next: '', confirm: '',
      saving: false, status: '', statusKind: '',
    });
    async function changePassword() {
      pwForm.value.status = '';
      pwForm.value.statusKind = '';
      const cur = pwForm.value.current;
      const next = pwForm.value.next;
      const conf = pwForm.value.confirm;
      if (!cur || !next) {
        pwForm.value.status = 'fill both passwords';
        pwForm.value.statusKind = 'err';
        return;
      }
      if (next !== conf) {
        pwForm.value.status = 'new + confirm don\'t match';
        pwForm.value.statusKind = 'err';
        return;
      }
      if (next.length < 6) {
        pwForm.value.status = 'new password too short (≥6)';
        pwForm.value.statusKind = 'err';
        return;
      }
      pwForm.value.saving = true;
      try {
        await api('/api/auth/change_password', {
          method: 'POST',
          body: JSON.stringify({ current_password: cur, new_password: next }),
        });
        pwForm.value.status = 'password updated';
        pwForm.value.statusKind = 'ok';
        pwForm.value.current = '';
        pwForm.value.next = '';
        pwForm.value.confirm = '';
      } catch (e) {
        pwForm.value.status = e.message || 'failed';
        pwForm.value.statusKind = 'err';
      } finally {
        pwForm.value.saving = false;
      }
    }

    // -- Settings: schedules --
    const schedules = ref({
      list: [],            // [{id, name, cron, type, dirs, enabled, last_run_at, saving}]
      status: '',
      statusKind: '',
    });
    async function reloadSchedules() {
      try {
        const d = await api('/api/schedules');
        schedules.value.list = (d.items || []).map(s => ({
          id: s.id, name: s.name, cron: s.cron, type: s.type,
          dirs: s.dirs || [], enabled: s.enabled, last_run_at: s.last_run_at,
          saving: false,
        }));
        schedules.value.status = '';
        schedules.value.statusKind = '';
      } catch (e) {
        schedules.value.status = e.message || 'load failed';
        schedules.value.statusKind = 'err';
      }
    }
    function addSchedule() {
      schedules.value.list.push({
        id: 0, name: '', cron: '0 2 * * *', type: 'fix-big',
        dirs: [], enabled: true, last_run_at: 0, saving: false,
      });
    }
    async function saveSchedule(i) {
      const s = schedules.value.list[i];
      if (!s.name.trim() || !s.cron.trim()) {
        schedules.value.status = 'name + cron required';
        schedules.value.statusKind = 'err';
        return;
      }
      s.saving = true;
      try {
        const body = {
          name: s.name.trim(), cron: s.cron.trim(), type: s.type,
          dirs: s.dirs, enabled: s.enabled,
        };
        if (s.id) {
          await api('/api/schedules/' + s.id, { method: 'PUT', body: JSON.stringify(body) });
        } else {
          const r = await api('/api/schedules', { method: 'POST', body: JSON.stringify(body) });
          s.id = r.id;
        }
        schedules.value.status = 'saved';
        schedules.value.statusKind = 'ok';
      } catch (e) {
        schedules.value.status = e.message || 'save failed';
        schedules.value.statusKind = 'err';
      } finally {
        s.saving = false;
      }
    }
    async function removeSchedule(i) {
      const s = schedules.value.list[i];
      if (!s.id) { schedules.value.list.splice(i, 1); return; }
      try {
        await api('/api/schedules/' + s.id, { method: 'DELETE' });
        schedules.value.list.splice(i, 1);
        schedules.value.status = 'deleted';
        schedules.value.statusKind = 'ok';
      } catch (e) {
        schedules.value.status = e.message || 'delete failed';
        schedules.value.statusKind = 'err';
      }
    }

    // -- Settings: history --
    const history = ref({ items: [] });
    async function reloadHistory() {
      try {
        const d = await api('/api/job/history?limit=20');
        history.value.items = d.items || [];
      } catch (e) { /* silent */ }
    }

    // -- Settings (bots.json) --
    const settingsDlg = ref(null);
    const bots = ref({
      api_url: '',
      channel_id: '',
      list: [],            // [{name, token}]
      path: '',
      showTokens: false,
      saving: false,
      status: '',
      statusKind: '',      // '', 'err', 'ok'
    });
    async function reloadBots() {
      bots.value.status = 'loading…';
      bots.value.statusKind = '';
      try {
        const d = await api('/api/bots');
        bots.value.api_url = d.api_url || '';
        bots.value.channel_id = d.channel_id || '';
        bots.value.path = d._path || '';
        bots.value.list = Object.entries(d.bots || {}).map(([name, token]) => ({ name, token }));
        bots.value.status = '';
      } catch (e) {
        bots.value.status = e.message || String(e);
        bots.value.statusKind = 'err';
      }
    }
    async function openSettings() {
      await Promise.all([reloadBots(), reloadEnv(), reloadSchedules(), reloadHistory()]);
      const d = settingsDlg.value;
      if (d && !d.open) d.showModal();
    }
    function closeSettings() {
      const d = settingsDlg.value;
      if (d && d.open) d.close();
    }
    function onSettingsClose() {
      bots.value.status = '';
      bots.value.statusKind = '';
    }
    // Native <dialog> doesn't close on backdrop click by default — wire it up.
    function onDialogBackdropClick(ev) {
      // The click reaches dialog element itself only when user clicks the
      // backdrop (the dialog's own padding area). Content clicks bubble from
      // children with a non-dialog `target`.
      if (ev.target === settingsDlg.value) closeSettings();
    }
    function addBot() {
      bots.value.list.push({ name: '', token: '' });
    }
    function removeBot(i) {
      bots.value.list.splice(i, 1);
    }
    async function saveBots() {
      // Client-side validation: unique non-empty names
      const seen = new Set();
      for (const b of bots.value.list) {
        if (!b.name.trim() || !b.token.trim()) {
          bots.value.status = 'every bot needs a name AND a token';
          bots.value.statusKind = 'err';
          return;
        }
        if (seen.has(b.name)) {
          bots.value.status = `duplicate bot name: ${b.name}`;
          bots.value.statusKind = 'err';
          return;
        }
        seen.add(b.name);
      }
      if (bots.value.list.length === 0) {
        bots.value.status = 'at least one bot required';
        bots.value.statusKind = 'err';
        return;
      }
      const payload = {
        api_url: bots.value.api_url.trim(),
        channel_id: bots.value.channel_id.trim(),
        bots: Object.fromEntries(bots.value.list.map(b => [b.name.trim(), b.token.trim()])),
      };
      bots.value.saving = true;
      bots.value.status = '';
      try {
        const r = await api('/api/bots', { method: 'POST', body: JSON.stringify(payload) });
        bots.value.status = `saved · ${r.bots_count} bot(s) · ${r.note || ''}`;
        bots.value.statusKind = 'ok';
      } catch (e) {
        bots.value.status = e.message || String(e);
        bots.value.statusKind = 'err';
      } finally {
        bots.value.saving = false;
      }
    }

    // Wire 401 → drop back to login overlay (no page reload)
    onUnauthorized = () => { token.value = ''; };

    // Login form state
    const loginForm = ref({ username: '', password: '', loading: false });
    const loginErr = ref('');

    async function doLogin() {
      loginErr.value = '';
      loginForm.value.loading = true;
      try {
        const res = await fetch('/api/auth/login', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            username: loginForm.value.username,
            password: loginForm.value.password,
          }),
        });
        // Don't blindly call res.json() — nginx can return HTML on rate-limit (503),
        // gateway errors, etc. Safari throws "The string did not match the expected
        // pattern" on res.json() of non-JSON. Branch on content-type.
        const ct = res.headers.get('content-type') || '';
        let d = {};
        if (ct.includes('application/json')) {
          try { d = await res.json(); } catch { d = {}; }
        } else {
          // Read text just so we can show something helpful
          const t = await res.text().catch(() => '');
          if (!res.ok) {
            if (res.status === 503) {
              throw new Error('Too many login attempts — wait a minute and try again');
            }
            throw new Error(`HTTP ${res.status}${t ? ': ' + t.slice(0, 120) : ''}`);
          }
        }
        if (!res.ok) throw new Error(d.detail || `Login failed (HTTP ${res.status})`);
        if (!d.token) throw new Error('No token in response');
        localStorage.setItem(TOKEN_KEY, d.token);
        token.value = d.token;
        loginForm.value.password = '';
      } catch (e) {
        loginErr.value = e.message || 'Login failed';
      } finally {
        loginForm.value.loading = false;
      }
    }
    function logout() {
      dropToken();
      token.value = '';
      loginForm.value = { username: '', password: '', loading: false };
    }

    function applyStatus(s) {
      if (!s) return;
      if (s['fix-big']) fixState.value = s['fix-big'];
      if (s['bot-upload']) botState.value = s['bot-upload'];
      activePathCount.value = s.active_paths || 0;
    }
    // Initial fetch on load; live updates arrive over SSE (`event: status`).
    async function refreshStatus() {
      try { applyStatus(await api('/api/job/status')); }
      catch (e) { setErr(e.message || String(e)); }
    }
    async function refreshPending() {
      try {
        const p = await api('/api/job/pending');
        uploadDir.value = p.root || '';
        pendingFolders.value = (p.folders || []).slice().sort((a, b) => b.count - a.count);
      } catch (e) { setErr(e.message || String(e)); }
    }
    async function refreshFolders() {
      try {
        const r = await api('/api/folders');
        if (r.root) uploadDir.value = r.root;
        folders.value = r.folders || [];
        totalBytes.value = r.total_bytes || 0;
      } catch (e) { setErr(e.message || String(e)); }
    }
    async function refreshDisk() {
      try {
        const d = await api('/api/system/disk');
        disk.value = { total: d.total || 0, used: d.used || 0, free: d.free || 0 };
      } catch (e) { /* silent */ }
    }
    // One-time backfill of the 60-sample sparkline history (on connect).
    async function refreshNet() {
      try {
        const n = await api('/api/system/net');
        netSamples.value = n.samples || [];
        latestTx.value = n.latest_tx || 0;
        latestRx.value = n.latest_rx || 0;
      } catch (e) { /* silent */ }
    }
    // Live updates arrive pushed over SSE (`event: net`), not by polling —
    // append one sample to the ring and keep the last 60.
    function pushNetSample(o) {
      if (!o) return;
      latestTx.value = o.tx || 0;
      latestRx.value = o.rx || 0;
      const arr = netSamples.value.slice();
      arr.push({ at: o.at, tx: o.tx, rx: o.rx });
      if (arr.length > 60) arr.splice(0, arr.length - 60);
      netSamples.value = arr;
    }
    async function startJob(type, subdir = '') {
      try {
        const dir = subdir ? (uploadDir.value + '/' + subdir) : '';
        await api('/api/job/start', { method: 'POST', body: JSON.stringify({ type, dir }) });
        await refreshStatus();
      } catch (e) { setErr(e.message || String(e)); }
    }
    // Track stop requests so a second click within a few seconds escalates
    // to a force-kill (SIGTERM/SIGKILL the running ffmpeg + abort curl mid-
    // upload). One click = polite cancel between files; two clicks = nuke.
    const stopArm = ref({ fix: 0, bot: 0, all: 0 });   // last click ts per target
    const STOP_FORCE_WINDOW_MS = 4000;

    // Computed: is any stop button "armed" (clicked once recently)? Needs `tick`
    // dependency so the indicator decays when the window expires.
    const anyStopArmed = computed(() => {
      tick.value;  // re-evaluate every second
      const now = Date.now();
      return (now - Math.max(stopArm.value.fix, stopArm.value.bot, stopArm.value.all))
             < STOP_FORCE_WINDOW_MS;
    });

    async function stopJob(type) {
      const key = type === 'fix-big' ? 'fix' : 'bot';
      const now = Date.now();
      const force = (now - stopArm.value[key]) < STOP_FORCE_WINDOW_MS;
      stopArm.value[key] = now;
      try {
        await api('/api/job/stop?type=' + encodeURIComponent(type) + (force ? '&force=1' : ''),
                  { method: 'POST' });
      } catch (e) { setErr(e.message || String(e)); }
    }
    async function stopAll() {
      const now = Date.now();
      const force = (now - stopArm.value.all) < STOP_FORCE_WINDOW_MS;
      stopArm.value.all = now;
      try {
        await api('/api/job/stop?type=all' + (force ? '&force=1' : ''),
                  { method: 'POST' });
      } catch (e) { setErr(e.message || String(e)); }
    }

    // Log append
    function buildRec(raw, isFresh) {
      const c = parseLine(raw);
      const lno = logLines.value.length + 1;
      return {
        id: nextId++,
        lno,
        ts: c.ts,
        tag: c.tag,
        tagClass: c.tagClass,
        progress: c.progress,
        html: c.rest ? highlightContent(c.rest) : '',
        fresh: !!isFresh,
      };
    }
    function appendLine(raw, channel, isFresh = true) {
      if (!channel) channel = 'web';
      if (!logsByChannel.value[channel]) {
        // Unknown channel — coerce to web so we never silently drop lines.
        channel = 'web';
      }
      const c = parseLine(raw);
      // Feed the live tracker (current upload, ETA, failures)
      pushLogParse(c);
      const bucket = logsByChannel.value[channel];
      // Progress: replace last progress line of same kind in place
      if (c.progress && bucket.length > 0) {
        const last = bucket[bucket.length - 1];
        if (last.progress) {
          last.ts = c.ts;
          last.tag = c.tag;
          last.tagClass = c.tagClass;
          last.progress = c.progress;
          last.html = '';
          return;
        }
      }
      channelLnoCounters[channel] = (channelLnoCounters[channel] || 0) + 1;
      const rec = buildRec(raw, isFresh);
      rec.lno = channelLnoCounters[channel];   // override using per-channel counter
      bucket.push(rec);
      if (bucket.length > MAX_LINES) bucket.splice(0, bucket.length - MAX_LINES);
      recentLineCount++;
      if (isFresh) {
        heartbeat.value = false;
        nextTick(() => { heartbeat.value = true; });
        if (channel === 'fix-big' || channel === 'bot-upload') {
          // Auto-switch to the channel currently producing output, but only
          // when the user isn't already actively viewing a different active
          // channel (so manual selection sticks).
          if (activeChannel.value === 'web') activeChannel.value = channel;
        }
      }
      if (channel === activeChannel.value) scrollToBottomIfPinned();
      if (isFresh) setTimeout(() => { rec.fresh = false; }, 360);
    }
    function clearLogs() {
      for (const k of CHANNELS) {
        logsByChannel.value[k] = [];
        channelLnoCounters[k] = 0;
      }
    }
    function switchChannel(ch) {
      activeChannel.value = ch;
      // Move scroll-bottom hint to the newly-active terminal viewport
      nextTick(() => scrollToBottomIfPinned());
    }
    function scrollToBottomIfPinned() {
      if (!autoScroll.value) return;
      nextTick(() => {
        const el = termEl.value;
        if (!el) return;
        el.scrollTop = el.scrollHeight;
      });
    }
    function toggleAutoScroll() {
      autoScroll.value = !autoScroll.value;
      if (autoScroll.value) scrollToBottomIfPinned();
    }
    function onTermScroll() {
      const el = termEl.value;
      if (!el) return;
      const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 8;
      if (!atBottom && autoScroll.value) autoScroll.value = false;
    }

    // SSE
    let es = null;
    function startSSE() {
      if (es) try { es.close(); } catch {}
      const url = '/api/job/stream?token=' + encodeURIComponent(getToken());
      es = new EventSource(url);
      es.addEventListener('open', () => { sseConnected.value = true; });
      es.addEventListener('error', () => { sseConnected.value = false; });
      // Live network throughput, pushed ~1/s (replaces /api/system/net polling).
      es.addEventListener('net', (ev) => { try { pushNetSample(JSON.parse(ev.data)); } catch {} });
      // Live job status, pushed ~2/s (replaces /api/job/status polling).
      es.addEventListener('status', (ev) => { try { applyStatus(JSON.parse(ev.data)); } catch {} });
      // Server emits `event: replay.<channel>` for history and `event: log.<channel>` for live.
      for (const ch of CHANNELS) {
        es.addEventListener('replay.' + ch, (ev) => appendLine(ev.data || '', ch, false));
        es.addEventListener('log.' + ch,    (ev) => appendLine(ev.data || '', ch, true));
      }
      es.addEventListener('ready', () => {
        scrollToBottomIfPinned();
        refreshStatus();
        refreshPending();
        refreshFolders();
        refreshDisk();
        refreshNet();
      });
      // Legacy: any line that arrives without a channel event lands in web.
      es.onmessage = (ev) => appendLine(ev.data || '', 'web', true);
    }
    function stopSSE() {
      if (es) { es.close(); es = null; }
      sseConnected.value = false;
    }

    // Keyboard
    function openKeymap() {
      const d = document.getElementById('keymap');
      if (d && !d.open) d.showModal();
    }
    function isTypingTarget(t) {
      const tag = (t.tagName || '').toLowerCase();
      return tag === 'input' || tag === 'textarea' || t.isContentEditable;
    }
    function onKeydown(ev) {
      if (isTypingTarget(ev.target)) return;
      if (ev.metaKey || ev.ctrlKey || ev.altKey) return;
      const k = ev.key;
      if (k === '?') { ev.preventDefault(); openKeymap(); }
      else if (k === 's') { ev.preventDefault(); if (!botRunning.value) startJob('bot-upload'); }
      else if (k === 'f') { ev.preventDefault(); if (!fixRunning.value) startJob('fix-big'); }
      else if (k === 'X') { ev.preventDefault(); if (anyRunning.value) stopAll(); }
      else if (k === 'r') { ev.preventDefault(); refreshPending(); refreshNet(); }
      else if (k === 'c') { ev.preventDefault(); clearLogs(); }
      else if (k === 'p') { ev.preventDefault(); toggleAutoScroll(); }
    }

    // Lifecycle
    let statusTimer = null, rateTimer = null, netTimer = null;
    let foldersTimer = null;
    onMounted(async () => {
      if (!token.value) return;
      startSSE();
      await refreshStatus();
      await refreshPending();
      await refreshFolders();
      await refreshDisk();
      await refreshNet();
      document.addEventListener('keydown', onKeydown);
      // status + net now arrive over SSE (`event: status` / `event: net`); no polling timers.
      foldersTimer = setInterval(() => { refreshFolders(); refreshDisk(); }, 20000);
      rateTimer = setInterval(() => {
        lineRate.value = lineRate.value * 0.55 + (recentLineCount / 2) * 0.45;
        recentLineCount = 0;
      }, 2000);
    });
    onBeforeUnmount(() => {
      stopSSE();
      document.removeEventListener('keydown', onKeydown);
      [statusTimer, rateTimer, netTimer, foldersTimer].forEach(t => t && clearInterval(t));
    });

    watch(anyRunning, () => {
      if (document.startViewTransition) {
        document.startViewTransition(() => {});
      }
    });

    return {
      token, sseConnected, heartbeat,
      fixState, botState, fixRunning, botRunning, anyRunning, activePathCount,
      fixMeta, botMeta, runningSummary,
      pendingFolders, pendingTotal, uploadDir,
      folders, folderQuery, visibleFolders, maxFolderSize, totalBig,
      folderTotalStr, disk, diskUsedPct, diskUsedStr, diskTotalStr,
      selected, toggleSelect, selectAllVisible, clearSelection,
      bulkUpload, bulkFixBig,
      currentUpload, currentBot, currentFix, sessionStats, failedFiles,
      botEtaStr, fixEtaStr, totalAvailable, bot_done, fix_done,
      clearFailed, rerunFolderForFailed,
      netSamples, latestTx, latestRx, txStr, rxStr,
      sparkW, sparkH, sparkPathTx, sparkPathRx, sparkPathTxArea, sparkPathRxArea, lastTxPoint,
      logLines, displayLines, lineRate,
      logsByChannel, activeChannel, switchChannel, channelLineCounts, CHANNELS,
      termEl, autoScroll,
      lastErr,
      loginForm, loginErr, doLogin, logout,
      startJob, stopJob, stopAll, anyStopArmed,
      refreshPending, refreshFolders, refreshDisk,
      openKeymap, clearLogs, toggleAutoScroll, onTermScroll,
      fmtBytes, fmtBytesPair, fmtBytesPerSec,
      settingsDlg, bots,
      openSettings, closeSettings, onSettingsClose,
      addBot, removeBot, saveBots, reloadBots,
      pwForm, changePassword,
      onDialogBackdropClick,
      env, reloadEnv, saveEnv,
      schedules, addSchedule, saveSchedule, removeSchedule, reloadSchedules,
      history, reloadHistory, formatDur,
    };
  },
};

/* Spark line helpers (top-level so they can use closure-less math) */
function buildSparkLine(samples, key, W, H) {
  if (!samples || samples.length < 2) return '';
  const max = Math.max(1, ...samples.map(s => s[key]));
  const stepX = W / Math.max(1, samples.length - 1);
  let d = '';
  samples.forEach((s, i) => {
    const x = (i * stepX).toFixed(1);
    const y = (H - (s[key] / max) * (H - 10) - 4).toFixed(1);
    d += (i === 0 ? 'M' : 'L') + ' ' + x + ' ' + y + ' ';
  });
  return d;
}
function buildSparkArea(samples, key, W, H) {
  if (!samples || samples.length < 2) return '';
  const line = buildSparkLine(samples, key, W, H);
  return line + ` L ${W} ${H} L 0 ${H} Z`;
}
function buildLastPoint(samples, key, W, H) {
  if (!samples || samples.length < 1) return null;
  const max = Math.max(1, ...samples.map(s => s[key]));
  const i = samples.length - 1;
  const stepX = W / Math.max(1, samples.length - 1);
  return {
    x: (i * stepX).toFixed(1),
    y: (H - (samples[i][key] / max) * (H - 10) - 4).toFixed(1),
  };
}

// -- TweenN: smooth-tweens any numeric value (or any value via :fmt) ----
// Usage:
//   <tween-n :value="pendingTotal" />
//   <tween-n :value="bytes" :fmt="fmtBytes" />
//   <tween-n :value="pct" :fmt="v => Math.round(v) + '%'" :duration="500" />
// Default formatting rounds to an integer; pass :fmt for anything else.
const TweenN = {
  props: {
    value: { type: [Number, String], default: 0 },
    fmt:   { type: Function,         default: null },
    duration: { type: Number,        default: 400 },
    // If true: don't tween, just snap (handy for first paint).
    snap:  { type: Boolean,          default: false },
  },
  setup(props) {
    const display = ref(+props.value || 0);
    let raf = null;
    const tween = (toRaw) => {
      const to = +toRaw;
      if (!isFinite(to)) { display.value = toRaw; return; }
      const from = +display.value || 0;
      if (from === to) { display.value = to; return; }
      if (props.snap || props.duration <= 0) { display.value = to; return; }
      if (raf) cancelAnimationFrame(raf);
      const t0 = performance.now();
      const ease = (t) => 1 - Math.pow(1 - t, 3);
      const step = (now) => {
        const t = Math.min(1, (now - t0) / props.duration);
        display.value = from + (to - from) * ease(t);
        if (t < 1) raf = requestAnimationFrame(step);
        else raf = null;
      };
      raf = requestAnimationFrame(step);
    };
    watch(() => props.value, (v) => tween(v));
    // First-paint: don't animate from 0 to a huge number, snap immediately.
    return { display };
  },
  template: `<span>{{ fmt ? fmt(display) : Math.round(display) }}</span>`,
};

// Car-dashboard style gauge (tachometer) for network speed — log scale,
// sweeping needle (spring-eased), glowing value arc, decade ticks.
const Gauge = {
  props: { value: { type: Number, default: 0 }, label: { type: String, default: '' }, accent: { type: String, default: 'var(--brand)' } },
  setup(props) {
    const MIN = 1e4, MAX = 1e8, S = 135, SW = 270;          // 10KB/s..100MB/s, 270° sweep
    const Lo = Math.log10(MIN), Rng = Math.log10(MAX) - Lo;
    const ALEN = 2 * Math.PI * 78 * SW / 360;               // value-arc dash length
    const polar = (r, deg) => { const a = deg * Math.PI / 180; return [100 + r * Math.cos(a), 100 + r * Math.sin(a)]; };
    const arc = (r, a0, a1) => { const [x0, y0] = polar(r, a0), [x1, y1] = polar(r, a1); const la = (a1 - a0) > 180 ? 1 : 0; return `M${x0.toFixed(1)} ${y0.toFixed(1)} A${r} ${r} 0 ${la} 1 ${x1.toFixed(1)} ${y1.toFixed(1)}`; };
    const frac = computed(() => Math.max(0, Math.min(1, (Math.log10(Math.max(props.value, 1)) - Lo) / Rng)));
    const ndeg = computed(() => S + frac.value * SW);
    const track = arc(78, S, S + SW);
    const ticks = []; for (let i = 0; i <= 8; i++) { const d = S + i / 8 * SW, mj = i % 2 === 0; const [x0, y0] = polar(mj ? 67 : 72, d), [x1, y1] = polar(80, d); ticks.push({ x0, y0, x1, y1, mj }); }
    const LB = ['10K', '100K', '1M', '10M', '100M'], labs = []; for (let i = 0; i <= 4; i++) { const d = S + i / 4 * SW; const [x, y] = polar(55, d); labs.push({ x, y, t: LB[i] }); }
    const disp = computed(() => fmtBytesPerSec(props.value));
    const pct = computed(() => Math.max(2, Math.round(frac.value * 100)) + '%');
    return { track, frac, pct, ndeg, ticks, labs, disp, ALEN };
  },
  template: `
  <div class="gauge">
    <svg viewBox="0 0 200 172" class="gsvg">
      <path :d="track" class="g-track"/>
      <path :d="track" class="g-arc" :style="{ stroke: accent, strokeDasharray: ALEN, strokeDashoffset: ALEN * (1 - frac) }"/>
      <line v-for="(t,i) in ticks" :key="'t'+i" :x1="t.x0" :y1="t.y0" :x2="t.x1" :y2="t.y1" class="g-tick" :class="{ major: t.mj }"/>
      <text v-for="(l,i) in labs" :key="'l'+i" :x="l.x" :y="l.y" class="g-lab" text-anchor="middle" dominant-baseline="middle">{{ l.t }}</text>
      <g class="g-needle" :style="{ transform: 'rotate(' + (ndeg - 270) + 'deg)' }">
        <path d="M100 100 L100 31" class="g-nline" :style="{ stroke: accent }"/>
        <path d="M100 100 L100 115" class="g-ntail"/>
      </g>
      <circle cx="100" cy="100" r="7" class="g-hub"/>
      <circle cx="100" cy="100" r="2.6" :style="{ fill: accent }"/>
    </svg>
    <div class="g-read">
      <span class="g-num" :style="{ color: accent }">{{ disp[0] }}</span><span class="g-unit">{{ disp[1] }}</span>
      <div class="g-label">{{ label }}</div>
    </div>
    <div class="speedbar" :style="{ color: accent }"><span class="speedfill" :style="{ '--p': pct }"></span></div>
  </div>`
};

const app = createApp(App);
app.component('tween-n', TweenN);
app.component('gauge', Gauge);
app.mount('#app');

// ---- lightweight startup only; no pointer-follow animation on upload page ----
(() => {
  // stagger entrance: assign incremental delay to the cards present at load
  document.querySelectorAll('.card').forEach((c, i) => {
    if (!c.style.getPropertyValue('--d')) c.style.setProperty('--d', Math.min(i * 0.05, 0.4) + 's');
  });
  requestAnimationFrame(() => requestAnimationFrame(() => document.body.classList.add('ready')));
})();

// Shared motion engine: desktop (mouse), mobile (gyroscope), and always-on idle
// float (so depth is visibly moving even with no input). Respects reduced-motion.
function installMotion(root, tiltSel, withScroll){
  return;
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) return;
  let tpx = 0, tpy = 0, cpx = 0, cpy = 0, cur = null, cx = 0, cy = 0, last = 0;
  const now = () => performance.now();
  const loop = () => {
    const t = now();
    if (t - last > 1100) { tpx = Math.sin(t / 3400) * 0.42; tpy = Math.cos(t / 4600) * 0.36; }
    cpx += (tpx - cpx) * 0.06; cpy += (tpy - cpy) * 0.06;
    root.style.setProperty('--px', cpx.toFixed(3));
    root.style.setProperty('--py', cpy.toFixed(3));
    if (cur) {
      const r = cur.getBoundingClientRect();
      const lx = (cx - r.left) / r.width, ly = (cy - r.top) / r.height;
      cur.style.setProperty('--mx', (lx * 100) + '%');
      cur.style.setProperty('--my', (ly * 100) + '%');
      cur.style.transform = `perspective(1200px) rotateX(${((0.5 - ly) * 3.6).toFixed(2)}deg) rotateY(${((lx - 0.5) * 3.6).toFixed(2)}deg)`;
    }
    requestAnimationFrame(loop);
  };
  requestAnimationFrame(loop);
  addEventListener('pointermove', (e) => {
    last = now();
    tpx = e.clientX / innerWidth - 0.5; tpy = e.clientY / innerHeight - 0.5;
    const el = e.target.closest && e.target.closest(tiltSel);
    if (el !== cur) { if (cur) cur.style.transform = ''; cur = el; }
    cx = e.clientX; cy = e.clientY;
  }, { passive: true });
  addEventListener('pointerleave', () => { if (cur) { cur.style.transform = ''; cur = null; } });
  const onTilt = (e) => {
    if (e.gamma == null && e.beta == null) return;
    last = now();
    tpx = Math.max(-0.5, Math.min(0.5, (e.gamma || 0) / 40));
    tpy = Math.max(-0.5, Math.min(0.5, ((e.beta || 0) - 40) / 40));
  };
  const enableGyro = () => {
    try {
      if (typeof DeviceOrientationEvent !== 'undefined' && DeviceOrientationEvent.requestPermission) {
        DeviceOrientationEvent.requestPermission().then(s => { if (s === 'granted') addEventListener('deviceorientation', onTilt); }).catch(() => {});
      } else if ('ondeviceorientation' in window) { addEventListener('deviceorientation', onTilt); }
    } catch (_) {}
  };
  enableGyro();
  addEventListener('touchend', enableGyro, { once: true, passive: true });
}

// Register the PWA service worker so iOS treats this as a real "Add to
// Home Screen" app with the right icon + standalone display mode.
if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('/upload-sw.js', { scope: '/upload/' })
      .catch(err => console.warn('[sw]', err));
  });
}
