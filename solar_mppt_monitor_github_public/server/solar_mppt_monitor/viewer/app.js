const API_LATEST = "../api/latest.php";
const API_RECIP_GET = "../api/recipients_get.php";
const API_RECIP_SET = "../api/recipients_set.php";
const API_EXPORT = "../api/export.php";

function $(id){ return document.getElementById(id); }

function fmt(n, d=2){
  if (n === null || n === undefined || n === "") return "-";
  const x = Number(n);
  if (!Number.isFinite(x)) return String(n);
  return x.toFixed(d);
}
function isoNice(iso){
  if (!iso) return "-";
  return String(iso).replace("T"," ").slice(0,19);
}

function openModal(id){ $(id).classList.add("show"); }
function closeModal(id){ $(id).classList.remove("show"); }

let latestCache = [];
let recipients = [];

function renderCards(items){
  const wrap = $("cards");
  wrap.innerHTML = "";

  items.forEach(it => {
    const id = it.id;
    const name = it.name || id;

    const latest = it.latest || {};
    const m = latest.metrics || {};
    const st = it.state || {};
    const offline = !!st.offline;

    const el = document.createElement("div");
    el.className = `card ${offline ? "off":"ok"}`;

    el.innerHTML = `
      <div class="card-head">
        <div>
          <div class="site">
            ${id}
            <span class="badge ${offline ? "off":"ok"}">
              ${offline ? "OFFLINE":"OK"}
            </span>
          </div>
          <div style="font-size:17px;font-weight:700;color:rgba(255,255,255,.85);margin-top:2px;">
            ${name}
          </div>
          <div class="time">
            last: ${isoNice(latest.iso)}
          </div>
        </div>
      </div>

      <div class="big">
        <div class="v">${fmt(m.batt_v,2)}<span class="u">V</span></div>
        <div class="soc">${m.soc ?? "-"}<span class="u">%</span></div>
      </div>

      <div class="kv">
        <div class="k">pv_w</div><div class="val">${fmt(m.pv_w,0)} W</div>
        <div class="k">batt_a</div><div class="val">${fmt(m.batt_a,2)} A</div>
        <div class="k">pv_v</div><div class="val">${fmt(m.pv_v,2)} V</div>
        <div class="k">pv_a</div><div class="val">${fmt(m.pv_a,2)} A</div>
        <div class="k">load_w</div><div class="val">${fmt(m.load_w,1)} W</div>
        <div class="k">charge_state</div><div class="val">${m.charge_state ?? "-"}</div>
      </div>
    `;

    wrap.appendChild(el);
  });
}

async function loadLatest(){
  const r = await fetch(API_LATEST, {cache:"no-store"});
  const j = await r.json();
  if (!j.ok) throw new Error("latest failed");

  latestCache = j.items || [];
  renderCards(latestCache);

  // export device selector
  const sel = $("exportDev");
  sel.innerHTML = "";
  latestCache.forEach(it => {
    const opt = document.createElement("option");
    opt.value = it.id;
    opt.textContent = `${it.id} / ${it.name || it.id}`;
    sel.appendChild(opt);
  });
}

loadLatest().catch(console.error);
