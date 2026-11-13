// Simulador del Sistema de Seguridad - Arduino UNO R4 WiFi
// Modo "Simulación": reproduce en el navegador la misma lógica del firmware.
// Modo "Arduino (USB)": usa Web Serial para controlar el Arduino real
// con los mismos comandos (ARMAR, DESARMAR <pin>, PANICO, ...).

const $ = (id) => document.getElementById(id);

// ---------- Ajustes (iguales que en el firmware) ----------
const CFG = { SEG_SALIDA: 10, SEG_ENTRADA: 10, SEG_ALARMA_MAX: 120, MS_MOTOR: 800, PIN: "1234" };

// ---------- Display de 7 segmentos (misma fuente que el firmware) ----------
const FUENTE = {
  "0": 0x7E, "1": 0x30, "2": 0x6D, "3": 0x79, "4": 0x33, "5": 0x5B, "6": 0x5F, "7": 0x70, "8": 0x7F, "9": 0x7B,
  A: 0x77, b: 0x1F, C: 0x4E, d: 0x3D, E: 0x4F, F: 0x47, H: 0x37, L: 0x0E, n: 0x15, o: 0x1D, O: 0x7E,
  P: 0x67, r: 0x05, t: 0x0F, U: 0x3E, "-": 0x01,
};
// bits: A=0x40 B=0x20 C=0x10 D=0x08 E=0x04 F=0x02 G=0x01
const SEGMENTOS = [
  [0x40, "M6 2 L28 2 L24 6 L10 6 Z"],          // A
  [0x20, "M29 3 L29 26 L25 23 L25 7 Z"],        // B
  [0x10, "M29 30 L29 53 L25 49 L25 33 Z"],      // C
  [0x08, "M6 54 L28 54 L24 50 L10 50 Z"],       // D
  [0x04, "M5 30 L5 53 L9 49 L9 33 Z"],          // E
  [0x02, "M5 3 L5 26 L9 23 L9 7 Z"],            // F
  [0x01, "M7 28 L11 25 L23 25 L27 28 L23 31 L11 31 Z"], // G
];
const digitos = [];
for (let i = 0; i < 8; i++) {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", "0 0 34 56");
  const segs = SEGMENTOS.map(([bit, d]) => {
    const p = document.createElementNS("http://www.w3.org/2000/svg", "path");
    p.setAttribute("d", d); p.setAttribute("class", "seg"); svg.appendChild(p);
    return [bit, p];
  });
  $("display").appendChild(svg);
  digitos.push(segs);
}
function pintarDisplay(texto) {
  texto = (texto || "").padEnd(8).slice(0, 8);
  for (let i = 0; i < 8; i++) {
    const v = FUENTE[texto[i]] || 0;
    for (const [bit, p] of digitos[i]) p.classList.toggle("on", (v & bit) !== 0);
  }
}
const der = (n, w) => String(n).padStart(w);
function textoDisplay(s, ahora) {
  switch (s.estado) {
    case "DESARMADO": return "OFF";
    case "ARMANDO": return "ArnA" + der(s.cuenta, 4);
    case "ARMADO": return "On  " + (s.dist >= 0 ? der(s.dist, 4) : "----");
    case "INTRUSION": return "Ent " + der(s.cuenta, 4);
    case "ALARMA": return Math.floor(ahora / 500) % 2 ? "ALAr-ALA" : "";
  }
  return "";
}

// ---------- Sonido (buzzer) ----------
let audio = null, sirena = null;
function ctx() { if (!audio) audio = new (window.AudioContext || window.webkitAudioContext)(); return audio; }
function pitido(freq, ms) {
  if (!$("chkSonido").checked) return;
  const a = ctx(), o = a.createOscillator(), g = a.createGain();
  o.type = "square"; o.frequency.value = freq; g.gain.value = 0.05;
  o.connect(g).connect(a.destination); o.start(); o.stop(a.currentTime + ms / 1000);
}
function sirenaOn(on) {
  on = on && $("chkSonido").checked;
  if (on && !sirena) {
    const a = ctx(), o = a.createOscillator(), g = a.createGain();
    o.type = "square"; g.gain.value = 0.05; o.connect(g).connect(a.destination); o.start();
    sirena = { o, g, t: setInterval(() => { o.frequency.value = o.frequency.value > 1000 ? 800 : 1300; }, 300) };
  } else if (!on && sirena) {
    clearInterval(sirena.t); sirena.o.stop(); sirena = null;
  }
}

