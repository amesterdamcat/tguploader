import { createApp, ref, reactive, computed, onMounted, onBeforeUnmount, nextTick, h } from 'vue';
import mpegts from 'mpegts.js';
import Hls from 'hls.js';
import './styles/lequ.css';

window.mpegts = mpegts;
window.Hls = Hls;

const TOKEN_KEY = 'token';
const getToken = () => localStorage.getItem(TOKEN_KEY) || '';
const dropToken = () => localStorage.removeItem(TOKEN_KEY);
(function adoptUrlToken(){
  const u = new URL(location.href);
  const tok = u.searchParams.get('token');
  if (tok){ localStorage.setItem(TOKEN_KEY, tok); u.searchParams.delete('token'); history.replaceState({}, '', u.pathname+(u.search||'')); }
})();

let onUnauthorized = () => {};
let onLequExpired = () => {};
async function api(path, opts = {}){
  const headers = { 'Authorization':'Bearer '+getToken(), ...(opts.body?{'Content-Type':'application/json'}:{}), ...opts.headers };
  const res = await fetch(path, { ...opts, headers });
  if (res.status === 401){ let d={}; try{ d=await res.json(); }catch{}
    if(d.lequ_auth_expired){ onLequExpired(); throw new Error(d.error||'LeQu 登录已失效，请重新登录'); }
    if ((d.detail||'')==='Unauthorized'){ dropToken(); onUnauthorized(); throw new Error('Unauthorized'); }
    throw new Error(d.error || d.detail || 'Unauthorized'); }
  if (!res.ok){ let d={}; try{ d=await res.json(); }catch{} throw new Error(d.error || d.detail || d.msg || ('HTTP '+res.status)); }
  return res.json();
}

const ICONS = {
  live:'<rect x="2" y="6" width="14" height="12" rx="2"/><path d="m16 13 5.2 3.5a.5.5 0 0 0 .8-.4V7.9a.5.5 0 0 0-.8-.4L16 10.5"/>',
  play:'<path d="m7 4 13 8-13 8z"/>',
  rec:'<circle cx="12" cy="12" r="8"/>',
  stop:'<rect x="6" y="6" width="12" height="12" rx="2"/>',
  pause:'<path d="M8 5v14M16 5v14"/>',
  plus:'<path d="M5 12h14M12 5v14"/>',
  check:'<path d="M20 6 9 17l-5-5"/>',
  x:'<path d="M18 6 6 18M6 6l12 12"/>',
  search:'<circle cx="11" cy="11" r="8"/><path d="m21 21-4.3-4.3"/>',
  refresh:'<path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8"/><path d="M21 3v5h-5"/><path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16"/><path d="M8 16H3v5"/>',
  star:'<path d="M12 2 15.09 8.26 22 9.27l-5 4.87 1.18 6.88L12 17.77l-6.18 3.25L7 14.14 2 9.27l6.91-1.01L12 2z"/>',
  heart:'<path d="M20.84 4.61a5.5 5.5 0 0 0-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 0 0-7.78 7.78L12 21.23l8.84-8.84a5.5 5.5 0 0 0 0-7.78z"/>',
  eye:'<path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>',
  copy:'<rect x="9" y="9" width="13" height="13" rx="2"/><rect x="2" y="2" width="13" height="13" rx="2"/>',
  external:'<path d="M15 3h6v6"/><path d="M10 14 21 3"/><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/>',
  logout:'<path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><path d="m16 17 5-5-5-5"/><path d="M21 12H9"/>',
  phone:'<path d="M22 16.92v3a2 2 0 0 1-2.18 2 19.79 19.79 0 0 1-8.63-3.07 19.5 19.5 0 0 1-6-6 19.79 19.79 0 0 1-3.07-8.67A2 2 0 0 1 4.11 2h3a2 2 0 0 1 2 1.72c.13.96.36 1.9.7 2.81a2 2 0 0 1-.45 2.11L8.09 9.91a16 16 0 0 0 6 6l1.27-1.27a2 2 0 0 1 2.11-.45c.91.34 1.85.57 2.81.7A2 2 0 0 1 22 16.92z"/>',
  msg:'<path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/>',
  danmaku:'<path d="M3 5h18M3 12h12M3 19h16"/><path d="M17 10l4 2-4 2z"/>',
  gift:'<rect x="3" y="8" width="18" height="4" rx="1"/><path d="M12 8v13M5 12v8a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-8"/><path d="M12 8S9 3 6.5 4.5 8.5 8 12 8zm0 0s3-5 5.5-3.5S15.5 8 12 8z"/>',
  chevL:'<path d="M15 18l-6-6 6-6"/>',
  chevR:'<path d="M9 18l6-6-6-6"/>',
  download:'<path d="M12 3v12"/><path d="m7 10 5 5 5-5"/><path d="M5 21h14"/>',
  image:'<rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="8.5" cy="8.5" r="1.5"/><path d="m21 15-5-5L5 21"/>',
};
const Icon = {
  props:{ n:{type:String,required:true}, s:{type:Number,default:16} },
  setup(p){ return ()=> h('svg', { class:'ic', width:p.s, height:p.s, viewBox:'0 0 24 24', fill:'none',
    stroke:'currentColor','stroke-width':1.8,'stroke-linecap':'round','stroke-linejoin':'round',
    'aria-hidden':'true', innerHTML: ICONS[p.n]||'' }); }
};

