#pragma once

// Served on the board's own address while it is running normally, so settings
// can be changed without clearing anything. The setup portal is for first run;
// this is for every day after it.
//
// Unlike the portal, this page is reached over the house network, so it can load
// from the internet if it wants to. It still does not, because a settings page
// that needs a working connection to tell you your connection is broken is not
// much use.

static const char SETTINGS_PAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>esp git</title>
<style>
*{box-sizing:border-box}
body{background:#0d1117;color:#e6edf3;font:16px/1.55 system-ui,-apple-system,sans-serif;margin:0;padding:26px}
.w{max-width:440px;margin:0 auto}
h1{font-size:19px;margin:0 0 18px}
.st{display:flex;justify-content:space-between;padding:7px 0;border-bottom:1px solid #21262d;font-size:14px}
.st span:first-child{color:#8b949e}
label{display:block;margin:20px 0 6px;font-size:13px;color:#8b949e}
select,input{width:100%;padding:12px;border-radius:8px;border:1px solid #30363d;background:#161b22;color:#e6edf3;font-size:16px}
input[type=range]{padding:0;height:34px}
button{width:100%;padding:13px;border-radius:8px;border:1px solid #238636;background:#238636;color:#fff;font-size:16px;font-weight:600;margin-top:22px}
button.ghost{background:transparent;border-color:#30363d;color:#8b949e;font-weight:400;margin-top:10px;padding:10px}
button:disabled{opacity:.5}
.msg{margin-top:14px;padding:11px 13px;border-radius:8px;font-size:14px;display:none}
.msg.ok{display:block;background:#0f2417;border:1px solid #2b5c38;color:#7ee294}
.msg.bad{display:block;background:#2d1214;border:1px solid #6e2831;color:#ff8f96}
details{margin-top:26px;color:#8b949e;font-size:13px}
summary{cursor:pointer}
</style>
<div class=w>
<h1>esp git</h1>

<div class=st><span>version</span><b id=ver>...</b></div>
<div class=st><span>address</span><b id=ip>...</b></div>
<div class=st><span>signal</span><b id=rssi>...</b></div>
<div class=st><span>contributions</span><b id=total>...</b></div>
<div class=st><span>uptime</span><b id=up>...</b></div>

<label>celebration when you push</label>
<select id=fx>
  <option value=1>confetti</option>
  <option value=0>green flash</option>
  <option value=2>border pulse</option>
</select>
<button id=try class=ghost>show me on the screen</button>

<label>brightness <span id=bv></span></label>
<input id=br type=range min=25 max=255>

<label>orientation</label>
<select id=rot>
  <option value=1>normal</option>
  <option value=3>flipped</option>
</select>

<label>screen</label>
<select id=vw>
  <option value=0>contributions</option>
  <option value=1>total</option>
  <option value=2>per year</option>
  <option value=3>repo</option>
  <option value=4>controls</option>
</select>

<button id=save>save</button>
<div class=msg id=m></div>

<details id=upd>
  <summary>updates</summary>
  <p style="font-size:13px;margin:10px 0">
    Off by default. Nobody should be able to change what your board runs without
    you asking for it.
  </p>
  <label style="display:flex;align-items:center;gap:10px;margin:14px 0">
    <input id=auto type=checkbox style="width:auto">
    <span style="color:#e6edf3;font-size:15px">install updates automatically</span>
  </label>
  <div id=avail class=st style="display:none"><span>available</span><b id=availv></b></div>
  <button id=doUpd class=ghost>check now</button>
</details>

<details>
  <summary>network</summary>
  <label>network</label>
  <select id=ssid><option>tap to scan</option></select>
  <button id=doScan class=ghost>scan</button>
  <label>password</label>
  <input id=wpass type=password placeholder="leave blank if unchanged">
  <button id=saveWifi class=ghost>save and restart</button>
</details>

<details>
  <summary>server</summary>
  <label>server url</label>
  <input id=host type=url placeholder="leave blank to keep">
  <label>device token</label>
  <input id=token placeholder="leave blank to keep">
  <button id=saveSrv class=ghost>save and restart</button>
</details>
</div>

<script>
const $=i=>document.getElementById(i)
const show=(c,t)=>{const m=$('m');m.className='msg '+c;m.textContent=t;
  if(c=='ok')setTimeout(()=>m.className='msg',2500)}

function load(){
  fetch('/api/state').then(r=>r.json()).then(s=>{
    $('ver').textContent='v'+s.version
    $('ip').textContent=s.ip
    $('rssi').textContent=s.rssi+' dBm'
    $('total').textContent=s.total
    const m=Math.floor(s.uptime/60000), h=Math.floor(m/60)
    $('up').textContent = h ? h+'h '+(m%60)+'m' : m+'m'
    $('rot').value=s.rotation
    $('fx').value=s.effect; $('br').value=s.brightness; $('vw').value=s.view
    $('auto').checked=!!s.autoUpdate
    if(s.available){$('avail').style.display='flex';$('availv').textContent='v'+s.available}
    else $('avail').style.display='none'
    if(s.ssid && $('ssid').options.length<2) $('ssid').innerHTML='<option>'+s.ssid+'</option>' 
    $('bv').textContent=Math.round((s.brightness-25)/230*100)+'%'
  }).catch(()=>{})
}
load(); setInterval(load,10000)

$('br').oninput=()=>{$('bv').textContent=Math.round(($('br').value-25)/230*100)+'%'}

$('try').onclick=()=>{
  const b=$('try'); b.disabled=true
  fetch('/api/preview',{method:'POST',headers:{'content-type':'application/json'},
    body:JSON.stringify({effect:+$('fx').value})})
  .finally(()=>setTimeout(()=>b.disabled=false,2600))
}

$('save').onclick=()=>{
  fetch('/api/settings',{method:'POST',headers:{'content-type':'application/json'},
    body:JSON.stringify({effect:+$('fx').value,brightness:+$('br').value,view:+$('vw').value,rotation:+$('rot').value})})
  .then(r=>r.json()).then(()=>show('ok','saved')).catch(()=>show('bad','could not reach the board'))
}

$('auto').onchange=()=>{
  fetch('/api/settings',{method:'POST',headers:{'content-type':'application/json'},
    body:JSON.stringify({autoUpdate:$('auto').checked})})
  .then(()=>show('ok',$('auto').checked?'will install updates on its own':'will only tell you'))
}

$('doUpd').onclick=()=>{
  $('doUpd').disabled=true; show('ok','checking...')
  fetch('/api/update',{method:'POST'})
  .finally(()=>setTimeout(()=>{$('doUpd').disabled=false;load()},4000))
}

$('doScan').onclick=()=>{
  $('doScan').disabled=true; $('doScan').textContent='scanning...'
  fetch('/api/scan').then(r=>r.json()).then(n=>{
    const s=$('ssid'); s.innerHTML=''
    n.sort((a,b)=>b.r-a.r).forEach(x=>{const o=document.createElement('option')
      o.value=x.s;o.textContent=x.s+'  ('+x.r+'dBm)';s.appendChild(o)})
  }).finally(()=>{$('doScan').disabled=false;$('doScan').textContent='scan'})
}

$('saveWifi').onclick=()=>{
  if(!$('ssid').value){show('bad','scan and pick a network first');return}
  $('saveWifi').disabled=true
  fetch('/api/settings',{method:'POST',headers:{'content-type':'application/json'},
    body:JSON.stringify({ssid:$('ssid').value,pass:$('wpass').value,restart:true})})
  .then(()=>show('ok','saved, restarting')).catch(()=>show('ok','saved, restarting'))
}

$('saveSrv').onclick=()=>{
  const h=$('host').value.trim(), t=$('token').value.trim()
  if(!h&&!t){show('bad','nothing to change');return}
  $('saveSrv').disabled=true
  fetch('/api/settings',{method:'POST',headers:{'content-type':'application/json'},
    body:JSON.stringify({host:h,token:t,restart:true})})
  .then(r=>r.json()).then(r=>show(r.ok?'ok':'bad',r.ok?'saved, restarting':'that did not work'))
  .catch(()=>show('ok','saved, restarting'))
}
</script>)HTML";
