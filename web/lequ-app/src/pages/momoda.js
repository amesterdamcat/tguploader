
import { createApp, ref, reactive, computed, onMounted, onBeforeUnmount, h } from 'vue';
import '../styles/pages/momoda.css';

const TOKEN_KEY='token';
const getToken=()=>localStorage.getItem(TOKEN_KEY)||'';
const dropToken=()=>localStorage.removeItem(TOKEN_KEY);
(function(){const u=new URL(location.href),t=u.searchParams.get('token');if(t){localStorage.setItem(TOKEN_KEY,t);u.searchParams.delete('token');history.replaceState({},'',u.pathname+(u.search||''));}})();

let onUnauthorized=()=>{};
async function api(path,opts={}){
  const headers={'Authorization':'Bearer '+getToken(),...(opts.body?{'Content-Type':'application/json'}:{}),...opts.headers};
  const res=await fetch(path,{...opts,headers});
  if(res.status===401){let d={};try{d=await res.json();}catch{}
    if((d.detail||'')==='Unauthorized'){dropToken();onUnauthorized();throw new Error('Unauthorized');}
    throw new Error(d.error||d.detail||'Unauthorized');}
  if(!res.ok){let d={};try{d=await res.json();}catch{}throw new Error(d.error||d.detail||('HTTP '+res.status));}
  return res.json();
}
const ICONS={
  heart:'<path d="M20.84 4.61a5.5 5.5 0 0 0-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 0 0-7.78 7.78L12 21.23l8.84-8.84a5.5 5.5 0 0 0 0-7.78z"/>',
  power:'<path d="M18.36 6.64a9 9 0 1 1-12.73 0"/><line x1="12" y1="2" x2="12" y2="12"/>',
  refresh:'<path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8"/><path d="M21 3v5h-5"/><path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16"/><path d="M8 16H3v5"/>',
  external:'<path d="M15 3h6v6"/><path d="M10 14 21 3"/><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/>',
  logout:'<path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><path d="m16 17 5-5-5-5"/><path d="M21 12H9"/>',
};
const Icon={props:{n:String,s:{type:Number,default:16}},setup(p){return()=>h('svg',{class:'ic',width:p.s,height:p.s,viewBox:'0 0 24 24',fill:'none',stroke:'currentColor','stroke-width':1.8,'stroke-linecap':'round','stroke-linejoin':'round',innerHTML:ICONS[p.n]||''});}};

