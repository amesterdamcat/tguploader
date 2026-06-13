
import { createApp, ref, reactive, computed, onMounted, onBeforeUnmount, watch, h } from 'vue';
import '../styles/pages/record.css';

const TOKEN_KEY = 'token';
const getToken = () => localStorage.getItem(TOKEN_KEY) || '';
const dropToken = () => localStorage.removeItem(TOKEN_KEY);
(function adoptUrlToken(){
  const u = new URL(location.href);
  const tok = u.searchParams.get('token');
  if (tok){ localStorage.setItem(TOKEN_KEY, tok); u.searchParams.delete('token'); history.replaceState({}, '', u.pathname + (u.search||'')); }
})();

let onUnauthorized = () => {};
async function api(path, opts = {}){
  const headers = { 'Authorization':'Bearer '+getToken(), ...(opts.body?{'Content-Type':'application/json'}:{}), ...opts.headers };
  const res = await fetch(path, { ...opts, headers });
  if (res.status === 401){ dropToken(); onUnauthorized(); throw new Error('Unauthorized'); }
  if (!res.ok){ let d={}; try{ d=await res.json(); }catch{} throw new Error(d.detail || d.msg || ('HTTP '+res.status)); }
  return res.json();
}
const reduceMotion = matchMedia('(prefers-reduced-motion: reduce)').matches;

// site name from ctbrec model "type": ctbrec.sites.chaturbate.ChaturbateModel → Chaturbate
function siteOf(m){
  if (m && m.type){
    const seg = m.type.split('.');
    const last = seg[seg.length-1] || '';
    const name = last.replace(/Model$/,'');
    if (name) return name;
  }
  if (m && m.url){ try{ return new URL(m.url).hostname.replace(/^www\./,'').split('.')[0]; }catch{} }
  return '?';
}
const fmtGB = b => (b/1e9 >= 1000 ? (b/1e12).toFixed(2)+' TB' : (b/1e9).toFixed(0)+' GB');

const TweenN = {
  props:{ value:{type:Number,default:0}, fmt:{type:Function,default:v=>Math.round(v).toLocaleString()}, duration:{type:Number,default:600} },
  setup(props){
    const disp = ref(props.value); let raf=0, from=props.value, to=props.value, t0=0;
    const ease = x => 1-Math.pow(1-x,3);
    watch(()=>props.value, nv=>{
      if(reduceMotion){ disp.value=nv; return; }
      from=disp.value; to=nv; t0=performance.now(); cancelAnimationFrame(raf);
      const step=now=>{ const k=Math.min(1,(now-t0)/props.duration); disp.value=from+(to-from)*ease(k); if(k<1) raf=requestAnimationFrame(step); };
      raf=requestAnimationFrame(step); });
    onBeforeUnmount(()=>cancelAnimationFrame(raf));
    return ()=> h('span', props.fmt(disp.value));
  }
};

const ICONS = {
  video:'<path d="m16 13 5.223 3.482a.5.5 0 0 0 .777-.416V7.87a.5.5 0 0 0-.752-.432L16 10.5"/><rect x="2" y="6" width="14" height="12" rx="2"/>',
  external:'<path d="M15 3h6v6"/><path d="M10 14 21 3"/><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/>',
  logout:'<path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><path d="m16 17 5-5-5-5"/><path d="M21 12H9"/>',
  stop:'<rect x="6" y="6" width="12" height="12" rx="2"/>',
  play:'<path d="m5 4 14 8-14 8V4z"/>',
  pause:'<rect x="6" y="4" width="4" height="16" rx="1"/><rect x="14" y="4" width="4" height="16" rx="1"/>',
  plus:'<path d="M5 12h14M12 5v14"/>',
  check:'<path d="M20 6 9 17l-5-5"/>',
  x:'<path d="M18 6 6 18M6 6l12 12"/>',
  alert:'<path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3Z"/><path d="M12 9v4"/><path d="M12 17h.01"/>',
  search:'<circle cx="11" cy="11" r="8"/><path d="m21 21-4.3-4.3"/>',
  refresh:'<path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8"/><path d="M21 3v5h-5"/><path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16"/><path d="M8 16H3v5"/>',
  trash:'<path d="M3 6h18"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>',
  disk:'<circle cx="12" cy="12" r="10"/><circle cx="12" cy="12" r="3"/><path d="M12 2v4M12 18v4"/>',
  film:'<rect x="2" y="3" width="20" height="18" rx="2"/><path d="M7 3v18M17 3v18M2 8h5M2 16h5M17 8h5M17 16h5"/>',
  redo:'<path d="M21 7v6h-6"/><path d="M3 17a9 9 0 0 1 9-9 9 9 0 0 1 6 2.3L21 13"/>',
  wand:'<path d="m3 21 9-9"/><path d="M15 4V2M15 10V8M11 6H9M21 6h-2M18.4 3.6l-1.4 1.4M18.4 8.4 17 7"/>',
  activity:'<path d="M22 12h-4l-3 9L9 3l-3 9H2"/>',
};
const Icon = {
  props:{ n:{type:String,required:true}, s:{type:Number,default:16} },
  setup(p){ return ()=> h('svg', { class:'ic', width:p.s, height:p.s, viewBox:'0 0 24 24', fill:'none',
    stroke:'currentColor', 'stroke-width':1.8, 'stroke-linecap':'round', 'stroke-linejoin':'round',
    'aria-hidden':'true', innerHTML: ICONS[p.n]||'' }); }
};