// ---------- Registro de eventos ----------
function evento(msg, hora) {
  const li = document.createElement("li");
  hora = hora || new Date().toLocaleTimeString();
  li.textContent = `${hora}  ${msg}`;
  $("eventos").prepend(li);
  while ($("eventos").children.length > 200) $("eventos").lastChild.remove();
}
$("btnLimpiar").onclick = () => ($("eventos").innerHTML = "");

// =====================================================================
//                       MOTOR DE SIMULACIÓN
// =====================================================================
const sim = {
  estado: "DESARMADO", t0: Date.now(), cuenta: 0, causa: "",
  pir: false, dist: -1, laser: true, hazRoto: false, umbral: 50,
  cerradura: "ABIERTA", tMotor: 0, cerca: 0, ultimoSeg: -1,
  pirHasta: 0, hazHasta: 0, receptor: false,
};

function simCambiar(e) {
  sim.estado = e; sim.t0 = Date.now(); sim.ultimoSeg = -1;
  if (e === "ARMANDO") sim.cuenta = CFG.SEG_SALIDA;
  if (e === "INTRUSION") sim.cuenta = CFG.SEG_ENTRADA;
  if (e === "ALARMA") simCerrar();
  evento("Estado: " + e);
}
function simCerrar() {
  if (sim.cerradura === "CERRADA" || sim.cerradura === "CERRANDO") return;
  sim.cerradura = "CERRANDO"; sim.tMotor = Date.now(); evento("Cerradura: cerrando");
}
function simAbrir() {
  if (sim.cerradura === "ABIERTA" || sim.cerradura === "ABRIENDO") return;
  sim.cerradura = "ABRIENDO"; sim.tMotor = Date.now(); evento("Cerradura: abriendo");
}

function simComando(cmd) {
  const up = cmd.trim().toUpperCase();
  if (up === "ARMAR") {
    if (sim.estado !== "DESARMADO") return "ERROR ya esta armado";
    sim.causa = ""; simCerrar(); simCambiar("ARMANDO"); return "OK";
  }
  if (up.startsWith("DESARMAR")) {
    if (cmd.slice(8).trim() !== CFG.PIN) { evento("PIN incorrecto"); pitido(300, 400); return "ERROR pin incorrecto"; }
    sim.causa = ""; simCambiar("DESARMADO"); simAbrir(); return "OK";
  }
  if (up === "PANICO") { sim.causa = "boton de panico"; evento("PANICO activado"); simCambiar("ALARMA"); return "OK"; }
  if (up === "CERRAR") { simCerrar(); return "OK"; }
  if (up === "ABRIR") { if (sim.estado !== "DESARMADO") return "ERROR desarma primero"; simAbrir(); return "OK"; }
  if (up === "LASER ON") { sim.laser = true; evento("Laser encendido"); return "OK"; }
  if (up === "LASER OFF") { sim.laser = false; evento("Laser apagado"); return "OK"; }
  if (up.startsWith("UMBRAL")) {
    const v = parseInt(cmd.slice(6)); if (!(v >= 5 && v <= 400)) return "ERROR umbral 5-400";
    sim.umbral = v; evento(`Umbral = ${v} cm`); return "OK";
  }
  return "ERROR comando desconocido";
}

// Posición de la persona en la escena (coordenadas SVG)
const persona = { x: 470, y: 240, movidoEn: 0 };

