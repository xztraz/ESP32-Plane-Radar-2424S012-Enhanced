#include "services/radar_web_pages.h"
#include <Arduino.h>
namespace services::radar_web_pages {
const char kGeneral[] PROGMEM = R"rdr(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Plane Radar - General</title><link rel="stylesheet" href="/assets/radar.css"></head><body><h1>Plane Radar</h1><nav><a href="/general" class="active">General</a><a href="/aircraft" >Aircraft</a><a href="/wifi-settings" >Wi-Fi</a><a href="/status" >Status</a></nav>
<section><h2>Location and range</h2>
<div class="cols"><label>Longitude (degrees)<input type="number" step="0.000001" min="-180" max="180" id="lon"></label><label>Latitude (degrees)<input type="number" step="0.000001" min="-90" max="90" id="lat"></label></div>
<label>Radar third-ring range<select id="range"><option value="0">5 km</option><option value="1">10 km</option><option value="2">15 km</option><option value="3">25 km</option><option value="4">50 km</option><option value="5">75 km</option><option value="6">100 km</option></select></label>
<p class="meta">The BOOT button still cycles radar range as before.</p></section>
<section><h2>Display options</h2><div class="cols"><label>Radar color<select id="bgtheme"><option value="0">Green</option><option value="1">Red</option><option value="2">Amber</option><option value="3">Cyan</option><option value="4">Violet</option><option value="5">White</option></select></label><label>Radar pattern<select id="bgpattern"><option value="0">Original</option><option value="1">Classic PPI - bearing ticks</option><option value="2">Minimal - rings only</option><option value="3">Polar grid - 30 degrees</option><option value="4">Dashed rings</option><option value="5">Super Minimal</option></select></label></div><label class="check"><input type="checkbox" id="miles">Show distances in miles</label><label class="check"><input type="checkbox" id="runways">Show airport runways</label><label class="check"><input type="checkbox" id="metres">Show altitude in metres</label></section>
<button id="save" disabled>Save general settings</button><p id="msg" role="status">Loading settings...</p><script src="/assets/general.js" defer></script>
</body></html>)rdr";
const char kAircraft[] PROGMEM = R"rdr(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Plane Radar - Aircraft</title><link rel="stylesheet" href="/assets/radar.css"></head><body><h1>Plane Radar</h1><nav><a href="/general" >General</a><a href="/aircraft" class="active">Aircraft</a><a href="/wifi-settings" >Wi-Fi</a><a href="/status" >Status</a></nav>
<section><h2>Aircraft colors and priority</h2><p class="meta">Choose a hue and brightness. White aircraft can have colored trails: the trail follows the selected hue downward through darker shades.</p><div id="base"></div></section>
<section><h2>Label information</h2><p class="meta">Checked lines are shown whenever they fit. Labels automatically move closer and remove lines in order: type, altitude, registration. When needed, the existing smaller pixel font is used; labels are never scaled.</p>
<label class="check"><input type="checkbox" id="callsign">Callsign</label>
<label class="check"><input type="checkbox" id="registration">Registration</label>
<label class="check"><input type="checkbox" id="type">Aircraft type</label>
<label class="check"><input type="checkbox" id="altitude">Altitude</label>
<label>Label brightness: <strong id="label_brightness_value">100%</strong><input type="range" id="label_brightness" min="20" max="100" step="10" value="100"></label></section>
<section><h2>Trails and icons</h2>
<label>Trail length: <span id="trail_step">6</span><input type="range" id="trail" min="0" max="8" step="1" value="6"></label>
<p class="meta">0 = Off · 8 = Maximum</p>
<label>Trail start darkening (global): <strong id="trail_dark_label">0%</strong><input type="range" id="trail_dark" min="0" max="100" step="5" value="0"></label>
<p class="meta">0% starts at the aircraft palette color; higher values start farther down that color gradient. The aircraft stays at its selected brightness.</p>
<label>Trail fade curve (global)<select id="trail_curve"><option value="0">Smooth S (refined)</option><option value="1">Linear</option><option value="2">Phosphor</option><option value="3">Late Fade</option></select></label>
<canvas id="fade_curve_preview" width="320" height="110" aria-label="Preview of selected trail fading curve"></canvas>
<p class="meta">The curve controls how quickly trail colors move down the selected palette toward black. Crossings brighten by saturating additive blending.</p>
<label class="check"><input type="checkbox" id="dots">Show trail dots</label>
<label>Aircraft icon size: <strong id="size_label">100%</strong><input type="range" id="size" min="10" max="200" step="10" value="100"></label>
<p class="meta">Refresh adapts to range: 6.25 FPS at 5–10 km, down to 2 FPS at 100 km. Aircraft fetching remains every 5 seconds.</p></section>
<section><h2>Watched aircraft (maximum 50)</h2>
<p class="meta">Case-insensitive exact or trailing * prefix matching on callsign/registration. Exact matches win, then the longest prefix.</p>
<div class="cols"><label>Match field<select id="kind"><option value="C">Callsign</option><option value="R">Registration</option></select></label><label>Value or prefix*<input type="text" id="id" maxlength="13" placeholder="e.g. SAS*"></label></div>
<label>Optional display name<input type="text" id="name" maxlength="16" placeholder="My aircraft"></label>
<div class="color-option-row"><span>Aircraft color</span><button type="button" class="swatch" id="new_color" aria-label="Choose watched aircraft color"></button><label class="check"><input type="checkbox" id="priority">Priority label</label></div>
<button id="add" type="button">Add / update rule</button><p id="rule_msg" role="status" class="meta"></p><div id="rules"></div>
</section><button id="save" disabled>Save aircraft settings</button><p id="msg" role="status">Loading settings...</p>
<div id="palette_overlay" role="dialog" aria-modal="true" aria-label="Choose aircraft color"><div id="palette_dialog">
<div class="palette-header"><strong>Choose aircraft color</strong><button id="palette_close" type="button">Cancel</button></div>
<p class="meta">Left to right: hue. Top: white. Middle: vivid. Bottom: black. No dithering.</p>
<div class="palette-field"><canvas id="palette_canvas" width="256" height="256" aria-label="Hue and brightness color picker" tabindex="0"></canvas><span id="palette_marker" aria-hidden="true"></span></div>
<div class="palette-scale"><span>Hue → red · yellow · green · cyan · blue · purple</span></div>
<div class="palette-choice"><span id="palette_current" class="palette-sample"></span><span id="palette_code">#FFFFFF</span></div>
<label class="palette-preview-label">Trail preview (newest → oldest)</label><canvas id="trail_preview" width="256" height="24"></canvas>
<div class="palette-actions"><button id="palette_apply" type="button">Use this color</button></div>
</div></div>
<script src="/assets/aircraft.js" defer></script>
</body></html>)rdr";
const char kStatus[] PROGMEM = R"rdr(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Plane Radar - Status</title><link rel="stylesheet" href="/assets/radar.css"></head><body><h1>Plane Radar</h1><nav><a href="/general" >General</a><a href="/aircraft" >Aircraft</a><a href="/wifi-settings" >Wi-Fi</a><a href="/status" class="active">Status</a></nav>
<section><h2>Live diagnostics</h2><p class="meta">These values are fetched only while this page is open, once every 5 seconds.</p>
<table style="width:100%;text-align:left"><tbody>
<tr><th>Firmware</th><td id="version">-</td></tr>
<tr><th>CPU frequency now</th><td id="cpuNow">-</td></tr>
<tr><th>CPU frequency during last frame</th><td id="cpuFrame">-</td></tr>
<tr><th>Slow SPI transfers (&gt;=22 ms)</th><td id="slowSpi">-</td></tr><tr><th>Wi-Fi signal</th><td id="rssi">-</td></tr><tr><th>Free heap</th><td id="heap">-</td></tr><tr><th>Largest free block (TLS)</th><td id="largest">-</td></tr><tr><th>Minimum free heap</th><td id="minheap">-</td></tr><tr><th>Aircraft in feed</th><td id="count">-</td></tr><tr><th>Last ADS-B fetch</th><td id="fetch">-</td></tr><tr><th>Radar refresh target</th><td id="refresh">-</td></tr><tr><th>Full-screen buffers</th><td id="frames">-</td></tr><tr><th>Latest full frame</th><td id="render">-</td></tr><tr><th>Scene drawing (excluding transfer)</th><td id="slow">-</td></tr>
<tr><th>Flash background</th><td id="flashGrid">-</td></tr><tr><th>Flash background copy</th><td id="flashCopy">-</td></tr>

