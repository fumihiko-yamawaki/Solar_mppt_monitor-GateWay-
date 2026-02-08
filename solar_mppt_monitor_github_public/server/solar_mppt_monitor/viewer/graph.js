const API_LATEST  = "../api/latest.php";
const API_HISTORY = "../api/history.php"; // month指定（ym）
const API_EXPORT  = "../api/export.php";  // range指定（from/to）※CSVを返す

let chart;
function $(id){ return document.getElementById(id); }
function setMsg(s){ $("msg").textContent = s || ""; }

function ymd(d){
  return `${d.getFullYear()}-${String(d.getMonth()+1).padStart(2,"0")}-${String(d.getDate()).padStart(2,"0")}`;
}
function todayYmd(){ return ymd(new Date()); }
function daysAgoYmd(days){
  const d = new Date();
  d.setDate(d.getDate() - days);
  return ymd(d);
}

let graphRangeDays = null; // null: month指定, number: range日数

async function loadDevices(){
  const r = await fetch(API_LATEST, {cache:"no-store"});
  const j = await r.json();
  if (!j.ok) throw new Error("latest failed");

  const sel = $("devSel");
  sel.innerHTML = "";

  j.items.forEach(it => {
    const opt = document.createElement("option");
    opt.value = it.id;
    opt.textContent = `${it.id} / ${it.name || it.id}`;
    sel.appendChild(opt);
  });

  const d = new Date();
  $("ymSel").value =
    `${d.getFullYear()}-${String(d.getMonth()+1).padStart(2,"0")}`;
}

async function fetchCsvText(){
  const dev = $("devSel").value;

  let url;
  if (graphRangeDays) {
    const to = todayYmd();
    const from = daysAgoYmd(graphRangeDays);
    url = `${API_EXPORT}?device=${encodeURIComponent(dev)}&from=${encodeURIComponent(from)}&to=${encodeURIComponent(to)}`;
  } else {
    const ym = $("ymSel").value;
    url = `${API_HISTORY}?device=${encodeURIComponent(dev)}&ym=${encodeURIComponent(ym)}`;
  }

  const r = await fetch(url, {cache:"no-store"});
  if (!r.ok) throw new Error(`fetch failed: ${r.status} ${r.statusText}\n${url}`);
  return await r.text();
}

function parseCsv(text){
  // BOM除去
  text = text.replace(/^\uFEFF/, "");
  const lines = text.trim().split("\n");
  if (lines.length < 2) return null;

  const header = lines.shift().replace(/\r/g,"").split(",");
  const idx = Object.fromEntries(header.map((h,i)=>[h,i]));

  // 必須列の存在チェック（無い場合でも落ちないように）
  const need = ["ts","batt_v","batt_a","pv_v","pv_a","pv_w","soc"];
  const missing = need.filter(k => !(k in idx));
  return { header, idx, lines, missing };
}

async function loadChart(){
  setMsg("");
  let csvText;
  try {
    csvText = await fetchCsvText();
  } catch (e) {
    setMsg(String(e));
    return;
  }

  const parsed = parseCsv(csvText);
  if (!parsed) { setMsg("no data"); return; }

  const { idx, lines, missing } = parsed;
  if (missing.length) {
    setMsg(`CSV列が不足しています: ${missing.join(", ")}`);
    // 続行は一応可能だが、表示は欠けます
  }

  const labels = [];
  const battV = [], battA = [], pvV = [], pvA = [], pvW = [], soc = [];

  for (const ln of lines) {
    const c = ln.replace(/\r/g,"").split(",");
    const ts = Number(c[idx.ts] ?? 0);
    const d = new Date(ts * 1000);

    labels.push(
      `${d.getMonth()+1}/${d.getDate()} ${String(d.getHours()).padStart(2,"0")}:${String(d.getMinutes()).padStart(2,"0")}`
    );

    battV.push(Number(c[idx.batt_v] ?? 0));
    battA.push(Number(c[idx.batt_a] ?? 0));
    pvV.push(Number(c[idx.pv_v] ?? 0));
    pvA.push(Number(c[idx.pv_a] ?? 0));
    pvW.push(Number(c[idx.pv_w] ?? 0));
    soc.push(Number(c[idx.soc] ?? 0));
  }

  const ctx = $("chart").getContext("2d");
  if (chart) chart.destroy();

  chart = new Chart(ctx, {
    type: "line",
    data: {
      labels,
      datasets: [
        { label: "Batt V (V)", data: battV, yAxisID: "yV",   tension: 0.2, pointRadius: 0 },
        { label: "Batt A (A)", data: battA, yAxisID: "yA",   tension: 0.2, pointRadius: 0 },

        { label: "PV V (V)",   data: pvV,   yAxisID: "yV",   tension: 0.2, pointRadius: 0 },
        { label: "PV A (A)",   data: pvA,   yAxisID: "yA",   tension: 0.2, pointRadius: 0 },

        { label: "PV W (W)",   data: pvW,   yAxisID: "yW",   tension: 0.2, pointRadius: 0 },

        { label: "SOC (%)",    data: soc,   yAxisID: "ySOC", tension: 0.2, pointRadius: 0 }
      ]
    },
    options: {
      interaction: { mode: "index", intersect: false },
      plugins: {
        legend: {
          labels: {
            font: { size: 26 } // ★凡例文字を約2倍
          }
        }
      },
      scales: {
        yV:   { type: "linear", position: "left",  title: { display: true, text: "Voltage (V)" } },
        ySOC: { type: "linear", position: "left",  min: 0, max: 100, grid: { drawOnChartArea: false },
                title: { display: true, text: "SOC (%)" } },

        yA:   { type: "linear", position: "right", grid: { drawOnChartArea: false },
                title: { display: true, text: "Current (A)" } },

        yW:   { type: "linear", position: "right", grid: { drawOnChartArea: false },
                title: { display: true, text: "Power (W)" } }
      }
    }
  });
}

// ---- UI wiring ----
$("range24h").addEventListener("click", ()=>{ graphRangeDays = 1;  loadChart(); });
$("range3d").addEventListener("click",  ()=>{ graphRangeDays = 3;  loadChart(); });
$("range7d").addEventListener("click",  ()=>{ graphRangeDays = 7;  loadChart(); });
$("range30d").addEventListener("click", ()=>{ graphRangeDays = 30; loadChart(); });

// 「表示」は month指定に戻す
$("loadBtn").addEventListener("click", ()=>{
  graphRangeDays = null;
  loadChart();
});

$("reloadBtn").addEventListener("click", ()=>{
  loadDevices().then(loadChart).catch(e=>setMsg(String(e)));
});

// 初期表示
loadDevices().then(loadChart).catch(e=>setMsg(String(e)));
