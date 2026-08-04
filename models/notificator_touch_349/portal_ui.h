#pragma once

#include <Arduino.h>

/**
 * Offline presentation for the captive setup portal.
 *
 * Everything is embedded because a phone connected to the device access point
 * usually has no internet connection. The small script inserts the device ID,
 * improves password handling, and gives clear feedback while settings save.
 */
static const char NOTIFICATOR_TOUCH_PORTAL_HEAD[] PROGMEM = R"HTML(
<style>
:root{--ink:#101828;--muted:#5b6980;--blue:#2864e8;--blue2:#4085ff;--paper:#eef4ff;--card:#fff;--line:#d9e3f2;--ok:#067647}
*{box-sizing:border-box}html,body{width:100%;min-height:100%;margin:0}
body{padding:18px 12px;color:var(--ink);background:radial-gradient(circle at 5% 0,rgba(64,133,255,.25),transparent 25rem),linear-gradient(160deg,#f8fbff,var(--paper));font-family:Inter,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
.wrap{width:min(100%,620px);max-width:none;margin:auto;padding:clamp(20px,5vw,36px);border-radius:28px;background:rgba(255,255,255,.96);box-shadow:0 25px 80px rgba(36,63,120,.16)}
.wrap:before{display:flex;align-items:center;min-height:44px;margin-bottom:24px;padding-left:57px;background:linear-gradient(135deg,var(--blue2),var(--blue)) 0/44px 44px no-repeat;border-radius:13px;color:var(--ink);content:"Notificator";font-size:21px;font-weight:850;letter-spacing:-.04em}
h1{margin:0;color:var(--ink);font-size:clamp(30px,8vw,44px);line-height:1.05;letter-spacing:-.05em}h1:after{display:block;margin:12px 0 24px;color:var(--muted);content:"Connect this display to Wi-Fi and your HiveMQ Cloud cluster.";font-size:15px;font-weight:500;letter-spacing:0;line-height:1.55}
h2{margin:5px 0 7px;font-size:20px;letter-spacing:-.025em}.device-identity,.mqtt-card{display:block;margin:20px 0;padding:19px;border:1px solid var(--line);border-radius:19px;background:#f8faff}
.device-identity span,.provider{display:block;margin-bottom:7px;color:var(--blue);font-size:10px;font-weight:850;letter-spacing:.13em}.device-identity strong{display:block;font-size:28px;letter-spacing:.08em}.device-identity small,.mqtt-card p{display:block;margin:6px 0 0;color:var(--muted);font-size:13px;line-height:1.5}
label{display:block;margin:14px 0 7px;font-size:14px;font-weight:750}input,select{width:100%;min-height:50px;margin:0!important;padding:0 14px!important;border:1px solid #c6d3e5!important;border-radius:13px!important;background:#fff!important;color:var(--ink)!important;font:inherit}input:focus,select:focus{border-color:var(--blue)!important;outline:0;box-shadow:0 0 0 4px rgba(40,100,232,.14)!important}
input[type=checkbox]{width:18px;min-height:18px;margin:12px 7px 0 0!important;padding:0!important;accent-color:var(--blue)}button,.btn{width:100%;min-height:52px;margin-top:16px;border:0!important;border-radius:999px!important;background:linear-gradient(135deg,var(--blue2),var(--blue))!important;color:#fff!important;font:inherit;font-weight:850!important;box-shadow:0 12px 28px rgba(40,100,232,.22)}
a{color:var(--blue);font-weight:750}.msg{padding:14px 16px;border-radius:13px;background:#eff8ff;line-height:1.5}.msg.S{background:#ecfdf3;color:var(--ok)}hr{height:1px;margin:24px 0;border:0;background:var(--line)}
.saving{display:flex;gap:9px;align-items:center;margin-top:14px;color:var(--muted);font-size:13px}.saving:before{width:14px;height:14px;border:2px solid #bdd1ff;border-top-color:var(--blue);border-radius:50%;content:"";animation:spin .8s linear infinite}@keyframes spin{to{transform:rotate(360deg)}}
.portal-foot{margin:22px 0 0;color:var(--muted);font-size:12px;line-height:1.45;text-align:center}@media(max-width:480px){body{padding:10px}.wrap{padding:22px 17px;border-radius:22px}}@media(prefers-reduced-motion:reduce){.saving:before{animation:none}}
</style>
<script>
document.addEventListener("DOMContentLoaded",function(){
 var id="%DEVICE_ID%";
 document.querySelectorAll("#pairing-id").forEach(function(el){el.textContent=id;});
 var pass=document.getElementById("mqtt_pass");if(pass){var row=document.createElement("label");row.style.fontWeight="500";row.style.fontSize="13px";var cb=document.createElement("input");cb.type="checkbox";cb.addEventListener("change",function(){pass.type=cb.checked?"text":"password";});row.appendChild(cb);row.append(" Show MQTT password");pass.after(row);}
 var offset=document.getElementById("clock_offset");if(offset){offset.value=String(-new Date().getTimezoneOffset());var hint=document.createElement("small");hint.style.display="block";hint.style.marginTop="7px";hint.style.color="var(--muted)";hint.textContent="Detected from this phone. Adjust it if the display will live in another timezone.";offset.after(hint);}
 var form=document.querySelector("form[action*='wifisave'],form[action*='paramsave']");if(form)form.addEventListener("submit",function(){var b=form.querySelector("button[type='submit']");if(b){b.disabled=true;b.textContent="Saving and connecting…";}var s=document.createElement("div");s.className="saving";s.setAttribute("role","status");s.textContent="Keep this page open while the device reconnects.";form.appendChild(s);});
 var foot=document.createElement("p");foot.className="portal-foot";foot.textContent="Credentials are stored only on this device. Notificator does not provide an MQTT broker.";document.querySelector(".wrap")?.appendChild(foot);
});
</script>
)HTML";