// common ctbrec site names for the add selector
const SITES = ['Chaturbate','Stripchat','MyFreeCams','BongaCams','Cam4','CamSoda','Flirt4Free','Streamate','XloveCam','Showup','Fc2Live'];

const App = {
  components:{ TweenN, Icon },
  setup(){
    const token = ref(getToken());
    const loggedIn = computed(()=> !!token.value);
    onUnauthorized = () => { token.value=''; };

    const lf = reactive({ user:'', pass:'', err:'', busy:false });
    async function doLogin(){
      lf.busy=true; lf.err='';
      try{
        const res = await fetch('/api/auth/login', { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({username:lf.user, password:lf.pass}) });
        const ct = res.headers.get('content-type')||'';
        if(!res.ok || !ct.includes('json')) throw new Error('login failed');
        const d = await res.json();
        if(!d.token) throw new Error(d.detail||'login failed');
        localStorage.setItem(TOKEN_KEY, d.token); token.value=d.token; boot();
      }catch(e){ lf.err = e.message || 'login failed'; }
      finally{ lf.busy=false; }
    }
    function logout(){ dropToken(); token.value=''; stopPoll(); }

    const toast = reactive({ msg:'', bad:false, t:null });
    function notify(msg, bad=false){ toast.msg=msg; toast.bad=bad; clearTimeout(toast.t); toast.t=setTimeout(()=>toast.msg='', 3800); }

    // ---- add by name ----
    const af = reactive({ site:'Chaturbate', name:'', busy:false });
    async function addModel(){
      const raw = af.name.trim();
      if(!raw){ return; }
      // support batch: split on whitespace / comma / newline
      const names = raw.split(/[\s,]+/).map(s=>s.replace(/^@/,'').trim()).filter(Boolean);
      af.busy = true;
      let ok=0, fail=0;
      for(const n of names){
        try{
          const d = await api('/api/rec/add', { method:'POST', body: JSON.stringify({ name:n, site: af.site }) });
          if(d.status === 'success') ok++; else { fail++; }
        }catch(e){ fail++; }
      }
      af.busy=false; af.name='';
      notify(fail ? `已添加 ${ok} 个，失败 ${fail} 个` : (ok===1 ? `已开始录制 ${names[0]} ✓` : `已添加 ${ok} 个 ✓`), fail>0 && ok===0);
      refresh();
    }

    // ---- state ----
    const tracked = ref([]);
    const online  = ref([]);
    const recording = ref([]);
    const space = ref(null);
    // real-time NIC throughput (bytes/sec) from /api/system/net — not ctbrec's
    // tiny disk-write `throughput`, which always rounded to 0.0 MB/s.
    const net = reactive({ tx:0, rx:0 });
    const fmtRate = bps => bps>=1e6 ? (bps/1e6).toFixed(1)+' MB/s' : (bps/1e3).toFixed(0)+' KB/s';
    const loading = ref(true);
    const connected = ref(false);
    const q = ref('');
    const tab = ref('all');   // all | recording | online | paused

    const onlineSet = computed(()=> new Set(online.value.map(m=>m.name)));
    const recSet    = computed(()=> new Set(recording.value.map(m=>m.name)));

    function statusOf(m){
      if(recSet.value.has(m.name)) return 'recording';
      if(m.suspended) return 'paused';
      if(onlineSet.value.has(m.name)) return 'online';
      return 'offline';
    }

    const decorated = computed(()=> tracked.value.map(m=>({ m, name:m.name, site:siteOf(m), status:statusOf(m) })));
    const counts = computed(()=>{
      const c={ all:decorated.value.length, recording:0, online:0, paused:0 };
      for(const d of decorated.value){ if(d.status==='recording')c.recording++; else if(d.status==='online')c.online++; if(d.m.suspended)c.paused++; }
      return c;
    });
    const filtered = computed(()=>{
      const term = q.value.trim().toLowerCase();
      let list = decorated.value;
      if(tab.value==='recording') list = list.filter(d=>d.status==='recording');
      else if(tab.value==='online') list = list.filter(d=>d.status==='online'||d.status==='recording');
      else if(tab.value==='paused') list = list.filter(d=>d.m.suspended);
      if(term) list = list.filter(d=> d.name.toLowerCase().includes(term));
      // sort: recording → online → paused → offline, then name
      const rank = { recording:0, online:1, paused:2, offline:3 };
      return list.slice().sort((a,b)=> (rank[a.status]-rank[b.status]) || a.name.localeCompare(b.name));
    });

    async function refresh(){
      try{
        const [r, o, m, s] = await Promise.all([
          api('/api/rec/recording'), api('/api/rec/online'),
          api('/api/rec/models'),    api('/api/rec/space'),
        ]);
        recording.value = r.models || [];
        online.value    = o.models || [];
        tracked.value   = m.models || [];
        space.value     = s && s.status==='success' ? s : null;
        connected.value = true;
      }catch(e){ connected.value=false; if(e.message!=='Unauthorized') notify(e.message, true); }
      finally{ loading.value=false; }
    }

    // per-model actions (send the full model object ctbrec gave us)
    const busyName = ref('');
    async function act(m, action, label){
      busyName.value = m.name;
      try{
        const d = await api('/api/rec/call', { method:'POST', body: JSON.stringify({ action, model:m }) });
        if(d.status && d.status!=='success') throw new Error(d.msg||'failed');
        notify(`${label} · ${m.name} ✓`);
        await refresh();
      }catch(e){ notify(`${label} 失败: ${e.message}`, true); }
      finally{ busyName.value=''; }
    }
    const start  = m => act(m,'start','开始');
    const pause  = m => act(m,'suspend','暂停');
    const resume = m => act(m,'resume','恢复');
    async function stop(m){
      if(!confirm(`停止并从录制器移除 ${m.name}？\n（已录制的文件不会被删除）`)) return;
      act(m,'stop','移除');
    }

    // ---- recordings browser (server-side paged; defaults to today) ----
    const view = ref('rec');   // 'rec' (recorder) | 'recordings'
    const rf = reactive({ date:'today', q:'', status:'', sort:'date_desc', page:0, page_size:60 });
    const rdata = reactive({ items:[], filtered:0, total:0, counts:{}, loading:false });
    const today = new Date().toLocaleDateString('sv');   // YYYY-MM-DD
    const pages = computed(()=> Math.max(1, Math.ceil(rdata.filtered / rf.page_size)));
    let rqSeq = 0;
    async function loadRecordings(){
      const seq = ++rqSeq; rdata.loading = true;
      try{
        const p = new URLSearchParams({ date:rf.date, q:rf.q, status:rf.status, sort:rf.sort, page:rf.page, page_size:rf.page_size });
        const d = await api('/api/rec/recordings?'+p.toString());
        if(seq !== rqSeq) return;   // a newer request superseded this one
        rdata.items = d.items||[]; rdata.filtered = d.filtered||0; rdata.total = d.total||0; rdata.counts = d.counts||{};
      }catch(e){ if(e.message!=='Unauthorized') notify(e.message, true); }
      finally{ if(seq===rqSeq) rdata.loading=false; }
    }
    let rqTimer=null;
    function reloadRecordings(resetPage=true){ if(resetPage) rf.page=0; clearTimeout(rqTimer); rqTimer=setTimeout(loadRecordings, 220); }
    function gotoPage(p){ rf.page = Math.min(Math.max(0,p), pages.value-1); loadRecordings(); }
    function openRecordings(){ view.value='recordings'; if(!rdata.items.length) loadRecordings(); loadCleanup(); }
    watch(()=>[rf.date, rf.q, rf.status, rf.sort], ()=> reloadRecordings(true));
    async function delRecording(it){
      if(!confirm(`删除录像 ${it.model} · ${it.file}？\n${it.exists?'磁盘文件会被一并删除。':'文件已不在磁盘，仅清除记录。'}`)) return;
      try{
        const d = await api('/api/rec/recording/delete', { method:'POST', body: JSON.stringify({ id: it.id }) });
        if(d.status && d.status!=='success') throw new Error(d.msg||'failed');
        notify(`已删除 ${it.model} ✓`); loadRecordings();
      }catch(e){ notify('删除失败: '+e.message, true); }
    }
    // ---- cleanup: drop records whose disk file is gone (already uploaded) ----
    const clean = reactive({ total:0, on_disk:0, eligible:0, waiting:0, running:false, done:0, failed:0, job_total:0, reprocess:{running:false,done:0,failed:0,job_total:0} });
    let cleanTimer=null;
    const anyJob = () => clean.running || (clean.reprocess && clean.reprocess.running);
    async function loadCleanup(){
      try{ Object.assign(clean, await api('/api/rec/recordings/cleanup')); }catch(e){}
      if(anyJob() && !cleanTimer){ cleanTimer = setInterval(loadCleanup, 1500); }
      if(!anyJob() && cleanTimer){ clearInterval(cleanTimer); cleanTimer=null; loadRecordings(); }
    }
    async function runCleanup(){
      if(clean.running) return;
      if(!confirm(`磁盘对比结果：\n  记录总数 ${clean.total.toLocaleString()}\n  磁盘文件还在（保留）${clean.on_disk.toLocaleString()}\n  文件已不存在（清理）${clean.eligible.toLocaleString()}\n\n仅删除磁盘上文件已不存在的记录（已上传/已清理），不碰文件还在的。确定执行？`)) return;
      try{
        await api('/api/rec/recordings/cleanup', { method:'POST', body: JSON.stringify({ confirm:true }) });
        clean.running = true; notify(`开始清理 ${clean.eligible.toLocaleString()} 条…`);
        loadCleanup();
      }catch(e){ notify('清理失败: '+e.message, true); }
    }
    // re-run post-processing on all WAITING (file present) recordings
    async function runReprocess(){
      if(clean.reprocess && clean.reprocess.running) return;
      if(!confirm(`对 ${clean.waiting.toLocaleString()} 条 WAITING 录像重新跑后处理？\n（这些是已下完原始文件但后处理未完成的，文件仍在磁盘）`)) return;
      try{
        await api('/api/rec/recordings/reprocess', { method:'POST', body: JSON.stringify({}) });
        clean.reprocess.running = true; notify(`开始重跑 ${clean.waiting.toLocaleString()} 条后处理…`);
        loadCleanup();
      }catch(e){ notify('重跑失败: '+e.message, true); }
    }
    // per-row: re-run post-processing on one recording
    async function postprocessOne(it){
      try{
        const d = await api('/api/rec/recording/postprocess', { method:'POST', body: JSON.stringify({ id: it.id }) });
        if(d.status && d.status!=='success') throw new Error(d.msg||'failed');
        notify(`已触发后处理 · ${it.model} ✓`);
      }catch(e){ notify('后处理失败: '+e.message, true); }
    }

    // recording poster thumbnail (proxied + signed by our backend; token in query
    // because it loads as an <img>). Click opens the full poster in a lightbox.
    const lightbox = ref('');
    const thumbSrc = it => '/api/rec/thumb?id=' + encodeURIComponent(it.id) + '&token=' + encodeURIComponent(getToken());

    // formatting helpers
    const fmtSize = b => b>=1e9 ? (b/1e9).toFixed(2)+' GB' : b>=1e6 ? (b/1e6).toFixed(0)+' MB' : (b/1e3).toFixed(0)+' KB';
    const fmtTime = ms => { const d=new Date(ms); const p=n=>String(n).padStart(2,'0'); return `${p(d.getHours())}:${p(d.getMonth()+1>0?d.getMinutes():0)}`; };
    const fmtClock = ms => { const d=new Date(ms); const p=n=>String(n).padStart(2,'0'); return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`; };
    const fmtDate = ms => new Date(ms).toLocaleDateString('sv');
    const siteShort = url => { try{ return new URL(url).hostname.replace(/^www\./,'').split('.')[0]; }catch{ return ''; } };

    // ---- live thumbnails ----
    // The model `preview` is a static CDN url (thumb.live.mmcdn.com/.../<name>.jpg)
    // that the source refreshes server-side. The browser caches it, so we append
    // a changing cache-buster to pull the current frame. Bumped on its own cadence
    // so the picture stays "live" without re-fetching the model lists every time.
    const thumbTick = ref(Date.now());
    const thumbUrl = m => m.preview ? (m.preview + (m.preview.includes('?')?'&':'?') + 't=' + thumbTick.value) : '';

    // real NIC speed — fast cadence so it actually reads "live".
    async function pollNet(){
      try{ const d = await api('/api/system/net'); net.tx = d.latest_tx||0; net.rx = d.latest_rx||0; }
      catch(e){ /* keep last value; net is best-effort */ }
    }

    // ---- polling (only while visible; light cadence) ----
    let timer=null, thumbTimer=null, netTimer=null;
    function startPoll(){
      stopPoll(); refresh(); pollNet();
      timer = setInterval(()=>{ if(document.visibilityState==='visible') refresh(); }, 9000);
      netTimer = setInterval(()=>{ if(document.visibilityState==='visible') pollNet(); }, 2000);
      thumbTimer = setInterval(()=>{ if(document.visibilityState==='visible' && recording.value.length) thumbTick.value = Date.now(); }, 12000);
    }
    function stopPoll(){ if(timer){ clearInterval(timer); timer=null; } if(thumbTimer){ clearInterval(thumbTimer); thumbTimer=null; } if(netTimer){ clearInterval(netTimer); netTimer=null; } }
    function boot(){ loading.value=true; startPoll(); }
    document.addEventListener('visibilitychange', ()=>{ if(document.visibilityState==='visible' && loggedIn.value){ refresh(); pollNet(); thumbTick.value = Date.now(); } });

    onMounted(()=>{
      if(loggedIn.value){
        boot();
        const qs = new URLSearchParams(location.search);
        if(qs.get('view')==='recordings'){
          if(qs.get('date'))   rf.date   = qs.get('date');
          if(qs.get('status')) rf.status = qs.get('status');
          if(qs.get('q'))      rf.q      = qs.get('q');
          if(qs.get('sort'))   rf.sort   = qs.get('sort');
          openRecordings();
        }
      }
    });
    onBeforeUnmount(stopPoll);

    return {
      loggedIn, lf, doLogin, logout, toast,
      af, SITES, addModel,
      tracked, online, recording, space, loading, connected, q, tab,
      counts, filtered, statusOf, siteOf, fmtGB, busyName, thumbUrl,
      net, fmtRate,
      start, pause, resume, stop, refresh, recSet,
      view, rf, rdata, today, pages, gotoPage, openRecordings, delRecording, loadRecordings,
      clean, runCleanup, runReprocess, postprocessOne,
      lightbox, thumbSrc,
      fmtSize, fmtTime, fmtClock, fmtDate, siteShort,
    };
  },
  template: `
  <!-- LOGIN -->
  <div v-if="!loggedIn" class="login">
    <div class="box">
      <div class="lbrand">tg.<b>rec</b></div>
      <div class="lsub">live recorder</div>
      <div class="panel spot"><form class="pbody" @submit.prevent="doLogin">
        <div class="field"><label>username</label><input class="input" v-model="lf.user" autocomplete="username" autofocus></div>
        <div class="field"><label>password</label><input class="input" type="password" v-model="lf.pass" autocomplete="current-password"></div>
        <div class="err" role="alert">{{ lf.err }}</div>
        <button class="btn primary" type="submit" :disabled="lf.busy" style="justify-content:center;">{{ lf.busy?'…':'sign in' }}</button>
      </form></div>
    </div>
  </div>

  <!-- APP -->
  <div v-else class="wrap">
    <div class="bar rise" style="--d:0s">
      <span class="brand"><icon n="video" :s="20"/>tg<span class="d">.</span><b>rec</b></span>
      <span class="heart" :class="{on: connected}"><i></i></span>
      <span class="chip" v-if="space"><icon n="disk" :s="13"/> <b>{{ fmtGB(space.spaceFree) }}</b> free</span>
      <span class="chip net" title="实时网卡吞吐 (下行/上行)"><icon n="activity" :s="13"/> <b>↓ {{ fmtRate(net.rx) }}</b> · <b>↑ {{ fmtRate(net.tx) }}</b></span>
      <span class="viewtog">
        <button :class="{active:view==='rec'}" @click="view='rec'"><icon n="video" :s="13"/> 录制器</button>
        <button :class="{active:view==='recordings'}" @click="openRecordings"><icon n="film" :s="13"/> 录像</button>
      </span>
      <span class="spacer"></span>
      <button class="btn ghost sm" @click="view==='recordings'?loadRecordings():refresh()" aria-label="refresh"><icon n="refresh" :s="14"/> refresh</button>
      <a class="btn ghost sm" href="/upload/"><icon n="external"/> uploader</a>
      <a class="btn ghost sm" href="/backup/"><icon n="external"/> backup</a>
      <button class="btn ghost sm" @click="logout"><icon n="logout"/> logout</button>
    </div>

    <!-- ============ RECORDER VIEW ============ -->
    <template v-if="view==='rec'">
    <!-- ADD band -->
    <div class="panel spot add rise" style="--d:.05s">
      <div class="pbody">
        <div class="add-head">
          <h1>添加 <em>录制</em></h1>
          <span class="k">add model · start recording</span>
        </div>
        <div class="add-row">
          <select class="sel" v-model="af.site" aria-label="site">
            <option v-for="s in SITES" :key="s" :value="s">{{ s }}</option>
          </select>
          <input class="input big" v-model="af.name" placeholder="model 名称（可粘贴多个，空格/逗号/换行分隔）"
                 @keyup.enter="addModel" :disabled="af.busy" aria-label="model name">
          <button class="btn primary" :disabled="af.busy || !af.name.trim()" @click="addModel">
            <icon n="plus" :s="16"/> {{ af.busy ? '添加中…' : '开始录制' }}
          </button>
        </div>
        <div class="add-hint">直接调用本地 ctbrec 的 <b>startByName</b> · 立即加入录制器并开始录制。粘贴多个名字可批量添加。</div>
      </div>
    </div>

    <!-- stats -->
    <div class="stats">
      <div class="stat rec rise" style="--d:.1s"><span class="spark"></span><div class="v"><tween-n :value="counts.recording"/></div><div class="l">正在录制</div></div>
      <div class="stat on rise" style="--d:.14s"><div class="v"><tween-n :value="counts.online"/></div><div class="l">在线</div></div>
      <div class="stat rise" style="--d:.18s"><div class="v"><tween-n :value="counts.all"/></div><div class="l">跟踪中</div></div>
      <div class="stat rise" style="--d:.22s"><div class="v">{{ space ? fmtGB(space.spaceFree) : '—' }}</div><div class="l">剩余空间</div></div>
    </div>

    <!-- main grid -->
    <div class="grid">
      <!-- LIVE -->
      <div class="panel spot col rise" style="--d:.16s">
        <div class="phead"><span class="t">正在录制</span><span class="sub">live · {{ recording.length }}</span></div>
        <div class="pbody">
          <div v-if="recording.length" class="lives">
            <div v-for="m in recording" :key="m.url||m.name" class="live">
              <img :src="thumbUrl(m)" :alt="m.name" loading="lazy" referrerpolicy="no-referrer"
                   @error="$event.target.style.visibility='hidden'"
                   @load="$event.target.style.visibility='visible'">
              <div class="grad"></div>
              <span class="recdot"><i></i>REC</span>
              <div class="stopx"><button @click="stop(m)" :title="'停止 '+m.name" aria-label="stop"><icon n="stop" :s="14"/></button></div>
              <div class="meta"><div class="nm">{{ m.name }}</div><div class="st">{{ siteOf(m) }}</div></div>
            </div>
          </div>
          <div v-else class="empty">当前没有正在录制的 model</div>
        </div>
      </div>

      <!-- TRACKED -->
      <div class="panel spot col rise" style="--d:.2s">
        <div class="phead"><span class="t">录制器</span><span class="sub">{{ counts.all }} tracked</span></div>
        <div class="pbody">
          <div class="toolbar">
            <span class="search-wrap"><icon n="search" :s="14"/><input class="input" v-model="q" placeholder="搜索 model…" aria-label="search"></span>
          </div>
          <div class="tabs" role="tablist">
            <button class="seg" :class="{active:tab==='all'}" @click="tab='all'">全部 <b>{{ counts.all }}</b></button>
            <button class="seg" :class="{active:tab==='recording'}" @click="tab='recording'">录制 <b>{{ counts.recording }}</b></button>
            <button class="seg" :class="{active:tab==='online'}" @click="tab='online'">在线 <b>{{ counts.online }}</b></button>
            <button class="seg" :class="{active:tab==='paused'}" @click="tab='paused'">暂停 <b>{{ counts.paused }}</b></button>
          </div>
          <div class="mlist">
            <template v-if="loading">
              <div v-for="i in 7" :key="'s'+i" class="skel"></div>
            </template>
            <template v-else>
              <div v-for="d in filtered" :key="d.m.url||d.name" class="mrow">
                <span class="dot" :class="d.status" :title="d.status"></span>
                <div class="m-main">
                  <div class="nm">{{ d.name }}</div>
                  <div class="sub"><span class="site">{{ d.site }}</span><span>{{ d.status }}</span></div>
                </div>
                <div class="m-act">
                  <button v-if="d.status!=='recording'" class="btn xs" :disabled="busyName===d.name" @click="start(d.m)" title="开始录制"><icon n="play" :s="12"/></button>
                  <button v-if="d.m.suspended" class="btn xs" :disabled="busyName===d.name" @click="resume(d.m)" title="恢复"><icon n="play" :s="12"/> 恢复</button>
                  <button v-else class="btn xs" :disabled="busyName===d.name" @click="pause(d.m)" title="暂停"><icon n="pause" :s="12"/></button>
                  <button class="btn xs danger" :disabled="busyName===d.name" @click="stop(d.m)" title="移除"><icon n="trash" :s="12"/></button>
                </div>
              </div>
              <div v-if="filtered.length===0" class="empty">{{ tracked.length ? '无匹配' : '录制器里还没有 model' }}</div>
            </template>
          </div>
        </div>
      </div>
    </div>
    </template>

    <!-- ============ RECORDINGS VIEW ============ -->
    <template v-else>
    <div class="panel spot rise" style="--d:.05s">
      <div class="phead">
        <span class="t">录像</span>
        <span class="sub">{{ rdata.filtered.toLocaleString() }} / {{ rdata.total.toLocaleString() }} total</span>
        <span class="spacer"></span>
        <span class="sub" v-if="clean.reprocess && clean.reprocess.running" style="color:var(--acc)">后处理中 {{ clean.reprocess.done.toLocaleString() }}/{{ clean.reprocess.job_total.toLocaleString() }}<span v-if="clean.reprocess.failed"> · 失败 {{ clean.reprocess.failed }}</span></span>
        <button v-else-if="clean.waiting>0" class="btn sm" @click="runReprocess" :title="clean.waiting+' 条 WAITING 重跑后处理'">
          <icon n="wand" :s="13"/> 重跑后处理 ({{ clean.waiting.toLocaleString() }})
        </button>
        <span class="sub" v-if="clean.running" style="color:var(--amber)">清理中 {{ clean.done.toLocaleString() }}/{{ clean.job_total.toLocaleString() }}<span v-if="clean.failed"> · 失败 {{ clean.failed }}</span></span>
        <button v-else-if="clean.eligible>0" class="btn sm danger" @click="runCleanup" :title="'磁盘文件已不存在的记录：'+clean.eligible+' 条'">
          <icon n="trash" :s="13"/> 清理已上传 ({{ clean.eligible.toLocaleString() }})
        </button>
        <span class="sub" v-if="clean.eligible===0 && clean.waiting===0 && !clean.running && !(clean.reprocess&&clean.reprocess.running) && rdata.counts.RECORDING">{{ rdata.counts.RECORDING }} 录制中 · {{ rdata.counts.FINISHED||0 }} 完成</span>
      </div>
      <div class="pbody">
        <div class="rtoolbar">
          <div class="datepick">
            <button class="seg" :class="{active:rf.date==='today'}" @click="rf.date='today'">今天</button>
            <button class="seg" :class="{active:rf.date==='all'}" @click="rf.date='all'">全部</button>
            <input class="input dt" type="date" :max="today" :value="rf.date!=='today'&&rf.date!=='all'?rf.date:''"
                   @change="rf.date=$event.target.value||'today'" aria-label="pick date">
          </div>
          <span class="search-wrap"><icon n="search" :s="14"/><input class="input" v-model="rf.q" placeholder="搜索 model…" aria-label="search recordings"></span>
          <select class="sel auto" v-model="rf.status" aria-label="status">
            <option value="">全部状态</option>
            <option value="RECORDING">录制中</option>
            <option value="FINISHED">已完成</option>
            <option value="WAITING">等待</option>
            <option value="POST_PROCESSING">后处理</option>
            <option value="FAILED">失败</option>
          </select>
          <select class="sel auto" v-model="rf.sort" aria-label="sort">
            <option value="date_desc">最新优先</option>
            <option value="date_asc">最早优先</option>
            <option value="size_desc">大小↓</option>
            <option value="size_asc">大小↑</option>
          </select>
        </div>

        <div class="rtable">
          <div class="rhead">
            <span>model</span><span>开始</span><span class="ar">分辨率</span><span class="ar">大小</span><span>状态</span><span class="ar">操作</span>
          </div>
          <template v-if="rdata.loading && !rdata.items.length">
            <div v-for="i in 12" :key="'rs'+i" class="skel" style="height:46px;margin-bottom:6px;"></div>
          </template>
          <template v-else>
            <div v-for="it in rdata.items" :key="it.id" class="rrow" :class="{gone: !it.exists}">
              <div class="rmodel">
                <span class="dot" :class="(it.status||'').toLowerCase()"></span>
                <img v-if="it.exists" class="rthumb" :src="thumbSrc(it)" loading="lazy" decoding="async"
                     @click="lightbox=thumbSrc(it)" @error="$event.target.style.visibility='hidden'" alt="">
                <div class="rm-main"><div class="nm">{{ it.model }}</div><div class="fn" :title="it.file">{{ it.file }}</div></div>
              </div>
              <div class="rt mono">{{ rf.date==='today' ? fmtClock(it.start) : fmtDate(it.start)+' '+fmtClock(it.start) }}</div>
              <div class="ar mono">{{ it.res>0 ? it.res+'p' : '—' }}</div>
              <div class="ar mono">{{ fmtSize(it.size) }}</div>
              <div><span class="rbadge" :class="(it.status||'').toLowerCase()">{{ it.status }}</span><span v-if="!it.exists" class="rbadge gone" title="磁盘文件已不存在">已清理</span></div>
              <div class="ar racts">
                <button v-if="it.exists" class="btn xs" @click="postprocessOne(it)" title="重跑后处理"><icon n="wand" :s="12"/></button>
                <button class="btn xs danger" @click="delRecording(it)" title="删除"><icon n="trash" :s="12"/></button>
              </div>
            </div>
            <div v-if="!rdata.items.length" class="empty">{{ rf.date==='today' ? '今天还没有录像' : '没有匹配的录像' }}</div>
          </template>
        </div>

        <div class="pager" v-if="rdata.filtered > rf.page_size">
          <button class="btn sm ghost" :disabled="rf.page<=0" @click="gotoPage(rf.page-1)">‹ 上一页</button>
          <span class="mono">第 {{ rf.page+1 }} / {{ pages }} 页</span>
          <button class="btn sm ghost" :disabled="rf.page>=pages-1" @click="gotoPage(rf.page+1)">下一页 ›</button>
        </div>
      </div>
    </div>
    </template>
  </div>

  <div v-if="lightbox" class="lbox" @click="lightbox=''" role="dialog" aria-label="poster">
    <img :src="lightbox" alt="poster">
    <button class="lbox-x" @click.stop="lightbox=''" aria-label="close"><icon n="x" :s="20"/></button>
  </div>

  <div v-if="toast.msg" class="toast" :class="{bad: toast.bad}" role="status" aria-live="polite">
    <icon :n="toast.bad?'alert':'check'" :s="15"/> {{ toast.msg }}
  </div>
  `
};

createApp(App).mount('#app');