const App = {
  components:{ Icon },
  setup(){
    // ── panel JWT auth ──
    const token = ref(getToken());
    const loggedIn = computed(()=> !!token.value);
    onUnauthorized = ()=> { token.value=''; };
    const lf = reactive({ user:'', pass:'', err:'', busy:false });
    async function doLogin(){
      lf.busy=true; lf.err='';
      try{
        const r = await fetch('/api/auth/login', { method:'POST', headers:{'Content-Type':'application/json'},
          body: JSON.stringify({username:lf.user, password:lf.pass}) });
        const ct = r.headers.get('content-type')||'';
        if(!r.ok || !ct.includes('json')) throw new Error('登录失败');
        const d = await r.json();
        if(!d.token) throw new Error(d.detail||'登录失败');
        localStorage.setItem(TOKEN_KEY, d.token); token.value=d.token; boot();
      }catch(e){ lf.err = e.message||'登录失败'; } finally{ lf.busy=false; }
    }
    function logout(){ closePlayer(); stopStateStream(); dropToken(); token.value=''; }

    // ── toast ──
    const toast = reactive({ msg:'', bad:false, t:null });
    function notify(msg, bad=false){ toast.msg=msg; toast.bad=bad; clearTimeout(toast.t); toast.t=setTimeout(()=>toast.msg='', 3600); }

    // ── LeQu account ──
    const lequ = reactive({ logged_in:false, user_id:'', phone:'' });
    const sms = reactive({ phone:'', country:'86', code:'', challenge:'', stage:'phone', busy:false, err:'' });
    onLequExpired = ()=>{
      closePlayer(); lequ.logged_in=false; stopStateStream(); list.value=[]; tracked.value=[]; recs.value=[];
      sms.stage='phone'; sms.code=''; sms.challenge='';
      notify('LeQu 登录已失效，请重新获取验证码登录', true);
    };
    async function refreshLequ(){
      try{ const d = await api('/api/lequ/auth/status');
        if(d.expired){ onLequExpired(); return; }
        lequ.logged_in=d.logged_in; lequ.user_id=d.user_id; lequ.phone=d.phone;
      }
      catch(e){ if(e.message==='Unauthorized') return; }
    }
    async function smsSend(){
      sms.busy=true; sms.err='';
      try{
        const d = await api('/api/lequ/auth/sms/send', { method:'POST', body: JSON.stringify({phone:sms.phone, country:sms.country}) });
        sms.challenge = d.challenge_id; sms.stage='code';
        notify('验证码已发送 ✓');
      }catch(e){ sms.err = e.message; } finally{ sms.busy=false; }
    }
    async function smsComplete(){
      sms.busy=true; sms.err='';
      try{
        await api('/api/lequ/auth/sms/complete', { method:'POST', body: JSON.stringify({challenge_id:sms.challenge, code:sms.code}) });
        sms.stage='phone'; sms.code=''; sms.challenge='';
        await refreshLequ(); notify('LeQu 登录成功 ✓'); load();
      }catch(e){ sms.err = e.message; } finally{ sms.busy=false; }
    }
    async function lequLogout(){
      try{ await api('/api/lequ/auth/logout', { method:'POST', body:'{}' }); await refreshLequ(); notify('已退出 LeQu'); }
      catch(e){ notify(e.message, true); }
    }

    // ── views ──
    const view = ref('live');          // live | tracked | recordings | userinfo
    const liveTab = ref('hot');        // hot | following | recommended
    const list = ref([]);
    const loadingList = ref(false);
    const q = ref('');
    const tracked = ref([]);
    const recs = ref([]);
    const recHistory = ref([]);
    const recordingScanBusy = ref(false);
    const contactBatchBusy = ref(false);
    const recPage = ref(1);
    const recPageSize = 9;
    const recPages = computed(()=>Math.max(1,Math.ceil(recHistory.value.length/recPageSize)));
    const pagedRecHistory = computed(()=>{
      const page=Math.min(recPage.value,recPages.value);
      return recHistory.value.slice((page-1)*recPageSize,page*recPageSize);
    });
    const followBusy = reactive({});
    const userInfo = reactive({ query:'', loading:false, error:'', profile:null, followBusy:false });
    const debugLogs = ref([]);
    const debugExpanded = ref(new Set());
    const debugDisplayLogs = computed(()=>debugLogs.value.slice().reverse());
    const runtimeLogs = ref([]);
    const runtimeLogBusy = ref(false);
    const player = reactive({ open:false, loading:false, error:'', session:'', title:'', vid:'', name:'',
      playlist:'', mode:'', followed:false, followBusy:false, recordBusy:false,
      sourceUrl:'', sourceBusy:false, speed:0, buffer:0, stalls:0 });
    const playerVideo = ref(null);
    const archivePlayer = reactive({open:false,title:'',url:''});
    const gift = reactive({ open:false, giftId:150, giftNum:1, buyType:0, pkId:0, busy:false });
    // 么么哒: driven by real watched_secs from the comment poll (server-side heartbeat)
    const HEART_AUTO_KEY = 'lequ_heart_auto';
    const heart = reactive({ giftName:'么么哒', watched:0, need:300,
      auto:localStorage.getItem(HEART_AUTO_KEY)==='1', sent:0, busy:false, retryAt:0 });
    const heartEarned = computed(()=> Math.floor((heart.watched||0)/(heart.need||300)));
    const heartAvailable = computed(()=> Math.max(0, heartEarned.value - heart.sent));
    const heartReady = computed(()=> heartAvailable.value > 0);
    const heartRemain = computed(()=> heartReady.value ? 0 :
      Math.max(0, (heart.need||300) - ((heart.watched||0) % (heart.need||300))));
    const heartProgress = computed(()=> heartReady.value ? 100 :
      Math.min(100, ((heart.watched||0) % (heart.need||300)) / (heart.need||300) * 100));
    // live comments / danmaku
    const danmaku = reactive({ mode:'danmaku' });   // 'danmaku' | 'list'
    const comments = ref([]);     // chat feed (both modes)
    const flying = ref([]);       // active floating bullets
    const commentList = ref(null);
    const DANMAKU_TRACKS = 6;
    let commentTimer = null, commentBusy = false, commentEpoch = 0, commentCid = 0, flyKey = 0, seenIds = new Set();
    const trackedNames = computed(()=> new Set(tracked.value.map(t=>t.name)));
    const playerRecording = computed(()=>recs.value.some(r=>
      (player.vid && r.vid===player.vid) || (player.name && r.name===player.name)));
    const visibleVids = new Set();
    const statsQueued = new Set();
    const statsQueue = [];
    let statsActive = 0;
    let statsObserver = null;
    let stateTimer = null;
    let stateBusy = false;
    let debugTimer = null;
    let debugBusy = false;
    let debugAfter = 0;
    let hlsPlayer = null;
    let flvPlayer = null;
    let hlsTried = false;
    let playerHealthTimer = null;

    function fmtCount(n){
      if(n==null) return '—';
      return n >= 10000 ? (n/10000).toFixed(n>=100000?0:1)+'万' : Number(n).toLocaleString('zh-CN');
    }
    function prettyJson(value){
      if(value==null || value==='') return '';
      try{
        const parsed = typeof value === 'string' ? JSON.parse(value) : value;
        return JSON.stringify(parsed, null, 2);
      }catch{
        return String(value);
      }
    }
    function debugPreview(value, limit=420){
      if(value==null || value==='') return '';
      const raw = typeof value === 'string' ? value : JSON.stringify(value);
      const compact = raw.replace(/\s+/g, ' ').trim();
      return compact.length > limit ? compact.slice(0, limit) + ' ...' : compact;
    }
    function isDebugOpen(seq){ return debugExpanded.value.has(seq); }
    function toggleDebug(seq){
      const next = new Set(debugExpanded.value);
      next.has(seq) ? next.delete(seq) : next.add(seq);
      debugExpanded.value = next;
    }
    function pumpStats(){
      while(statsActive < 3 && statsQueue.length){
        const a = statsQueue.shift();
        statsQueued.delete(a.vid);
        statsActive++;
        a.stats_loading=true;
        api('/api/lequ/live/stats?vid='+encodeURIComponent(a.vid))
          .then(d=>{
            a.watching_count=d.watching_count;
            a.watch_count=d.watch_count;
            a.followed=d.followed;
            a.follow_known=true;
            a.stats_at=Date.now();
          })
          .catch(e=>{ a.stats_error=e.message||'获取失败'; })
          .finally(()=>{ a.stats_loading=false; statsActive--; pumpStats(); });
      }
    }
    function queueStats(a, force=false){
      if(!a?.living || !a.vid || statsQueued.has(a.vid)) return;
      if(!force && a.stats_at && Date.now()-a.stats_at < 30000) return;
      statsQueued.add(a.vid); statsQueue.push(a); pumpStats();
    }
    function bindStatsObserver(){
      if(statsObserver) statsObserver.disconnect();
      visibleVids.clear();
      statsObserver = new IntersectionObserver(entries=>{
        entries.forEach(entry=>{
          const vid=entry.target.dataset.vid;
          if(entry.isIntersecting){
            visibleVids.add(vid);
            queueStats(list.value.find(a=>a.vid===vid));
          }else visibleVids.delete(vid);
        });
      }, {rootMargin:'160px 0px', threshold:.01});
      document.querySelectorAll('.card[data-vid]').forEach(el=>statsObserver.observe(el));
    }
    function refreshVisibleStats(){
      visibleVids.forEach(vid=>queueStats(list.value.find(a=>a.vid===vid)));
    }

    async function loadList(){
      if(!lequ.logged_in) return;
      loadingList.value=true;
      try{
        const d = q.value.trim()
          ? await api('/api/lequ/search?q='+encodeURIComponent(q.value.trim()))
          : await api('/api/lequ/live?type='+liveTab.value);
        list.value = d.list||[];
        loadingList.value=false;
        await nextTick();
        bindStatsObserver();
      }catch(e){ notify(e.message, true); loadingList.value=false; }
    }
    async function loadTracked(){ try{ const d=await api('/api/lequ/tracked'); tracked.value=d.tracked||[]; }catch(e){} }
    async function loadRecs(){ try{ const d=await api('/api/lequ/recordings');
      recs.value=d.active||[]; recHistory.value=d.history||[];
      recPage.value=Math.min(recPage.value,Math.max(1,Math.ceil(recHistory.value.length/recPageSize)));
    }catch(e){} }
    async function load(){
      await refreshLequ();
      if(!lequ.logged_in) return;
      loadTracked();
      loadRecs();
      refreshSettings();
      refreshFarm();
      if(view.value==='live') loadList();
    }
    async function pollState(){
      if(stateBusy || document.visibilityState!=='visible') return;
      stateBusy=true;
      try{
        const d=await api('/api/lequ/state');
        tracked.value=d.tracked||[];
        recs.value=d.active||[];
        if(Array.isArray(d.history)) recHistory.value=d.history;
      }catch{} finally{ stateBusy=false; }
    }
    function startStateStream(){
      stopStateStream();
      pollState();
      stateTimer=setInterval(pollState, 3000);
    }
    function stopStateStream(){
      if(stateTimer){ clearInterval(stateTimer); stateTimer=null; }
      stateBusy=false;
    }

    async function track(a){
      try{ await api('/api/lequ/track', { method:'POST', body: JSON.stringify({name:a.name, nickname:a.nickname}) });
        notify('已加入追踪：'+(a.nickname||a.name)); loadTracked(); }
      catch(e){ notify(e.message, true); }
    }
    async function untrack(name){
      try{ await api('/api/lequ/untrack', { method:'POST', body: JSON.stringify({name}) }); notify('已取消追踪'); loadTracked(); }
      catch(e){ notify(e.message, true); }
    }
    async function toggleFollow(a){
      if(followBusy[a.name]) return;
      const next = !a.followed;
      followBusy[a.name]=true;
      try{
        await api('/api/lequ/follow', { method:'POST',
          body: JSON.stringify({name:a.name, vid:a.vid, follow:next}) });
        list.value.forEach(x=>{ if(x.name===a.name){ x.followed=next; x.follow_known=true; } });
        if(player.name===a.name) player.followed=next;
        if(!next && liveTab.value==='following' && !q.value.trim())
          list.value = list.value.filter(x=>x.name!==a.name);
        notify(next ? '已关注：'+(a.nickname||a.name) : '已取消关注：'+(a.nickname||a.name));
      }catch(e){ notify(e.message, true); }
      finally{ followBusy[a.name]=false; }
    }
    async function recordNow(a){
      try{ const d = await api('/api/lequ/record', { method:'POST', body: JSON.stringify({name:a.name, nickname:a.nickname, vid:a.vid}) });
        notify(d.already ? '已在录制中' : '开始录制：'+(a.nickname||a.name)); setTimeout(loadRecs, 800); }
      catch(e){ notify(e.message, true); }
    }
    async function togglePlayerFollow(){
      if(player.followBusy || !player.name) return;
      const next=!player.followed;
      player.followBusy=true;
      try{
        await api('/api/lequ/follow',{method:'POST',body:JSON.stringify({
          name:player.name,vid:player.vid,follow:next
        })});
        player.followed=next;
        list.value.forEach(x=>{ if(x.name===player.name){ x.followed=next; x.follow_known=true; } });
        notify(next?'已关注：'+player.title:'已取消关注：'+player.title);
      }catch(e){ notify(e.message,true); }
      finally{ player.followBusy=false; }
    }
    async function recordPlayer(){
      if(player.recordBusy || playerRecording.value || !player.vid) return;
      player.recordBusy=true;
      try{
        const d=await api('/api/lequ/record',{method:'POST',body:JSON.stringify({
          name:player.name,nickname:player.title,vid:player.vid
        })});
        notify(d.already?'已在录制中':'开始录制：'+player.title);
        await pollState();
      }catch(e){ notify(e.message,true); }
      finally{ player.recordBusy=false; }
    }
    async function ensureSourceUrl(){
      if(player.sourceUrl) return player.sourceUrl;
      if(!player.vid) throw new Error('缺少直播 vid');
      player.sourceBusy=true;
      try{
        const d=await api('/api/lequ/playurl?vid='+encodeURIComponent(player.vid));
        player.sourceUrl=d.playUrl||'';
        if(!player.sourceUrl) throw new Error('源流地址为空');
        return player.sourceUrl;
      }finally{ player.sourceBusy=false; }
    }
    async function copySourceUrl(){
      try{
        const url=await ensureSourceUrl();
        await navigator.clipboard.writeText(url);
        notify('已复制 RTMP 源流地址');
      }catch(e){ notify(e.message||'复制失败', true); }
    }
    async function openSourceUrl(){
      try{
        const url=await ensureSourceUrl();
        window.location.href=url;
        notify('已交给外部播放器处理');
      }catch(e){ notify(e.message||'打开失败', true); }
    }
    async function stopRec(vid){
      try{ await api('/api/lequ/recording/stop', { method:'POST', body: JSON.stringify({vid}) }); notify('已发送停止'); setTimeout(loadRecs, 800); }
      catch(e){ notify(e.message, true); }
    }
    function recordingAsset(url){
      if(!url) return '';
      return url+(url.includes('?')?'&':'?')+'token='+encodeURIComponent(getToken());
    }
    function mediaSpec(r){
      const w=Number(r.width||0), h=Number(r.height||0), fps=Number(r.fps||0);
      const bits=[];
      if(w>0 && h>0) bits.push(`${w}×${h}`);
      if(fps>0) bits.push(`${fps.toFixed(fps>=10?1:2).replace(/\.0$/,'')}fps`);
      return bits.join(' · ');
    }
    async function generateContactSheet(r){
      if(r.thumb_generating) return;
      r.thumb_generating=true;
      try{
        await api('/api/lequ/recordings/'+encodeURIComponent(r.id)+'/contactsheet',
          {method:'POST',body:'{}'});
        notify('接触表正在后台生成');
        setTimeout(loadRecs, 3000);
      }catch(e){ r.thumb_generating=false; notify(e.message,true); }
    }
    async function generateMissingContactSheets(){
      if(contactBatchBusy.value) return;
      contactBatchBusy.value=true;
      try{
        const d=await api('/api/lequ/recordings/contactsheet/batch',{method:'POST',body:'{}'});
        if(Array.isArray(d.history)) recHistory.value=d.history;
        notify(d.queued ? `已入队 ${d.queued} 个接触表任务（并发3）` : '没有缺失的接触表');
        setTimeout(loadRecs, 3000);
      }catch(e){ notify(e.message,true); }
      finally{ contactBatchBusy.value=false; }
    }
    async function scanInterruptedRecordings(){
      if(recordingScanBusy.value) return;
      recordingScanBusy.value=true;
      try{
        const d=await api('/api/lequ/recordings/scan',{method:'POST',body:'{}'});
        recHistory.value=d.history||recHistory.value;
        notify(d.added ? `发现 ${d.added} 条异常中断录制` : '扫描完成，没有发现新文件');
      }catch(e){ notify(e.message,true); }
      finally{ recordingScanBusy.value=false; }
    }
    async function remuxRecording(r){
      if(r.remuxing) return;
      r.remuxing=true; r.state='remuxing'; delete r.remux_error;
      try{
        await api('/api/lequ/recordings/'+encodeURIComponent(r.id)+'/remux',
          {method:'POST',body:'{}'});
        notify('已加入 MP4 转封装任务');
        setTimeout(loadRecs, 1500);
      }catch(e){ r.remuxing=false; r.state='interrupted'; notify(e.message,true); }
    }
    function setRecPage(page){
      recPage.value=Math.max(1,Math.min(recPages.value,page));
      window.scrollTo({top:0,behavior:'smooth'});
    }
    function playRecording(r){
      if(!r.play_url) return;
      archivePlayer.title=r.nickname||r.name||r.file;
      archivePlayer.url=recordingAsset(r.play_url);
      archivePlayer.open=true;
    }
    function closeRecordingPlayer(){
      archivePlayer.open=false;
      archivePlayer.url='';
    }
    async function waitPlaylist(url){
      for(let i=0;i<30;i++){
        try{ const r=await fetch(url, {cache:'no-store'}); if(r.ok) return true; }catch{}
        await new Promise(resolve=>setTimeout(resolve, 300));
      }
      return false;
    }
    function cmtClass(t){ return t===1?'sys':(t===2?'fol':''); }
    function pushFlying(c){
      if(!c.content || danmaku.mode!=='danmaku') return;
      const track = flyKey % DANMAKU_TRACKS;
      flying.value.push({ key:'f'+(flyKey++), top:(track*15+3)+'%', dur:(7+Math.random()*4).toFixed(2)+'s',
        cls:cmtClass(c.type), nick:c.nickname, title:c.title, text:c.content, type:c.type });
      if(flying.value.length>80) flying.value.splice(0, flying.value.length-80);
    }
    function flyEnd(key){ const i=flying.value.findIndex(f=>f.key===key); if(i>=0) flying.value.splice(i,1); }
    async function pollComments(vid, epoch=commentEpoch){
      if(epoch!==commentEpoch || commentBusy || !player.open || player.vid!==vid) return;
      commentBusy=true;
      try{
        const d = await api('/api/lequ/comments?vid='+encodeURIComponent(vid)+'&cid='+commentCid);
        if(epoch!==commentEpoch) return;
        if(typeof d.cid==='number') commentCid = Math.max(commentCid, d.cid);
        if(typeof d.watched_secs==='number'){     // drives the 么么哒 countdown
          heart.watched = d.watched_secs; heart.need = d.need_secs || 300;
          if(heart.auto && heartReady.value && !heart.busy && Date.now()>=heart.retryAt) sendHeart(true);
        }
        const arr = (d.comments||[]).filter(c=>{
          const k = c.id+'|'+c.type;            // dedup guard (pinned/system never loops)
          if(seenIds.has(k)) return false; seenIds.add(k); return true;
        });
        if(seenIds.size>1000){ seenIds = new Set([...seenIds].slice(-500)); }
        if(arr.length){
          for(const c of arr){ comments.value.push(c); pushFlying(c); }
          if(comments.value.length>240) comments.value.splice(0, comments.value.length-240);
          if(danmaku.mode==='list'){ await nextTick(); const el=commentList.value; if(el) el.scrollTop=el.scrollHeight; }
        }
      }catch(e){ /* keep polling quietly */ }
      finally{ if(epoch===commentEpoch) commentBusy=false; }
    }
    function startComments(vid){
      stopComments(); comments.value=[]; flying.value=[]; commentCid=0; flyKey=0; seenIds.clear();
      const epoch=commentEpoch;
      pollComments(vid, epoch);
      commentTimer = setInterval(()=>{ if(player.open && document.visibilityState==='visible') pollComments(vid, epoch); }, 2000);
    }
    function stopComments(){ commentEpoch++; if(commentTimer){ clearInterval(commentTimer); commentTimer=null; } commentBusy=false; flying.value=[]; }
    function setDmMode(m){ danmaku.mode=m; if(m!=='danmaku') flying.value=[]; }

    // HLS fallback (segmented, higher latency) — used only if HTTP-FLV is unavailable.
    async function startHls(a, video){
      player.mode='hls';
      const d=await api('/api/lequ/player/start', {method:'POST',
        body:JSON.stringify({vid:a.vid,name:a.name,nickname:a.nickname||a.name})});
      player.session=d.session_id; player.playlist=d.playlist;
      if(!await waitPlaylist(player.playlist)) throw new Error('等待直播分片超时');
      if(window.Hls?.isSupported()){
        hlsPlayer=new window.Hls({lowLatencyMode:true, backBufferLength:8, maxBufferLength:10, liveSyncDurationCount:2});
        hlsPlayer.loadSource(player.playlist);
        hlsPlayer.attachMedia(video);
        hlsPlayer.on(window.Hls.Events.MANIFEST_PARSED, ()=>video.play().catch(()=>{}));
        hlsPlayer.on(window.Hls.Events.ERROR, (_,data)=>{
          if(data.fatal){
            if(data.type===window.Hls.ErrorTypes.NETWORK_ERROR) hlsPlayer.startLoad();
            else if(data.type===window.Hls.ErrorTypes.MEDIA_ERROR) hlsPlayer.recoverMediaError();
            else player.error='直播播放失败';
          }
        });
      }else if(video.canPlayType('application/vnd.apple.mpegurl')){
        video.src=player.playlist; video.play().catch(()=>{});
      }else throw new Error('当前浏览器不支持播放');
    }
    // jump to the prev/next LIVING room in the current list (wrap-around)
    const hasAdjacent = computed(()=> (list.value||[]).filter(x=>x.living && x.vid).length > 1);
    function playAdjacent(dir){
      const arr = list.value || [];
      if(arr.length < 2) return;
      let i = arr.findIndex(x=>x.vid===player.vid);
      if(i < 0) i = 0;
      for(let k=1; k<=arr.length; k++){
        const j = ((i + dir*k) % arr.length + arr.length) % arr.length;
        const x = arr[j];
        if(x && x.living && x.vid && x.vid!==player.vid){ playLive(x); return; }
      }
    }
    async function playLive(a){
      await closePlayer();
      player.open=true; player.loading=true; player.error=''; player.title=a.nickname||a.name; player.vid=a.vid; player.name=a.name;
      player.followed=!!a.followed; player.followBusy=false; player.recordBusy=false;
      player.speed=0; player.buffer=0; player.stalls=0;
      hlsTried=false;
      await nextTick();
      const video=playerVideo.value;
      if(!video){ player.error='播放器初始化失败'; player.loading=false; return; }
      player.sourceUrl=''; player.sourceBusy=false;
      const startSideFeeds=()=>{
        if(player.vid!==a.vid) return;
        startComments(a.vid);
        startHeart(a.vid);
        loadHeartMeta(a.vid);
      };
      video.addEventListener('playing', startSideFeeds, {once:true});
      setTimeout(startSideFeeds, 2500);
      const mpegtsOk = !!(window.mpegts && window.mpegts.isSupported && window.mpegts.isSupported());
      console.info('[player] mpegts.isSupported =', mpegtsOk);
      try{
        if(mpegtsOk){
          // low-latency HTTP-FLV: one continuous stream, ~1-2s latency, no segment stalls
          player.mode='flv';
          // absolute URL: mpegts worker thread can't resolve relative URLs
          const url=location.origin+'/api/lequ/flv?vid='+encodeURIComponent(a.vid)+'&token='+encodeURIComponent(getToken());
          console.info('[player] FLV →', url);
          flvPlayer=mpegts.createPlayer({type:'flv', isLive:true, url},
            {enableWorker:true, enableStashBuffer:true, stashInitialSize:524288,
             liveBufferLatencyChasing:true, liveBufferLatencyMaxLatency:5,
             liveBufferLatencyMinRemain:1.5, lazyLoad:false,
             autoCleanupSourceBuffer:true, autoCleanupMaxBackwardDuration:30,
             autoCleanupMinBackwardDuration:10});
          flvPlayer.attachMediaElement(video);
          flvPlayer.on(mpegts.Events.STATISTICS_INFO, info=>{
            player.speed=Number(info.speed)||0;
          });
	          flvPlayer.on(mpegts.Events.ERROR, (type,detail,info)=>{   // fall back to HLS once
	            console.warn('[player] FLV error → HLS fallback:', type, detail, info);
	            const errText = [type, detail, info && JSON.stringify(info)].filter(Boolean).join(' ');
	            if(errText.includes('409') || errText.includes('404')){
	              player.error='直播已结束或源流不可用';
	              return;
	            }
	            if(hlsTried){ player.error='直播播放失败'; return; }
            hlsTried=true;
            try{ flvPlayer.destroy(); }catch(e){} flvPlayer=null;
            startHls(a, video).catch(e=>{ player.error=e.message||'播放失败'; });
          });
          flvPlayer.load();
          flvPlayer.play().catch(()=>{});
        }else{
          console.info('[player] mpegts unsupported → HLS');
          await startHls(a, video);
        }
      }catch(e){ player.error=e.message||'播放失败'; }
      finally{
        player.loading=false;
        if(playerHealthTimer) clearInterval(playerHealthTimer);
        playerHealthTimer=setInterval(()=>{
          const v=playerVideo.value;
          if(!player.open||!v) return;
          let ahead=0;
          for(let i=0;i<v.buffered.length;i++){
            if(v.buffered.start(i)<=v.currentTime && v.buffered.end(i)>=v.currentTime){
              ahead=v.buffered.end(i)-v.currentTime; break;
            }
          }
          player.buffer=Math.max(0,ahead);
        },1000);
      }
    }
    const HEART_GIFT_ID = 348;   // 么么哒 (5-min watch free gift), from capture
    function startHeart(){ heart.watched=0; heart.need=300; heart.sent=0; heart.busy=false; heart.retryAt=0; }
    function stopHeart(){ heart.watched=0; heart.sent=0; heart.busy=false; heart.retryAt=0; }
    function setHeartAuto(value){
      heart.auto=!!value;
      localStorage.setItem(HEART_AUTO_KEY, heart.auto?'1':'0');
      if(heart.auto && heartReady.value) sendHeart(true);
    }
    async function loadHeartMeta(vid){
      try{
        const d=await api('/api/lequ/countdown_gift?vid='+encodeURIComponent(vid));
        heart.giftName=d.giftName||'么么哒';
        heart.need=Number(d.countTime)||300;
      }catch(e){}
    }
    async function sendHeart(automatic=false){
      if(heart.busy || !player.name) return;
      if(!heartReady.value){ notify('么么哒还没攒够（看播满5分钟可得）', true); return; }
      heart.busy=true;
      try{
        await api('/api/lequ/gift/send', { method:'POST', body: JSON.stringify({
          vid:player.vid, name:player.name, giftId:HEART_GIFT_ID, giftNum:1, buyType:0, pkId:0 }) });
        heart.sent++; heart.retryAt=0; notify('已送出 '+heart.giftName);
      }catch(e){
        heart.retryAt=Date.now()+15000;
        if(!automatic) notify(e.message, true);
      } finally{ heart.busy=false; }
    }
    async function sendGift(){
      if(gift.busy) return;
      if(!player.name){ notify('缺少主播 ID', true); return; }
      const id=Number(gift.giftId)||0, num=Number(gift.giftNum)||0;
      if(id<=0 || num<=0){ notify('请填写礼物 ID 和数量', true); return; }
      gift.busy=true;
      try{
        await api('/api/lequ/gift/send', { method:'POST', body: JSON.stringify({
          vid:player.vid, name:player.name, giftId:id, giftNum:num,
          buyType:Number(gift.buyType)||0, pkId:Number(gift.pkId)||0 }) });
        notify('已送出 ✓'); gift.open=false;
      }catch(e){ notify(e.message, true); }
      finally{ gift.busy=false; }
    }
    async function closePlayer(){
      const session=player.session;
      gift.open=false;
      stopComments(); stopHeart();
      if(flvPlayer){ try{ flvPlayer.destroy(); }catch(e){} flvPlayer=null; }
      if(hlsPlayer){ try{ hlsPlayer.destroy(); }catch(e){} hlsPlayer=null; }
      if(playerHealthTimer){ clearInterval(playerHealthTimer); playerHealthTimer=null; }
      if(playerVideo.value){ playerVideo.value.pause(); playerVideo.value.removeAttribute('src'); playerVideo.value.load(); }
      player.open=false; player.loading=false; player.session=''; player.playlist=''; player.error='';
      player.mode=''; player.sourceUrl=''; player.sourceBusy=false; player.speed=0; player.buffer=0; player.stalls=0;
      if(session) api('/api/lequ/player/stop', {method:'POST',body:JSON.stringify({session_id:session})}).catch(()=>{});
    }
    async function loadUserInfo(self=false){
      userInfo.loading=true; userInfo.error='';
      try{
        const name=self ? '' : userInfo.query.trim();
        const d=await api('/api/lequ/user/info'+(name?'?name='+encodeURIComponent(name):''));
        userInfo.profile=d.profile||null;
        if(userInfo.profile && !self) userInfo.query=userInfo.profile.name||name;
      }catch(e){ userInfo.error=e.message; }
      finally{ userInfo.loading=false; }
    }
    async function followProfile(){
      const p=userInfo.profile;
      if(!p || userInfo.followBusy) return;
      const next=!p.followed;
      userInfo.followBusy=true;
      try{
        // vid is contextual (only when in a room) — follow is keyed on the anchor name
        const vid=(list.value||[]).find(x=>x.name===p.name && x.living)?.vid || '';
        await api('/api/lequ/follow', { method:'POST', body: JSON.stringify({name:p.name, vid, follow:next}) });
        p.followed=next;
        notify(next ? '已关注：'+(p.nickname||p.name) : '已取消关注');
      }catch(e){ notify(e.message, true); }
      finally{ userInfo.followBusy=false; }
    }
    async function trackProfile(){
      const p=userInfo.profile;
      if(!p) return;
      await track({name:p.name, nickname:p.nickname||p.name});
    }
    async function loadDebugLogs(){
      try{
        const d=await api('/api/lequ/debug/logs');
        debugLogs.value=(d.logs||[]).slice(-120);
        debugExpanded.value = new Set();
        loadRuntimeLogs();
        startDebugStream(d.latest_seq||0);
      }catch(e){ notify(e.message,true); }
    }
    async function loadRuntimeLogs(){
      if(runtimeLogBusy.value) return;
      runtimeLogBusy.value=true;
      try{
        const d=await api('/api/job/logs?channel=lequ&max=300');
        runtimeLogs.value=(d.lines||[]).map(x=>x.text||'').filter(Boolean).reverse();
      }catch(e){ notify(e.message,true); }
      finally{ runtimeLogBusy.value=false; }
    }
    function clearDebugLogs(){ debugLogs.value=[]; debugExpanded.value = new Set(); }
    function startDebugStream(after=0){
      stopDebugStream();
      debugAfter=after;
      pollDebugLogs();
      debugTimer=setInterval(pollDebugLogs, 2000);
    }
    async function pollDebugLogs(){
      if(debugBusy || view.value!=='debug' || document.visibilityState!=='visible') return;
      debugBusy=true;
      try{
        const d=await api('/api/lequ/debug/logs?after='+encodeURIComponent(debugAfter));
        for(const item of (d.logs||[])){
          if(!debugLogs.value.some(x=>x.seq===item.seq)) debugLogs.value.push(item);
        }
        debugAfter=Math.max(debugAfter, Number(d.latest_seq)||0);
        if(debugLogs.value.length>120) debugLogs.value.splice(0, debugLogs.value.length-120);
        if(debugExpanded.value.size){
          const keep = new Set(debugLogs.value.map(x=>x.seq));
          const next = new Set([...debugExpanded.value].filter(seq=>keep.has(seq)));
          if(next.size !== debugExpanded.value.size) debugExpanded.value = next;
        }
      }catch{} finally{ debugBusy=false; }
    }
    function stopDebugStream(){
      if(debugTimer){ clearInterval(debugTimer); debugTimer=null; }
      debugBusy=false;
    }

    function setTab(t){ liveTab.value=t; q.value=''; loadList(); }
    function setView(v){
      view.value=v;
      if(v!=='debug') stopDebugStream();
      if(v!=='farm') stopFarmPoll();
      if(v==='live') loadList();
      if(v==='tracked') loadTracked();
      if(v==='recordings') loadRecs();
      if(v==='farm') startFarmPoll();
      if(v==='userinfo' && !userInfo.profile) loadUserInfo(true);
      if(v==='debug') loadDebugLogs();
    }

    // polling
    let timer=null;
    async function boot(){ await load(); if(lequ.logged_in) startStateStream(); if(timer) clearInterval(timer);
      timer = setInterval(()=>{ if(document.visibilityState==='visible') refreshVisibleStats(); }, 30000); }
    function onPlayerKey(e){
      if(!player.open) return;
      const t=e.target;
      if(t && (t.tagName==='INPUT' || t.tagName==='TEXTAREA' || t.isContentEditable)) return;
      if(e.key==='ArrowLeft'){ playAdjacent(-1); e.preventDefault(); }
      else if(e.key==='ArrowRight'){ playAdjacent(1); e.preventDefault(); }
      else if(e.key==='Escape'){ closePlayer(); }
    }
    onMounted(()=>{ if(loggedIn.value) boot(); window.addEventListener('keydown', onPlayerKey); });
    onBeforeUnmount(()=>{ if(timer) clearInterval(timer); stopStateStream(); stopDebugStream(); stopFarmPoll();
      stopComments(); stopHeart(); closePlayer(); window.removeEventListener('keydown', onPlayerKey);
      if(statsObserver) statsObserver.disconnect(); });

    const fmtClock = ts => { if(!ts) return '—'; const d=new Date(ts*1000); const p=n=>String(n).padStart(2,'0');
      return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`; };
    const fmtDateTime = ts => { if(!ts) return '—'; const d=new Date(ts*1000); const p=n=>String(n).padStart(2,'0');
      return `${d.getFullYear()}-${p(d.getMonth()+1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`; };
    const stateLabel = s => ({resolving:'解析中',recording:'录制中',remuxing:'转封装',interrupted:'异常中断',done:'完成',failed:'失败',stopped:'已停止'}[s]||s);
    const fmtMMSS = s => { s=Math.max(0,Math.floor(s||0)); const m=Math.floor(s/60),x=s%60; return m+':'+String(x).padStart(2,'0'); };
    const fmtDuration = s => { s=Math.max(0,Math.floor(s||0)); const h=Math.floor(s/3600),m=Math.floor(s%3600/60),x=s%60;
      return h ? `${h}:${String(m).padStart(2,'0')}:${String(x).padStart(2,'0')}` : `${m}:${String(x).padStart(2,'0')}`; };
    const fmtBytes = n => { n=Number(n)||0; if(n<1024)return n+' B';
      const u=['KB','MB','GB','TB']; let i=-1; do{n/=1024;i++;}while(n>=1024&&i<u.length-1);
      return n.toFixed(n>=100?0:n>=10?1:2)+' '+u[i]; };

    // ── 么么哒 farm (ported into this page) ──
    const farm = reactive({ enabled:false, concurrency:1, mode:'random', targets:[], total_sent:0, workers:[], log:[] });
    const lequSettings = reactive({ watch_priority:'farm', record_heartbeat_enabled:true });
    const farmTargets = ref('');
    const farmEditorMode = ref('random');
    const farmBusy = ref(false);
    const settingsBusy = ref(false);
    const farmActive = computed(()=> (farm.workers||[]).filter(w=>w.state==='watching'||w.state==='sending').length);
    let farmTimer = null;
    async function refreshFarm(){ try{ const d=await api('/api/lequ/farm/status'); Object.assign(farm,d);
      if(document.activeElement?.id!=='ftgt') farmTargets.value=(farm.targets||[]).join(', '); }catch(e){} }
    async function farmConfig(extra={}){
      const targets=farmTargets.value.split(/[\s,，]+/).map(s=>s.trim()).filter(Boolean);
      farmBusy.value=true;
      try{ const d=await api('/api/lequ/farm/config',{method:'POST',body:JSON.stringify({
        enabled:farm.enabled,concurrency:farm.concurrency,mode:farmEditorMode.value,targets,...extra})});
        Object.assign(farm,d); return true;
      }catch(e){ notify(e.message,true); await refreshFarm(); return false; }
      finally{ farmBusy.value=false; }
    }
    async function farmToggle(){
      if(farmBusy.value) return;
      if(!lequ.logged_in){ notify('请先登录乐趣账号',true); return; }
      const next=!farm.enabled;
      if(await farmConfig({enabled:next})) notify(next?'已启动么么哒农场':'已停止么么哒农场');
    }
    function farmConc(n){ if(farmBusy.value)return; farm.concurrency=Math.max(0,Math.min(8,n)); farmConfig(); }
    function farmSetMode(m){ farmEditorMode.value=m; }
    async function farmWorkerAction(w, action){
      if(farmBusy.value) return;
      farmBusy.value=true;
      try{
        const d=await api('/api/lequ/farm/worker/'+encodeURIComponent(w.id), {
          method:'POST', body:JSON.stringify({action})
        });
        Object.assign(farm,d);
        if(document.activeElement?.id!=='ftgt') farmTargets.value=(farm.targets||[]).join(', ');
      }catch(e){ notify(e.message,true); }
      finally{ farmBusy.value=false; }
    }
    function startFarmPoll(){ if(farmTimer) clearInterval(farmTimer); refreshFarm();
      farmTimer=setInterval(()=>{ if(document.visibilityState==='visible' && view.value==='farm') refreshFarm(); }, 3000); }
    function stopFarmPoll(){ if(farmTimer){ clearInterval(farmTimer); farmTimer=null; } }
    async function refreshSettings(){
      try{ const d=await api('/api/lequ/settings'); Object.assign(lequSettings,d); }catch(e){}
    }
    async function setWatchPriority(v){
      if(settingsBusy.value) return;
      settingsBusy.value=true;
      try{
        const d=await api('/api/lequ/settings',{method:'POST',body:JSON.stringify({watch_priority:v})});
        Object.assign(lequSettings,d);
        notify(v==='record'?'已切到录制优先':(v==='ab'?'已切到AB对比':(v==='off'?'已关闭录制心跳':'已切到农场优先')));
        refreshFarm();
      }catch(e){ notify(e.message,true); }
      finally{ settingsBusy.value=false; }
    }

    return { token, loggedIn, lf, doLogin, logout, toast,
      lequ, sms, smsSend, smsComplete, lequLogout,
      view, liveTab, list, loadingList, q, tracked, recs, recHistory, recordingScanBusy, contactBatchBusy,
      recPage, recPages, pagedRecHistory, setRecPage, trackedNames, followBusy, userInfo,
      debugLogs, debugDisplayLogs, debugExpanded, runtimeLogs, runtimeLogBusy, loadRuntimeLogs,
      player, playerVideo, archivePlayer,
      danmaku, comments, flying, commentList, flyEnd, setDmMode, cmtClass, gift,
      heart, heartEarned, heartAvailable, heartReady, heartRemain, heartProgress, setHeartAuto, sendGift, sendHeart,
      playerRecording, togglePlayerFollow, recordPlayer, copySourceUrl, openSourceUrl,
      farm, lequSettings, farmTargets, farmEditorMode, farmBusy, settingsBusy, farmActive,
      farmToggle, farmConc, farmSetMode, setWatchPriority,
      farmWorkerAction, farmConfig, refreshFarm,
      loadList, setTab, setView, track, untrack, toggleFollow, recordNow, stopRec, recordingAsset, mediaSpec, generateContactSheet, generateMissingContactSheets,
      scanInterruptedRecordings, remuxRecording, playRecording, closeRecordingPlayer,
      playLive, playAdjacent, hasAdjacent, closePlayer, load, loadUserInfo, followProfile, trackProfile, loadDebugLogs, clearDebugLogs,
      fmtClock, fmtDateTime, fmtCount, fmtMMSS, fmtDuration, fmtBytes, prettyJson,
      debugPreview, isDebugOpen, toggleDebug, stateLabel };
  },
  template: `
  <!-- LOGIN -->
  <div v-if="!loggedIn" class="login">
    <div class="box">
      <div class="lbrand">tg.<b>lequ</b></div>
      <div class="lsub">乐趣直播 · 自动录制控制台</div>
      <div class="panel"><form @submit.prevent="doLogin">
        <div class="field"><label>用户名</label><input class="input" v-model="lf.user" autocomplete="username" autofocus></div>
        <div class="field"><label>密码</label><input class="input" type="password" v-model="lf.pass" autocomplete="current-password"></div>
        <div class="err">{{ lf.err }}</div>
        <button class="btn primary" type="submit" :disabled="lf.busy" style="width:100%">{{ lf.busy?'…':'登录' }}</button>
      </form></div>
    </div>
  </div>

  <!-- APP -->
  <div v-else class="wrap">
    <div class="bar">
      <span class="brand"><icon n="live" :s="20"/>tg<span class="d">.</span><b>lequ</b></span>
      <span class="heart" :class="{on: lequ.logged_in}"></span>
      <span class="chip" :class="{ok: lequ.logged_in}" v-if="lequ.logged_in"><icon n="check" :s="13"/> LeQu <b>{{ lequ.user_id }}</b></span>
      <span class="chip" v-else>未登录 LeQu</span>
      <span class="spacer"></span>
      <button class="btn ghost sm" @click="load"><icon n="refresh" :s="14"/> 刷新</button>
      <a class="btn ghost sm" href="/record/"><icon n="external" :s="14"/> ctbrec</a>
      <a class="btn ghost sm" href="/upload/"><icon n="external" :s="14"/> 上传</a>
      <button class="btn ghost sm" @click="logout"><icon n="logout" :s="14"/> 退出</button>
    </div>

    <!-- LeQu account band -->
    <div class="panel" v-if="!lequ.logged_in">
      <div class="sectitle"><icon n="phone" :s="13"/> 登录乐趣账号（短信验证码）</div>
      <div class="acct" v-if="sms.stage==='phone'">
        <select class="sel" v-model="sms.country" style="width:90px"><option value="86">+86</option></select>
        <input class="input" v-model="sms.phone" placeholder="手机号" @keyup.enter="smsSend">
        <button class="btn primary" :disabled="sms.busy || !sms.phone" @click="smsSend">{{ sms.busy?'发送中…':'发送验证码' }}</button>
      </div>
      <div class="acct" v-else>
        <input class="input" v-model="sms.code" placeholder="短信验证码" @keyup.enter="smsComplete" autofocus>
        <button class="btn primary" :disabled="sms.busy || !sms.code" @click="smsComplete">{{ sms.busy?'登录中…':'登录' }}</button>
        <button class="btn ghost sm" @click="sms.stage='phone'">返回</button>
      </div>
      <div class="err">{{ sms.err }}</div>
      <div class="hint">登录态保存在服务端，sessionId 不会下发到浏览器。验证码 5 分钟内有效。</div>
    </div>

    <template v-if="lequ.logged_in">
      <div class="tabs">
        <button class="tab" :class="{active:view==='live'}" @click="setView('live')">直播列表</button>
        <button class="tab" :class="{active:view==='tracked'}" @click="setView('tracked')">追踪主播 <span class="count">{{ tracked.length }}</span></button>
        <button class="tab" :class="{active:view==='recordings'}" @click="setView('recordings')">录制 <span class="count">{{ recs.length + recHistory.length }}</span></button>
        <button class="tab" :class="{active:view==='farm'}" @click="setView('farm')">么么哒农场 <span class="count">{{ farm.total_sent }}</span></button>
        <button class="tab" :class="{active:view==='userinfo'}" @click="setView('userinfo')">用户信息</button>
        <button class="tab" :class="{active:view==='debug'}" @click="setView('debug')">请求日志</button>
      </div>

      <!-- LIVE -->
      <div v-if="view==='live'">
        <div class="acct" style="margin-bottom:14px">
          <span class="seg">
            <button :class="{active:liveTab==='hot' && !q}" @click="setTab('hot')">热门</button>
            <button :class="{active:liveTab==='following' && !q}" @click="setTab('following')">关注</button>
            <button :class="{active:liveTab==='recommended' && !q}" @click="setTab('recommended')">推荐</button>
          </span>
          <input class="input" v-model="q" placeholder="搜索 主播ID / 昵称 / vid" @keyup.enter="loadList" style="flex:1">
          <button class="btn sm" @click="loadList"><icon n="search" :s="14"/> 搜索</button>
        </div>
        <div v-if="loadingList" class="grid">
          <div v-for="i in 8" :key="i" class="skel" style="height:230px"></div>
        </div>
        <div v-else-if="list.length" class="grid">
          <div v-for="a in list" :key="a.name+'|'+a.vid" class="card" :data-vid="a.vid">
            <div class="cover">
              <img :src="a.thumb||a.logo" :alt="a.nickname" loading="lazy" referrerpolicy="no-referrer">
              <span v-if="a.living" class="live"><span class="dot"></span>LIVE</span>
              <span v-else class="off">未直播</span>
              <button v-if="a.living" class="btn coverplay" @click="playLive(a)" title="在线播放">
                <icon n="play" :s="18"/>
              </button>
            </div>
            <div class="meta">
              <div class="nm">{{ a.nickname||a.name }}</div>
              <div class="detail">
                <div class="id">ID {{ a.name }}</div>
                <div v-if="a.living" class="viewers" :class="{loading:a.stats_loading}" title="当前房间观看人数">
                  <icon n="eye" :s="13"/> {{ fmtCount(a.watching_count ?? a.watch_count) }}
                </div>
              </div>
            </div>
            <div class="acts">
              <button class="btn xs" :class="{following:a.followed}" :disabled="followBusy[a.name]"
                @click="toggleFollow(a)" :title="a.followed?'取消平台关注':'平台关注主播'">
                <icon :n="a.followed?'check':'heart'" :s="12"/> {{ followBusy[a.name]?'…':(a.followed?'已关注':'关注') }}
              </button>
              <button v-if="trackedNames.has(a.name)" class="btn xs ghost" @click="untrack(a.name)" title="关闭开播自动录制"><icon n="check" :s="12"/> 已自动</button>
              <button v-else class="btn xs" @click="track(a)" title="开播后自动录制"><icon n="star" :s="12"/> 自动录制</button>
              <button class="btn xs rec" :disabled="!a.living" @click="recordNow(a)"><icon n="rec" :s="12"/> 录制</button>
            </div>
          </div>
        </div>
        <div v-else class="empty">{{ q ? '没有匹配的主播（可能未开播）' : '列表为空' }}</div>
      </div>

      <!-- TRACKED -->
      <div v-else-if="view==='tracked'" class="panel">
        <div class="sectitle"><icon n="star" :s="13"/> 追踪列表 <span class="count">{{ tracked.length }}</span> · 开播自动录制</div>
        <div v-if="tracked.length">
          <div v-for="t in tracked" :key="t.name" class="row">
            <div class="main">
              <div class="nm">{{ t.nickname||t.name }}</div>
              <div class="sub">ID {{ t.name }}<span v-if="t.vid"> · vid {{ t.vid }}</span></div>
            </div>
            <span class="badge" :class="t.recording?'rec':'idle'">{{ t.recording ? stateLabel(t.state) : '等待开播' }}</span>
            <button class="btn xs danger" @click="untrack(t.name)"><icon n="x" :s="12"/> 取消</button>
          </div>
        </div>
        <div v-else class="empty">还没有追踪任何主播 · 去“直播列表”里点“追踪”</div>
        <div class="hint" style="margin-top:12px">自动追踪依赖主播出现在 热门/关注/推荐 列表中（轮询间隔约 30 秒）。建议把想录的主播先“关注”。</div>
      </div>

      <!-- RECORDINGS -->
      <div v-else-if="view==='recordings'" class="panel">
        <div class="sectitle"><icon n="rec" :s="13"/> 正在录制 <span class="count">{{ recs.length }}</span></div>
        <div v-if="recs.length">
          <div v-for="r in recs" :key="r.vid" class="row">
            <div class="main">
              <div class="nm">{{ r.nickname||r.name }}</div>
              <div class="sub">ID {{ r.name }} · {{ r.file }}</div>
              <div class="sub">HB {{ r.heartbeat_group||'—' }} · 心跳 {{ r.last_heartbeat_at ? fmtClock(r.last_heartbeat_at) : '—' }}
                · OK {{ r.heartbeat_ok||0 }} · FAIL {{ r.heartbeat_failed||0 }} · SKIP {{ r.heartbeat_skipped||0 }}</div>
            </div>
            <span class="badge mono">{{ fmtDuration(r.duration) }} · {{ fmtBytes(r.size) }}</span>
            <span class="badge rec">{{ stateLabel(r.state) }}</span>
            <button class="btn xs danger" @click="stopRec(r.vid)"><icon n="stop" :s="12"/> 停止</button>
          </div>
        </div>
        <div v-else class="empty">当前没有进行中的录制</div>
        <div class="sectitle" style="margin-top:22px">
          <icon n="image" :s="13"/> 录制历史 <span class="count">{{ recHistory.length }}</span>
          <button class="btn xs" style="margin-left:auto" :disabled="contactBatchBusy" @click="generateMissingContactSheets">
            <icon n="image" :s="13"/> {{ contactBatchBusy?'入队中':'一键生成接触表' }}
          </button>
          <button class="btn xs" :disabled="recordingScanBusy" @click="scanInterruptedRecordings">
            <icon n="refresh" :s="13"/> {{ recordingScanBusy?'扫描中':'扫描异常文件' }}
          </button>
        </div>
        <div v-if="recHistory.length" class="recordgrid">
          <article v-for="r in pagedRecHistory" :key="r.id" class="recorditem">
            <div class="recordthumb">
              <a v-if="r.thumb_url" :href="recordingAsset(r.thumb_url)" target="_blank" title="打开高清接触表">
                <img :src="recordingAsset(r.preview_url||r.thumb_url)" :alt="r.nickname||r.name" loading="lazy" decoding="async">
              </a>
              <div v-else class="nothumb"><icon n="image" :s="28"/></div>
            </div>
            <div class="recordbody">
              <div class="recordname">{{ r.nickname||r.name }}</div>
              <div class="recordfile" :title="r.file">{{ r.file }}</div>
              <div class="recordmeta">
                <span>{{ fmtDateTime(r.started_at) }}</span>
                <span>{{ fmtDuration(r.duration) }}</span>
                <span>{{ fmtBytes(r.size) }}</span>
                <span v-if="mediaSpec(r)">{{ mediaSpec(r) }}</span>
                <span v-if="r.heartbeat_group">HB {{ r.heartbeat_group }}</span>
                <span v-if="r.source_count>1">{{ r.source_count }} 个分片</span>
                <span v-if="r.recoverable" class="badge idle">{{ stateLabel(r.state) }}</span>
              </div>
              <div v-if="r.remux_error" class="err">{{ r.remux_error }}</div>
              <div class="recordactions">
                <a class="btn xs primary" :href="recordingAsset(r.download_url)" download>
                  <icon n="download" :s="13"/> 下载
                </a>
                <button v-if="r.play_url" class="btn xs" @click="playRecording(r)">
                  <icon n="play" :s="13"/> 播放
                </button>
                <button v-if="r.recoverable" class="btn xs primary" :disabled="r.remuxing" @click="remuxRecording(r)">
                  <icon n="refresh" :s="13"/> {{ r.remuxing?'转封装中':'转为 MP4' }}
                </button>
                <button v-else class="btn xs" :disabled="r.thumb_generating" @click="generateContactSheet(r)">
                  <icon n="image" :s="13"/> {{ r.thumb_generating?'生成中':(r.thumb_url?'重新生成':'生成接触表') }}
                </button>
              </div>
            </div>
          </article>
        </div>
        <div v-else class="empty">还没有完成的录制</div>
        <div v-if="recPages>1" class="recordpager">
          <button class="btn xs" :disabled="recPage<=1" @click="setRecPage(recPage-1)">上一页</button>
          <span class="pageinfo">{{ recPage }} / {{ recPages }}</span>
          <button class="btn xs" :disabled="recPage>=recPages" @click="setRecPage(recPage+1)">下一页</button>
        </div>
        <div class="hint" style="margin-top:12px">系统每 5 分钟扫描静止超过 3 分钟的异常 FLV/分片。先手动转为 MP4，再按需生成高清接触表。</div>
      </div>

      <!-- MOMODA FARM -->
      <div v-else-if="view==='farm'">
        <div class="farmtop">
          <section class="farmlead">
            <div class="farmlabel">24 小时观看任务</div>
            <div class="farmheadline">
              <div>
                <h2>{{ farm.enabled ? '农场正在运行' : '么么哒农场' }}</h2>
                <p>worker 自动进入直播间，完成五分钟观看心跳后送出免费么么哒。配置和运行状态会在服务重启后恢复。</p>
              </div>
              <button class="btn" :class="farm.enabled?'danger':'primary'" :disabled="farmBusy" @click="farmToggle">
                <icon :n="farm.enabled?'stop':'play'" :s="15"/>
                {{ farmBusy?'处理中':(farm.enabled?'停止农场':'启动农场') }}
              </button>
            </div>
          </section>
          <section class="farmmetric">
            <div>
              <div class="value">{{ farm.total_sent }}</div>
              <div class="label">累计送出么么哒</div>
            </div>
            <div class="sub"><span>活跃 {{ farmActive }}</span><span>worker {{ farm.workers.length }}</span></div>
          </section>
        </div>

        <div class="panel">
          <div class="sectitle">运行配置</div>
          <div class="farmcontrols">
            <div class="farmcontrol">
              <label>随机 Worker 数量</label>
              <div class="stepper">
                <button :disabled="farmBusy || farm.concurrency<=0" @click="farmConc(farm.concurrency-1)">−</button>
                <span>{{ farm.concurrency }}</span>
                <button :disabled="farmBusy || farm.concurrency>=8" @click="farmConc(farm.concurrency+1)">+</button>
              </div>
            </div>
            <div class="farmcontrol">
              <label>房间来源</label>
              <span class="seg">
                <button :class="{active:farmEditorMode==='random'}" @click="farmSetMode('random')">随机直播</button>
                <button :class="{active:farmEditorMode==='specific'}" @click="farmSetMode('specific')">指定主播</button>
              </span>
            </div>
            <div class="farmcontrol">
              <label>观看心跳策略</label>
              <span class="seg">
                <button :disabled="settingsBusy" :class="{active:lequSettings.watch_priority==='farm'}" @click="setWatchPriority('farm')">农场优先</button>
                <button :disabled="settingsBusy" :class="{active:lequSettings.watch_priority==='record'}" @click="setWatchPriority('record')">录制优先</button>
                <button :disabled="settingsBusy" :class="{active:lequSettings.watch_priority==='ab'}" @click="setWatchPriority('ab')">AB对比</button>
                <button :disabled="settingsBusy" :class="{active:lequSettings.watch_priority==='off'}" @click="setWatchPriority('off')">关闭录制心跳</button>
              </span>
            </div>
            <div v-if="farmEditorMode==='specific'" class="farmcontrol targets">
              <label>主播 ID，多个用逗号分隔</label>
              <input id="ftgt" class="input" v-model="farmTargets" placeholder="46775539, 13145255" @keyup.enter="farmConfig()">
            </div>
            <button v-if="farmEditorMode==='specific'" class="btn farmsave" :disabled="farmBusy" @click="farmConfig()">
              <icon n="check" :s="14"/> 保存目标
            </button>
          </div>
          <div class="hint" style="margin-top:13px">同一个 LeQu 账号的观看心跳疑似只能稳定绑定一个房间。AB对比会让一部分录制打心跳、一部分完全不打，farm 在录制活跃时等待；关闭录制心跳则只靠断线重连。</div>
        </div>

        <div class="panel">
          <div class="sectitle">Worker 状态 <span class="count">{{ farm.workers.length }}</span></div>
          <div v-if="farm.workers.length" class="workergrid">
            <article v-for="w in farm.workers" :key="w.id" class="worker">
              <div class="workerhead">
                <div style="min-width:0">
                  <div class="workername">{{ w.nickname || (w.target ? '指定主播 '+w.target : '正在选择直播间') }}</div>
                  <div class="workerid">{{ w.name ? 'ID '+w.name : (w.target ? '目标 '+w.target : 'RANDOM') }}</div>
                </div>
                <span class="workerstate" :class="w.state">{{ ({picking:'选房',watching:'观看',sending:'送出',idle:'等待',paused:'已暂停'})[w.state]||w.state }}</span>
              </div>
              <div class="workerprogress"><i :style="{width:Math.min(100,(w.watched/Math.max(1,w.need)*100))+'%'}"></i></div>
              <div class="workerfoot">
                <span>{{ fmtMMSS(w.watched) }} / {{ fmtMMSS(w.need) }}</span>
                <span>{{ w.state==='watching' ? '剩余 '+fmtMMSS(w.need-w.watched) : '已送 '+w.sent }}</span>
              </div>
              <div v-if="w.error" class="workererror">{{ w.error }}</div>
              <div class="workeractions">
                <button class="btn xs" :disabled="farmBusy" @click="farmWorkerAction(w,w.paused?'resume':'pause')">
                  <icon :n="w.paused?'play':'pause'" :s="12"/> {{ w.paused?'继续':'暂停' }}
                </button>
                <button class="btn xs danger" :disabled="farmBusy" @click="farmWorkerAction(w,'cancel')">
                  <icon n="x" :s="12"/> 取消
                </button>
              </div>
            </article>
          </div>
          <div v-else class="empty">{{ farm.enabled ? '正在创建 worker' : '农场未运行' }}</div>
        </div>

        <div class="panel">
          <div class="acct" style="margin-bottom:12px">
            <div class="sectitle" style="margin:0">运行日志 <span class="count">{{ (farm.log||[]).length }}</span></div>
            <span class="spacer"></span>
            <button class="btn xs ghost" @click="refreshFarm"><icon n="refresh" :s="13"/> 刷新</button>
          </div>
          <div v-if="(farm.log||[]).length" class="farmlog">
            <div v-for="(l,i) in farm.log" :key="l.at+'|'+i" class="farmlogrow"
              :class="{good:l.text.includes('送出')||l.text.includes('领取'),bad:l.text.includes('失败')}">
              <time>{{ fmtClock(l.at) }}</time><span>{{ l.text }}</span>
            </div>
          </div>
          <div v-else class="empty">暂无运行日志</div>
        </div>
      </div>

      <!-- USER INFO -->
      <div v-else-if="view==='userinfo'" class="panel">
        <div class="acct" style="margin-bottom:16px">
          <input class="input" v-model="userInfo.query" placeholder="输入主播 ID" @keyup.enter="loadUserInfo(false)">
          <button class="btn" :disabled="userInfo.loading || !userInfo.query.trim()" @click="loadUserInfo(false)"><icon n="search" :s="14"/> 查询主播</button>
          <button class="btn ghost" :disabled="userInfo.loading" @click="loadUserInfo(true)">查看自己</button>
        </div>
        <div class="err">{{ userInfo.error }}</div>
        <div v-if="userInfo.loading" class="skel" style="height:150px"></div>
        <div v-else-if="userInfo.profile" class="profile">
          <img class="avatar" :src="userInfo.profile.logo" referrerpolicy="no-referrer">
          <div>
            <div class="pname">{{ userInfo.profile.nickname||userInfo.profile.name }}</div>
            <div class="id">ID {{ userInfo.profile.name }} · {{ userInfo.profile.location||userInfo.profile.country||'未知地区' }}</div>
            <div class="psig">{{ userInfo.profile.signature||'暂无签名' }}</div>
            <div class="facts">
              <span class="badge">关注 {{ fmtCount(userInfo.profile.follow_count) }}</span>
              <span class="badge">粉丝 {{ fmtCount(userInfo.profile.fans_count) }}</span>
              <span class="badge">等级 {{ userInfo.profile.level }}</span>
              <span class="badge">VIP {{ userInfo.profile.vip_level }}</span>
              <span v-if="userInfo.profile.anchor_level" class="badge">主播 {{ userInfo.profile.anchor_level }}</span>
              <span class="badge" :class="userInfo.profile.living?'rec':'idle'">{{ userInfo.profile.living?'直播中':(userInfo.profile.online?'在线':'离线') }}</span>
            </div>
            <div v-if="!userInfo.profile.self" class="facts" style="margin-top:14px">
              <button class="btn sm" :class="{following:userInfo.profile.followed}" :disabled="userInfo.followBusy"
                @click="followProfile" :title="userInfo.profile.followed?'取消平台关注':'平台关注该主播'">
                <icon :n="userInfo.profile.followed?'check':'heart'" :s="14"/>
                {{ userInfo.followBusy?'…':(userInfo.profile.followed?'已关注':'关注') }}
              </button>
              <button v-if="trackedNames.has(userInfo.profile.name)" class="btn sm ghost"
                @click="untrack(userInfo.profile.name)" title="取消开播自动录制">
                <icon n="check" :s="14"/> 已追踪录制
              </button>
              <button v-else class="btn sm" :class="{rec:false}" @click="trackProfile" title="开播后自动录制该主播">
                <icon n="star" :s="14"/> 追踪自动录制
              </button>
            </div>
          </div>
        </div>
      </div>

      <!-- DEBUG LOG -->
      <div v-else-if="view==='debug'" class="logstack">
        <section class="panel">
          <div class="acct" style="margin-bottom:12px">
            <div class="sectitle" style="margin:0">LeQu 运行日志 <span class="count">{{ runtimeLogs.length }}</span></div>
            <span class="spacer"></span>
            <button class="btn sm ghost" :disabled="runtimeLogBusy" @click="loadRuntimeLogs">
              <icon n="refresh" :s="13"/> {{ runtimeLogBusy?'刷新中':'刷新' }}
            </button>
          </div>
          <div class="runlog" v-if="runtimeLogs.length">
            <div v-for="(line,i) in runtimeLogs" :key="i" class="runlogline">{{ line }}</div>
          </div>
          <div v-else class="empty">暂无 LeQu 运行日志</div>
          <div class="hint" style="margin-top:10px">这里只显示 lequ 通道，录制、农场、空闲接触表队列等后台日志不会再混进 upload 的 web 日志栏。</div>
        </section>

        <section class="panel">
          <div class="acct" style="margin-bottom:12px">
            <div class="sectitle" style="margin:0">LeQu 加密请求日志 <span class="count">{{ debugLogs.length }}</span></div>
            <span class="spacer"></span>
            <button class="btn sm ghost" @click="loadDebugLogs"><icon n="refresh" :s="13"/> 刷新历史</button>
            <button class="btn sm ghost" @click="clearDebugLogs">清空页面</button>
          </div>
          <div class="debuglog" v-if="debugLogs.length">
            <div class="debugline" v-for="l in debugDisplayLogs" :key="l.seq">
              <div class="head">
                <span>#{{ l.seq }}</span>
                <span class="path">{{ l.path }}</span>
                <span :class="{bad:!l.transport_ok || l.http_status>=400}">HTTP {{ l.http_status||'ERR' }}</span>
                <span>{{ l.elapsed_ms }}ms</span>
                <span>{{ l.el_ver }}/ECT{{ l.el_ect }}</span>
                <span>REQ {{ l.request_bytes }}B</span>
                <span>RES {{ l.response_bytes }}B</span>
                <button class="linkbtn" @click="toggleDebug(l.seq)">{{ isDebugOpen(l.seq) ? '收起' : '展开' }}</button>
              </div>
              <div class="crypto" v-if="isDebugOpen(l.seq)">
                <div class="crypto-line"><span class="crypto-key">EL-AUTH</span><span class="crypto-value">{{ l.el_auth }}</span></div>
                <div class="crypto-line"><span class="crypto-key">EL-VER</span><span class="crypto-value">{{ l.el_ver }}</span></div>
                <div class="crypto-line"><span class="crypto-key">EL-ECT</span><span class="crypto-value">{{ l.el_ect }}</span></div>
                <div class="crypto-line"><span class="crypto-key">EL-NS</span><span class="crypto-value">{{ l.el_ns }}</span></div>
                <div class="crypto-line"><span class="crypto-key">EL-SIGN</span><span class="crypto-value">{{ l.el_sign }}</span></div>
              </div>
              <pre v-if="isDebugOpen(l.seq)">BACKEND {{ l.backend_ip }}
PLAIN {{ l.request_plain }}
ENCRYPTED {{ l.request_encrypted }}
RESPONSE
{{ prettyJson(l.response_body) }}</pre>
              <pre v-else>BACKEND {{ l.backend_ip }} · {{ debugPreview(l.response_body) }}</pre>
            </div>
          </div>
          <div v-else class="empty">暂无 LeQu 请求记录</div>
          <div class="hint" style="margin-top:10px">完整记录请求明文、密文、EL-AUTH、签名参数、上游节点与原始响应，仅保存在服务内存中，服务重启后清空。</div>
        </section>
      </div>
    </template>
  </div>

  <div v-if="toast.msg" class="toast" :class="{bad:toast.bad}">{{ toast.msg }}</div>
  <div v-if="archivePlayer.open" class="playerback" @click.self="closeRecordingPlayer">
    <div class="playerbox">
      <div class="playerhead">
        <div class="title">{{ archivePlayer.title }}</div>
        <button class="btn xs ghost closeplayer" @click="closeRecordingPlayer" title="关闭"><icon n="x" :s="18"/></button>
      </div>
      <div class="playerstage">
        <video :src="archivePlayer.url" controls autoplay playsinline preload="metadata"></video>
      </div>
    </div>
  </div>
  <div v-if="player.open" class="playerback" @click.self="closePlayer">
    <div class="playerbox">
      <div class="playerhead">
        <div class="heart on"></div>
        <div class="title">{{ player.title }} <span class="mono" style="color:var(--ink-4)">· {{ player.vid }}</span>
          <span v-if="player.mode" class="badge" :class="player.mode==='flv'?'rec':'idle'" style="margin-left:8px;font-size:10px">{{ player.mode==='flv'?'FLV 低延迟':'HLS' }}</span>
          <span v-if="player.mode==='flv'" class="badge idle" style="margin-left:6px;font-size:10px">
            {{ player.speed ? player.speed.toFixed(0)+' KB/s' : '测速中' }} · 缓冲 {{ player.buffer.toFixed(1) }}s
          </span>
        </div>
        <span class="dmseg" title="弹幕显示方式">
          <button :class="{active:danmaku.mode==='danmaku'}" @click="setDmMode('danmaku')"><icon n="danmaku" :s="13"/> 弹幕</button>
          <button :class="{active:danmaku.mode==='list'}" @click="setDmMode('list')"><icon n="msg" :s="13"/> 列表</button>
        </span>
        <button class="btn xs" :class="{following:player.followed}" :disabled="player.followBusy" @click="togglePlayerFollow">
          <icon :n="player.followed?'check':'heart'" :s="13"/> {{ player.followBusy?'…':(player.followed?'已关注':'关注') }}
        </button>
        <button class="btn xs rec" :disabled="player.recordBusy||playerRecording" @click="recordPlayer">
          <icon n="rec" :s="13"/> {{ player.recordBusy?'启动中':(playerRecording?'录制中':'录制') }}
        </button>
        <button class="btn xs" :disabled="player.sourceBusy" @click="copySourceUrl" title="复制 App 同款 RTMP 源流">
          <icon n="copy" :s="13"/> {{ player.sourceBusy?'获取中':'源流' }}
        </button>
        <button class="btn xs" :disabled="player.sourceBusy" @click="openSourceUrl" title="交给 VLC/PotPlayer/IINA 等外部播放器">
          <icon n="external" :s="13"/> 外部
        </button>
        <button class="btn xs gifttrigger" :class="{primary:gift.open}" @click="gift.open=!gift.open" title="送其它礼物"><icon n="gift" :s="14"/> 送礼</button>
        <button class="btn xs ghost closeplayer" @click="closePlayer" title="关闭播放器"><icon n="x" :s="18"/></button>
      </div>
      <div v-if="gift.open" class="giftbox" @click.stop>
        <div class="gt"><icon n="gift" :s="15"/> 送礼物给 {{ player.title }}</div>
        <div class="gr">
          <div style="flex:2"><label>礼物 ID</label><input class="input" v-model="gift.giftId" inputmode="numeric"></div>
          <div style="flex:1"><label>数量</label><input class="input" v-model="gift.giftNum" inputmode="numeric"></div>
        </div>
        <div class="gr">
          <div style="flex:1"><label>来源</label>
            <select class="sel" v-model.number="gift.buyType"><option :value="0">余额购买</option><option :value="1">免费背包</option></select>
          </div>
          <div style="flex:1" v-if="gift.buyType===1"><label>背包 pkId</label><input class="input" v-model="gift.pkId" inputmode="numeric"></div>
        </div>
        <div class="warn">⚠ 余额购买会真实扣除你的账号余额。小心心(免费)请选「免费背包」并填 pkId —— 其 giftId/pkId 需先抓包确认。</div>
        <button class="btn primary" style="width:100%" :disabled="gift.busy" @click="sendGift">{{ gift.busy?'送出中…':'确认送出' }}</button>
      </div>
      <div class="playerstage">
        <video ref="playerVideo" controls autoplay playsinline></video>
        <div v-if="danmaku.mode==='danmaku'" class="dmlayer">
          <div v-for="f in flying" :key="f.key" class="dm" :class="f.cls"
               :style="{ top:f.top, animationDuration:f.dur }" @animationend="flyEnd(f.key)">
            <span v-if="f.type===0" class="who"><span v-if="f.title" class="tt">{{ f.title }}</span>{{ f.nick }}:</span>{{ f.text }}
          </div>
        </div>
        <div v-if="player.loading || player.error" class="playerstate">
          {{ player.error || '正在建立低延迟直播流…' }}
        </div>
        <button v-if="hasAdjacent" class="navarrow left" @click="playAdjacent(-1)" title="上一个直播间 (←)"><icon n="chevL" :s="26"/></button>
        <button v-if="hasAdjacent" class="navarrow right" @click="playAdjacent(1)" title="下一个直播间 (→)"><icon n="chevR" :s="26"/></button>
      </div>
      <div class="watchbar">
        <div class="watchmeta">
          <div class="watchmark"><icon n="heart" :s="18"/></div>
          <div class="watchcopy">
            <div class="watchtitle">
              <strong>{{ heart.giftName }}</strong>
              <span class="mono">{{ heartReady ? '可以送出' : '剩余 '+fmtMMSS(heartRemain) }}</span>
            </div>
            <div class="watchtrack"><i :style="{width:heartProgress+'%'}"></i></div>
          </div>
        </div>
        <div class="watchactions">
          <label class="switch" title="满足观看时间后自动送出">
            <input type="checkbox" :checked="heart.auto" @change="setHeartAuto($event.target.checked)">
            <b></b><span>自动送出</span>
          </label>
          <button class="btn xs" :class="{primary:heartReady}" :disabled="heart.busy || !heartReady" @click="sendHeart(false)">
            <icon n="heart" :s="14"/> {{ heart.busy?'送出中':(heartReady?'立即送出':'观看计时中') }}
          </button>
        </div>
      </div>
      <div v-if="danmaku.mode==='list'" class="cmtlist" ref="commentList">
        <div v-if="!comments.length" class="cmtempty">等待评论…</div>
        <div v-for="c in comments" :key="c.id" class="cmt" :class="cmtClass(c.type)">
          <img v-if="c.type===0 && c.logo" class="ava" :src="c.logo" referrerpolicy="no-referrer" alt="">
          <div class="body">
            <span v-if="c.type===0" class="who"><span v-if="c.title" class="tt">{{ c.title }}</span>{{ c.nickname }}<span class="lv" v-if="c.level">Lv{{ c.level }}</span> </span><span class="txt">{{ c.content }}</span>
          </div>
        </div>
      </div>
    </div>
  </div>
  `
};

createApp(App).mount('#app');
