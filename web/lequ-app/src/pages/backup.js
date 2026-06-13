
import { createApp, ref, reactive, computed, onMounted, onBeforeUnmount, watch, h, nextTick } from 'vue';
import '../styles/pages/backup.css';

// Shared with the channel-link visualization (canvas) at the bottom of this module.
const linkState = { done: 0, total: 0, running: false };
let onBackupCopy = () => {};

const TOKEN_KEY = 'token';
const getToken = () => localStorage.getItem(TOKEN_KEY) || '';
const dropToken = () => localStorage.removeItem(TOKEN_KEY);

// Allow arriving from the uploader page with ?token=... (shared session).
(function adoptUrlToken(){
  const u = new URL(location.href);
  let tok = u.searchParams.get('token');
  if (tok){ localStorage.setItem(TOKEN_KEY, tok); u.searchParams.delete('token'); history.replaceState({}, '', u.pathname + (u.search||'')); }
})();

let onUnauthorized = () => {};
async function api(path, opts = {}){
  const headers = { 'Authorization':'Bearer '+getToken(), ...(opts.body?{'Content-Type':'application/json'}:{}), ...opts.headers };
  const res = await fetch(path, { ...opts, headers });
  if (res.status === 401){ dropToken(); onUnauthorized(); throw new Error('Unauthorized'); }
  if (!res.ok){ let d={}; try{ d=await res.json(); }catch{} throw new Error(d.detail || ('HTTP '+res.status)); }
  return res.json();
}
const stripAnsi = s => s.replace(/\x1b\[[0-9;]*m/g, '');
const reduceMotion = matchMedia('(prefers-reduced-motion: reduce)').matches;

// Smooth integer tween component (snaps instantly when reduced-motion is on).
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

// Inline SVG icon set (Lucide-style, single consistent 1.8 stroke). No emoji.
const ICONS = {
  settings:'<path d="M20 7h-9"/><path d="M14 17H5"/><circle cx="17" cy="17" r="3"/><circle cx="7" cy="7" r="3"/>',
  external:'<path d="M15 3h6v6"/><path d="M10 14 21 3"/><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/>',
  logout:'<path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><path d="m16 17 5-5-5-5"/><path d="M21 12H9"/>',
  stop:'<rect x="6" y="6" width="12" height="12" rx="2"/>',
  check:'<path d="M20 6 9 17l-5-5"/>',
  x:'<path d="M18 6 6 18M6 6l12 12"/>',
  alert:'<path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3Z"/><path d="M12 9v4"/><path d="M12 17h.01"/>',
  search:'<circle cx="11" cy="11" r="8"/><path d="m21 21-4.3-4.3"/>',
  shield:'<path d="M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z"/><path d="m9 12 2 2 4-4"/>',
  play:'<path d="m5 4 14 8-14 8V4z"/>',
  refresh:'<path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8"/><path d="M21 3v5h-5"/><path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16"/><path d="M8 16H3v5"/>',
  layers:'<path d="m12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 0l8.58-3.9a1 1 0 0 0 0-1.83Z"/><path d="m22 17.65-9.17 4.16a2 2 0 0 1-1.66 0L2 17.65"/><path d="m22 12.65-9.17 4.16a2 2 0 0 1-1.66 0L2 12.65"/>',
};
const Icon = {
  props:{ n:{type:String,required:true}, s:{type:Number,default:16} },
  setup(p){ return ()=> h('svg', { class:'ic', width:p.s, height:p.s, viewBox:'0 0 24 24', fill:'none',
    stroke:'currentColor', 'stroke-width':1.8, 'stroke-linecap':'round', 'stroke-linejoin':'round',
    'aria-hidden':'true', innerHTML: ICONS[p.n]||'' }); }
};

const App = {
  components:{ TweenN, Icon },
  setup(){
    const token = ref(getToken());
    const loggedIn = computed(()=> !!token.value);
    onUnauthorized = () => { token.value=''; };

    // login
    const lf = reactive({ user:'', pass:'', err:'', busy:false });
    async function doLogin(){
      lf.busy=true; lf.err='';
      try{
        const res = await fetch('/api/auth/login', { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({username:lf.user, password:lf.pass}) });
        const ct = res.headers.get('content-type')||'';
        if(!res.ok || !ct.includes('json')){ throw new Error('login failed'); }
        const d = await res.json();
        if(!d.token) throw new Error(d.detail||'login failed');
        localStorage.setItem(TOKEN_KEY, d.token); token.value=d.token;
        boot();
      }catch(e){ lf.err = e.message || 'login failed'; }
      finally{ lf.busy=false; }
    }
    function logout(){ dropToken(); token.value=''; if(es){es.close(); es=null;} }

    const toast = reactive({ msg:'', bad:false, t:null });
    function notify(msg, bad=false){ toast.msg=msg; toast.bad=bad; clearTimeout(toast.t); toast.t=setTimeout(()=>toast.msg='', 3500); }

    // config
    const cfg = reactive({ api_url:'', bot_name:'', source_channel_id:'', backup_channel_id:'', delay_ms:1500, has_token:false, configured:false });
    const cfgForm = reactive({ bot_name:'', bot_token:'', source_channel_id:'', backup_channel_id:'', delay_ms:1500, saving:false });
    const verify = ref(null);
    const showConfig = ref(false);
    async function loadConfig(){
      const d = await api('/api/backup/config');
      Object.assign(cfg, d);
      cfgForm.bot_name = d.bot_name||''; cfgForm.source_channel_id = d.source_channel_id||'';
      cfgForm.backup_channel_id = d.backup_channel_id||''; cfgForm.delay_ms = d.delay_ms||1500;
      cfgForm.bot_token = '';
    }
    async function saveConfig(){
      cfgForm.saving=true; verify.value=null;
      try{
        const body = { bot_name:cfgForm.bot_name, source_channel_id:cfgForm.source_channel_id, backup_channel_id:cfgForm.backup_channel_id, delay_ms: Number(cfgForm.delay_ms)||1500 };
        if(cfgForm.bot_token.trim()) body.bot_token = cfgForm.bot_token.trim();
        const d = await api('/api/backup/config', { method:'POST', body: JSON.stringify(body) });
        Object.assign(cfg, d.config); verify.value = d.verify; cfgForm.bot_token='';
        notify(d.verify && d.verify.ok ? '已保存，校验通过 ✓' : '已保存，但校验有问题，见下方', !(d.verify&&d.verify.ok));
        loadModels();
      }catch(e){ notify(e.message, true); }
      finally{ cfgForm.saving=false; }
    }
    async function reverify(){
      try{ verify.value = await api('/api/backup/verify'); }catch(e){ notify(e.message, true); }
    }

    // TDLib account used by the pre-backup full scan
    const tdAccounts = ref([]);
    const tdForm = reactive({ name:'main', phone:'', api_id:'', api_hash:'', channel_id:'', saving:false });
    const tdLogin = reactive({ session_id:'', state:'idle', code:'', password:'', busy:false, error:'' });
    const tdScan = reactive({ running:false, fetch_thumbnails:false });
    const tdAccountOptions = computed(()=> tdAccounts.value.map(a => a.name));
    function pickTdAccount(name){
      const acc = tdAccounts.value.find(a => a.name === name);
      if(!acc) return;
      tdForm.name = acc.name || tdForm.name;
      tdForm.phone = acc.phone || '';
      tdForm.api_id = acc.api_id || '';
      tdForm.channel_id = acc.channel_id || cfg.source_channel_id || '';
      tdForm.api_hash = acc.api_hash || '';
      tdLogin.state = acc.session_dir ? 'saved' : 'idle';
      tdLogin.error = '';
    }
    async function loadTdAccounts(){
      try{
        const d = await api('/api/td/accounts');
        tdAccounts.value = d.accounts || [];
        const first = tdAccounts.value[0];
        if(first){
          pickTdAccount(tdForm.name && tdAccounts.value.some(a=>a.name===tdForm.name) ? tdForm.name : first.name);
        }else if(cfg.source_channel_id && !tdForm.channel_id){
          tdForm.channel_id = cfg.source_channel_id;
        }
      }catch(e){ notify(e.message, true); }
    }
    async function saveTdAccount(){
      tdForm.saving = true;
      try{
        const body = { name:tdForm.name.trim(), phone:tdForm.phone.trim(), api_id:tdForm.api_id, api_hash:tdForm.api_hash.trim(), channel_id:tdForm.channel_id.trim() };
        const d = await api('/api/td/accounts', { method:'POST', body: JSON.stringify(body) });
        tdAccounts.value = d.accounts || [];
        notify('真实账号配置已保存');
      }catch(e){ notify(e.message, true); }
      finally{ tdForm.saving = false; }
    }
    function applyTdLogin(d){
      tdLogin.session_id = d.session_id || tdLogin.session_id || '';
      tdLogin.state = d.state || 'error';
      tdLogin.error = d.error || '';
      if(d.authorized || d.state === 'ready'){
        tdLogin.code=''; tdLogin.password=''; tdLogin.session_id='';
        notify('真实账号登录成功，backup 前 scan 可用 ✓');
      }else if(tdLogin.error){
        notify(tdLogin.error, true);
      }
    }
    async function startTdLogin(){
      tdLogin.busy=true; tdLogin.error='';
      try{
        const d = await api('/api/td/login/start', { method:'POST', body: JSON.stringify({ name:tdForm.name.trim() }) });
        applyTdLogin(d);
      }catch(e){ tdLogin.error=e.message; notify(e.message, true); }
      finally{ tdLogin.busy=false; }
    }
    async function submitTdCode(){
      tdLogin.busy=true; tdLogin.error='';
      try{
        const d = await api('/api/td/login/code', { method:'POST', body: JSON.stringify({ session_id:tdLogin.session_id, code:tdLogin.code.trim() }) });
        applyTdLogin(d);
      }catch(e){ tdLogin.error=e.message; notify(e.message, true); }
      finally{ tdLogin.busy=false; }
    }
    async function submitTdPassword(){
      tdLogin.busy=true; tdLogin.error='';
      try{
        const d = await api('/api/td/login/password', { method:'POST', body: JSON.stringify({ session_id:tdLogin.session_id, password:tdLogin.password }) });
        applyTdLogin(d);
      }catch(e){ tdLogin.error=e.message; notify(e.message, true); }
      finally{ tdLogin.busy=false; }
    }
    async function startTdScan(){
      tdScan.running = true;
      try{
        await api('/api/td/scan', { method:'POST', body: JSON.stringify({ name:tdForm.name.trim(), fetch_thumbnails: !!tdScan.fetch_thumbnails }) });
        notify(tdScan.fetch_thumbnails ? '已开始 full scan（含缩略图），日志在下方 Log' : '已开始 full scan，日志在下方 Log');
      }catch(e){ notify(e.message, true); tdScan.running = false; }
    }

    // models
    const models = ref([]);
    const loadingModels = ref(true);
    const sel = reactive(new Set());
    const q = ref('');
    const coverageFilter = ref('pending');
    const modelCounts = computed(()=>({
      all: models.value.length,
      pending: models.value.filter(m => Math.max(0, m.total - m.backed_up) > 0).length,
      complete: models.value.filter(m => m.total > 0 && m.backed_up >= m.total).length,
    }));
    const filtered = computed(()=>{
      const s=q.value.trim().toLowerCase();
      return models.value.filter(m => {
        const pending = Math.max(0, m.total - m.backed_up);
        if (coverageFilter.value === 'pending' && pending <= 0) return false;
        if (coverageFilter.value === 'complete' && !(m.total > 0 && m.backed_up >= m.total)) return false;
        return s ? m.model.toLowerCase().includes(s) : true;
      });
    });
    const selCount = computed(()=> sel.size);
    const selTotal = computed(()=> models.value.filter(m=>sel.has(m.model)).reduce((a,m)=>a+(m.total-m.backed_up),0));
    async function loadModels(){
      try{ const d = await api('/api/backup/models'); models.value = d; }
      catch(e){ /* unauth handled */ }
      finally{ loadingModels.value = false; }
    }
    function toggle(m){ if(sel.has(m.model)) sel.delete(m.model); else sel.add(m.model); }
    function selectAll(){ filtered.value.forEach(m=> sel.add(m.model)); }
    function clearSel(){ sel.clear(); }

    // status / progress
    const st = reactive({ running:false, total:0, done:0, skipped:0, failed:0, current_model:'', scope:'', started_at:0, finished_at:0, last_error:'', cancel_requested:false });
    let statusTimer=null, lastDone=0, lastTs=0, rate=ref(0);
    const processed = computed(()=> st.done + st.skipped);
    const pct = computed(()=> st.total>0 ? Math.min(100, processed.value/st.total*100) : 0);
    const etaStr = computed(()=>{ if(!st.running || rate.value<=0) return '—'; const rem=st.total-processed.value; if(rem<=0) return '—'; const secs=rem/rate.value; return fmtDur(secs); });
    function fmtDur(s){ s=Math.max(0,Math.floor(s)); if(s<60) return s+'s'; if(s<3600) return Math.floor(s/60)+'m '+(s%60)+'s'; const h=Math.floor(s/3600); return h+'h '+Math.floor((s%3600)/60)+'m'; }
    function applyBackupStatus(d){
      if(!d) return;
      const now = Date.now()/1000;
      if(lastTs && d.done>lastDone){ const dr=(d.done-lastDone)/(now-lastTs); rate.value = rate.value? rate.value*0.6+dr*0.4 : dr; }
      lastDone=d.done; lastTs=now;
      if(!d.running) tdScan.running = false;
      Object.assign(st, d);
      linkState.done = d.done||0; linkState.total = d.total||0; linkState.running = !!d.running;
    }
    // Initial fetch; live updates arrive over SSE (`event: backup_status`).
    async function loadStatus(){
      try{ applyBackupStatus(await api('/api/backup/status')); }catch(e){}
    }

    async function startBackup(scope){
      try{
        if(scope==='all'){ if(!confirm('全量备份：把【所有】model 的视频复制到备份频道。数据量很大、受 Telegram 限流，会很慢且可断点续传。确定开始？')) return; }
        const body = scope==='all' ? {scope:'all'} : { scope:'models', models:[...sel] };
        await api('/api/backup/start', { method:'POST', body: JSON.stringify(body) });
        notify('备份已启动'); rate.value=0; lastDone=0; lastTs=0;
        setTimeout(loadStatus, 400);
      }catch(e){ notify(e.message, true); }
    }
    async function stopBackup(){
      try{ await api('/api/backup/stop', { method:'POST' }); notify('已请求停止（处理完当前条后退出）'); }
      catch(e){ notify(e.message, true); }
    }

    // log terminal (SSE)
    const lines = ref([]);
    const connected = ref(false);
    const autoscroll = ref(true);
    let es=null, termEl=null, _lid=0;
    const MAX = 1500;
    function parseLine(raw){
      raw = stripAnsi(raw);
      let ts='', body=raw;
      const tm = raw.match(/^(\d{2}:\d{2}:\d{2}(?:\.\d+)?)\s+([\s\S]*)$/);
      if(tm){ ts=tm[1]; body=tm[2]; }
      let tag='', msg=body;
      const gm = body.match(/^\[([A-Za-z0-9_\-]+)\]\s*([\s\S]*)$/);
      if(gm){ tag=gm[1].toUpperCase(); msg=gm[2]; }
      let cls=''; if(/✓/.test(msg)) cls='ok'; else if(/✗/.test(msg)||tag==='ERROR') cls='bad';
      return { id:++_lid, ts, tag, msg, cls };
    }
    function pushLine(raw){
      const ln = parseLine(raw);
      lines.value.push(ln);
      if(ln.cls==='ok') onBackupCopy();   // a successful copy → fly a light packet across the link
      if(lines.value.length>MAX) lines.value.splice(0, lines.value.length-MAX);
      if(autoscroll.value) nextTick(()=>{ if(termEl) termEl.scrollTop = termEl.scrollHeight; });
    }
    function connectSSE(){
      if(es) es.close();
      es = new EventSource('/api/job/stream?channel=backup&token='+encodeURIComponent(getToken()));
      es.addEventListener('ready', ()=> connected.value=true);
      es.addEventListener('replay.backup', e=> pushLine(e.data));
      es.addEventListener('log.backup', e=>{ connected.value=true; pushLine(e.data); });
      es.addEventListener('backup_status', e=>{ connected.value=true; try{ applyBackupStatus(JSON.parse(e.data)); }catch{} });
      es.onerror = ()=>{ connected.value=false; };
    }
    function onTermScroll(){ if(!termEl) return; autoscroll.value = (termEl.scrollHeight - termEl.scrollTop - termEl.clientHeight) < 40; }
    function clearLog(){ lines.value=[]; }

    function boot(){
      loadConfig().then(loadTdAccounts); loadModels(); loadStatus(); connectSSE();
    }
    onMounted(()=>{ if(loggedIn.value) boot(); });
    onBeforeUnmount(()=>{ if(es) es.close(); if(statusTimer) clearInterval(statusTimer); });
    // grab terminal element after render
    watch(loggedIn, v=>{ if(v) nextTick(()=>{ termEl=document.getElementById('term'); }); });
    onMounted(()=> nextTick(()=>{ termEl=document.getElementById('term'); }));

    return { token, loggedIn, lf, doLogin, logout, toast,
      cfg, cfgForm, verify, showConfig, saveConfig, reverify,
      tdAccounts, tdAccountOptions, tdForm, tdLogin, tdScan, loadTdAccounts, pickTdAccount, saveTdAccount, startTdLogin, submitTdCode, submitTdPassword, startTdScan,
      models, loadingModels, filtered, coverageFilter, modelCounts, sel, selCount, selTotal, q, toggle, selectAll, clearSel, loadModels,
      st, processed, pct, rate, etaStr, startBackup, stopBackup,
      lines, connected, autoscroll, onTermScroll, clearLog };
  },
  template: `
<div>
  <!-- LOGIN -->
  <div v-if="!loggedIn" class="login">
    <div class="box">
      <div class="lbrand">tg.<b>backup</b></div>
      <div class="lsub">channel mirror</div>
      <div class="panel spot"><div class="pbody">
        <div class="field"><label>username</label><input class="input" v-model="lf.user" @keyup.enter="doLogin" autofocus></div>
        <div class="field"><label>password</label><input class="input" type="password" v-model="lf.pass" @keyup.enter="doLogin"></div>
        <div class="err" role="alert">{{ lf.err }}</div>
        <button class="btn primary" :disabled="lf.busy" @click="doLogin" style="justify-content:center;">{{ lf.busy?'…':'sign in' }}</button>
      </div></div>
    </div>
  </div>

  <!-- APP -->
  <div v-else class="wrap">
    <div class="bar rise" style="--d:0s">
      <span class="brand"><icon n="shield" :s="19"/>tg<span class="d">.</span><b>backup</b></span>
      <span class="heart" :class="{on: connected}"><i></i></span>
      <span class="route">
        <span class="ch">{{ cfg.source_channel_id || 'source?' }}</span>
        <span class="arrow">→</span>
        <span class="ch">{{ cfg.backup_channel_id || 'backup?' }}</span>
      </span>
      <span class="spacer"></span>
      <button class="btn ghost sm" @click="showConfig=!showConfig" aria-label="toggle config"><icon n="settings"/> config</button>
      <a class="btn ghost sm" href="/upload/" aria-label="open uploader"><icon n="external"/> uploader</a>
      <button class="btn ghost sm" @click="logout" aria-label="sign out"><icon n="logout"/> logout</button>
    </div>

    <!-- CONFIG -->
    <div v-if="showConfig" class="panel spot rise" style="margin-bottom:22px; --d:.05s">
      <div class="phead"><span class="t">备份配置</span><span class="sub">copyMessage · 独立副本</span></div>
      <div class="pbody">
        <div class="callout" style="margin-bottom:18px;">
          新建一个专用 bot，并把它设为 <b>源频道</b> 和 <b>备份频道</b> 两边的<b>管理员（有发消息权限）</b>。
          源频道默认取 bots.json 的 channel_id；备份频道是你新建用来存副本的频道。
        </div>
        <div class="row2">
          <div class="field"><label>bot 名称（备注）</label><input class="input" v-model="cfgForm.bot_name" placeholder="cbrec_backup_bot"></div>
          <div class="field"><label>bot token <span v-if="cfg.has_token" style="color:var(--acc)">· 已保存</span></label>
            <input class="input" type="password" v-model="cfgForm.bot_token" :placeholder="cfg.has_token?'•••••• (留空保持不变)':'123456:ABC...'"></div>
        </div>
        <div class="row2">
          <div class="field"><label>源频道 ID</label><input class="input" v-model="cfgForm.source_channel_id" placeholder="-1001810923743"></div>
          <div class="field"><label>备份频道 ID</label><input class="input" v-model="cfgForm.backup_channel_id" placeholder="-100xxxxxxxxxx"><span class="hint">在频道里把 bot 设为管理员后，转发一条消息给 @username_to_id_bot 拿 -100 开头的 ID</span></div>
        </div>
        <div class="row2">
          <div class="field"><label>每条间隔 (ms) · 限流保护</label><input class="input mono" type="number" v-model="cfgForm.delay_ms" min="0" max="60000"></div>
          <div class="field" style="align-self:end;"><button class="btn primary" :disabled="cfgForm.saving" @click="saveConfig" style="justify-content:center;">{{ cfgForm.saving?'校验中…':'保存并校验' }}</button></div>
        </div>

        <div v-if="verify" class="verify">
          <div class="vrow"><span class="vk">bot</span>
            <span class="vv" v-if="verify.bot && verify.bot.ok">@{{ verify.bot.username }} <span class="mono" style="color:var(--ink-4)">(id {{ verify.bot.id }})</span></span>
            <span class="vv" v-else style="color:var(--coral)">{{ (verify.bot&&verify.bot.error) || verify.error }}</span>
            <span class="spacer"></span><span class="badge" :class="(verify.bot&&verify.bot.ok)?'ok':'bad'">{{ (verify.bot&&verify.bot.ok)?'OK':'FAIL' }}</span>
          </div>
          <div class="vrow"><span class="vk">源频道</span>
            <span class="vv">{{ (verify.source&&verify.source.title) || cfg.source_channel_id }} <span v-if="verify.source&&verify.source.status" class="mono" style="color:var(--ink-4)">· {{ verify.source.status }}</span></span>
            <span v-if="verify.source && !verify.source.ok" class="vv" style="color:var(--coral)">— {{ verify.source.error }}</span>
            <span class="spacer"></span><span class="badge" :class="(verify.source&&verify.source.ok)?'ok':'bad'">{{ (verify.source&&verify.source.ok)?'可发帖':'不可' }}</span>
          </div>
          <div class="vrow"><span class="vk">备份频道</span>
            <span class="vv">{{ (verify.backup&&verify.backup.title) || cfg.backup_channel_id }} <span v-if="verify.backup&&verify.backup.status" class="mono" style="color:var(--ink-4)">· {{ verify.backup.status }}</span></span>
            <span v-if="verify.backup && !verify.backup.ok" class="vv" style="color:var(--coral)">— {{ verify.backup.error }}</span>
            <span class="spacer"></span><span class="badge" :class="(verify.backup&&verify.backup.ok)?'ok':'bad'">{{ (verify.backup&&verify.backup.ok)?'可发帖':'不可' }}</span>
          </div>
        </div>
        <div style="margin-top:12px;"><button class="btn ghost sm" @click="reverify"><icon n="refresh" :s="13"/> 重新校验</button></div>

        <div class="callout" style="margin-top:18px;">
          backup 开始前会用下面的<b>真实账号</b>做一次源频道 full scan，补齐上传成功但 scanner.db 漏记的消息。这里不是 bot 登录。
        </div>
        <div class="row2" style="margin-top:14px;">
          <div class="field"><label>真实账号</label>
            <select class="input" v-if="tdAccounts.length" v-model="tdForm.name" @change="pickTdAccount(tdForm.name)">
              <option v-for="a in tdAccounts" :key="a.name" :value="a.name">{{ a.name }} · {{ a.phone }} · {{ a.channel_id }}</option>
            </select>
            <input v-else class="input" v-model="tdForm.name" placeholder="main">
            <span class="hint">已有账号可直接下拉选择；要新增账号时改名称并保存。</span>
          </div>
          <div class="field"><label>手机号</label><input class="input mono" v-model="tdForm.phone" placeholder="+8613800000000"></div>
        </div>
        <div class="row2">
          <div class="field"><label>api_id</label><input class="input mono" v-model="tdForm.api_id" placeholder="123456"></div>
          <div class="field"><label>api_hash</label><input class="input mono" type="password" v-model="tdForm.api_hash" placeholder="my.telegram.org 获取"></div>
        </div>
        <div class="row2">
          <div class="field"><label>扫描频道 ID / 用户名</label><input class="input" v-model="tdForm.channel_id" :placeholder="cfg.source_channel_id || '@source_channel'"><span class="hint">建议和上面的源频道一致；多个真实账号时 backup 会按这个匹配 scan 账号。</span></div>
          <div class="field" style="align-self:end;"><button class="btn primary" :disabled="tdForm.saving" @click="saveTdAccount" style="justify-content:center;">{{ tdForm.saving?'保存中…':'保存真实账号配置' }}</button></div>
        </div>
        <div class="row2" style="align-items:end;">
          <div class="field"><label>登录状态</label><div class="input mono" style="display:flex;align-items:center;">{{ tdLogin.state || 'idle' }}<span v-if="tdAccounts.length" style="margin-left:auto;color:var(--ink-4)">已配置 {{ tdAccounts.length }} 个</span></div></div>
          <div class="field"><button class="btn ghost" :disabled="tdLogin.busy || !tdForm.name" @click="startTdLogin" style="justify-content:center;"><icon n="shield" :s="13"/> {{ tdLogin.busy?'连接中…':'登录 / 验证真实账号' }}</button></div>
        </div>
        <div class="row2" style="align-items:end;">
          <div class="field"><label>手动补库</label><div class="input mono" style="display:flex;align-items:center;">full scan · {{ tdForm.name || '未选择账号' }}<span style="margin-left:auto;color:var(--ink-4)">scanner.db</span></div>
            <label class="hint" style="display:flex;align-items:center;gap:8px;margin-top:8px;"><input type="checkbox" v-model="tdScan.fetch_thumbnails"> 同时下载缩略图（更慢）</label>
          </div>
          <div class="field"><button class="btn primary" :disabled="tdScan.running || st.running || !tdForm.name" @click="startTdScan" style="justify-content:center;"><icon n="refresh" :s="13"/> {{ tdScan.running?'扫描中…':'先全量 scan 补齐 DB' }}</button></div>
        </div>
        <div class="row2" v-if="tdLogin.state==='wait_code'">
          <div class="field"><label>Telegram 验证码</label><input class="input mono" v-model="tdLogin.code" placeholder="12345" @keyup.enter="submitTdCode"></div>
          <div class="field" style="align-self:end;"><button class="btn primary" :disabled="tdLogin.busy || !tdLogin.code" @click="submitTdCode" style="justify-content:center;">提交验证码</button></div>
        </div>
        <div class="row2" v-if="tdLogin.state==='wait_password'">
          <div class="field"><label>2FA 密码</label><input class="input mono" type="password" v-model="tdLogin.password" @keyup.enter="submitTdPassword"></div>
          <div class="field" style="align-self:end;"><button class="btn primary" :disabled="tdLogin.busy || !tdLogin.password" @click="submitTdPassword" style="justify-content:center;">提交 2FA</button></div>
        </div>
        <div v-if="tdLogin.error" class="hint" style="color:var(--coral);margin-top:8px;">{{ tdLogin.error }}</div>
      </div>
    </div>

    <!-- 备份进度: full-width link banner -->
    <div class="panel spot rise" :class="{ 'live-on': st.running }" style="margin-bottom:22px; --d:.08s">
      <div class="phead"><span class="t">备份进度</span><span class="sub">{{ st.running ? (st.scope==='all'?'全量':'选中') : (st.finished_at? '已结束':'idle') }}</span>
        <span class="spacer"></span>
        <button class="btn danger sm" :disabled="!st.running" @click="stopBackup" aria-label="stop backup"><icon n="stop" :s="13"/> 停止</button>
      </div>
      <div class="pbody flush">
        <div class="linkstage" :class="{ running: st.running }">
          <canvas class="link-canvas"></canvas>
          <div class="lk-node lk-src"><span class="lk-name">源频道</span><span class="lk-id">{{ cfg.source_channel_id || '—' }}</span></div>
          <div class="lk-node lk-dst"><span class="lk-name">备份频道</span><span class="lk-id">{{ cfg.backup_channel_id || '未设置' }}</span></div>
          <div class="lk-center">
            <div class="lk-count"><tween-n :value="st.done"/><i> / {{ st.total.toLocaleString() }}</i></div>
            <div class="lk-sub">{{ st.running ? '链接中 · '+(st.current_model||'') : (st.total ? '已暂停' : '待机') }}</div>
          </div>
        </div>
        <div class="hero-foot2">
          <div class="stats">
            <div class="stat"><div class="l">已复制</div><div class="v" style="color:var(--acc)"><tween-n :value="st.done"/></div></div>
            <div class="stat"><div class="l">已跳过</div><div class="v" style="color:var(--ink-3)"><tween-n :value="st.skipped"/></div></div>
            <div class="stat"><div class="l">失败</div><div class="v" :style="{color: st.failed?'var(--coral)':'var(--ink-3)'}"><tween-n :value="st.failed"/></div></div>
            <div class="stat"><div class="l">总计</div><div class="v" style="color:var(--ink-2)"><tween-n :value="st.total"/></div></div>
          </div>
          <div class="footrow">
            <span>速率 <b class="mono" style="color:var(--ink)">{{ rate>0? rate.toFixed(2):'—' }}</b>/s</span>
            <span>ETA <b style="color:var(--ink)">{{ etaStr }}</b></span>
            <span v-if="st.last_error" class="warnmsg">⚠ {{ st.last_error }}</span>
          </div>
        </div>
      </div>
    </div>

    <div class="grid">
      <!-- MODELS -->
      <div class="panel spot rise mcol" style="--d:.1s">
        <div class="phead"><span class="t">Backup</span><span class="sub">{{ models.length }} · 已选 {{ selCount }}</span></div>
        <div class="pbody">
          <div class="toolbar">
            <span class="search-wrap"><icon n="search" :s="14"/><input class="input" v-model="q" placeholder="搜索 model…" aria-label="search models"></span>
            <button class="btn sm" @click="selectAll">全选</button>
            <button class="btn sm ghost" @click="clearSel">清空</button>
          </div>
          <div class="coverage-tabs" role="tablist" aria-label="coverage filter">
            <button class="seg" :class="{active: coverageFilter==='all'}" @click="coverageFilter='all'">All <b>{{ modelCounts.all }}</b></button>
            <button class="seg" :class="{active: coverageFilter==='pending'}" @click="coverageFilter='pending'">Pending <b>{{ modelCounts.pending }}</b></button>
            <button class="seg" :class="{active: coverageFilter==='complete'}" @click="coverageFilter='complete'">Complete <b>{{ modelCounts.complete }}</b></button>
          </div>
          <div class="toolbar actions">
            <button class="btn primary sm" :disabled="st.running || selCount===0" @click="startBackup('models')">
              <icon n="play" :s="14"/> 备份选中 ({{ selCount }}) · {{ selTotal.toLocaleString() }} 待复制
            </button>
            <button class="btn sm" :disabled="st.running" @click="startBackup('all')"><icon n="layers" :s="14"/> 全量备份</button>
          </div>
          <div class="mlist" role="listbox" aria-label="models" aria-multiselectable="true">
            <template v-if="loadingModels">
              <div v-for="i in 8" :key="'s'+i" class="skel"></div>
            </template>
            <template v-else>
              <div v-for="(m, i) in filtered" :key="m.model" class="mrow"
                   role="option" tabindex="0" :aria-selected="sel.has(m.model)"
                   :class="{ sel: sel.has(m.model), done100: m.total>0 && m.backed_up>=m.total }"
                   :style="{ '--drow': Math.min(i, 12) * 18 + 'ms' }"
                   @click="toggle(m)" @keydown.enter.prevent="toggle(m)" @keydown.space.prevent="toggle(m)">
                <span class="cbx"><icon v-if="sel.has(m.model)" n="check" :s="13"/></span>
                <span class="m-main">
                  <span class="nm">{{ m.model }}</span>
                  <span class="m-sub">
                    <span class="m-chip"><b>{{ m.backed_up.toLocaleString() }}</b> copied</span>
                    <span class="m-chip hot">{{ Math.max(0, m.total - m.backed_up).toLocaleString() }} pending</span>
                    <span class="m-chip">{{ m.total.toLocaleString() }} total</span>
                  </span>
                </span>
                <span class="m-orb" :style="{ '--pct': (m.total? Math.min(100, m.backed_up/m.total*100):0)+'%' }">
                  <span><b>{{ m.total? Math.round(Math.min(100, m.backed_up/m.total*100)):0 }}</b>%</span>
                </span>
                <div class="mbar"><div class="f" :style="{ width: (m.total? Math.min(100, m.backed_up/m.total*100):0)+'%' }"></div></div>
              </div>
              <div v-if="filtered.length===0" class="empty">{{ models.length? '无匹配' : '暂无数据（scanner.db 为空？）' }}</div>
            </template>
          </div>
        </div>
      </div>

      <!-- LOG -->
      <div class="panel rise logcol" style="--d:.2s">
          <div class="phead"><span class="t">Log</span><span class="sub">channel · backup</span><span class="spacer"></span>
            <span class="dotlive" :class="{on: connected}"><i></i>{{ connected?'live':'off' }}</span>
          </div>
          <div id="term" class="term" @scroll="onTermScroll">
            <div v-for="l in lines" :key="l.id" class="ln" :class="l.cls">
              <span class="ts">{{ l.ts }}</span>
              <span class="tag" :class="l.tag">{{ l.tag ? '['+l.tag+']' : '' }}</span>
              <span class="msg">{{ l.msg }}</span>
            </div>
          </div>
          <div class="tfoot">
            <span>{{ lines.length }} lines</span>
            <label><input type="checkbox" v-model="autoscroll"> autoscroll</label>
            <span class="spacer"></span>
            <button class="btn ghost sm" @click="clearLog">clear</button>
          </div>
      </div>
    </div>
  </div>

  <div v-if="toast.msg" class="toast" :class="{bad: toast.bad}" role="status" aria-live="polite">
    <icon :n="toast.bad?'alert':'check'" :s="15"/> {{ toast.msg }}
  </div>
</div>
`
};

createApp(App).mount('#app');

// ---- ambient motion: idle float + pointer/gyro parallax + 3D tilt + scroll ----
(() => {
  const root = document.documentElement;
  installGlowFollow(root);
  if (location.search.includes('fx')) installMotion(root, '.spot', true);
  requestAnimationFrame(() => requestAnimationFrame(() => document.body.classList.add('ready')));
})();

function installGlowFollow(root){
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) return;
  let raf = 0, x = 0, y = 0, spot = null;
  const apply = () => {
    raf = 0;
    root.style.setProperty('--px', (x / innerWidth - 0.5).toFixed(3));
    root.style.setProperty('--py', (y / innerHeight - 0.5).toFixed(3));
    if (spot) {
      const r = spot.getBoundingClientRect();
      if (r.width > 0 && r.height > 0) {
        spot.style.setProperty('--mx', ((x - r.left) / r.width * 100).toFixed(1) + '%');
        spot.style.setProperty('--my', ((y - r.top) / r.height * 100).toFixed(1) + '%');
      }
    }
  };
  addEventListener('pointermove', e => {
    x = e.clientX; y = e.clientY;
    spot = e.target.closest && e.target.closest('.spot');
    if (!raf) raf = requestAnimationFrame(apply);
  }, { passive:true });
  addEventListener('pointerleave', () => { spot = null; }, { passive:true });
}

