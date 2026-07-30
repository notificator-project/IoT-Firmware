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
.wifi-network{display:grid;grid-template-columns:minmax(0,1fr) 46px;align-items:center;min-height:48px;margin:7px 0;border:1px solid var(--line);border-radius:14px;background:#fff;overflow:hidden}
.wifi-network:hover{border-color:#93b4ff;background:#f8fbff}
.wifi-network a[data-ssid]{display:flex;align-items:center;align-self:stretch;min-width:0;padding:0 13px;color:var(--ink);font-size:14px;text-decoration:none}
.wifi-network .q{display:flex!important;float:none!important;align-items:center;justify-content:flex-end;gap:4px;width:auto!important;min-width:0!important;height:18px!important;margin:0 11px 0 0!important;padding:0!important}
.wifi-network .q:before,.wifi-network .q:after{display:block!important;width:16px!important;height:16px!important;padding:0!important;background-color:var(--blue)!important;background-image:none!important;background-position:initial!important;background-repeat:no-repeat!important;content:""!important}
.wifi-network .q:after{-webkit-mask:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath d='M12 18.5a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3Zm0-5c-2.15 0-4.12.8-5.62 2.12l1.55 1.76A6.1 6.1 0 0 1 12 15.85c1.56 0 2.99.58 4.07 1.53l1.55-1.76A8.48 8.48 0 0 0 12 13.5Zm0-5c-3.4 0-6.5 1.28-8.84 3.38l1.56 1.74A10.82 10.82 0 0 1 12 10.85c2.8 0 5.35 1.05 7.28 2.77l1.56-1.74A13.15 13.15 0 0 0 12 8.5Zm0-5C7.3 3.5 3.03 5.27 0 8.16L1.6 9.85A14.96 14.96 0 0 1 12 5.85c4.04 0 7.72 1.51 10.4 4l1.6-1.69A17.3 17.3 0 0 0 12 3.5Z'/%3E%3C/svg%3E") center/contain no-repeat;mask:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath d='M12 18.5a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3Zm0-5c-2.15 0-4.12.8-5.62 2.12l1.55 1.76A6.1 6.1 0 0 1 12 15.85c1.56 0 2.99.58 4.07 1.53l1.55-1.76A8.48 8.48 0 0 0 12 13.5Zm0-5c-3.4 0-6.5 1.28-8.84 3.38l1.56 1.74A10.82 10.82 0 0 1 12 10.85c2.8 0 5.35 1.05 7.28 2.77l1.56-1.74A13.15 13.15 0 0 0 12 8.5Zm0-5C7.3 3.5 3.03 5.27 0 8.16L1.6 9.85A14.96 14.96 0 0 1 12 5.85c4.04 0 7.72 1.51 10.4 4l1.6-1.69A17.3 17.3 0 0 0 12 3.5Z'/%3E%3C/svg%3E") center/contain no-repeat}
.wifi-network .q.l:before{-webkit-mask:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath d='M17 8h-1V6a4 4 0 0 0-8 0v2H7a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2v-9a2 2 0 0 0-2-2Zm-7-2a2 2 0 1 1 4 0v2h-4V6Z'/%3E%3C/svg%3E") center/contain no-repeat;mask:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath d='M17 8h-1V6a4 4 0 0 0-8 0v2H7a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2v-9a2 2 0 0 0-2-2Zm-7-2a2 2 0 1 1 4 0v2h-4V6Z'/%3E%3C/svg%3E") center/contain no-repeat}
.wifi-network .q:not(.l):before{display:none!important}
.wifi-network .q.q-0:after{opacity:.25}.wifi-network .q.q-1:after{opacity:.42}.wifi-network .q.q-2:after{opacity:.62}.wifi-network .q.q-3:after{opacity:.8}
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
document.querySelectorAll("a[data-ssid]").forEach(function(link){
if(link.parentElement)link.parentElement.classList.add("wifi-network");
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
