// ====================================================================
// Dashboard do Irrigador com Nível
// Lê /status em tempo real do Firebase e escreve em /config para
// controlar a bomba (manual) e os horários automáticos.
// ====================================================================

firebase.initializeApp(firebaseConfig);
const db = firebase.database();

const statusRef = db.ref("/status");
const configRef = db.ref("/config");

const el = (id) => document.getElementById(id);

// Volume total configurado (vem de /config/tank; usado só para mostrar
// "X L de Y L" no card de nível). Começa com o padrão até carregar do Firebase.
let volumeTotalAtual = 5;

// ---------------- Mostra o /status em tempo real ----------------
statusRef.on("value", (snap) => {
  const s = snap.val() || {};

  const nivel = typeof s.level_pct === "number" ? s.level_pct : -1;
  const nivelValido = nivel >= 0;

  el("tankFill").style.height = (nivelValido ? nivel : 0) + "%";
  el("tankPct").textContent = nivelValido ? nivel.toFixed(0) + "%" : "--";
  el("tankLitros").textContent = nivelValido
    ? `${(s.volume_l ?? 0).toFixed(2)} L de ${volumeTotalAtual.toFixed(1)} L`
    : "sem leitura do sensor";
  el("tankDist").textContent =
    typeof s.distance_cm === "number" && s.distance_cm >= 0
      ? `sensor: ${s.distance_cm.toFixed(1)} cm`
      : "";

  el("temp").textContent = (s.temp_c !== undefined && s.temp_c > -50)
    ? s.temp_c.toFixed(1) + "°C" : "--°C";
  el("umid").textContent = (s.humidity_pct !== undefined && s.humidity_pct >= 0)
    ? s.humidity_pct.toFixed(0) + "%" : "--%";

  const aguaBadge = el("aguaBadge");
  if (s.water_present) {
    aguaBadge.textContent = "OK - água detectada";
    aguaBadge.className = "badge good";
  } else {
    aguaBadge.textContent = "Sem água";
    aguaBadge.className = "badge critical";
  }

  const bombaBadge = el("bombaBadge");
  if (s.pump_on) {
    bombaBadge.textContent = "Ligada";
    bombaBadge.className = "badge good";
  } else {
    bombaBadge.textContent = "Desligada";
    bombaBadge.className = "badge";
  }

  el("modoTexto").textContent =
    "Modo: " + (s.mode === "on" ? "manual (ligada)" :
                s.mode === "off" ? "manual (desligada)" : "automático (horários)");

  el("alertaSemAgua").hidden = !s.blocked_no_water;

  const connDot = el("connDot");
  const connText = el("connText");
  if (s.wifi_ok) {
    connDot.className = "dot ok";
    connText.textContent = "ESP01 online";
  } else {
    connDot.className = "dot bad";
    connText.textContent = "ESP01 sem WiFi";
  }

  if (s.last_update) {
    const d = new Date(s.last_update * 1000);
    el("lastUpdate").textContent = d.toLocaleString("pt-BR");
  }
});

// Se não receber nenhum dado por um tempo, avisa que pode estar offline
let ultimoRecebimento = Date.now();
statusRef.on("value", () => { ultimoRecebimento = Date.now(); });
setInterval(() => {
  if (Date.now() - ultimoRecebimento > 40000) {
    el("connDot").className = "dot bad";
    el("connText").textContent = "Sem dados recentes";
  }
}, 10000);

// ---------------- Carrega /config para preencher o formulário ----------------
configRef.on("value", (snap) => {
  const c = snap.val() || {};
  const sched = c.schedule || {};

  if (sched["1"]) {
    el("h1_enabled").checked = !!sched["1"].enabled;
    el("h1_time").value = horaParaInput(sched["1"].hour, sched["1"].minute);
    el("h1_dur").value = sched["1"].duration_min ?? 5;
  }
  if (sched["2"]) {
    el("h2_enabled").checked = !!sched["2"].enabled;
    el("h2_time").value = horaParaInput(sched["2"].hour, sched["2"].minute);
    el("h2_dur").value = sched["2"].duration_min ?? 5;
  }

  const tank = c.tank;
  if (tank) {
    if (typeof tank.volume_l === "number") {
      el("cx_volume").value = tank.volume_l;
      volumeTotalAtual = tank.volume_l;
    }
    if (typeof tank.dist_fundo_cm === "number") el("cx_fundo").value = tank.dist_fundo_cm;
    if (typeof tank.dist_cheio_cm === "number") el("cx_cheio").value = tank.dist_cheio_cm;
  }
});

function horaParaInput(h, m) {
  const hh = String(h ?? 0).padStart(2, "0");
  const mm = String(m ?? 0).padStart(2, "0");
  return `${hh}:${mm}`;
}

// ---------------- Botões manuais ----------------
function enviarManual(comando) {
  configRef.child("manual").set({
    command: comando,
    requestedAt: Math.floor(Date.now() / 1000)
  });
}

el("btnLigar").addEventListener("click", () => enviarManual("on"));
el("btnDesligar").addEventListener("click", () => enviarManual("off"));
el("btnAuto").addEventListener("click", () => enviarManual("auto"));

// ---------------- Salvar horários ----------------
el("formHorarios").addEventListener("submit", (ev) => {
  ev.preventDefault();

  const [h1h, h1m] = el("h1_time").value.split(":").map(Number);
  const [h2h, h2m] = el("h2_time").value.split(":").map(Number);

  const novoSchedule = {
    "1": {
      enabled: el("h1_enabled").checked,
      hour: h1h,
      minute: h1m,
      duration_min: Number(el("h1_dur").value) || 5
    },
    "2": {
      enabled: el("h2_enabled").checked,
      hour: h2h,
      minute: h2m,
      duration_min: Number(el("h2_dur").value) || 5
    }
  };

  configRef.child("schedule").set(novoSchedule).then(() => {
    const msg = el("saveMsg");
    msg.textContent = "Horários salvos!";
    setTimeout(() => { msg.textContent = ""; }, 3000);
  });
});

// ---------------- Salvar configuração da caixa d'água ----------------
// Essas 3 medidas são o que permite reaproveitar o mesmo firmware em
// qualquer caixa d'água: o ESP01 lê esses valores do Firebase e calcula o
// nível/volume a partir da distância bruta que o Nano manda.
el("formCaixa").addEventListener("submit", (ev) => {
  ev.preventDefault();

  const volumeL = Number(el("cx_volume").value);
  const distFundoCm = Number(el("cx_fundo").value);
  const distCheioCm = Number(el("cx_cheio").value);

  const alerta = el("alertaCaixaInvalida");
  const valido =
    Number.isFinite(volumeL) && volumeL > 0 &&
    Number.isFinite(distFundoCm) && distFundoCm > 0 &&
    Number.isFinite(distCheioCm) && distCheioCm >= 0 &&
    (distFundoCm - distCheioCm) > 1;

  if (!valido) {
    alerta.hidden = false;
    return;
  }
  alerta.hidden = true;

  configRef.child("tank").set({
    volume_l: volumeL,
    dist_fundo_cm: distFundoCm,
    dist_cheio_cm: distCheioCm
  }).then(() => {
    volumeTotalAtual = volumeL;
    const msg = el("saveMsgCaixa");
    msg.textContent = "Configuração da caixa salva!";
    setTimeout(() => { msg.textContent = ""; }, 3000);
  });
});
