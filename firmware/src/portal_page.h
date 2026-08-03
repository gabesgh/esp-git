#pragma once

// Served from the board during setup. One page, three steps, driven by fetch
// against the handlers in provision.cpp. Kept in its own header so the C++ is
// readable without scrolling past a wall of markup.
//
// The phone is attached to the board, not the internet, so this cannot pull in
// a stylesheet or a font. Everything is inline and the instructions are written
// out rather than linked, because a link will not load from here.

static const char PORTAL_PAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>esp git setup</title>
<style>
*{box-sizing:border-box}
body{background:#0d1117;color:#e6edf3;font:16px/1.55 system-ui,-apple-system,sans-serif;margin:0;padding:26px}
.w{max-width:440px;margin:0 auto}
h1{font-size:19px;margin:0 0 2px;letter-spacing:.2px}
.step{color:#8b949e;font-size:13px;margin:0 0 22px}
.bar{display:flex;gap:6px;margin:0 0 24px}
.bar i{flex:1;height:3px;background:#21262d;border-radius:2px}
.bar i.on{background:#238636}
label{display:block;margin:18px 0 6px;font-size:13px;color:#8b949e}
select,input{width:100%;padding:12px;border-radius:8px;border:1px solid #30363d;background:#161b22;color:#e6edf3;font-size:16px}
button{width:100%;padding:13px;border-radius:8px;border:1px solid #238636;background:#238636;color:#fff;font-size:16px;font-weight:600;margin-top:24px}
button:disabled{opacity:.5}
button.ghost{background:transparent;border-color:#30363d;color:#8b949e;font-weight:400;margin-top:10px;padding:10px}
.msg{margin-top:16px;padding:11px 13px;border-radius:8px;font-size:14px;display:none}
.msg.bad{display:block;background:#2d1214;border:1px solid #6e2831;color:#ff8f96}
.msg.ok{display:block;background:#0f2417;border:1px solid #2b5c38;color:#7ee294}
.msg.wait{display:block;background:#161b22;border:1px solid #30363d;color:#8b949e}
.help{margin-top:22px;padding:14px;border:1px solid #21262d;border-radius:8px;font-size:13px;color:#8b949e}
.help b{color:#e6edf3;font-weight:600}
.help code{background:#161b22;padding:1px 5px;border-radius:4px;font-size:12px}
.help ol{margin:8px 0 0;padding-left:20px}
.help li{margin:5px 0}
section{display:none}section.on{display:block}
</style>
<div class=w>
<h1>esp git</h1>
<p class=step id=stepname>step 1 of 2: your wifi</p>
<div class=bar><i id=b1 class=on></i><i id=b2></i></div>

<section id=s1 class=on>
  <label>network</label>
  <select id=ssid><option>scanning...</option></select>
  <label>password</label>
  <input id=pass type=password autocomplete=off placeholder="leave empty if open">
  <button id=go1>connect</button>
  <div class=msg id=m1></div>
</section>

<section id=s2>
  <label>server url</label>
  <input id=host type=url autocomplete=off placeholder="https://your-app.vercel.app">
  <label>device token</label>
  <input id=token autocomplete=off placeholder="the DEVICE_TOKEN you set on it">

  <label>what happens when you push</label>
  <select id=fx>
    <option value=1 selected>confetti</option>
    <option value=0>green flash</option>
    <option value=2>border pulse</option>
  </select>
  <button id=try class=ghost>show me on the screen</button>

  <button id=go2>finish</button>
  <div class=msg id=m2></div>

  <div class=help>
    <b>Do not have these yet?</b>
    <ol>
      <li>Clone the repo and open <code>AGENTS.md</code>. It walks the whole
          thing, and you can hand it to a coding agent to do for you.</li>
      <li>You need a GitHub token, classic, <code>repo</code> scope only.</li>
      <li>Deploy the <code>server/</code> folder to Vercel or anywhere that runs
          Next.js. Free tier is plenty.</li>
      <li>The <code>DEVICE_TOKEN</code> you set on that deployment is what goes
          in the box above.</li>
    </ol>
    Come back here when you have them. Holding <b>BOOT</b> for three seconds
    always brings this page back.
  </div>
</section>
</div>

<script>
const $=i=>document.getElementById(i)
const show=(el,cls,txt)=>{el.className='msg '+cls;el.textContent=txt}

fetch('/scan').then(r=>r.json()).then(n=>{
  const s=$('ssid');s.innerHTML=''
  if(!n.length){s.innerHTML='<option>no networks found</option>';return}
  n.sort((a,b)=>b.r-a.r).forEach(x=>{
    const o=document.createElement('option');o.value=x.s
    o.textContent=x.s+'  ('+x.r+'dBm)';s.appendChild(o)})
}).catch(()=>{$('ssid').innerHTML='<option>scan failed</option>'})

$('go1').onclick=()=>{
  $('go1').disabled=true
  show($('m1'),'wait','joining '+$('ssid').value+'...')
  fetch('/wifi',{method:'POST',headers:{'content-type':'application/json'},
    body:JSON.stringify({ssid:$('ssid').value,pass:$('pass').value})})
  .then(()=>poll(0))
}

function poll(n){
  fetch('/status').then(r=>r.json()).then(s=>{
    if(s.wifi=='ok'){
      show($('m1'),'ok','connected, got '+s.ip)
      setTimeout(()=>{
        $('s1').className='section';$('s1').classList.remove('on')
        $('s2').classList.add('on');$('b2').className='on'
        $('stepname').textContent='step 2 of 2: where to fetch from'
      },700)
    } else if(s.wifi=='fail'){
      show($('m1'),'bad','could not join. wrong password, or too far away.')
      $('go1').disabled=false
    } else if(n<40){ setTimeout(()=>poll(n+1),700) }
    else { show($('m1'),'bad','timed out'); $('go1').disabled=false }
  }).catch(()=>{ if(n<40) setTimeout(()=>poll(n+1),900) })
}

$('try').onclick=()=>{
  const b=$('try'); b.disabled=true; b.textContent='watch the board'
  fetch('/preview',{method:'POST',headers:{'content-type':'application/json'},
    body:JSON.stringify({effect:+$('fx').value})})
  .finally(()=>setTimeout(()=>{b.disabled=false;b.textContent='show me on the screen'},2600))
}

$('go2').onclick=()=>{
  const h=$('host').value.trim(), t=$('token').value.trim()
  if(!h||!t){show($('m2'),'bad','both are needed');return}
  $('go2').disabled=true
  show($('m2'),'wait','checking that it answers...')
  fetch('/server',{method:'POST',headers:{'content-type':'application/json'},
    body:JSON.stringify({host:h,token:t,effect:+$('fx').value})})
  .then(r=>r.json()).then(r=>{
    if(r.ok){
      show($('m2'),'ok','working. restarting, you can close this.')
      fetch('/done',{method:'POST'})
    } else {
      show($('m2'),'bad',r.error||'that did not work')
      $('go2').disabled=false
    }
  }).catch(()=>{show($('m2'),'bad','no response from the board');$('go2').disabled=false})
}
</script>)HTML";