function simSensores(ahora) {
  // PIR: detecta si la persona se mueve dentro del cono (x < 240) o si se inyectó la señal
  const enCono = persona.x < 240;
  if (enCono && ahora - persona.movidoEn < 400) sim.pirHasta = Math.max(sim.pirHasta, ahora + 3000);
  sim.pir = ahora < sim.pirHasta;

  // Ultrasónico: mide si la persona está sobre su línea (y ≈ 239). 2 px = 1 cm.
  const enLinea = Math.abs(persona.y - 225) < 40 && persona.x > 54;
  sim.dist = enLinea ? Math.round((persona.x - 54) / 2) : 300;
  if (sim.dist > 0 && sim.dist < sim.umbral) sim.cerca++; else sim.cerca = 0;

  // Receptor láser: haz en x = 338
  const cruza = Math.abs(persona.x - 338) < 14;
  sim.receptor = $("chkReceptor").checked;
  sim.hazRoto = sim.receptor && sim.laser && (cruza || ahora < sim.hazHasta);

  // Motor
  if ((sim.cerradura === "CERRANDO" || sim.cerradura === "ABRIENDO") && ahora - sim.tMotor >= CFG.MS_MOTOR)
    sim.cerradura = sim.cerradura === "CERRANDO" ? "CERRADA" : "ABIERTA";
}

function simDetectar() {
  if (sim.pir) return "movimiento (PIR)";
  if (sim.cerca >= 2) return `proximidad (${sim.dist} cm)`;
  if (sim.hazRoto) return "haz laser interrumpido";
  return "";
}

function simPaso() {
  const ahora = Date.now();
  simSensores(ahora);
  const seg = Math.floor((ahora - sim.t0) / 1000);

  if (sim.estado === "ARMANDO" || sim.estado === "INTRUSION") {
    const total = sim.estado === "ARMANDO" ? CFG.SEG_SALIDA : CFG.SEG_ENTRADA;
    sim.cuenta = total - seg;
    if (seg !== sim.ultimoSeg) { sim.ultimoSeg = seg; pitido(sim.estado === "ARMANDO" ? 2000 : 2500, sim.cuenta <= 3 ? 250 : 80); }
    if (sim.cuenta <= 0) {
      if (sim.estado === "ARMANDO") simCambiar("ARMADO");
      else { sim.causa = sim.causa || "sin desarmar"; simCambiar("ALARMA"); }
    }
  } else if (sim.estado === "ARMADO") {
    const c = simDetectar();
    if (c) { sim.causa = c; evento("DETECCION: " + c); simCambiar("INTRUSION"); }
  } else if (sim.estado === "ALARMA" && seg >= CFG.SEG_ALARMA_MAX) {
    evento("Sirena apagada por tiempo; sistema sigue armado"); simCambiar("ARMADO");
  }

  render({
    estado: sim.estado, cuenta: sim.cuenta, pir: sim.pir, dist: sim.dist, umbral: sim.umbral,
    laser: sim.laser, receptor: sim.receptor, haz_roto: sim.hazRoto, cerradura: sim.cerradura,
    buzzer: sim.estado === "ALARMA", causa: sim.causa,
  });
}

// =====================================================================
//                     MODO ARDUINO REAL (Web Serial)
// =====================================================================
let modo = "sim", puerto = null, escritor = null, ultimoReal = null, cuentaPrev = null;

async function conectarUsb() {
  if (!("serial" in navigator)) {
    alert("Tu navegador no soporta Web Serial. Usa Google Chrome o Microsoft Edge en una computadora.");
    return;
  }
  try {
    puerto = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x2341 }] });
    await puerto.open({ baudRate: 115200 });
  } catch (e) { evento("No se pudo abrir el puerto: " + e.message); return; }

  const enc = new TextEncoderStream(); enc.readable.pipeTo(puerto.writable);
  escritor = enc.writable.getWriter();
  modo = "usb"; actualizarModo();
  evento("Conectado al Arduino por USB");
  leerUsb();
}

