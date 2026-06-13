
import { createApp, ref, reactive, onMounted, computed, watch, nextTick } from 'vue';
import Chart from 'chart.js/auto';
import Hls from 'hls.js';
import '../styles/pages/index.css';

createApp({
  setup() {
    const token = ref(localStorage.getItem('token') || '');
    const privacyMode = ref(localStorage.getItem('privacyMode') !== '0');
    function togglePrivacy() { privacyMode.value = !privacyMode.value; localStorage.setItem('privacyMode', privacyMode.value ? '1' : '0'); }
    const gridCols = ref(parseInt(localStorage.getItem('gridCols')) || 0);
    const gridRows = ref(parseInt(localStorage.getItem('gridRows')) || 0);
    function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
    function onGridColsChange() { gridCols.value = clamp(gridCols.value || 0, 0, 20); localStorage.setItem('gridCols', gridCols.value); doSearch(page.value); }
    function onGridRowsChange() { gridRows.value = clamp(gridRows.value || 0, 0, 20); localStorage.setItem('gridRows', gridRows.value); doSearch(page.value); }
    const gridStyle = computed(() => {
      if (!gridCols.value) return {};
      return { 'grid-template-columns': `repeat(${gridCols.value}, 1fr)` };
    });
    const loginForm = reactive({ username: '', password: '' });
    const loginLoading = ref(false);
    const loginError = ref('');
    const currentView = ref('search');

    // ─── Archive Login Page ──────────────────────────────────────────────────
    const lpCardEl     = ref(null);
    const lpBgEl       = ref(null);
    const splashCols   = ref([[], [], [], [], [], []]);
    const splashStats  = ref({ total_videos: 0, total_models: 0 });
    const loginSuccess = ref(false);
    const showPw       = ref(false);
    const lpZoomSrc    = ref('');
    const lpColEls     = reactive({});   // ci → column DOM el (set via :ref callback)
    const _lpFocused   = {};             // ci → focused img el (plain, DOM-only)
    const _lpPaused    = {};             // ci → boolean (plain, no Vue reactivity needed)

    // RAF animation: speeds in px/frame @60fps, directions (1=scroll-up, -1=scroll-down)
    const _LP_SPEEDS = [0.32, 0.24, 0.38, 0.28, 0.35, 0.22];
    const _LP_DIRS   = [1, -1, 1, -1, 1, -1];
    const _lpYs      = [0, 0, 0, 0, 0, 0];
    let   _lpRafId   = null;
    let   _lpRafTs   = null;
    let   _lpExiting = false;  // true during login exit — ramps up column speed

    function _startLpAnimation() {
      if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) return;
      // Seed even columns at halfway so they appear to scroll the opposite direction immediately
      for (let ci = 1; ci < 6; ci += 2) {
        const el = lpColEls[ci];
        if (el) { const hh = el.scrollHeight / 2; if (hh > 0) _lpYs[ci] = hh * 0.5; }
      }
      function frame(ts) {
        if (!_lpRafTs) _lpRafTs = ts;
        const dt = Math.min(ts - _lpRafTs, 50);  // cap prevents jump after tab switch
        _lpRafTs = ts;
        const mul = _lpExiting ? 9 : 1;
        for (let ci = 0; ci < 6; ci++) {
          const el = lpColEls[ci];
          if (!el || _lpPaused[ci]) continue;
          const hh = el.scrollHeight / 2;
          if (hh <= 0) continue;
          _lpYs[ci] += _LP_SPEEDS[ci] * mul * (dt / 16.667);
          if (_lpYs[ci] >= hh) _lpYs[ci] -= hh;
          // odd cols (0,2,4): scroll upward → negative translateY
          // even cols (1,3,5): scroll downward → starts at -hh, goes toward 0
          const ty = _LP_DIRS[ci] === 1 ? -_lpYs[ci] : (_lpYs[ci] - hh);
          el.style.transform = `translateY(${ty.toFixed(1)}px)`;
        }
        _lpRafId = requestAnimationFrame(frame);
      }
      _lpRafId = requestAnimationFrame(frame);
    }

    function lpColClick(ci, imgEl) {
      if (_lpPaused[ci]) {
        // Resume scrolling — remove focus from this column's highlighted image
        _lpPaused[ci] = false;
        if (_lpFocused[ci]) { _lpFocused[ci].classList.remove('lp-focused'); _lpFocused[ci] = null; }
      } else {
        // Pause + focus clicked image
        _lpPaused[ci] = true;
        if (_lpFocused[ci]) _lpFocused[ci].classList.remove('lp-focused');
        imgEl.classList.add('lp-focused');
        _lpFocused[ci] = imgEl;
      }
    }

    function lpZoomOpen(src) {
      lpZoomSrc.value = src;
      document.addEventListener('keydown', _lpEscKey);
    }
    function lpZoomClose() {
      lpZoomSrc.value = '';
      document.removeEventListener('keydown', _lpEscKey);
    }
    function _lpEscKey(e) { if (e.key === 'Escape') lpZoomClose(); }

    async function initSplash() {
      const COLS = 6, P1 = 4, P2 = 8;  // Phase 1: 4/col; Phase 2: +8/col
      let ids = [];
      try {
        ids = await fetch('/api/splash').then(r => r.json());
        // Fisher-Yates shuffle
        for (let i = ids.length - 1; i > 0; i--) {
          const j = Math.floor(Math.random() * (i + 1));
          [ids[i], ids[j]] = [ids[j], ids[i]];
        }
      } catch(e) {}

      function makeCols(n) {
        const cols = Array.from({ length: COLS }, () => []);
        const want = COLS * n;
        for (let i = 0; i < want; i++) cols[i % COLS].push(ids[i % ids.length]);
        return cols;
      }

      if (!ids.length) return;

      // Phase 1 — first 4 per column; columns start scrolling immediately
      splashCols.value = makeCols(P1);
      await nextTick();
      _startLpAnimation();

      // Phase 2 — fill remaining images with a short stagger delay
      setTimeout(() => { splashCols.value = makeCols(P1 + P2); }, 350);

      // Stats (non-blocking)
      fetch('/api/splash-stats').then(r => r.json()).then(s => {
        splashStats.value = { total_videos: s.total || 0, total_models: s.models || 0 };
      }).catch(() => {});

      await nextTick();
      _initLoginInteractions();
    }

    function stopSplash() {
      if (_lpRafId) { cancelAnimationFrame(_lpRafId); _lpRafId = null; }
      _lpExiting = false;
      document.removeEventListener('keydown', _lpEscKey);
    }

    function _initLoginInteractions() {
      const card = lpCardEl.value;
      const bg   = lpBgEl.value;
      if (!card || !bg) return;
      function onMove(e) {
        if (token.value || loginSuccess.value) return;
        const cx = window.innerWidth  / 2 - e.clientX;
        const cy = window.innerHeight / 2 - e.clientY;
        card.style.transform = `rotateX(${cy / 35}deg) rotateY(${-cx / 35}deg)`;
        bg.style.transform = `translate(${cx / 18}px, ${cy / 18}px)`;
        const rect = card.getBoundingClientRect();
        card.style.setProperty('--mx', `${e.clientX - rect.left}px`);
        card.style.setProperty('--my', `${e.clientY - rect.top}px`);
      }
      function onLeave() {
        if (token.value || loginSuccess.value) return;
        card.style.transform = '';
        bg.style.transform   = '';
      }
      document.addEventListener('mousemove', onMove);
      document.addEventListener('mouseleave', onLeave);
    }

    const search = reactive({ q: '', platform: '', date_from: '', date_to: '', duration_min: '0', duration_max: '0', sort: 'date_desc', tag: '' });
    const topTags = ref([]);
    const tagSearchQ = ref('');
    const showTagDrop = ref(false);
    const filteredTags = computed(() => {
      const q = tagSearchQ.value.trim().toLowerCase().replace(/^#/, '');
      if (!q) return topTags.value.slice(0, 60);
      return topTags.value.filter(t => t.tag.includes(q)).slice(0, 60);
    });
    function onTagSearchInput() { showTagDrop.value = true; }
    function hideTagDropSoon() { setTimeout(() => { showTagDrop.value = false; }, 150); }
    function pickTag(t) { search.tag = t; tagSearchQ.value = ''; showTagDrop.value = false; doSearch(1); }
    const videos = ref([]);
    const loading = ref(false);
    const page = ref(1);
    const totalPages = ref(0);
    const total = ref(0);
    const stats = ref(null);
    const platforms = ref([]);
    const suggestions = ref([]);
    const suggestIdx = ref(-1);
    let suggestTimer = null;

    // Leaderboard
    const lbItems = ref([]);
    const lbLoading = ref(false);
    const lbSort = ref('count');
    const lbResolution = ref('');
    const lbPeriod = ref('');
    const lbPage = ref(1);
    const lbTotalPages = ref(0);
    const lbTotal = ref(0);
    const resolutions = ref([]);
    const lbMaxCount = computed(() => lbItems.value.length ? lbItems.value[0].video_count || 1 : 1);
    const lbMaxDuration = computed(() => lbItems.value.length ? lbItems.value[0].total_duration || 1 : 1);

    // Dashboard
    const heatmapData = ref(null);
    const heatmapDays = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
    let heatMax = 1;
    let chartInstances = {};
    let dashLoaded = false;

    // Stream player
    const streamModel = ref(null);      // model currently playing
    const streamData = ref(null);       // {hls_source, room_title, num_viewers, ...}
    const streamLoading = ref(false);
    let hlsInstance = null;
    const streamVideoEl = ref(null);    // template ref

    async function openStream(modelName) {
      streamModel.value = modelName;
      streamData.value = null;
      streamLoading.value = true;
      destroyHls();
      try {
        streamData.value = await apiFetch('/api/live/room/' + encodeURIComponent(modelName));
      } catch(e) {
        streamModel.value = null;
        console.error('stream error', e);
        return;
      } finally {
        streamLoading.value = false;
      }
      nextTick(() => startHls());
    }

    function startHls() {
      const video = streamVideoEl.value;
      const src = streamData.value?.hls_source;
      if (!video || !src) return;
      if (Hls.isSupported()) {
        hlsInstance = new Hls({ enableWorker: true, lowLatencyMode: true });
        hlsInstance.loadSource(src);
        hlsInstance.attachMedia(video);
        hlsInstance.on(Hls.Events.MANIFEST_PARSED, () => video.play().catch(() => {}));
      } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
        video.src = src;
        video.play().catch(() => {});
      }
    }

    function destroyHls() {
      if (hlsInstance) { hlsInstance.destroy(); hlsInstance = null; }
      if (streamVideoEl.value) streamVideoEl.value.src = '';
    }

    function closeStream() {
      destroyHls();
      streamModel.value = null;
      streamData.value = null;
    }

    // Model Detail
    const detailModel = ref(null);
    const detailData = ref(null);
    const detailLoading = ref(false);
    const detailLive = ref(false);
    const liveTs = ref(Date.now());
    let liveTimer = null;

    // Live strip
    const liveModels = ref([]);
    const liveLoading = ref(false);
    const liveTsStrip = ref(Date.now());
    let liveStripTimer = null;
    let liveTsStripTimer = null;
    const detailBio = computed(() => parseBio(detailData.value?.profile?.about_me || ''));
    const showBioPreview = ref(false);
    const bioIframeEl = ref(null);   // template ref to the iframe element

    function parsePhotoSets(jsonStr) {
      if (!jsonStr || jsonStr === '[]') return [];
      try {
        return JSON.parse(jsonStr).map(ps => ({
          id:        ps.id,
          name:      ps.name || 'Untitled',
          cover_url: ps.cover_url || '',
          is_video:  ps.is_video || false,
          count:     ps.photo_count || 0,
          duration:  ps.video_duration_in_seconds
                       ? (ps.video_duration_in_seconds >= 60
                           ? Math.floor(ps.video_duration_in_seconds/60) + 'm' + (ps.video_duration_in_seconds%60||'')+'s'
                           : ps.video_duration_in_seconds + 's')
                       : '0s',
          locked:    !ps.user_can_access,
          fan_club:  ps.fan_club_only || false,
          tokens:    ps.tokens || 0,
        }));
      } catch { return []; }
    }

    function buildBioIframe(el) {
      if (!el) return;
      const bio = detailData.value?.profile?.about_me || '';
      if (!bio) return;
      const patched = bio.replace(/<a /gi, '<a target="_blank" ');
      const html = `<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*{box-sizing:border-box}
html,body{margin:0;padding:0;overflow-x:hidden}
body{padding:10px;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;font-size:14px;line-height:1.5}
img{max-width:100%;height:auto;display:inline-block}
a{color:#818cf8;word-break:break-all}
</style>
</head><body>${patched}
<script>
(function(){
  function report(){
    var h = document.documentElement.scrollHeight || document.body.scrollHeight;
    parent.postMessage({type:'bio-height',h:h},'*');
  }
  // Report on load + after images finish loading
  report();
  window.addEventListener('load', report);
  if(window.ResizeObserver){
    new ResizeObserver(report).observe(document.body);
  } else {
    setInterval(report, 400);
  }
})();
<\/script>
</body></html>`;
      const blob = new Blob([html], { type: 'text/html' });
      const url  = URL.createObjectURL(blob);
      el.src = url;
      el.onload = () => URL.revokeObjectURL(url);
    }

    // Listen for height reports from bio iframe
    window.addEventListener('message', e => {
      if (e.data?.type === 'bio-height' && bioIframeEl.value) {
        const h = e.data.h;
        if (h > 50) bioIframeEl.value.style.height = (h + 20) + 'px';
      }
    });

    watch(showBioPreview, (val) => {
      if (val) nextTick(() => buildBioIframe(bioIframeEl.value));
    });
    const tlMax = computed(() => {
      if (!detailData.value || !detailData.value.timeline.length) return 1;
      return Math.max(...detailData.value.timeline.map(t => t.count)) || 1;
    });

    // Lightbox
    const lightboxIdx = ref(null);
    const lbVideo = computed(() => lightboxIdx.value !== null ? videos.value[lightboxIdx.value] : {});
    const lbZoom = ref(1);
    const lbDX = ref(0);
    const lbDY = ref(0);
    let lbDragging = false, lbSX = 0, lbSY = 0, lbOX = 0, lbOY = 0;

    function openLightbox(idx) { lightboxIdx.value = idx; lbZoom.value = 1; lbDX.value = 0; lbDY.value = 0; }
    function closeLightbox() { if (lbZoom.value > 1.05) { lbResetZoom(); return; } lightboxIdx.value = null; }
    function lbPrevFn() { if (lightboxIdx.value > 0) { lightboxIdx.value--; } }
    function lbNextFn() { if (lightboxIdx.value < videos.value.length - 1) { lightboxIdx.value++; } }
    function lbResetZoom() { lbZoom.value = 1; lbDX.value = 0; lbDY.value = 0; }
    function onLbWheel(e) {
      if (lightboxIdx.value === null) return;
      lbZoom.value = Math.min(8, Math.max(0.5, lbZoom.value + (e.deltaY > 0 ? -0.15 : 0.15)));
      if (lbZoom.value <= 1.05) { lbDX.value = 0; lbDY.value = 0; }
    }
    function onLbDragStart(e) {
      if (lbZoom.value <= 1.05) return;
      lbDragging = true; lbSX = e.clientX; lbSY = e.clientY; lbOX = lbDX.value; lbOY = lbDY.value;
      const mv = ev => { if (!lbDragging) return; lbDX.value = lbOX + (ev.clientX - lbSX) / lbZoom.value; lbDY.value = lbOY + (ev.clientY - lbSY) / lbZoom.value; };
      const up = () => { lbDragging = false; window.removeEventListener('mousemove', mv); window.removeEventListener('mouseup', up); };
      window.addEventListener('mousemove', mv); window.addEventListener('mouseup', up);
    }
    window.addEventListener('keydown', e => {
      if (e.key === 'Escape') { if (lightboxIdx.value !== null) { lightboxIdx.value = null; return; } if (detailModel.value) { closeDetail(); } }
      if (lightboxIdx.value !== null) { if (e.key === 'ArrowLeft') lbPrevFn(); if (e.key === 'ArrowRight') lbNextFn(); }
    });

    // Autocomplete
    function onModelInput() {
      clearTimeout(suggestTimer); suggestIdx.value = -1;
      const q = search.q.trim();
      if (q.length < 1) { suggestions.value = []; return; }
      suggestTimer = setTimeout(async () => { try { suggestions.value = await apiFetch('/api/models?q=' + encodeURIComponent(q)); } catch(e) { suggestions.value = []; } }, 200);
    }
    function suggestDown() { if (!suggestions.value.length) return; suggestIdx.value = (suggestIdx.value + 1) % suggestions.value.length; search.q = suggestions.value[suggestIdx.value].model_name; }
    function suggestUp() { if (!suggestions.value.length) return; suggestIdx.value = (suggestIdx.value - 1 + suggestions.value.length) % suggestions.value.length; search.q = suggestions.value[suggestIdx.value].model_name; }
    function pickSuggestion(s) { search.q = s.model_name; suggestions.value = []; doSearch(1); }
    function selectAndSearch() { suggestions.value = []; doSearch(1); }
    function hideSuggestSoon() { setTimeout(() => { suggestions.value = []; showHistory.value = false; }, 150); }

    // API
    const headers = () => ({ 'Authorization': 'Bearer ' + token.value, 'Content-Type': 'application/json' });
    async function apiFetch(url, opts = {}) {
      const res = await fetch(url, { ...opts, headers: { ...headers(), ...opts.headers } });
      if (res.status === 401) { token.value = ''; localStorage.removeItem('token'); throw new Error('Unauthorized'); }
      if (!res.ok) { const d = await res.json().catch(() => ({})); throw new Error(d.detail || 'Request failed'); }
      return res.json();
    }

    async function doLogin() {
      loginLoading.value = true; loginError.value = '';
      try {
        const data = await fetch('/api/login', {
          method: 'POST', headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(loginForm)
        }).then(r => r.json().then(d => { if (!r.ok) throw new Error(d.detail); return d; }));
        localStorage.setItem('token', data.token);
        // Reset card tilt, ramp up column speed, trigger exit animations
        if (lpCardEl.value) lpCardEl.value.style.transform = '';
        _lpExiting = true;
        loginSuccess.value = true;  // → lp-collapse on card-wrap + delayed page fade-out
        setTimeout(() => {
          token.value = data.token;
          loginSuccess.value = false;
          stopSplash();
          loadInitialData();
        }, 1150);  // card collapse ~520ms + fade-out delay 720ms + fade 400ms
      } catch(e) { loginError.value = e.message; }
      finally { loginLoading.value = false; }
    }

    // Search
    function calcPerPage() {
      const cols = gridCols.value || Math.max(1, Math.floor((window.innerWidth - 48) / 276));
      const rows = gridRows.value || Math.max(2, Math.floor((window.innerHeight - 160) / 310));
      return cols * rows;
    }
    async function doSearch(p) {
      page.value = p || 1; loading.value = true; showHistory.value = false;
      if (search.q.trim()) saveSearchHistory(search.q.trim());
      try {
        const params = new URLSearchParams({ page: page.value, per_page: calcPerPage(), sort: search.sort });
        if (search.q) params.set('q', search.q);
        if (search.platform) params.set('platform', search.platform);
        if (search.date_from) params.set('date_from', search.date_from);
        if (search.date_to) params.set('date_to', search.date_to);
        if (search.duration_min !== '0') params.set('duration_min', search.duration_min);
        if (search.duration_max !== '0') params.set('duration_max', search.duration_max);
        if (search.tag) params.set('tag', search.tag);
        const data = await apiFetch('/api/videos?' + params);
        videos.value = data.items; totalPages.value = data.pages; total.value = data.total;
      } catch (e) { console.error(e); }
      finally { loading.value = false; }
    }
    function jumpToPage(e) { let p = parseInt(e.target.value); if (isNaN(p)||p<1) p=1; if (p>totalPages.value) p=totalPages.value; if (p!==page.value) doSearch(p); }
    function searchModel(name) { search.q = name; currentView.value = 'search'; doSearch(1); }
    function goHome() { currentView.value = 'search'; search.q=''; search.platform=''; search.date_from=''; search.date_to=''; search.duration_min='0'; search.duration_max='0'; search.sort='date_desc'; suggestions.value=[]; doSearch(1); }

    // Model Detail
    async function loadLiveModels(force = false) {
      if (liveLoading.value && !force) return;
      liveLoading.value = true;
      try {
        const r = await apiFetch('/api/live');
        liveModels.value = r.models || [];
        // If server is still doing its first refresh, poll again in 10s
        if (r.refreshing && liveModels.value.length === 0) {
          setTimeout(loadLiveModels, 10000);
        }
      } catch(e) { console.error(e); }
      finally { liveLoading.value = false; }
    }

    async function openModelDetail(name) {
      detailModel.value = name; detailLoading.value = true; detailData.value = null;
      detailLive.value = false; showBioPreview.value = false;
      if (liveTimer) { clearInterval(liveTimer); liveTimer = null; }
      try { detailData.value = await apiFetch('/api/model/' + encodeURIComponent(name) + '/detail'); } catch (e) { console.error(e); }
      finally { detailLoading.value = false; }
      // Start live thumbnail refresh every 5s
      liveTs.value = Date.now();
      liveTimer = setInterval(() => { liveTs.value = Date.now(); }, 5000);
    }
    function closeDetail() {
      detailModel.value = null;
      if (liveTimer) { clearInterval(liveTimer); liveTimer = null; }
    }
    function searchFromDetail() { const n = detailData.value?.model_name || detailModel.value; closeDetail(); search.q = n; currentView.value = 'search'; doSearch(1); }

    // Leaderboard
    async function loadLeaderboard(p) {
      lbPage.value = p || 1; lbLoading.value = true;
      try {
        const params = new URLSearchParams({ sort: lbSort.value, page: lbPage.value, per_page: 50 });
        if (lbResolution.value) params.set('resolution', lbResolution.value);
        if (lbPeriod.value) params.set('period', lbPeriod.value);
        const data = await apiFetch('/api/leaderboard?' + params);
        lbItems.value = data.items; lbTotalPages.value = data.pages; lbTotal.value = data.total;
      } catch (e) { console.error(e); }
      finally { lbLoading.value = false; }
    }
    async function loadResolutions() { try { resolutions.value = await apiFetch('/api/resolutions'); } catch (e) {} }
    function setLbSort(s) { lbSort.value = s; loadLeaderboard(1); }
    function setLbResolution(r) { lbResolution.value = r; loadLeaderboard(1); }
    function setLbPeriod(p) { lbPeriod.value = p; loadLeaderboard(1); }
    function jumpToLbPage(e) { let p = parseInt(e.target.value); if (isNaN(p)||p<1) p=1; if (p>lbTotalPages.value) p=lbTotalPages.value; if (p!==lbPage.value) loadLeaderboard(p); }

    // Search history
    const searchHistory = ref(JSON.parse(localStorage.getItem('searchHistory') || '[]'));
    const showHistory = ref(false);
    function saveSearchHistory(q) {
      if (!q || !q.trim()) return;
      const h = searchHistory.value.filter(x => x !== q);
      h.unshift(q);
      if (h.length > 8) h.length = 8;
      searchHistory.value = h;
      localStorage.setItem('searchHistory', JSON.stringify(h));
    }
    function pickHistory(h) { search.q = h; showHistory.value = false; doSearch(1); }
    function removeHistory(i) { searchHistory.value.splice(i, 1); localStorage.setItem('searchHistory', JSON.stringify(searchHistory.value)); }
    function onSearchFocus() {
      if (search.q.trim().length >= 1) { onModelInput(); }
      else { showHistory.value = true; }
    }

    function resetSearch() { search.q=''; search.platform=''; search.date_from=''; search.date_to=''; search.duration_min='0'; search.duration_max='0'; search.sort='date_desc'; search.tag=''; tagSearchQ.value=''; suggestions.value=[]; showHistory.value=false; doSearch(1); }
    function filterByTag(t) { search.tag = search.tag === t ? '' : t; doSearch(1); }

    function timeAgo(ts) {
      if (!ts) return '';
      const diff = Math.floor(Date.now()/1000) - ts;
      if (diff < 3600) return Math.floor(diff/60) + 'm ago';
      if (diff < 86400) return Math.floor(diff/3600) + 'h ago';
      if (diff < 604800) return Math.floor(diff/86400) + 'd ago';
      if (diff < 2592000) return Math.floor(diff/604800) + 'w ago';
      return Math.floor(diff/2592000) + 'mo ago';
    }

    // Leaderboard CSV export
    function exportLeaderboard() {
      const params = new URLSearchParams({ sort: lbSort.value });
      if (lbResolution.value) params.set('resolution', lbResolution.value);
      if (lbPeriod.value) params.set('period', lbPeriod.value);
      const url = '/api/leaderboard/export?' + params;
      const a = document.createElement('a');
      a.href = url;
      a.setAttribute('download', 'leaderboard.csv');
      // Need auth header, so use fetch
      fetch(url, { headers: { 'Authorization': 'Bearer ' + token.value } })
        .then(r => r.blob()).then(b => { const u = URL.createObjectURL(b); a.href = u; a.click(); URL.revokeObjectURL(u); });
    }

    // Dashboard
    const dashDays = ref(30);
    const PLAT_COLORS = { Chaturbate:'#f97316', StripChat:'#ec4899', OnlyFans:'#3b82f6', ManyVids:'#a855f7', Cam4:'#14b8a6', Streamate:'#f43f5e', LiveJasmin:'#eab308' };
    const RES_COLORS = { '4K':'#a855f7', '1080p':'#6366f1', '720p':'#3b82f6', '480p':'#14b8a6', 'Other':'#8b8d97', 'Unknown':'#4b5563' };

    function destroyChart(id) { if (chartInstances[id]) { chartInstances[id].destroy(); delete chartInstances[id]; } }

    function setDashDays(d) { dashDays.value = d; dashLoaded = false; nextTick(() => loadDashboard()); }

    async function loadDashboard() {
      if (dashLoaded) return;
      dashLoaded = true;
      await nextTick();
      try {
        const [trend, platStats, resStats, hm] = await Promise.all([
          apiFetch('/api/dashboard/daily-trend?days=' + dashDays.value),
          apiFetch('/api/dashboard/platform-stats'),
          apiFetch('/api/dashboard/resolution-stats'),
          apiFetch('/api/dashboard/hourly-heatmap'),
        ]);

        heatmapData.value = hm;
        heatMax = 1;
        for (const row of hm.matrix) for (const v of row) if (v > heatMax) heatMax = v;

        await nextTick();
        const opts = { responsive: true, maintainAspectRatio: false, plugins: { legend: { labels: { color: '#8b8d97' } } }, scales: {} };
        const axisColor = '#2a2d3a';
        const tickColor = '#8b8d97';

        // Trend chart
        destroyChart('trendChart');
        const trendCtx = document.getElementById('trendChart');
        if (trendCtx) {
          chartInstances['trendChart'] = new Chart(trendCtx, {
            type: 'line',
            data: {
              labels: trend.map(d => d.date.slice(5)),
              datasets: [{ label: 'Videos', data: trend.map(d => d.count), borderColor: '#6366f1', backgroundColor: 'rgba(99,102,241,.15)', fill: true, tension: .3, pointRadius: 2 }]
            },
            options: { ...opts, plugins: { ...opts.plugins, legend: { display: false } },
              scales: { x: { ticks: { color: tickColor }, grid: { color: axisColor } }, y: { ticks: { color: tickColor }, grid: { color: axisColor }, beginAtZero: true } } }
          });
        }

        // Platform pie
        destroyChart('platformChart');
        const platCtx = document.getElementById('platformChart');
        if (platCtx) {
          chartInstances['platformChart'] = new Chart(platCtx, {
            type: 'doughnut',
            data: {
              labels: platStats.map(p => p.platform),
              datasets: [{ data: platStats.map(p => p.video_count), backgroundColor: platStats.map(p => PLAT_COLORS[p.platform] || '#6b7280') }]
            },
            options: { ...opts, cutout: '55%' }
          });
        }

        // Resolution pie
        destroyChart('resChart');
        const resCtx = document.getElementById('resChart');
        if (resCtx) {
          chartInstances['resChart'] = new Chart(resCtx, {
            type: 'doughnut',
            data: {
              labels: resStats.map(r => r.resolution),
              datasets: [{ data: resStats.map(r => r.video_count), backgroundColor: resStats.map(r => RES_COLORS[r.resolution] || '#6b7280') }]
            },
            options: { ...opts, cutout: '55%' }
          });
        }

        // Storage by platform bar
        destroyChart('storagePlatChart');
        const spCtx = document.getElementById('storagePlatChart');
        if (spCtx) {
          chartInstances['storagePlatChart'] = new Chart(spCtx, {
            type: 'bar',
            data: {
              labels: platStats.map(p => p.platform),
              datasets: [{ label: 'Storage (GB)', data: platStats.map(p => +(p.total_size / 1073741824).toFixed(1)),
                           backgroundColor: platStats.map(p => PLAT_COLORS[p.platform] || '#6b7280') }]
            },
            options: { ...opts, indexAxis: 'y', plugins: { ...opts.plugins, legend: { display: false } },
              scales: { x: { ticks: { color: tickColor }, grid: { color: axisColor } }, y: { ticks: { color: tickColor }, grid: { color: axisColor } } } }
          });
        }

        // Storage by resolution bar
        destroyChart('storageResChart');
        const srCtx = document.getElementById('storageResChart');
        if (srCtx) {
          chartInstances['storageResChart'] = new Chart(srCtx, {
            type: 'bar',
            data: {
              labels: resStats.map(r => r.resolution),
              datasets: [{ label: 'Storage (GB)', data: resStats.map(r => +(r.total_size / 1073741824).toFixed(1)),
                           backgroundColor: resStats.map(r => RES_COLORS[r.resolution] || '#6b7280') }]
            },
            options: { ...opts, indexAxis: 'y', plugins: { ...opts.plugins, legend: { display: false } },
              scales: { x: { ticks: { color: tickColor }, grid: { color: axisColor } }, y: { ticks: { color: tickColor }, grid: { color: axisColor } } } }
          });
        }
      } catch (e) { console.error(e); }
    }

    function heatColor(val) {
      if (!val) return 'rgba(99,102,241,.03)';
      const ratio = val / heatMax;
      if (ratio < 0.15) return 'rgba(99,102,241,.1)';
      if (ratio < 0.3) return 'rgba(99,102,241,.25)';
      if (ratio < 0.5) return 'rgba(99,102,241,.4)';
      if (ratio < 0.75) return 'rgba(99,102,241,.6)';
      return 'rgba(99,102,241,.9)';
    }

    function switchView(view) {
      currentView.value = view;
      if (view === 'leaderboard' && lbItems.value.length === 0) { loadResolutions(); loadLeaderboard(1); }
      if (view === 'dashboard') { nextTick(() => loadDashboard()); }
    }

    async function loadInitialData() {
      try {
        const [s, p, tags] = await Promise.all([
          apiFetch('/api/stats'),
          apiFetch('/api/platforms'),
          apiFetch('/api/tags?limit=80'),
        ]);
        stats.value = s; platforms.value = p; topTags.value = tags;
      } catch (e) { console.error(e); }
      doSearch(1);
      // Start live strip
      loadLiveModels();
      liveStripTimer = setInterval(loadLiveModels, 30000);
      liveTsStripTimer = setInterval(() => { liveTsStrip.value = Date.now(); }, 5000);
    }
    function logout() { token.value = ''; localStorage.removeItem('token'); dashLoaded = false; }

    // Formatters
    function stripHtml(html) {
      if (!html) return '';
      const tmp = document.createElement('div');
      tmp.innerHTML = html;
      return (tmp.textContent || tmp.innerText || '').trim();
    }

    const PLATFORM_MAP = [
      { match: ['onlyfans.com'],               icon: '💜', name: 'OnlyFans' },
      { match: ['fansly.com'],                 icon: '💙', name: 'Fansly' },
      { match: ['twitter.com', 'x.com'],       icon: '🐦', name: 'Twitter / X' },
      { match: ['instagram.com'],              icon: '📸', name: 'Instagram' },
      { match: ['tiktok.com'],                 icon: '🎵', name: 'TikTok' },
      { match: ['t.me/', 'telegram.me', 'telegram.org'], icon: '✈️', name: 'Telegram' },
      { match: ['youtube.com', 'youtu.be'],    icon: '▶️', name: 'YouTube' },
      { match: ['reddit.com'],                 icon: '🤖', name: 'Reddit' },
      { match: ['snapchat.com'],               icon: '👻', name: 'Snapchat' },
      { match: ['manyvids.com'],               icon: '🎬', name: 'ManyVids' },
      { match: ['loyalfans.com'],              icon: '❤️', name: 'LoyalFans' },
      { match: ['chaturbate.com'],             icon: '🟠', name: 'Chaturbate' },
      { match: ['stripchat.com'],              icon: '🩷', name: 'StripChat' },
      { match: ['linktr.ee', 'linktree'],      icon: '🌿', name: 'Linktree' },
      { match: ['amazon.'],                    icon: '📦', name: 'Amazon Wishlist' },
      { match: ['lovense.com'],                icon: '💕', name: 'Lovense Wishlist' },
      { match: ['throne.com', 'throneapp'],    icon: '👑', name: 'Throne' },
      { match: ['wishlist.com', 'wish.com'],   icon: '🎁', name: 'Wishlist' },
      { match: ['whatsapp.com', 'wa.me'],      icon: '💬', name: 'WhatsApp' },
      { match: ['discord.com', 'discord.gg'],  icon: '🎮', name: 'Discord' },
      { match: ['patreon.com'],                icon: '🧡', name: 'Patreon' },
    ];
    function bioLinkMeta(href) {
      const h = (href || '').toLowerCase();
      for (const p of PLATFORM_MAP) {
        if (p.match.some(m => h.includes(m))) return { icon: p.icon, name: p.name };
      }
      try {
        const domain = new URL(href).hostname.replace(/^www\./, '');
        return { icon: '🔗', name: domain };
      } catch { return { icon: '🔗', name: (href || '').slice(0, 40) }; }
    }

    // Parse social_medias JSON array from API into display-ready objects
    function parseSocialMedias(jsonStr) {
      if (!jsonStr || jsonStr === '[]') return [];
      try {
        const arr = JSON.parse(jsonStr);
        return arr.map(s => {
          const rawLink = s.link || '';
          // Free = direct redirect link; paid = goes through /socials/social_media/ purchase flow
          const free = rawLink.startsWith('/external_link/') || rawLink.startsWith('http');
          const realHref = free ? cfDecodeHref(rawLink.startsWith('/') ? 'https://chaturbate.com' + rawLink : rawLink) : null;
          // Clean up malformed URLs (e.g. https://t.me/https://t.me/...)
          const cleanHref = realHref ? realHref.replace(/^(https?:\/\/[^/]+)\/(https?:\/\/)/, '$2') : null;
          // Derive platform from title_name first, then from URL
          const titleLow = (s.title_name || '').toLowerCase();
          let meta = bioLinkMeta(realHref);
          // title_name is more reliable than URL for platform name
          if (s.title_name) {
            const platform = s.title_name.replace(/\s*-\s*(Free|Lifetime|Monthly|Weekly)$/i, '').trim();
            meta = { icon: meta.icon, name: platform };
          }
          return { href: cleanHref, realHref: cleanHref, meta, free, tokens: s.tokens, label: s.label_text };
        });
      } catch { return []; }
    }

    // Cloudflare rewrites external links to /external_link/?url=encoded — decode back to real URL
    function cfDecodeHref(href) {
      if (!href) return href;
      const m = href.match(/[?&]url=([^&]+)/);
      if (m) { try { return decodeURIComponent(m[1]); } catch { return href; } }
      return href;
    }

    function parseBio(html) {
      if (!html) return { text: '', images: [], links: [] };
      const tmp = document.createElement('div');
      tmp.innerHTML = html;

      const imgs = [];
      // <img src="...">
      tmp.querySelectorAll('img[src]').forEach(el => {
        const src = el.getAttribute('src') || '';
        if (src && !imgs.includes(src)) imgs.push(src);
      });
      // CSS background-image: url(...) in style attributes
      tmp.querySelectorAll('[style]').forEach(el => {
        const style = el.getAttribute('style') || '';
        for (const m of style.matchAll(/url\(['"]?([^'")\s]+)['"]?\)/g)) {
          const src = m[1];
          if (src && !src.startsWith('data:') && !imgs.includes(src)) imgs.push(src);
        }
      });

      const links = [];
      const seen = new Set();
      tmp.querySelectorAll('a[href]').forEach(el => {
        const rawHref = el.getAttribute('href') || '';
        if (!rawHref || rawHref === '#') return;
        const realHref = cfDecodeHref(rawHref);   // real URL for display/detection
        if (seen.has(realHref)) return;
        seen.add(realHref);
        links.push({ href: rawHref, realHref });   // href = original (works with CF redirect)
      });

      const text = (tmp.textContent || tmp.innerText || '')
        .replace(/\s+/g, ' ').trim();

      return { text, images: imgs, links };
    }

    function fmtDur(s) { if (!s) return '0m'; const h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60; return h>0?`${h}h${String(m).padStart(2,'0')}m`:`${m}m${String(sec).padStart(2,'0')}s`; }
    function fmtDurLong(s) { if (!s) return '0h'; const d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60); return d>0?`${d}d ${h}h ${m}m`:h>0?`${h}h ${m}m`:`${m}m`; }
    function fmtSize(b) { if (!b) return '0'; if (b>=1099511627776) return (b/1099511627776).toFixed(1)+' TB'; if (b>=1073741824) return (b/1073741824).toFixed(1)+' GB'; if (b>=1048576) return (b/1048576).toFixed(0)+' MB'; return (b/1024).toFixed(0)+' KB'; }
    function badgeClass(p) { return {'Chaturbate':'badge-chaturbate','StripChat':'badge-stripchat','OnlyFans':'badge-onlyfans','ManyVids':'badge-manyvids','Cam4':'badge-cam4','Streamate':'badge-streamate','LiveJasmin':'badge-livejasmin'}[p]||'badge-default'; }

    onMounted(() => {
      if (token.value) loadInitialData();
      else initSplash();
    });

    return {
      token, privacyMode, togglePrivacy, gridCols, gridRows, gridStyle, onGridColsChange, onGridRowsChange,
      loginForm, loginLoading, loginError, doLogin, currentView,
      splashCols, splashStats, loginSuccess, showPw, lpCardEl, lpBgEl,
      lpColEls, lpColClick, lpZoomSrc, lpZoomOpen, lpZoomClose,
      search, videos, loading, page, totalPages, total,
      stats, platforms, topTags, filteredTags, tagSearchQ, showTagDrop, onTagSearchInput, hideTagDropSoon, pickTag,
      filterByTag, doSearch, searchModel, logout, goHome, switchView, resetSearch,
      fmtDur, fmtDurLong, fmtSize, badgeClass, jumpToPage, timeAgo, stripHtml, parseBio, bioLinkMeta, parseSocialMedias, parsePhotoSets,
      lightboxIdx, lbVideo, openLightbox, closeLightbox, lbPrevFn, lbNextFn,
      lbZoom, lbDX, lbDY, onLbWheel, onLbDragStart, lbResetZoom,
      suggestions, suggestIdx, onModelInput, suggestDown, suggestUp, pickSuggestion, selectAndSearch, hideSuggestSoon,
      onSearchFocus, showHistory, searchHistory, pickHistory, removeHistory,
      lbItems, lbLoading, lbSort, lbResolution, lbPeriod, lbPage, lbTotalPages, lbTotal,
      lbMaxCount, lbMaxDuration, resolutions,
      loadLeaderboard, setLbSort, setLbResolution, setLbPeriod, jumpToLbPage, exportLeaderboard,
      detailModel, detailData, detailLoading, tlMax, openModelDetail, closeDetail, searchFromDetail,
      detailLive, liveTs, detailBio, showBioPreview, bioIframeEl,
      liveModels, liveLoading, liveTsStrip, loadLiveModels,
      streamModel, streamData, streamLoading, streamVideoEl, openStream, closeStream,
      dashDays, setDashDays, heatmapData, heatmapDays, heatColor
    };
  }
}).mount('#app');