// Shared motion engine (also used by upload.html). Works on desktop (mouse),
// mobile (gyroscope), AND with no input at all (continuous idle float), so the
// depth/parallax is always visibly moving. Respects prefers-reduced-motion.
function installMotion(root, tiltSel, withScroll){
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) return;
  let tpx = 0, tpy = 0, cpx = 0, cpy = 0;   // target / smoothed parallax (-0.5..0.5)
  let cur = null, cx = 0, cy = 0, last = 0;
  const now = () => performance.now();
  const loop = () => {
    const t = now();
    if (t - last > 1100) {                  // idle → gentle auto-float (Lissajous)
      tpx = Math.sin(t / 3400) * 0.42;
      tpy = Math.cos(t / 4600) * 0.36;
    }
    cpx += (tpx - cpx) * 0.06; cpy += (tpy - cpy) * 0.06;
    root.style.setProperty('--px', cpx.toFixed(3));
    root.style.setProperty('--py', cpy.toFixed(3));
    if (cur) {
      const r = cur.getBoundingClientRect();
      const lx = (cx - r.left) / r.width, ly = (cy - r.top) / r.height;
      cur.style.setProperty('--mx', (lx * 100) + '%');
      cur.style.setProperty('--my', (ly * 100) + '%');
      cur.style.transform = `perspective(1200px) rotateX(${((0.5 - ly) * 4).toFixed(2)}deg) rotateY(${((lx - 0.5) * 4).toFixed(2)}deg)`;
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

  // gyroscope → parallax on phones (tilt the device, layers float)
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
      } else if ('ondeviceorientation' in window) {
        addEventListener('deviceorientation', onTilt);
      }
    } catch (_) {}
  };
  enableGyro();
  addEventListener('touchend', enableGyro, { once: true, passive: true });  // iOS needs a gesture

  if (withScroll) {
    const prog = document.createElement('div'); prog.className = 'scrollprog'; document.body.appendChild(prog);
    let rafS = 0;
    const onScroll = () => {
      rafS = 0;
      const y = window.scrollY || 0;
      const h = Math.max(1, root.scrollHeight - innerHeight);
      root.style.setProperty('--scl', y);
      prog.style.setProperty('--sp', Math.min(100, y / h * 100) + '%');
      const b = document.querySelector('.bar'); if (b) b.classList.toggle('scrolled', y > 8);
    };
    addEventListener('scroll', () => { if (!rafS) rafS = requestAnimationFrame(onScroll); }, { passive: true });
    onScroll();
  }
}