async function leerUsb() {
  const dec = new TextDecoderStream(); puerto.readable.pipeTo(dec.writable).catch(() => {});
  const lector = dec.readable.getReader();
  let buf = "";
  try {
    while (true) {
      const { value, done } = await lector.read();
      if (done) break;
      buf += value;
      let i;
      while ((i = buf.indexOf("\n")) >= 0) {
        const linea = buf.slice(0, i).trim(); buf = buf.slice(i + 1);
        procesarLinea(linea);
      }
    }
  } catch (e) { evento("Conexión USB perdida: " + e.message); }
  desconectarUsb();
}

function procesarLinea(linea) {
  if (!linea.startsWith("{")) { if (linea) evento(linea); return; }
  let j; try { j = JSON.parse(linea); } catch { return; }
  if (j.t === "st") {
    ultimoReal = j;
    if ((j.estado === "ARMANDO" || j.estado === "INTRUSION") && j.cuenta !== cuentaPrev && j.cuenta > 0)
      pitido(j.estado === "ARMANDO" ? 2000 : 2500, j.cuenta <= 3 ? 250 : 80);
    cuentaPrev = j.cuenta;
    render({ ...j, pir: !!j.pir, laser: !!j.laser, receptor: !!j.receptor, haz_roto: !!j.haz_roto, buzzer: !!j.buzzer });
    if (j.ip) $("conexion").textContent = `Arduino USB · WiFi: http://${j.ip}`;
  } else if (j.t === "ev") evento(j.msg, j.hora);
  else if (j.t === "resp" && j.r !== "OK") evento(`${j.cmd} → ${j.r}`);
}

function desconectarUsb() {
  if (modo !== "usb") return;
  modo = "sim"; puerto = null; escritor = null; actualizarModo();
}

async function comando(cmd) {
  ctx();  // habilita el audio con el primer clic
  if (modo === "usb" && escritor) { await escritor.write(cmd + "\n"); return; }
  const r = simComando(cmd);
  if (r !== "OK") evento(`${cmd.startsWith("DESARMAR") ? "DESARMAR" : cmd} → ${r}`);
}

// =====================================================================
//                              INTERFAZ
// =====================================================================
function render(s) {
  const ahora = Date.now();
  $("estado").textContent = s.estado + (s.cuenta > 0 && (s.estado === "ARMANDO" || s.estado === "INTRUSION") ? ` · ${s.cuenta}s` : "");
  $("estado").className = "estado " + s.estado;
  $("causa").textContent = s.causa ? "Causa: " + s.causa : "";
  pintarDisplay(modo === "usb" && s.display !== undefined && s.estado !== "ALARMA" ? s.display : textoDisplay(s, ahora));

  $("sPir").innerHTML = s.pir ? '<span class="on-txt">MOVIMIENTO</span>' : "quieto" + (s.pir_listo === 0 ? " (calentando)" : "");
  $("sDist").textContent = s.dist >= 0 ? `${s.dist} cm (umbral ${s.umbral})` : "sin lectura";
  $("sLaser").textContent = s.laser ? "encendido" : "apagado";
  $("sHaz").innerHTML = !s.receptor ? "sin receptor" : s.haz_roto ? '<span class="on-txt">INTERRUMPIDO</span>' : '<span class="ok-txt">ok</span>';
  $("sCerr").textContent = s.cerradura;
  $("sBuz").innerHTML = s.buzzer ? '<span class="on-txt">SONANDO</span>' : "silencio";
  if (document.activeElement !== $("umbral")) { $("umbral").value = s.umbral; $("umbralTxt").textContent = s.umbral; }

  // escena
  $("pirLed").classList.toggle("activo", s.pir);
  $("conoPir").classList.toggle("activo", s.pir);
  const cerca = s.dist >= 0 && s.dist < s.umbral;
  $("usSensor").classList.toggle("activo", cerca);
  $("usOnda").setAttribute("x2", s.dist >= 0 ? Math.min(54 + s.dist * 2, 590) : 54);
  $("haz").setAttribute("class", "haz" + (!s.laser ? " apagado" : s.haz_roto ? " roto" : ""));
  $("receptor").classList.toggle("activo", s.haz_roto);
  $("pestillo").setAttribute("x", s.cerradura === "CERRADA" || s.cerradura === "CERRANDO" ? 556 : 540);
  $("buzzer").classList.toggle("sonando", s.buzzer);
  sirenaOn(s.buzzer);
}

