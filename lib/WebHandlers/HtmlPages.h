#pragma once
#include <pgmspace.h>

const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>RFID Access - Dashboard</title>
<style>
 body{font-family:system-ui,Arial,sans-serif;margin:0;background:#0f1419;color:#e6e6e6}
 header{background:#1b2430;padding:16px 20px;border-bottom:1px solid #2a3645;display:flex;justify-content:space-between;align-items:center}
 h1{margin:0;font-size:18px}.status{font-size:13px;color:#8aa0b4;margin-top:4px}
 a.btn{color:#5fd3a0;text-decoration:none;border:1px solid #2a3645;padding:8px 14px;border-radius:6px;font-size:14px}
 .wrap{padding:20px;max-width:820px;margin:0 auto}
 table{width:100%;border-collapse:collapse;background:#161e29;border-radius:8px;overflow:hidden}
 th,td{text-align:left;padding:10px 14px;font-size:14px}
 th{background:#202c3a;color:#9fb3c8}tr:nth-child(even) td{background:#19222e}
 td.uid{font-family:ui-monospace,monospace;color:#cdd6e0;font-weight:600}
 .g{color:#43d17a;font-weight:700}.d{color:#e0556b;font-weight:700}
 .empty{color:#6b7d90;padding:20px;text-align:center}
</style></head><body>
<header><div><h1>RFID Access Control</h1><div class="status" id="status">Connecting...</div></div>
<a class="btn" href="/config">Manage fobs &rarr;</a></header>
<div class="wrap"><table>
<thead><tr><th>#</th><th>Result</th><th>UID</th><th>Name</th><th>When</th></tr></thead>
<tbody id="log"><tr><td colspan="5" class="empty">No taps yet.</td></tr></tbody>
</table></div>
<script>
function ago(s){return s<60?s+"s ago":s<3600?Math.floor(s/60)+"m ago":Math.floor(s/3600)+"h ago";}
async function refresh(){
 try{const r=await fetch('/api/taps');const d=await r.json();
 document.getElementById('status').textContent='Connected - '+d.count+' taps - uptime '+Math.floor(d.uptime/1000)+'s';
 const tb=document.getElementById('log');
 if(!d.taps.length){tb.innerHTML='<tr><td colspan="5" class="empty">No taps yet.</td></tr>';return;}
 let h='';d.taps.forEach((t,i)=>{const a=Math.floor((d.uptime-t.ms)/1000);
  h+='<tr><td>'+(i+1)+'</td><td class="'+(t.granted?'g':'d')+'">'+(t.granted?'GRANTED':'DENIED')+'</td>'+
     '<td class="uid">'+t.uid+'</td><td>'+(t.name||'-')+'</td><td>'+ago(a)+'</td></tr>';});
 tb.innerHTML=h;
 }catch(e){document.getElementById('status').textContent='Connection lost...';}}
setInterval(refresh,1000);refresh();
</script><script src="/footer.js"></script></body></html>
)HTML";

const char CONFIG_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>RFID Access - Manage Fobs</title>
<style>
 body{font-family:system-ui,Arial,sans-serif;margin:0;background:#0f1419;color:#e6e6e6}
 header{background:#1b2430;padding:16px 20px;border-bottom:1px solid #2a3645;display:flex;justify-content:space-between;align-items:center}
 h1{margin:0;font-size:18px}h2{font-size:15px;color:#9fb3c8;margin:24px 0 8px}
 a.btn{color:#5fd3a0;text-decoration:none;border:1px solid #2a3645;padding:8px 14px;border-radius:6px;font-size:14px}
 .wrap{padding:20px;max-width:820px;margin:0 auto}
 table{width:100%;border-collapse:collapse;background:#161e29;border-radius:8px;overflow:hidden}
 th,td{text-align:left;padding:10px 14px;font-size:14px}
 th{background:#202c3a;color:#9fb3c8}tr:nth-child(even) td{background:#19222e}
 td.uid{font-family:ui-monospace,monospace;color:#cdd6e0;font-weight:600}
 input{background:#0f1419;border:1px solid #2a3645;color:#e6e6e6;padding:6px 8px;border-radius:5px;font-size:14px}
 button{background:#2563eb;color:#fff;border:0;padding:7px 12px;border-radius:5px;cursor:pointer;font-size:14px}
 button.rm{background:#7f2233}
 .enroll{background:#15233a;border:1px solid #2a4365;border-radius:8px;padding:16px;margin-top:8px}
 .enroll .uid{font-family:ui-monospace,monospace;color:#7fb8ff;font-size:16px;font-weight:700}
 .muted{color:#6b7d90}
</style></head><body>
<header><h1>Manage Fobs</h1><a class="btn" href="/">&larr; Dashboard</a></header>
<div class="wrap">
 <h2>Enroll a new fob</h2>
 <div class="enroll" id="enroll"><span class="muted">Tap an un-enrolled fob on the reader; it will appear here.</span></div>
 <h2>Allowed fobs</h2>
 <table><thead><tr><th>UID</th><th>Name</th><th></th></tr></thead>
 <tbody id="list"><tr><td colspan="3" class="muted" style="padding:20px;text-align:center">No fobs enrolled yet.</td></tr></tbody></table>
 <h2>Unlock schedule</h2>
 <div class="enroll">
  <div><label><input type="checkbox" id="s_en"> Hold the door unlocked on a schedule</label></div>
  <div style="margin-top:10px">
   <input type="time" id="s_start" value="08:00"> &nbsp;to&nbsp; <input type="time" id="s_end" value="17:00">
   <span class="muted">(end before start = overnight window)</span>
  </div>
  <div style="margin-top:10px" id="s_days"></div>
  <div style="margin-top:12px"><button onclick="saveSched()">Save schedule</button>
   <span class="muted" id="s_stat" style="margin-left:10px"></span></div>
 </div>
</div>
<script>
let lastUnknownShown=null,typing=false,entries=[];
async function rn(uid){
 const cur=(entries.find(e=>e.uid===uid)||{}).name||'';
 const nm=prompt('New name for '+uid+':',cur);
 if(nm===null||!nm.trim())return;
 await fetch('/api/rename',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({uid:uid,name:nm.trim()})});
 refresh();}
function renderEnroll(unknown){
 const eb=document.getElementById('enroll');
 if(unknown){
  eb.innerHTML='<div>Unknown card: <span class="uid">'+unknown+'</span></div>'+
   '<div style="margin-top:10px"><input id="nm" placeholder="Name (e.g. John\'s fob)"> '+
   '<button onclick="add(\''+unknown+'\')">Add to allow-list</button></div>';
  const nm=document.getElementById('nm');
  nm.addEventListener('focus',()=>{typing=true;});
  nm.addEventListener('blur',()=>{typing=false;});
 } else { eb.innerHTML='<span class="muted">Tap an un-enrolled fob on the reader; it will appear here.</span>'; }
 lastUnknownShown=unknown||null;
}
async function refresh(){
 let d; try{const r=await fetch('/api/list');d=await r.json();}catch(e){return;}
 const lb=document.getElementById('list');
 if(!d.entries.length){lb.innerHTML='<tr><td colspan="3" class="muted" style="padding:20px;text-align:center">No fobs enrolled yet.</td></tr>';}
 else{entries=d.entries;let h='';d.entries.forEach(e=>{h+='<tr><td class="uid">'+e.uid+'</td><td>'+e.name+'</td>'+
   '<td><button onclick="rn(\''+e.uid+'\')">Rename</button> '+
   '<button class="rm" onclick="rm(\''+e.uid+'\')">Remove</button></td></tr>';});lb.innerHTML=h;}
 const unknown=d.unknown||null;
 if(unknown!==lastUnknownShown && !typing){renderEnroll(unknown);}
}
async function add(uid){const nm=document.getElementById('nm');const name=(nm&&nm.value)?nm.value:'Unnamed';
 await fetch('/api/add',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({uid:uid,name:name})});
 typing=false;lastUnknownShown=null;refresh();}
async function rm(uid){await fetch('/api/remove',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({uid:uid})});
 lastUnknownShown=null;refresh();}
const DAYS=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
(function(){let h='';DAYS.forEach((d,i)=>{h+='<label style="margin-right:10px"><input type="checkbox" class="s_d" data-i="'+i+'"> '+d+'</label>';});
 document.getElementById('s_days').innerHTML=h;})();
function mm(v){const p=v.split(':');return (+p[0])*60+(+p[1]);}
function hh(m){return String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%60).padStart(2,'0');}
async function loadSched(){try{const r=await fetch('/api/schedule');const d=await r.json();
 document.getElementById('s_en').checked=d.enabled;
 document.getElementById('s_start').value=hh(d.start);
 document.getElementById('s_end').value=hh(d.end);
 document.querySelectorAll('.s_d').forEach(cb=>{cb.checked=!!((d.days>>+cb.dataset.i)&1);});
 let s='Device time: '+d.now;
 if(d.active)s+=' — UNLOCKED now';
 if(!d.timeSynced)s+=' — time not synced; schedule inactive';
 document.getElementById('s_stat').textContent=s;
 }catch(e){}}
async function saveSched(){let days=0;
 document.querySelectorAll('.s_d').forEach(cb=>{if(cb.checked)days|=1<<+cb.dataset.i;});
 const st=document.getElementById('s_stat');
 try{
  const r=await fetch('/api/schedule',{method:'POST',headers:{'Content-Type':'application/json'},
   body:JSON.stringify({enabled:document.getElementById('s_en').checked,
    start:mm(document.getElementById('s_start').value),
    end:mm(document.getElementById('s_end').value),days:days})});
  if(!r.ok)throw 0;
  st.textContent='✓ Schedule saved';
  setTimeout(loadSched,1500);
 }catch(e){st.textContent='✗ Save failed — check connection and retry';}}
loadSched();
setInterval(refresh,1500);refresh();
</script><script src="/footer.js"></script></body></html>
)HTML";