// ---- channel-link visualization: two channel entities + flowing light packets ----
// Each successful copy (onBackupCopy) launches a light pulse from the source node,
// along a glowing arc, to the backup node; on arrival it ripples, then the link
// fades — waiting for the next. Driven by real copy events over SSE.
function initLink(){
  const canvas = document.querySelector('.link-canvas');
  if (!canvas) { return setTimeout(initLink, 300); }
  const ctx = canvas.getContext('2d');
  const dpr = Math.min(devicePixelRatio || 1, 1.5);
  const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
  let W = 0, H = 0;
  const resize = () => { const r = canvas.getBoundingClientRect(); W = r.width; H = r.height; canvas.width = W * dpr; canvas.height = H * dpr; ctx.setTransform(dpr, 0, 0, dpr, 0, 0); };
  resize(); addEventListener('resize', resize);

  const LIME = '189,242,90', AMBER = '232,182,90';
  const DUR = 1250;
  const packets = [], ripples = [];
  let lastRipple = 0, animating = false, lastRunning = false;
  const requestFrame = () => { if (!animating && !reduce) { animating = true; requestAnimationFrame(frame); } };
  onBackupCopy = () => { packets.push({ t0: performance.now() }); requestFrame(); };
  if (location.search.includes('lkdemo')) { linkState.running = true; setInterval(onBackupCopy, 650); }  // preview without a live backup

  const geom = () => ({ sx: W * 0.18, bx: W * 0.82, cy: H * 0.46, lift: Math.min(64, (W * 0.64) * 0.18), r: Math.max(13, Math.min(22, H * 0.11)) });
  const pt = (p, g) => { const mx = (g.sx + g.bx) / 2, my = g.cy - g.lift;
    const u = 1 - p; return { x: u * u * g.sx + 2 * u * p * mx + p * p * g.bx, y: u * u * g.cy + 2 * u * p * my + p * p * g.cy }; };
  const ease = t => t < .5 ? 2 * t * t : 1 - Math.pow(-2 * t + 2, 2) / 2;

  const orb = (x, y, r, rgb, energy) => {
    ctx.save();
    const grd = ctx.createRadialGradient(x, y, 0, x, y, r * 2.6);
    grd.addColorStop(0, `rgba(${rgb},${0.28 + energy * 0.35})`); grd.addColorStop(1, `rgba(${rgb},0)`);
    ctx.fillStyle = grd; ctx.beginPath(); ctx.arc(x, y, r * 2.6, 0, 7); ctx.fill();           // halo
    ctx.shadowColor = `rgb(${rgb})`; ctx.shadowBlur = 16 + energy * 16;
    ctx.beginPath(); ctx.arc(x, y, r, 0, 7); ctx.fillStyle = `rgba(${rgb},0.95)`; ctx.fill();   // core
    ctx.shadowBlur = 0; ctx.lineWidth = 1.5; ctx.strokeStyle = `rgba(${rgb},0.35)`;
    ctx.beginPath(); ctx.arc(x, y, r + 6 + energy * 4, 0, 7); ctx.stroke();                      // ring
    ctx.restore();
  };

  function frame(now){
    animating = false;
    ctx.clearRect(0, 0, W, H);
    const g = geom();
    const live = linkState.running, breathe = 1 + Math.sin(now / 760) * 0.05;

    // base connection (dashed, drifting when live)
    ctx.save();
    ctx.beginPath(); ctx.moveTo(g.sx, g.cy); ctx.quadraticCurveTo((g.sx + g.bx) / 2, g.cy - g.lift, g.bx, g.cy);
    ctx.setLineDash([3, 9]); ctx.lineDashOffset = -now / 36;
    ctx.strokeStyle = live ? `rgba(${LIME},0.22)` : 'rgba(255,255,255,0.07)'; ctx.lineWidth = 1.5; ctx.stroke();
    ctx.restore();

    // packets + their growing bright link
    for (let i = packets.length - 1; i >= 0; i--) {
      const t = (now - packets[i].t0) / DUR;
      if (t >= 1) { if (now - lastRipple > 40) { ripples.push({ t0: now, x: g.bx, y: g.cy }); lastRipple = now; } packets.splice(i, 1); continue; }
      const p = ease(t);
      ctx.save(); ctx.beginPath(); ctx.moveTo(g.sx, g.cy);
      for (let s = 1; s <= 24; s++) { const q = pt(Math.min(p, s / 24), g); ctx.lineTo(q.x, q.y); if (s / 24 >= p) break; }
      ctx.strokeStyle = `rgba(${LIME},0.55)`; ctx.lineWidth = 2; ctx.shadowColor = `rgb(${LIME})`; ctx.shadowBlur = 10; ctx.stroke(); ctx.restore();
      const c = pt(p, g);
      ctx.save(); ctx.shadowColor = `rgb(${LIME})`; ctx.shadowBlur = 20;
      ctx.beginPath(); ctx.arc(c.x, c.y, 4.5, 0, 7); ctx.fillStyle = '#e6ff8c'; ctx.fill(); ctx.restore();
    }

    // arrival ripples at backup node
    for (let i = ripples.length - 1; i >= 0; i--) {
      const t = (now - ripples[i].t0) / 720; if (t >= 1) { ripples.splice(i, 1); continue; }
      ctx.save(); ctx.beginPath(); ctx.arc(ripples[i].x, ripples[i].y, g.r + t * 34, 0, 7);
      ctx.strokeStyle = `rgba(${LIME},${(1 - t) * 0.6})`; ctx.lineWidth = 2; ctx.stroke(); ctx.restore();
    }

    const recv = Math.min(1, packets.length / 3);   // backup node glows with inflight traffic
    orb(g.sx, g.cy, g.r * breathe, AMBER, live ? 0.5 : 0.15);
    orb(g.bx, g.cy, g.r * breathe, LIME, live ? 0.5 + recv * 0.5 : 0.15);

    if (!document.hidden && (live || packets.length || ripples.length || lastRunning !== live)) {
      lastRunning = live;
      requestFrame();
    }
  }
  frame(performance.now());
  setInterval(() => {
    if (linkState.running !== lastRunning) requestFrame();
  }, 500);
}
initLink();