function actualizarModo() {
  const usb = modo === "usb";
  $("btnSim").classList.toggle("activo", !usb);
  $("btnUsb").classList.toggle("activo", usb);
  $("btnUsb").textContent = usb ? "Desconectar Arduino" : "Conectar Arduino (USB)";
  $("conexion").textContent = usb ? "Arduino conectado por USB" : "Modo simulación";
  $("conexion").classList.toggle("ok", usb);
  $("simControles").style.display = usb ? "none" : "";
  $("persona").style.display = usb ? "none" : "";
  $("escenaAyuda").textContent = usb ? "(lecturas reales del Arduino)" : "(arrastra a la persona para simular un intruso)";
}

$("btnSim").onclick = async () => { if (modo === "usb") { try { await puerto.close(); } catch {} desconectarUsb(); } };
$("btnUsb").onclick = async () => { if (modo === "usb") $("btnSim").onclick(); else conectarUsb(); };

document.querySelectorAll("[data-cmd]").forEach((b) => (b.onclick = () => comando(b.dataset.cmd)));
$("btnDesarmar").onclick = () => { comando("DESARMAR " + $("pin").value); $("pin").value = ""; };
$("pin").addEventListener("keydown", (e) => { if (e.key === "Enter") $("btnDesarmar").onclick(); });
$("umbral").oninput = () => ($("umbralTxt").textContent = $("umbral").value);
$("umbral").onchange = () => comando("UMBRAL " + $("umbral").value);

// teclado numérico
["1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "⌫", "OK"].forEach((k) => {
  const b = document.createElement("button"); b.textContent = k;
  b.onclick = () => {
    if (k === "⌫") $("pin").value = $("pin").value.slice(0, -1);
    else if (k === "OK") $("btnDesarmar").onclick();
    else { $("pin").value += k; pitido(1800, 40); }
  };
  $("teclado").appendChild(b);
});

$("btnMov").onclick = () => { sim.pirHasta = Date.now() + 3000; };
$("btnHaz").onclick = () => {
  if (!$("chkReceptor").checked) { $("chkReceptor").checked = true; evento("(Simulación) receptor láser activado"); }
  sim.hazHasta = Date.now() + 2000;
};

// Arrastrar a la persona
const svg = $("escena");
let arrastrando = false;
function puntoSvg(e) {
  const p = svg.createSVGPoint(); p.x = e.clientX; p.y = e.clientY;
  return p.matrixTransform(svg.getScreenCTM().inverse());
}
$("persona").addEventListener("pointerdown", (e) => { arrastrando = true; $("persona").setPointerCapture(e.pointerId); ctx(); });
$("persona").addEventListener("pointermove", (e) => {
  if (!arrastrando) return;
  const p = puntoSvg(e);
  persona.x = Math.max(30, Math.min(570, p.x)); persona.y = Math.max(60, Math.min(280, p.y));
  persona.movidoEn = Date.now();
  $("persona").setAttribute("transform", `translate(${persona.x},${persona.y})`);
});
$("persona").addEventListener("pointerup", () => (arrastrando = false));

// Bucle principal
setInterval(() => { if (modo === "sim") simPaso(); else if (ultimoReal?.estado === "ALARMA") render({ ...ultimoReal, pir: !!ultimoReal.pir, laser: !!ultimoReal.laser, receptor: !!ultimoReal.receptor, haz_roto: !!ultimoReal.haz_roto, buzzer: true }); }, 100);
actualizarModo();
evento("Simulador listo. Pulsa «Armar» y luego arrastra a la persona hacia los sensores.");