<tr><th>Grid and background</th><td id="gridUs">-</td></tr><tr><th>Runways (included in grid)</th><td id="runwaysUs">-</td></tr>
<tr><th>Aircraft projection</th><td id="projectionUs">-</td></tr><tr><th>Trails</th><td id="trailsUs">-</td></tr><tr><th>Edge dots</th><td id="dotsUs">-</td></tr>
<tr><th>Aircraft labels</th><td id="labelsUs">-</td></tr><tr><th>Aircraft icons</th><td id="iconsUs">-</td></tr>
<tr><th>SPI screen transfer</th><td id="transferUs">-</td></tr><tr><th>Last web response</th><td id="web">-</td></tr>
</tbody></table></section><p id="msg" role="status" class="meta"></p><script src="/assets/status.js" defer></script>
</body></html>)rdr";
const char kCss[] PROGMEM = R"rdr(:root{font-family:system-ui,-apple-system,Segoe UI,Arial,sans-serif;color-scheme:dark;background:#111b21;color:#e6f1f5}
*{box-sizing:border-box}body{margin:0 auto;padding:14px;max-width:740px;line-height:1.4}
h1{font-size:1.5rem;margin:.6em 0}h2{font-size:1.07rem;margin:.6em 0}p{color:#afc1c8}
a{color:#bce8ff}nav{display:flex;flex-wrap:wrap;gap:6px;margin:12px 0 20px}nav a{padding:9px 13px;border:1px solid #43626d;border-radius:8px;text-decoration:none;color:#f2f5f7}nav a.active{background:#21768a}
section{border:1px solid #3a5461;border-radius:10px;padding:14px;margin:14px 0}label{display:block;margin:9px 0;font-weight:500}
input[type=text],input[type=number],select{background:#101820;border:1px solid #60808e;border-radius:5px;color:#f6f9fa;min-height:38px;padding:6px 8px;max-width:100%;width:100%}
input[type=checkbox]{width:18px;height:18px;vertical-align:middle;margin-right:7px}button{background:#267d91;border:1px solid #4695a8;border-radius:7px;color:white;cursor:pointer;padding:9px 14px;font:inherit}button:hover{background:#3694a8}button:disabled{opacity:.5;cursor:default}
.row{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin:10px 0}.row>span:first-child{flex:1;min-width:125px}.row label{margin:0}.cols{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:12px}.check{display:block;line-height:1.6}.swatch{width:54px;height:34px;flex:none;border:2px solid #888;border-radius:6px;box-shadow:0 0 0 1px #000;cursor:pointer;padding:0}.meta{font-size:.85rem;color:#aec1c9}#msg{min-height:1.3em;color:#8df4b7}
#fade_curve_preview{display:block;width:min(100%,400px);height:110px;background:#101820;border:1px solid #666;border-radius:6px;margin:6px 0 10px}
#palette_overlay{display:none;position:fixed;inset:0;background:#000b;z-index:10;align-items:center;justify-content:center;padding:14px}
#palette_dialog{background:#1b2931;border:1px solid #666;border-radius:10px;width:min(420px,96vw);max-height:94vh;overflow-y:auto;padding:16px}
.palette-header{display:flex;align-items:center;justify-content:space-between;gap:12px}.palette-header button{padding:6px 12px}
.palette-field{position:relative;width:100%;aspect-ratio:1;touch-action:none;border:1px solid #777;border-radius:6px;overflow:hidden;background:#000}
#palette_canvas{display:block;width:100%;height:100%;image-rendering:pixelated;cursor:crosshair;touch-action:none}
#palette_marker{position:absolute;width:16px;height:16px;border-radius:50%;border:2px solid #888;box-shadow:0 0 0 1px #111;pointer-events:none;transform:translate(-50%,-50%)}
.palette-scale{display:flex;justify-content:space-between;color:#b7b7b7;font-size:.73rem;padding-top:5px}
.palette-choice{display:flex;align-items:center;gap:12px;margin:12px 0}.palette-sample{width:46px;height:30px;border:2px solid #888;border-radius:5px}
.palette-preview-label{font-size:.85rem;color:#bcbcbc;margin:10px 0 5px}#trail_preview{display:block;width:100%;height:24px;border:1px solid #777;border-radius:4px;image-rendering:pixelated}
.palette-actions{display:flex;justify-content:flex-end;margin-top:14px}.palette-actions button{min-width:130px}
.color-option-row{display:flex;align-items:center;gap:10px;margin:10px 0;flex-wrap:wrap}.color-option-row>span{flex:1;min-width:130px}.color-option-row label{margin:0}
.rule{display:flex;align-items:center;flex-wrap:wrap;gap:7px;padding:9px 0;border-bottom:1px solid #38505c}.rule>span{flex:1;min-width:100px;overflow-wrap:anywhere;font-size:.89rem}.rule button.remove{background:#663944;border-color:#8d4b5c;font-size:.85rem;padding:7px}
input[type=range]{width:100%}pre{white-space:pre-wrap;overflow-wrap:anywhere}
/* Shared appearance for WiFiManager's own /wifi and /wifinoscan forms. */
body.plane-radar-wifi{background:#111b21!important;color:#e6f1f5!important;max-width:740px!important;line-height:1.4}
body.plane-radar-wifi .wrap,body.plane-radar-wifi form{color:#e6f1f5}
body.plane-radar-wifi .wrap{background:#16242d;border:1px solid #3a5461;border-radius:10px;padding:14px;margin:14px auto;max-width:100%}
body.plane-radar-wifi h1,body.plane-radar-wifi h2,body.plane-radar-wifi h3{color:#e6f1f5}
body.plane-radar-wifi p,body.plane-radar-wifi label{color:#c1d1d8}
body.plane-radar-wifi input,body.plane-radar-wifi select{background:#101820;color:#f6f9fa;border-color:#60808e}
body.plane-radar-wifi button,body.plane-radar-wifi input[type=submit]{background:#267d91;color:#fff;border:1px solid #4695a8;border-radius:7px}
body.plane-radar-wifi nav#plane-radar-links{display:flex;flex-wrap:wrap;gap:6px;margin:12px 0 20px}
body.plane-radar-wifi nav#plane-radar-links a{padding:9px 13px;border:1px solid #43626d;border-radius:8px;text-decoration:none;color:#f2f5f7}
body.plane-radar-wifi nav#plane-radar-links a.active{background:#21768a}
)rdr";
const char kGeneralJs[] PROGMEM = R"rdr(
'use strict';
const get=id=>document.getElementById(id);
const msg=get('msg'),save=get('save');
async function postSettingsWhenReady(url,body){
 for(let attempt=0;attempt<18;attempt++){
  const ready=await fetch('/api/save-ready',{cache:'no-store'});
  if(ready.ok){
   const response=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
   if(response.status!==503)return response;
  }else if(ready.status!==503){throw Error('Save readiness HTTP '+ready.status);}
  msg.textContent='Waiting for aircraft download to finish...';
  await new Promise(resolve=>setTimeout(resolve,650));
 }
 throw Error('ADS-B download still busy; please try saving again.');
}
async function load(){
 try{const r=await fetch('/api/general',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);
 const d=await r.json();
 get('lat').value=d.lat;get('lon').value=d.lon;get('range').value=d.range;get('bgtheme').value=d.bgtheme||0;get('bgpattern').value=d.bgpattern||0;
 get('miles').checked=d.miles;get('runways').checked=d.runways;get('metres').checked=d.metres;
 save.disabled=false;msg.textContent='';
 }catch(e){msg.textContent='Could not load settings: '+e.message;}
}
save.addEventListener('click',async()=>{
 save.disabled=true;msg.textContent='Saving...';
 try{const v=new URLSearchParams({lat:get('lat').value,lon:get('lon').value,range:get('range').value,bgtheme:get('bgtheme').value,bgpattern:get('bgpattern').value,
 miles:get('miles').checked?'1':'0',runways:get('runways').checked?'1':'0',metres:get('metres').checked?'1':'0'});
 const r=await postSettingsWhenReady('/api/general',v.toString());
 if(!r.ok)throw Error(await r.text());msg.textContent='General settings saved.';
 }catch(e){msg.textContent='Save failed: '+e.message;}finally{save.disabled=false;}
});
load();
)rdr";
const char kAircraftJs[] PROGMEM = R"rdr(
'use strict';
const $=id=>document.getElementById(id);
const msg=$('msg'),ruleMsg=$('rule_msg');
async function postSettingsWhenReady(url,body){
 for(let attempt=0;attempt<18;attempt++){
  const ready=await fetch('/api/save-ready',{cache:'no-store'});
  if(ready.ok){
   const response=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
   if(response.status!==503)return response;
  }else if(ready.status!==503){throw Error('Save readiness HTTP '+ready.status);}
  msg.textContent='Waiting for aircraft download to finish...';
  await new Promise(resolve=>setTimeout(resolve,650));
 }
 throw Error('ADS-B download still busy; please try saving again.');
}

const overlay=$('palette_overlay'),canvas=$('palette_canvas');
const marker=$('palette_marker'),trailPreview=$('trail_preview');
let base=[],opts=['15','0','100'],list=[],newColor='';
let activeChange=null,currentHue=0,currentShade=102;
const fadeCurvesQ12=[
 [4096, 4083, 4047, 3988, 3909, 3811, 3695, 3564, 3418, 3261, 3092, 2915, 2730, 2539, 2344, 2147, 1949, 1752, 1557, 1366, 1181, 1004, 835, 678, 532, 401, 285, 187, 108, 49, 13, 0],
 [4096, 3964, 3832, 3700, 3567, 3435, 3303, 3171, 3039, 2907, 2775, 2643, 2510, 2378, 2246, 2114, 1982, 1850, 1718, 1586, 1453, 1321, 1189, 1057, 925, 793, 661, 529, 396, 264, 132, 0],
 [4096, 3223, 2903, 2664, 2466, 2294, 2140, 1999, 1869, 1748, 1634, 1526, 1424, 1326, 1232, 1141, 1054, 970, 889, 810, 733, 658, 586, 515, 446, 378, 312, 247, 183, 121, 60, 0],
 [4096, 4096, 4095, 4092, 4087, 4079, 4066, 4049, 4026, 3996, 3959, 3913, 3858, 3794, 3719, 3632, 3533, 3421, 3294, 3153, 2996, 2823, 2632, 2423, 2195, 1948, 1679, 1390, 1078, 743, 384, 0]
];
function drawFadeCurvePreview(){
 const ctx=$('fade_curve_preview').getContext('2d'),w=320,h=110;
 const p=fadeCurvesQ12[Number($('trail_curve').value)||0]||fadeCurvesQ12[0];
 ctx.clearRect(0,0,w,h);ctx.fillStyle='#101820';ctx.fillRect(0,0,w,h);
 ctx.strokeStyle='#555';ctx.lineWidth=1;
 ctx.beginPath();ctx.moveTo(8,h-9);ctx.lineTo(w-8,h-9);ctx.moveTo(8,8);ctx.lineTo(8,h-9);ctx.stroke();
 ctx.beginPath();for(let i=0;i<32;i++){
  const x=8+i*(w-16)/31,y=(h-9)-(p[i]/4096)*(h-17);
  if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);
 }
 ctx.strokeStyle='#e4e4e4';ctx.lineWidth=2.5;ctx.stroke();
}

// Exact integer RGB332 field used by the firmware: same six hue sectors,
// white -> vivid -> black shading and rounding to R3/G3/B2.
function rgb332At(hue,shade){
 const h=hue*6,sector=h>>8,f=h&255;
 let r=0,g=0,b=0;
 switch(sector){case 0:r=255;g=f;break;case 1:r=255-f;g=255;break;
 case 2:g=255;b=f;break;case 3:g=255-f;b=255;break;
 case 4:r=f;b=255;break;default:r=255;b=255-f;break;}
 const adjust=v=>shade<=102?255-Math.floor(((255-v)*shade+51)/102):
                             Math.floor((v*(255-shade)+76)/153);
 r=adjust(r);g=adjust(g);b=adjust(b);
 return ((Math.floor((r*7+127)/255)<<5)|
         (Math.floor((g*7+127)/255)<<2)|Math.floor((b*3+127)/255));
}
function rgbForByte(p){return [Math.floor(((p>>5&7)*255+3)/7),
 Math.floor(((p>>2&7)*255+3)/7),(p&3)*85];}
function hex2(v){return v.toString(16).padStart(2,'0').toUpperCase();}
function selected(h,y){const [r,g,b]=rgbForByte(rgb332At(h,y));
 return hex2(r)+hex2(g)+hex2(b)+hex2(h)+hex2(y);}
function colorHex(code){return '#'+code.slice(0,6);}
function coords(code){return /^[0-9A-F]{10}$/i.test(code)?
 [parseInt(code.slice(6,8),16),parseInt(code.slice(8,10),16)]:[0,102];}
function fadeBrightness(age){
 const arr=fadeCurvesQ12[Number($('trail_curve').value)||0]||fadeCurvesQ12[0];
 const pos=Math.max(0,Math.min(31,age)),j=Math.floor(pos),frac=pos-j;
 return (arr[j]*(1-frac)+arr[Math.min(31,j+1)]*frac)/4096;
}
function trailShade(y,age){
 const pct=Number($('trail_dark').value),start=y+Math.floor(((255-y)*pct+50)/100);
 return Math.min(255,start+Math.round((255-start)*(1-fadeBrightness(age))));
}
function drawPalette(){const ctx=canvas.getContext('2d');const data=ctx.createImageData(256,256);
 for(let y=0;y<256;y++)for(let x=0;x<256;x++){
  const [r,g,b]=rgbForByte(rgb332At(x,y)),i=(y*256+x)*4;
  data.data[i]=r;data.data[i+1]=g;data.data[i+2]=b;data.data[i+3]=255;
 }ctx.putImageData(data,0,0);
}
function drawTrailPreview(){const ctx=trailPreview.getContext('2d'),d=ctx.createImageData(256,24);
 for(let x=0;x<256;x++){
  const shade=trailShade(currentShade,x*31/255),[r,g,b]=rgbForByte(rgb332At(currentHue,shade));
  for(let y=0;y<24;y++){const i=(y*256+x)*4;
   d.data[i]=r;d.data[i+1]=g;d.data[i+2]=b;d.data[i+3]=255;}
 }ctx.putImageData(d,0,0);
}
function showSelected(){const token=selected(currentHue,currentShade);
 marker.style.left=(currentHue/255*100)+'%';marker.style.top=(currentShade/255*100)+'%';
 $('palette_current').style.backgroundColor=colorHex(token);
 $('palette_code').textContent=colorHex(token).toUpperCase()+' · RGB332 '+hex2(rgb332At(currentHue,currentShade));
 drawTrailPreview();
}
function chooser(code,callback){[currentHue,currentShade]=coords(code);
 activeChange=callback;showSelected();overlay.style.display='flex';}
function closePalette(){overlay.style.display='none';activeChange=null;}
$('palette_close').onclick=closePalette;
overlay.addEventListener('click',e=>{if(e.target===overlay)closePalette();});
document.addEventListener('keydown',e=>{if(e.key==='Escape')closePalette();});
function movePicker(e){const r=canvas.getBoundingClientRect();
 currentHue=Math.max(0,Math.min(255,Math.round((e.clientX-r.left)/r.width*255)));
 currentShade=Math.max(0,Math.min(255,Math.round((e.clientY-r.top)/r.height*255)));
 showSelected();}
canvas.addEventListener('pointerdown',e=>{canvas.setPointerCapture(e.pointerId);movePicker(e);});
canvas.addEventListener('pointermove',e=>{if(e.buttons)movePicker(e);});
canvas.addEventListener('keydown',e=>{if(e.key==='ArrowLeft')currentHue=Math.max(0,currentHue-1);
 else if(e.key==='ArrowRight')currentHue=Math.min(255,currentHue+1);
 else if(e.key==='ArrowUp')currentShade=Math.max(0,currentShade-1);
 else if(e.key==='ArrowDown')currentShade=Math.min(255,currentShade+1);
 else return;e.preventDefault();showSelected();});
$('palette_apply').onclick=()=>{if(activeChange)activeChange(selected(currentHue,currentShade));closePalette();};
drawPalette();
function swatch(code,changed){const b=document.createElement('button');b.type='button';b.className='swatch';
 function paint(value){b.style.backgroundColor=colorHex(value);b.title=colorHex(value).toUpperCase();}
 paint(code);b.onclick=()=>chooser(code,value=>{code=value;paint(value);changed(value);});return b;}
function paintBase(){const root=$('base');root.textContent='';
 [['Normal aircraft',0],['Military aircraft',1],['Helicopters',2]].forEach(([title,idx])=>{
  const row=document.createElement('div');row.className='color-option-row';
  const t=document.createElement('span');t.textContent=title;row.appendChild(t);
  row.appendChild(swatch(base[idx],c=>base[idx]=c));
  if(idx){const label=document.createElement('label');label.className='check';const box=document.createElement('input');box.type='checkbox';
   const pi=idx===1?3:4;box.checked=base[pi]==='1';box.onchange=()=>base[pi]=box.checked?'1':'0';
   label.appendChild(box);label.appendChild(document.createTextNode('Priority labels'));row.appendChild(label);}
  root.appendChild(row);
 });}
function parseWatches(encoded){return encoded.split(';').filter(Boolean).map(raw=>{
 const f=raw.split('|'),key=f[0].split(':');return {kind:key[0],id:key.slice(1).join(':'),
 color:(f[1]||selected(128,102)).toUpperCase(),name:f[2]||'',priority:f[3]==='1'};
 }).filter(e=>e.kind==='C'||e.kind==='R');}
function encodeWatches(){return list.map(e=>e.kind+':'+e.id+'|'+e.color+'|'+e.name+'|'+(e.priority?'1':'0')).join(';');}
function paintRules(){const root=$('rules');root.textContent='';
 list.forEach((e,i)=>{const row=document.createElement('div');row.className='rule';
  const t=document.createElement('span');t.textContent=(e.kind==='C'?'Callsign ':'Reg ')+e.id+(e.name?' — '+e.name:'');row.appendChild(t);
  row.appendChild(swatch(e.color,c=>e.color=c));
  const label=document.createElement('label');const cb=document.createElement('input');cb.type='checkbox';cb.checked=e.priority;cb.onchange=()=>e.priority=cb.checked;
  label.appendChild(cb);label.appendChild(document.createTextNode('Priority'));row.appendChild(label);
  const del=document.createElement('button');del.type='button';del.className='remove';del.textContent='Remove';
  del.onclick=()=>{list.splice(i,1);paintRules();};row.appendChild(del);root.appendChild(row);
 });}
function valid(e){if(!/^[A-Za-z0-9 _-]{0,16}$/.test(e.name))return false;
 const wild=e.id.endsWith('*'),prefix=wild?e.id.slice(0,-1):e.id;
 if(prefix.includes('*')||prefix.length<(wild?1:2))return false;
 return e.kind==='R'?/^[A-Z0-9-]+$/.test(prefix)&&prefix.length<=12:
 e.kind==='C'&&/^[A-Z0-9]+$/.test(prefix)&&prefix.length<=8;}
newColor=selected(128,102);
$('new_color').style.backgroundColor=colorHex(newColor);
$('new_color').onclick=()=>chooser(newColor,c=>{newColor=c;$('new_color').style.backgroundColor=colorHex(c);});
$('add').onclick=()=>{const e={kind:$('kind').value,id:$('id').value.trim().toUpperCase(),name:$('name').value.trim(),color:newColor,priority:$('priority').checked};
 if(!valid(e)){ruleMsg.textContent='Invalid pattern: use callsign or registration, exact or trailing *.';return;}
 const index=list.findIndex(x=>x.kind===e.kind&&x.id===e.id);
 if(index<0&&list.length>=50){ruleMsg.textContent='Maximum 50 watched aircraft.';return;}
 const old=index>=0?list[index]:null;if(index>=0)list[index]=e;else list.push(e);
 if(encodeWatches().length>3700){if(index>=0)list[index]=old;else list.pop();ruleMsg.textContent='Watchlist too long; shorten names.';return;}
 $('id').value='';$('name').value='';ruleMsg.textContent='Rule added. Press Save aircraft settings to persist.';paintRules();};
$('size').addEventListener('input',()=>{$('size_label').textContent=$('size').value+'%';});
$('trail').addEventListener('input',()=>{$('trail_step').textContent=$('trail').value;});
$('trail_dark').addEventListener('input',()=>{
 $('trail_dark_label').textContent=$('trail_dark').value+'%';
 if(overlay.style.display==='flex')drawTrailPreview();
});
$('trail_curve').addEventListener('change',()=>{
 drawFadeCurvePreview();
 if(overlay.style.display==='flex')drawTrailPreview();
});
function updatePatternHint(){$('id').placeholder=$('kind').value==='R'?'e.g. SE-*':'e.g. SAS*';}
$('kind').addEventListener('change',updatePatternHint);updatePatternHint();
async function load(){try{const r=await fetch('/api/aircraft',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);
 const parts=(await r.text()).split('\n');if(parts.length<4)throw Error('Incomplete response');
 base=parts[0].split(',');if(base.length===4)base.push('0');
 opts=parts[1].split(',');list=parseWatches(parts[3]||'');
 const savedDark=Number(parts[4]||'15');
 $('trail_dark').value=String(Number.isFinite(savedDark)?Math.max(0,Math.min(100,Math.round(savedDark/5)*5)):15);
 $('trail_dark_label').textContent=$('trail_dark').value+'%';
 const savedCurve=Number(parts[5]||'2');
 $('trail_curve').value=String(Number.isInteger(savedCurve)&&savedCurve>=0&&savedCurve<4?savedCurve:2);
 drawFadeCurvePreview();
 $('trail').value=String(Math.max(0,Math.min(8,Math.round(Number(parts[2])/4))));
 $('trail_step').textContent=$('trail').value;
 $('size').value=opts[2];$('size_label').textContent=opts[2]+'%';
 const labelBrightness=Number(opts[3]);
 $('label_brightness').value=String(Number.isInteger(labelBrightness)&&labelBrightness>=20&&labelBrightness<=100?labelBrightness:100);
 $('label_brightness_value').textContent=$('label_brightness').value+'%';
 $('dots').checked=opts[1]==='1';
 [['callsign',1],['registration',2],['type',4],['altitude',8]].forEach(([id,mask])=>$(id).checked=(Number(opts[0])&mask)!==0);
 paintBase();paintRules();$('save').disabled=false;msg.textContent='';
 }catch(e){msg.textContent='Could not load aircraft settings: '+e.message;}}
$('label_brightness').addEventListener('input',()=>{$('label_brightness_value').textContent=$('label_brightness').value+'%';});
$('save').onclick=async()=>{const mask=[['callsign',1],['registration',2],['type',4],['altitude',8]].reduce((a,[id,b])=>a|($(id).checked?b:0),0);
 const trailStep=Number($('trail').value);
 if(!Number.isInteger(trailStep)||trailStep<0||trailStep>8){msg.textContent='Trail length must be 0 to 8.';return;}
 const trailDark=Number($('trail_dark').value);
 if(!Number.isInteger(trailDark)||trailDark<0||trailDark>100){msg.textContent='Trail start darkening must be 0 to 100%.';return;}
 const trailCurve=Number($('trail_curve').value);
 if(!Number.isInteger(trailCurve)||trailCurve<0||trailCurve>3){msg.textContent='Invalid trail fade curve.';return;}
 const trail=trailStep*4,watches=encodeWatches();if(watches.length>3700){msg.textContent='Watchlist is too long.';return;}
 opts=[String(mask),$('dots').checked?'1':'0',$('size').value,$('label_brightness').value];
 const data=new URLSearchParams({colors:base.join(','),options:opts.join(','),trail:String(trail),trailDark:String(trailDark),trailCurve:String(trailCurve),watchlist:watches});
 $('save').disabled=true;msg.textContent='Saving...';try{
 const response=await postSettingsWhenReady('/api/aircraft',data.toString());
 if(!response.ok)throw Error(await response.text());msg.textContent='Aircraft settings saved.';
 }catch(e){msg.textContent='Save failed: '+e.message;}finally{$('save').disabled=false;}};
load();
)rdr";
const char kStatusJs[] PROGMEM = R"rdr(
'use strict';
async function tick(){
 try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);const v=await r.json();
 const fields={version:v.version,
 cpuNow:v.cpuMhz+' MHz',cpuFrame:v.frameCpuStart+' / '+v.frameCpuEnd+' MHz (start/end)',
 slowSpi:v.slowSpiCount+' total'+(v.slowSpiCount ? ' · last '+(v.slowSpiUs/1000).toFixed(1)+' ms at '+v.slowSpiCpuStart+'/'+v.slowSpiCpuEnd+' MHz' : ''),
 rssi:v.rssi+' dBm',heap:v.heap+' bytes',minheap:v.minheap+' bytes',count:v.count,
 fetch:v.fetchAgeMs===null?'No successful fetch yet':(v.fetchAgeMs/1000).toFixed(1)+' s ago',
 refresh:(1000/v.frameIntervalMs).toLocaleString('en-US',{maximumFractionDigits:3})+' FPS ('+v.frameIntervalMs+' ms)',
 render:(v.renderUs/1000).toFixed(1)+' ms',slow:(v.slowUs/1000).toFixed(1)+' ms',
 web:(v.webUs/1000).toFixed(1)+' ms',frames:v.frames+' (RGB332)',largest:v.largestHeap+' bytes',
 flashGrid:v.flashGrid?'Active (flash)':'Invalid asset fallback',flashCopy:(v.flashCopyUs/1000).toFixed(2)+' ms',
 gridUs:(v.gridUs/1000).toFixed(1)+' ms',runwaysUs:(v.runwaysUs/1000).toFixed(1)+' ms',
 projectionUs:(v.projectionUs/1000).toFixed(1)+' ms',trailsUs:(v.trailsUs/1000).toFixed(1)+' ms',
 dotsUs:(v.dotsUs/1000).toFixed(1)+' ms',labelsUs:(v.labelsUs/1000).toFixed(1)+' ms',
 iconsUs:(v.iconsUs/1000).toFixed(1)+' ms',transferUs:(v.transferUs/1000).toFixed(1)+' ms'};
 for(const [k,val] of Object.entries(fields))document.getElementById(k).textContent=val;
 document.getElementById('msg').textContent='';
 }catch(e){document.getElementById('msg').textContent='Status unavailable: '+e.message;}
}
tick();setInterval(()=>{if(!document.hidden)tick()},5000);
)rdr";
}