const App={components:{Icon},setup(){
  const token=ref(getToken());
  const loggedIn=computed(()=>!!token.value);
  onUnauthorized=()=>{token.value='';};
  const lf=reactive({user:'',pass:'',err:'',busy:false});
  async function doLogin(){
    lf.busy=true;lf.err='';
    try{
      const r=await fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:lf.user,password:lf.pass})});
      const ct=r.headers.get('content-type')||'';
      if(!r.ok||!ct.includes('json'))throw new Error('登录失败');
      const d=await r.json();if(!d.token)throw new Error(d.detail||'登录失败');
      localStorage.setItem(TOKEN_KEY,d.token);token.value=d.token;boot();
    }catch(e){lf.err=e.message||'登录失败';}finally{lf.busy=false;}
  }
  function logout(){dropToken();token.value='';stop();}

  const toast=reactive({msg:'',bad:false,t:null});
  function notify(m,bad=false){toast.msg=m;toast.bad=bad;clearTimeout(toast.t);toast.t=setTimeout(()=>toast.msg='',3200);}

  const lequ=reactive({logged_in:false,user_id:''});
  const farm=reactive({enabled:false,concurrency:1,mode:'random',targets:[],total_sent:0,workers:[]});
  const targetsText=ref('');
  let timer=null;

  async function refreshLequ(){try{const d=await api('/api/lequ/auth/status');lequ.logged_in=d.logged_in&&!d.expired;lequ.user_id=d.user_id;}catch(e){}}
  async function refreshFarm(){
    try{const d=await api('/api/lequ/farm/status');
      Object.assign(farm,d);
      if(document.activeElement?.id!=='tgt' && farm.mode==='specific') targetsText.value=(farm.targets||[]).join(', ');
    }catch(e){}
  }
  async function pushConfig(extra={}){
    const targets=targetsText.value.split(/[\s,，]+/).map(s=>s.trim()).filter(Boolean);
    try{
      const d=await api('/api/lequ/farm/config',{method:'POST',body:JSON.stringify({
        enabled:farm.enabled,concurrency:farm.concurrency,mode:farm.mode,targets,...extra})});
      Object.assign(farm,d);
    }catch(e){notify(e.message,true);}
  }
  async function toggle(){
    if(!lequ.logged_in){notify('请先到 /lequ/ 登录乐趣账号',true);return;}
    if(!farm.enabled && farm.mode==='specific' && !targetsText.value.trim()){notify('指定主播模式请先填主播 ID',true);return;}
    farm.enabled=!farm.enabled;
    await pushConfig();
    notify(farm.enabled?'已启动 24h 刷么么哒 🫶':'已停止');
  }
  function setConc(n){farm.concurrency=Math.max(1,Math.min(8,n));pushConfig();}
  function setMode(m){farm.mode=m;pushConfig();}

  async function load(){await refreshLequ();await refreshFarm();}
  async function boot(){await load();if(timer)clearInterval(timer);timer=setInterval(()=>{if(document.visibilityState==='visible'){refreshFarm();}},3000);}
  function stop(){if(timer){clearInterval(timer);timer=null;}}
  onMounted(()=>{if(loggedIn.value)boot();});
  onBeforeUnmount(stop);

  const fmtTime=s=>{s=s||0;const m=Math.floor(s/60),x=s%60;return m+':'+String(x).padStart(2,'0');};
  const fmtClock=ts=>{if(!ts)return'';const d=new Date(ts*1000),p=n=>String(n).padStart(2,'0');return p(d.getHours())+':'+p(d.getMinutes())+':'+p(d.getSeconds());};

  return{token,loggedIn,lf,doLogin,logout,toast,lequ,farm,targetsText,toggle,setConc,setMode,pushConfig,refreshFarm,fmtTime,fmtClock};
},
template:`
<div v-if="!loggedIn" class="login">
  <div class="box">
    <div class="lbrand">tg.<b>么么哒</b></div>
    <div class="lsub">乐趣直播 · 24h 自动刷小心心</div>
    <div class="panel"><form @submit.prevent="doLogin">
      <div class="field"><label>用户名</label><input class="input" v-model="lf.user" autocomplete="username" autofocus></div>
      <div class="field"><label>密码</label><input class="input" type="password" v-model="lf.pass" autocomplete="current-password"></div>
      <div class="err">{{ lf.err }}</div>
      <button class="btn" type="submit" :disabled="lf.busy" style="width:100%;background:var(--coral);color:#fff;border-color:transparent">{{ lf.busy?'…':'登录' }}</button>
    </form></div>
  </div>
</div>

<div v-else class="wrap">
  <div class="bar">
    <span class="brand"><icon n="heart" :s="20"/>么么哒<b>·农场</b></span>
    <span class="chip" :class="lequ.logged_in?'ok':'bad'">{{ lequ.logged_in?('乐趣 '+lequ.user_id):'乐趣未登录' }}</span>
    <span class="spacer"></span>
    <button class="btn ghost sm" @click="refreshFarm"><icon n="refresh" :s="14"/> 刷新</button>
    <a class="btn ghost sm" href="/lequ/"><icon n="external" :s="14"/> 直播</a>
    <button class="btn ghost sm" @click="logout"><icon n="logout" :s="14"/> 退出</button>
  </div>

  <div v-if="!lequ.logged_in" class="panel">
    <div class="warn">⚠ 还没登录乐趣账号。请先到 <a href="/lequ/" style="color:var(--acc)">/lequ/</a> 用短信验证码登录，再回来开刷。</div>
  </div>

  <div class="panel">
    <div class="power">
      <button class="bigbtn" :class="farm.enabled?'on':'off'" @click="toggle">
        <span v-if="farm.enabled" class="live-dot"></span><icon v-else n="power" :s="18"/>
        {{ farm.enabled?'刷取中 · 点击停止':'启动 24h 刷么么哒' }}
      </button>
      <div class="stat"><span class="num">{{ farm.total_sent }}</span><span class="lbl">累计送出么么哒</span></div>
    </div>
    <div class="controls">
      <div class="field">
        <label>同时刷的个数</label>
        <span class="step">
          <button :disabled="farm.concurrency<=1" @click="setConc(farm.concurrency-1)">−</button>
          <span class="v">{{ farm.concurrency }}</span>
          <button :disabled="farm.concurrency>=8" @click="setConc(farm.concurrency+1)">＋</button>
        </span>
      </div>
      <div class="field">
        <label>主播来源</label>
        <span class="seg">
          <button :class="{active:farm.mode==='random'}" @click="setMode('random')">随机主播</button>
          <button :class="{active:farm.mode==='specific'}" @click="setMode('specific')">指定主播</button>
        </span>
      </div>
      <div class="field" style="flex:1;min-width:200px" v-if="farm.mode==='specific'">
        <label>主播 ID（逗号分隔，多个分给多个 worker）</label>
        <input id="tgt" class="input" v-model="targetsText" placeholder="如 46775539, 13145255" @blur="pushConfig()">
      </div>
    </div>
    <div class="hint" style="margin-top:12px">原理：每个 worker 进一个直播间，挂满 5 分钟自动获得一个么么哒并送给该主播，然后继续。停服/重启后会自动恢复刷取。</div>
    <div class="warn" style="margin-top:6px">⚠ 这是用你的真实账号 24h 自动送礼，平台可能判定为机器行为存在风控/封号风险，请自行权衡。</div>
  </div>

  <div class="panel">
    <div class="sectitle"><icon n="heart" :s="13"/> Worker 状态 · {{ farm.workers.length }}</div>
    <div v-if="farm.workers.length" class="grid">
      <div v-for="(w,i) in farm.workers" :key="i" class="wk">
        <div class="top">
          <div style="min-width:0">
            <div class="nm">{{ w.nickname || (w.target?('指定 '+w.target):'随机主播') }}</div>
            <div class="id">{{ w.name?('ID '+w.name):'寻找直播间…' }}</div>
          </div>
          <span class="st" :class="w.state">{{ ({picking:'选房',watching:'观看中',sending:'送出',idle:'空闲'})[w.state]||w.state }}</span>
        </div>
        <div class="prog"><i :style="{width:Math.min(100,(w.watched/w.need*100))+'%'}"></i></div>
        <div class="foot"><span>{{ fmtTime(w.watched) }} / {{ fmtTime(w.need) }}</span><span>已送 <b>{{ w.sent }}</b></span></div>
        <div v-if="w.error" class="e">{{ w.error }}</div>
      </div>
    </div>
    <div v-else class="empty">未运行 · 点上面的按钮开始</div>
  </div>

  <div class="panel">
    <div class="sectitle">运行日志 · {{ (farm.log||[]).length }}</div>
    <div v-if="(farm.log||[]).length" class="log">
      <div v-for="(l,i) in farm.log" :key="i" class="logrow" :class="l.text.indexOf('🫶')>=0?'ok':(l.text.indexOf('⚠')>=0?'bad':'')">
        <span class="t">{{ fmtClock(l.at) }}</span><span class="x">{{ l.text }}</span>
      </div>
    </div>
    <div v-else class="empty">暂无日志</div>
  </div>
</div>

<div v-if="toast.msg" class="toast" :class="{bad:toast.bad}">{{ toast.msg }}</div>
`
};

createApp(App).mount('#app');
