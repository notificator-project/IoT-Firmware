#pragma once

#include <Arduino.h>

/**
 * Offline-safe presentation and small interaction enhancements for the
 * WiFiManager captive portal.
 *
 * The portal intentionally embeds all CSS and JavaScript. A phone connected to
 * the ESP32 access point usually has no internet connection, so external fonts,
 * images, and scripts would make setup unreliable.
 */
static const char NOTIFICATOR_PORTAL_HEAD[] PROGMEM = R"HTML(
<style>
:root{--ink:#0b1020;--paper:#f4f7fc;--surface:#fff;--surface-2:#f8fafc;--blue:#2459dc;--blue-2:#3f7cff;--line:#dce4f0;--muted:#5d6b82;--cyan:#0891b2;--danger:#b42318}
*{box-sizing:border-box}
html,body{width:100%;min-height:100%;margin:0}
body{padding:24px 16px;color:var(--ink);background:radial-gradient(circle at 12% 4%,rgba(63,124,255,.2),transparent 28rem),radial-gradient(circle at 94% 92%,rgba(14,165,233,.14),transparent 24rem),var(--paper);font-family:Inter,ui-sans-serif,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
.wrap{width:min(100%,640px);max-width:none;margin:0 auto;padding:clamp(22px,5vw,38px);border:1px solid rgba(11,16,32,.09);border-radius:26px;background:rgba(255,255,255,.94);box-shadow:0 24px 70px rgba(24,39,75,.13)}
.wrap:before{display:flex;align-items:center;min-height:42px;margin:0 0 28px;padding-left:54px;color:var(--ink);background:linear-gradient(145deg,var(--blue-2),var(--blue)) 0 0/42px 42px no-repeat;border-radius:12px;content:"Notificator";font-size:21px;font-weight:850;letter-spacing:-.035em}
h1{margin:0 0 12px;color:var(--ink);font-size:clamp(32px,8vw,48px);font-weight:850;letter-spacing:-.05em;line-height:1}
h1:before{display:block;margin:0 0 10px;color:var(--blue);content:"DEVICE SETUP";font-size:11px;font-weight:850;letter-spacing:.14em;line-height:1.2}
h1:after{display:block;margin-top:14px;color:var(--muted);content:"Connect Wi-Fi and your HiveMQ Cloud cluster.";font-size:15px;font-weight:500;letter-spacing:0;line-height:1.55}
h2,h3,h4{color:var(--ink)}
form{margin-top:24px}
label{display:block;margin:14px 0 7px;color:var(--ink);font-size:14px;font-weight:750}
input,select{width:100%;min-height:50px;margin:0!important;padding:0 14px!important;border:1px solid #cbd6e6!important;border-radius:14px!important;outline:0;background:#fff!important;color:var(--ink)!important;font:inherit}
input:focus,select:focus{border-color:var(--blue)!important;box-shadow:0 0 0 4px rgba(36,89,220,.13)!important}
input[type=checkbox]{width:18px;min-height:18px;margin:12px 7px 0 0!important;padding:0!important;accent-color:var(--blue)}
input[type=checkbox]+label{display:inline-block;margin:10px 0;color:var(--muted);font-size:13px}
button,.btn{width:100%;min-height:52px;margin-top:14px;padding:0 18px;border:0!important;border-radius:999px!important;background:linear-gradient(145deg,var(--blue-2),var(--blue))!important;box-shadow:0 12px 28px rgba(36,89,220,.22);color:#fff!important;font:inherit;font-weight:850!important;cursor:pointer}
button:hover,.btn:hover{filter:brightness(.96)}
button:focus-visible,.btn:focus-visible,a:focus-visible{outline:3px solid rgba(36,89,220,.35);outline-offset:3px}
button:disabled{cursor:wait;opacity:.7}
a{color:var(--blue);font-weight:750}
hr{height:1px;margin:26px 0;border:0;background:var(--line)}
.msg{margin:18px 0;padding:15px 17px;border:1px solid #bae6fd;border-radius:14px;background:#f0f9ff;color:#0c4a6e;line-height:1.5}
.msg.S,.msg.P{border-color:#bbf7d0;background:#f0fdf4;color:#166534}
.msg.D{border-color:#fecaca;background:#fef2f2;color:var(--danger)}
.notif-group{display:block;margin-top:26px;padding:20px;border:1px solid var(--line);border-radius:18px;background:var(--surface-2)}
.notif-section{display:grid;gap:5px;margin:0 0 14px}
.notif-section strong{color:var(--ink);font-size:18px;letter-spacing:-.02em}
.notif-section span{color:var(--muted);font-size:13px;line-height:1.5}
.notif-provider{display:inline-flex;width:max-content;margin-bottom:4px;padding:4px 8px;border-radius:999px;background:#e0f2fe;color:#075985!important;font-size:10px!important;font-weight:850;letter-spacing:.08em;text-transform:uppercase}
.notif-advanced,.network-advanced{margin-top:22px;border:1px solid var(--line);border-radius:18px;background:var(--surface-2);overflow:hidden}
.notif-advanced summary,.network-advanced summary{padding:17px 20px;color:var(--ink);cursor:pointer;font-size:15px;font-weight:800;list-style-position:inside}
.notif-advanced[open] summary,.network-advanced[open] summary{border-bottom:1px solid var(--line)}
.notif-advanced .notif-section{padding:18px 20px 0}
.notif-advanced>label,.notif-advanced>input{margin-right:20px!important;margin-left:20px!important}
.notif-advanced>input{width:calc(100% - 40px)}
.network-advanced-note{margin:0;padding:16px 20px 2px;color:var(--muted);font-size:13px;line-height:1.5}
.network-advanced>label,.network-advanced>input{margin-right:20px!important;margin-left:20px!important}
.network-advanced>input{width:calc(100% - 40px)}
.network-advanced>br{display:none}
.wifi-network{display:grid;grid-template-columns:minmax(0,1fr) auto;align-items:center;min-height:48px;margin:7px 0;border:1px solid var(--line);border-radius:14px;background:#fff;overflow:hidden}
.wifi-network:hover{border-color:#93b4ff;background:#f8fbff}
.wifi-network a[data-ssid]{display:flex;align-items:center;align-self:stretch;min-width:0;padding:0 13px;color:var(--ink);font-size:14px;text-decoration:none}
.wifi-network>.q{display:none!important}
.wifi-network-meta{display:flex;align-items:center;justify-content:flex-end;gap:5px;min-width:max-content;margin-right:12px;color:var(--muted);font-size:12px;font-weight:700;line-height:1}
.wifi-network-meta svg{display:block;width:17px;height:17px;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.wifi-network-meta .wifi-signal{color:var(--blue)}
.wifi-network-meta.q-0 .wifi-signal{opacity:.28}.wifi-network-meta.q-1 .wifi-signal{opacity:.45}.wifi-network-meta.q-2 .wifi-signal{opacity:.64}.wifi-network-meta.q-3 .wifi-signal{opacity:.82}
.wifi-network-meta .wifi-lock{width:14px;height:14px;color:#64748b}
.D{color:var(--danger)}
.c{text-align:left}
.footer-note{margin:22px 0 0;color:var(--muted);font-size:12px;line-height:1.5;text-align:center}
.portal-menu{display:grid;gap:12px;margin-top:24px}
.portal-menu-card{display:grid;grid-template-columns:auto minmax(0,1fr) auto;align-items:center;gap:13px;min-height:70px;padding:13px 15px;border:1px solid var(--line);border-radius:16px;background:#fff;color:var(--ink);text-decoration:none}
.portal-menu-card:hover{border-color:#93b4ff;background:#f8fbff}
.portal-menu-icon{display:grid;width:38px;height:38px;place-items:center;border-radius:12px;background:#eaf1ff;color:var(--blue);font-size:17px;font-weight:850}
.portal-menu-card strong,.portal-menu-card small{display:block}.portal-menu-card strong{font-size:15px}.portal-menu-card small{margin-top:3px;color:var(--muted);font-size:12px;font-weight:500;line-height:1.35}
.portal-menu-arrow{color:var(--blue);font-size:22px}
.portal-menu-exit{display:block;padding:8px;color:var(--muted);font-size:13px;text-align:center;text-decoration:none}
.portal-menu-exit:hover{color:var(--ink)}
.saving-state{display:flex;align-items:center;gap:9px;margin-top:14px;color:var(--muted);font-size:13px}
.saving-state:before{width:14px;height:14px;border:2px solid #bfdbfe;border-top-color:var(--blue);border-radius:50%;content:"";animation:notificator-spin .8s linear infinite}
@keyframes notificator-spin{to{transform:rotate(360deg)}}
@media(max-width:480px){body{padding:14px 10px}.wrap{padding:22px 17px;border-radius:22px}.wrap:before{margin-bottom:24px}.notif-group{padding:16px}.notif-advanced>label,.notif-advanced>input,.network-advanced>label,.network-advanced>input{margin-right:16px!important;margin-left:16px!important}.notif-advanced>input,.network-advanced>input{width:calc(100% - 32px)}}
@media(prefers-reduced-motion:reduce){*{scroll-behavior:auto!important}.saving-state:before{animation:none}}
</style>
<script>
document.addEventListener("DOMContentLoaded",function(){
var wrap=document.querySelector(".wrap");
if(wrap&&!document.querySelector(".footer-note")){
var foot=document.createElement("p");
foot.className="footer-note";
foot.textContent="Your credentials stay on this device or WordPress site. Notificator does not provide an MQTT broker.";
wrap.appendChild(foot);
}
function portalIcon(type){
var ns="http://www.w3.org/2000/svg";
var svg=document.createElementNS(ns,"svg");
var path=document.createElementNS(ns,"path");
svg.setAttribute("viewBox","0 0 24 24");
svg.setAttribute("aria-hidden","true");
svg.setAttribute("focusable","false");
svg.setAttribute("class",type==="lock"?"wifi-lock":"wifi-signal");
path.setAttribute("d",type==="lock"?"M8 10V7.5a4 4 0 0 1 8 0V10M6.5 10h11v10h-11z":"M3 9a13 13 0 0 1 18 0M6.5 12.5a8 8 0 0 1 11 0M10 16a3 3 0 0 1 4 0M12 20h.01");
svg.appendChild(path);
return svg;
}
document.querySelectorAll("a[data-ssid]").forEach(function(link){
var row=link.parentElement;
if(!row)return;
var signal=row.querySelector(".q[role='img']");
var percentage=row.querySelector(".q:not([role='img'])");
var secured=!!(signal&&signal.classList.contains("l"));
var strength=percentage&&percentage.textContent?percentage.textContent.trim():(signal&&signal.getAttribute("aria-label")||"");
var quality="q-4";
["q-0","q-1","q-2","q-3","q-4"].some(function(name){if(signal&&signal.classList.contains(name)){quality=name;return true;}return false;});
var meta=document.createElement("span");
// WiFiManager's c(this) handler checks the selected link's next sibling for
// its `l` security marker before enabling the Wi-Fi password input. Preserve
// that marker when replacing the stock signal elements with our metadata UI.
meta.className="wifi-network-meta "+quality+(secured?" l":"");
meta.setAttribute("role","img");
meta.setAttribute("aria-label",(strength?strength+", ":"")+(secured?"secured network":"open network"));
if(strength&&!(percentage&&percentage.classList.contains("h"))){var value=document.createElement("span");value.textContent=strength;meta.appendChild(value);}
meta.appendChild(portalIcon("wifi"));
if(secured)meta.appendChild(portalIcon("lock"));
row.querySelectorAll(".q").forEach(function(item){item.remove();});
row.appendChild(meta);
row.classList.add("wifi-network");
});
var firstStaticLabel=document.querySelector("label[for='ip']");
if(firstStaticLabel){
var staticSeparator=firstStaticLabel.previousElementSibling;
var staticRule=staticSeparator&&staticSeparator.tagName==="BR"?staticSeparator.previousElementSibling:null;
var advanced=document.createElement("details");
advanced.className="network-advanced";
var advancedSummary=document.createElement("summary");
advancedSummary.textContent="Advanced network";
var advancedNote=document.createElement("p");
advancedNote.className="network-advanced-note";
advancedNote.textContent="Optional. Leave these fields empty to use your router's automatic DHCP settings.";
advanced.appendChild(advancedSummary);
advanced.appendChild(advancedNote);
firstStaticLabel.parentNode.insertBefore(advanced,staticRule&&staticRule.tagName==="HR"?staticRule:firstStaticLabel);
["ip","gw","sn","dns"].forEach(function(id){
var field=document.getElementById(id);
var label=document.querySelector("label[for='"+id+"']");
if(label)advanced.appendChild(label);
if(field){
var trailingBreak=field.nextElementSibling;
advanced.appendChild(field);
if(trailingBreak&&trailingBreak.tagName==="BR")trailingBreak.remove();
}
});
if(staticSeparator&&staticSeparator.tagName==="BR")staticSeparator.remove();
if(staticRule&&staticRule.tagName==="HR")staticRule.remove();
}
var form=document.querySelector("form[action*='wifisave'],form[action*='paramsave']");
if(!form)return;
form.addEventListener("submit",function(){
var button=form.querySelector("button[type='submit']");
if(button){button.disabled=true;button.textContent="Saving configuration…";}
var state=document.createElement("div");
state.className="saving-state";
state.setAttribute("role","status");
state.textContent="Keep this page open while the device saves and reconnects.";
form.appendChild(state);
});
});
</script>
)HTML";

/**
 * Replaces WiFiManager's generic landing-page buttons with a compact device
 * setup menu. All routes continue to be handled by WiFiManager.
 */
static const char NOTIFICATOR_PORTAL_MENU[] = R"HTML(
<nav class="portal-menu" aria-label="Device setup">
	<a class="portal-menu-card" href="/wifi">
		<span class="portal-menu-icon" aria-hidden="true">1</span>
		<span><strong>Set up this device</strong><small>Connect Wi-Fi and your HiveMQ Cloud cluster.</small></span>
		<span class="portal-menu-arrow" aria-hidden="true">›</span>
	</a>
	<a class="portal-menu-card" href="/info">
		<span class="portal-menu-icon" aria-hidden="true">i</span>
		<span><strong>Device information</strong><small>Review network, hardware, and firmware details.</small></span>
		<span class="portal-menu-arrow" aria-hidden="true">›</span>
	</a>
	<a class="portal-menu-exit" href="/exit">Exit setup without changes</a>
</nav>
)HTML";
