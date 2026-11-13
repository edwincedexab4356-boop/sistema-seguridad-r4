/*
  SISTEMA DE SEGURIDAD - Arduino UNO R4 WiFi
  ------------------------------------------------------------
  Componentes actuales:
    - Sensor PIR HC-SR501        (movimiento)
    - Sensor ultrasónico HC-SR04 (distancia)
    - Display 7 segmentos MAX7219 (cuenta regresiva / estado)
    - Buzzer                     (alertas)
    - Motor DC + L298N           (cerradura)
    - Láser KY-008               (emisor del haz)

  Componentes futuros (activar con los #define USAR_xxx de abajo):
    - Receptor láser, RTC DS3231, módulo microSD, ESP32-CAM

  Control:
    - Por USB (Serial 115200) -> lo usa el simulador web (modo "Arduino real")
    - Por WiFi -> panel web en la IP del Arduino

  Comandos (texto, uno por línea):
    ARMAR | DESARMAR <pin> | PANICO | CERRAR | ABRIR
    LASER ON | LASER OFF | UMBRAL <cm> | ESTADO | EVENTOS
*/

#include <WiFiS3.h>
#include "arduino_secrets.h"

// ================== COMPONENTES FUTUROS ==================
// Cambia a 1 cuando conectes cada componente.
#define USAR_RECEPTOR_LASER 0
#define USAR_RTC_DS3231     0   // requiere librería "RTClib" de Adafruit
#define USAR_MICROSD        0   // usa la librería "SD" incluida
#define USAR_ESP32CAM       0   // ESP32-CAM conectada a Serial1 (D0/D1)

#if USAR_RTC_DS3231
  #include <Wire.h>
  #include <RTClib.h>
  RTC_DS3231 rtc;
#endif
#if USAR_MICROSD
  #include <SPI.h>
  #include <SD.h>
#endif

// ================== PINES ==================
// D0/D1 -> reservados para ESP32-CAM (Serial1)
const uint8_t PIN_PIR        = 2;
const uint8_t PIN_BUZZER     = 3;
const uint8_t PIN_TRIG       = 4;
const uint8_t PIN_ECHO       = 5;
const uint8_t PIN_MOTOR_ENA  = 6;   // PWM
const uint8_t PIN_MOTOR_IN1  = 7;
const uint8_t PIN_MOTOR_IN2  = 8;
const uint8_t PIN_LASER      = 9;
const uint8_t PIN_SD_CS      = 10;  // D10-D13 reservados para microSD (SPI)
const uint8_t PIN_RECEPTOR   = A0;
const uint8_t PIN_MAX_DIN    = A1;
const uint8_t PIN_MAX_CS     = A2;
const uint8_t PIN_MAX_CLK    = A3;
// A4/A5 (SDA/SCL) -> reservados para RTC DS3231

// ================== AJUSTES ==================
const uint16_t SEG_SALIDA        = 10;    // segundos para salir tras armar
const uint16_t SEG_ENTRADA       = 10;    // segundos para desarmar tras detección
const uint16_t SEG_ALARMA_MAX    = 120;   // la sirena se apaga sola tras este tiempo
const uint32_t MS_MOTOR          = 800;   // tiempo que gira el motor para cerrar/abrir
const uint8_t  VELOCIDAD_MOTOR   = 200;   // 0-255
const uint32_t MS_CALENTAR_PIR   = 30000; // el PIR tarda ~30 s en estabilizarse
const int      HAZ_ACTIVO_NIVEL  = HIGH;  // nivel del receptor cuando recibe el láser
uint16_t umbralCm = 50;                   // detección por ultrasonido si dist < umbral

// ================== ESTADO ==================
enum Estado { DESARMADO, ARMANDO, ARMADO, INTRUSION, ALARMA };
const char* NOMBRE_ESTADO[] = { "DESARMADO", "ARMANDO", "ARMADO", "INTRUSION", "ALARMA" };
Estado estado = DESARMADO;
uint32_t msCambioEstado = 0;

enum Cerradura { ABIERTA, CERRADA, CERRANDO, ABRIENDO };
const char* NOMBRE_CERRADURA[] = { "ABIERTA", "CERRADA", "CERRANDO", "ABRIENDO" };
Cerradura cerradura = ABIERTA;
uint32_t msMotor = 0;

bool pir = false, hazRoto = false, laserOn = true;
int  distanciaCm = -1;  // -1 = sin lectura
uint8_t lecturasCerca = 0;
int cuenta = 0;          // segundos restantes en ARMANDO / INTRUSION
String causa = "";

// ================== REGISTRO DE EVENTOS ==================
const uint8_t MAX_EVENTOS = 15;
String eventos[MAX_EVENTOS];
uint8_t numEventos = 0, inicioEventos = 0;

WiFiServer servidor(80);
bool wifiOk = false;
uint32_t epochInicio = 0;  // hora real (NTP) al arrancar, si hay internet

String marcaTiempo() {
#if USAR_RTC_DS3231
  DateTime t = rtc.now();
  char b[20];
  snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
  return String(b);
#else
  uint32_t s = millis() / 1000;
  if (epochInicio) {
    uint32_t e = epochInicio + s - 5 * 3600;  // UTC-5 (ajusta tu zona horaria)
    char b[12];
    snprintf(b, sizeof(b), "%02lu:%02lu:%02lu", (e / 3600) % 24, (e / 60) % 60, e % 60);
    return String(b);
  }
  char b[16];
  snprintf(b, sizeof(b), "+%lus", s);
  return String(b);
#endif
}

void registrar(const String& msg) {
  String linea = marcaTiempo() + " " + msg;
  uint8_t pos = (inicioEventos + numEventos) % MAX_EVENTOS;
  if (numEventos < MAX_EVENTOS) numEventos++;
  else inicioEventos = (inicioEventos + 1) % MAX_EVENTOS;
  eventos[pos] = linea;

  String m = msg; m.replace("\"", "'");
  Serial.print("{\"t\":\"ev\",\"hora\":\""); Serial.print(marcaTiempo());
  Serial.print("\",\"msg\":\""); Serial.print(m); Serial.println("\"}");

#if USAR_MICROSD
  File f = SD.open("eventos.txt", FILE_WRITE);
  if (f) { f.println(linea); f.close(); }
#endif
#if USAR_ESP32CAM
  if (msg.startsWith("DETECCION") || msg.startsWith("ALARMA")) Serial1.println("FOTO");
#endif
}

// ================== DISPLAY MAX7219 ==================
// Controlador propio (sin librerías) en modo "sin decodificación".
// Bits: DP A B C D E F G
uint8_t fuente(char c) {
  switch (c) {
    case '0': return 0x7E; case '1': return 0x30; case '2': return 0x6D; case '3': return 0x79;
    case '4': return 0x33; case '5': return 0x5B; case '6': return 0x5F; case '7': return 0x70;
    case '8': return 0x7F; case '9': return 0x7B;
    case 'A': return 0x77; case 'b': return 0x1F; case 'C': return 0x4E; case 'd': return 0x3D;
    case 'E': return 0x4F; case 'F': return 0x47; case 'H': return 0x37; case 'L': return 0x0E;
    case 'n': return 0x15; case 'o': return 0x1D; case 'O': return 0x7E; case 'P': return 0x67;
    case 'r': return 0x05; case 't': return 0x0F; case 'U': return 0x3E; case '-': return 0x01;
    default:  return 0x00;
  }
}

void maxEnviar(uint8_t reg, uint8_t val) {
  digitalWrite(PIN_MAX_CS, LOW);
  shiftOut(PIN_MAX_DIN, PIN_MAX_CLK, MSBFIRST, reg);
  shiftOut(PIN_MAX_DIN, PIN_MAX_CLK, MSBFIRST, val);
  digitalWrite(PIN_MAX_CS, HIGH);
}

void maxIniciar() {
  pinMode(PIN_MAX_DIN, OUTPUT); pinMode(PIN_MAX_CS, OUTPUT); pinMode(PIN_MAX_CLK, OUTPUT);
  digitalWrite(PIN_MAX_CS, HIGH);
  maxEnviar(0x0F, 0);  // test off
  maxEnviar(0x0C, 1);  // encender
  maxEnviar(0x0B, 7);  // 8 dígitos
  maxEnviar(0x09, 0);  // sin decodificación
  maxEnviar(0x0A, 6);  // brillo 0-15
}

String textoDisplay = "";
// Muestra 8 caracteres (izquierda a derecha)
void maxTexto(String t) {
  while (t.length() < 8) t += ' ';
  if (t == textoDisplay) return;
  textoDisplay = t;
  for (uint8_t i = 0; i < 8; i++) maxEnviar(8 - i, fuente(t[i]));  // dígito 8 = izquierda
}

String derecha(int n, uint8_t ancho) {
  String s = String(n);
  while (s.length() < ancho) s = " " + s;
  return s;
}

void actualizarDisplay() {
  switch (estado) {
    case DESARMADO: maxTexto("OFF     "); break;
    case ARMANDO:   maxTexto("ArnA" + derecha(cuenta, 4)); break;
    case ARMADO:    maxTexto("On  " + (distanciaCm >= 0 ? derecha(distanciaCm, 4) : String("----"))); break;
    case INTRUSION: maxTexto("Ent " + derecha(cuenta, 4)); break;
    case ALARMA:    maxTexto((millis() / 500) % 2 ? "ALAr-ALA" : "        "); break;
  }
}

// ================== MOTOR / CERRADURA ==================
void motorParar() {
  analogWrite(PIN_MOTOR_ENA, 0);
  digitalWrite(PIN_MOTOR_IN1, LOW); digitalWrite(PIN_MOTOR_IN2, LOW);
}

void cerrar() {
  if (cerradura == CERRADA || cerradura == CERRANDO) return;
  digitalWrite(PIN_MOTOR_IN1, HIGH); digitalWrite(PIN_MOTOR_IN2, LOW);
  analogWrite(PIN_MOTOR_ENA, VELOCIDAD_MOTOR);
  cerradura = CERRANDO; msMotor = millis();
  registrar("Cerradura: cerrando");
}

void abrir() {
  if (cerradura == ABIERTA || cerradura == ABRIENDO) return;
  digitalWrite(PIN_MOTOR_IN1, LOW); digitalWrite(PIN_MOTOR_IN2, HIGH);
  analogWrite(PIN_MOTOR_ENA, VELOCIDAD_MOTOR);
  cerradura = ABRIENDO; msMotor = millis();
  registrar("Cerradura: abriendo");
}

void actualizarMotor() {
  if ((cerradura == CERRANDO || cerradura == ABRIENDO) && millis() - msMotor >= MS_MOTOR) {
    motorParar();
    cerradura = (cerradura == CERRANDO) ? CERRADA : ABIERTA;
  }
}

// ================== SENSORES ==================
uint32_t msUltimaMedida = 0;

void leerSensores() {
  pir = (millis() > MS_CALENTAR_PIR) && digitalRead(PIN_PIR) == HIGH;

#if USAR_RECEPTOR_LASER
  hazRoto = laserOn && digitalRead(PIN_RECEPTOR) != HAZ_ACTIVO_NIVEL;
#else
  hazRoto = false;
#endif

  if (millis() - msUltimaMedida >= 100) {
    msUltimaMedida = millis();
    digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);
    unsigned long us = pulseIn(PIN_ECHO, HIGH, 25000UL);  // ~4 m máx
    distanciaCm = us ? (int)(us / 58) : -1;
    if (distanciaCm > 0 && distanciaCm < umbralCm) { if (lecturasCerca < 255) lecturasCerca++; }
    else lecturasCerca = 0;
  }
}

// ================== MÁQUINA DE ESTADOS ==================
int ultimoSegundo = -1;

void cambiarEstado(Estado e) {
  estado = e;
  msCambioEstado = millis();
  ultimoSegundo = -1;
  noTone(PIN_BUZZER);
  if (e == ARMANDO) cuenta = SEG_SALIDA;
  if (e == INTRUSION) cuenta = SEG_ENTRADA;
  if (e == ALARMA) cerrar();
  registrar(String("Estado: ") + NOMBRE_ESTADO[e]);
  enviarEstado();
}

String detectarIntrusion() {
  if (pir) return "movimiento (PIR)";
  if (lecturasCerca >= 2) return "proximidad (" + String(distanciaCm) + " cm)";
  if (hazRoto) return "haz laser interrumpido";
  return "";
}

void actualizarEstado() {
  uint32_t t = millis() - msCambioEstado;
  int seg = t / 1000;

  switch (estado) {
    case DESARMADO:
      break;

    case ARMANDO:
    case INTRUSION: {
      int total = (estado == ARMANDO) ? SEG_SALIDA : SEG_ENTRADA;
      cuenta = total - seg;
      if (seg != ultimoSegundo) {        // pitido cada segundo
        ultimoSegundo = seg;
        tone(PIN_BUZZER, estado == ARMANDO ? 2000 : 2500, cuenta <= 3 ? 250 : 80);
      }
      if (cuenta <= 0) {
        if (estado == ARMANDO) cambiarEstado(ARMADO);
        else { causa = causa.length() ? causa : "sin desarmar"; cambiarEstado(ALARMA); }
      }
      break;
    }

    case ARMADO: {
      String c = detectarIntrusion();
      if (c.length()) {
        causa = c;
        registrar("DETECCION: " + c);
        cambiarEstado(INTRUSION);
      }
      break;
    }

    case ALARMA:
      tone(PIN_BUZZER, (millis() / 300) % 2 ? 1300 : 800);  // sirena
      if (seg >= SEG_ALARMA_MAX) {
        noTone(PIN_BUZZER);
        registrar("Sirena apagada por tiempo; sistema sigue armado");
        cambiarEstado(ARMADO);
      }
      break;
  }
}

// ================== COMANDOS ==================
String ejecutar(String cmd) {
  cmd.trim();
  String up = cmd; up.toUpperCase();

  if (up == "ARMAR") {
    if (estado != DESARMADO) return "ERROR ya esta armado";
    causa = ""; cerrar(); cambiarEstado(ARMANDO); return "OK";
  }
  if (up.startsWith("DESARMAR")) {
    String pin = cmd.substring(8); pin.trim();
    if (pin != SECRET_PIN) { registrar("PIN incorrecto"); tone(PIN_BUZZER, 300, 400); return "ERROR pin incorrecto"; }
    causa = ""; cambiarEstado(DESARMADO); abrir(); return "OK";
  }
  if (up == "PANICO") { causa = "boton de panico"; registrar("PANICO activado"); cambiarEstado(ALARMA); return "OK"; }
  if (up == "CERRAR") { cerrar(); return "OK"; }
  if (up == "ABRIR") {
    if (estado != DESARMADO) return "ERROR desarma primero";
    abrir(); return "OK";
  }
  if (up == "LASER ON")  { laserOn = true;  digitalWrite(PIN_LASER, HIGH); registrar("Laser encendido"); return "OK"; }
  if (up == "LASER OFF") { laserOn = false; digitalWrite(PIN_LASER, LOW);  registrar("Laser apagado");   return "OK"; }
  if (up.startsWith("UMBRAL")) {
    int v = cmd.substring(6).toInt();
    if (v < 5 || v > 400) return "ERROR umbral 5-400";
    umbralCm = v; registrar("Umbral = " + String(v) + " cm"); return "OK";
  }
  if (up == "ESTADO") { enviarEstado(); return "OK"; }
  if (up == "EVENTOS") {
    for (uint8_t i = 0; i < numEventos; i++) Serial.println(eventos[(inicioEventos + i) % MAX_EVENTOS]);
    return "OK";
  }
  return "ERROR comando desconocido";
}

String jsonEstado() {
  String j = "{\"t\":\"st\",\"estado\":\"";
  j += NOMBRE_ESTADO[estado];
  j += "\",\"cuenta\":" + String(cuenta > 0 && (estado == ARMANDO || estado == INTRUSION) ? cuenta : 0);
  j += ",\"pir\":" + String(pir ? 1 : 0);
  j += ",\"dist\":" + String(distanciaCm);
  j += ",\"umbral\":" + String(umbralCm);
  j += ",\"laser\":" + String(laserOn ? 1 : 0);
  j += ",\"receptor\":" + String(USAR_RECEPTOR_LASER);
  j += ",\"haz_roto\":" + String(hazRoto ? 1 : 0);
  j += ",\"cerradura\":\"" + String(NOMBRE_CERRADURA[cerradura]) + "\"";
  j += ",\"buzzer\":" + String(estado == ALARMA ? 1 : 0);
  j += ",\"display\":\"" + textoDisplay + "\"";
  j += ",\"causa\":\"" + causa + "\"";
  j += ",\"pir_listo\":" + String(millis() > MS_CALENTAR_PIR ? 1 : 0);
  j += ",\"ip\":\"" + (wifiOk ? WiFi.localIP().toString() : String("")) + "\"";
  j += "}";
  return j;
}

void enviarEstado() { Serial.println(jsonEstado()); }

// Lectura de comandos por USB sin bloquear
String bufSerial = "";
void leerSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (bufSerial.length()) {
        String r = ejecutar(bufSerial);
        Serial.print("{\"t\":\"resp\",\"cmd\":\""); Serial.print(bufSerial);
        Serial.print("\",\"r\":\""); Serial.print(r); Serial.println("\"}");
        bufSerial = "";
      }
    } else if (bufSerial.length() < 64) bufSerial += c;
  }
}

// ================== PANEL WEB (WiFi) ==================
const char PANEL_HTML[] = R"HTML(<!DOCTYPE html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Seguridad R4</title>
<style>body{font-family:sans-serif;background:#111;color:#eee;max-width:480px;margin:auto;padding:12px}
h1{font-size:20px}.e{font-size:28px;font-weight:bold;padding:12px;border-radius:8px;text-align:center;background:#333}
button{font-size:16px;padding:12px;margin:4px;border:0;border-radius:8px;width:47%}
.v{background:#2a7}.r{background:#c33;color:#fff}.g{background:#555;color:#fff}
pre{background:#222;padding:8px;font-size:12px;white-space:pre-wrap}input{font-size:16px;padding:10px;width:40%}</style></head>
<body><h1>Sistema de seguridad</h1><div class="e" id="e">...</div><pre id="s"></pre>
<button class="v" onclick="c('ARMAR')">Armar</button>
<input id="p" type="password" placeholder="PIN"><button class="g" onclick="c('DESARMAR '+p.value)">Desarmar</button>
<button class="r" onclick="c('PANICO')">Pánico</button><button class="g" onclick="c('CERRAR')">Cerrar</button>
<button class="g" onclick="c('ABRIR')">Abrir</button><button class="g" onclick="c('LASER ON')">Láser ON</button>
<button class="g" onclick="c('LASER OFF')">Láser OFF</button><h3>Eventos</h3><pre id="ev"></pre>
<script>const col={DESARMADO:'#357',ARMANDO:'#a80',ARMADO:'#2a7',INTRUSION:'#d60',ALARMA:'#c33'};
async function c(x){const r=await fetch('/api/cmd?c='+encodeURIComponent(x));alert(await r.text());}
async function u(){try{const j=await(await fetch('/api/estado')).json();e.textContent=j.estado+(j.cuenta?' '+j.cuenta+'s':'');
e.style.background=col[j.estado];s.textContent='PIR: '+(j.pir?'MOVIMIENTO':'quieto')+'\nDistancia: '+j.dist+' cm (umbral '+j.umbral+')\nLáser: '+(j.laser?'ON':'OFF')+'\nCerradura: '+j.cerradura+(j.causa?'\nCausa: '+j.causa:'');
ev.textContent=await(await fetch('/api/eventos')).text();}catch(x){}}setInterval(u,1000);u();</script></body></html>)HTML";

String urlDecode(const String& s) {
  String r = "";
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '+') r += ' ';
    else if (c == '%' && i + 2 < s.length()) { r += (char)strtol(s.substring(i + 1, i + 3).c_str(), nullptr, 16); i += 2; }
    else r += c;
  }
  return r;
}

void responder(WiFiClient& cli, const char* tipo, const String& cuerpo) {
  cli.print("HTTP/1.1 200 OK\r\nContent-Type: "); cli.print(tipo);
  cli.print("\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: ");
  cli.print(cuerpo.length()); cli.print("\r\n\r\n"); cli.print(cuerpo);
}

void atenderWeb() {
  if (!wifiOk) return;
  WiFiClient cli = servidor.available();
  if (!cli) return;

  String linea = "";
  uint32_t t0 = millis();
  bool primera = true; String peticion = "";
  while (cli.connected() && millis() - t0 < 500) {
    if (!cli.available()) continue;
    char c = cli.read();
    if (c == '\n') {
      if (primera) { peticion = linea; primera = false; }
      if (linea.length() <= 1) break;  // fin de cabeceras
      linea = "";
    } else linea += c;
  }

  // "GET /ruta HTTP/1.1"
  int a = peticion.indexOf(' '), b = peticion.indexOf(' ', a + 1);
  String ruta = (a > 0 && b > a) ? peticion.substring(a + 1, b) : "/";

  if (ruta.startsWith("/api/estado")) responder(cli, "application/json", jsonEstado());
  else if (ruta.startsWith("/api/cmd?c=")) responder(cli, "text/plain", ejecutar(urlDecode(ruta.substring(11))));
  else if (ruta.startsWith("/api/eventos")) {
    String s = "";
    for (int i = numEventos - 1; i >= 0; i--) s += eventos[(inicioEventos + i) % MAX_EVENTOS] + "\n";
    responder(cli, "text/plain", s);
  }
  else responder(cli, "text/html; charset=utf-8", String(PANEL_HTML));
  delay(1);
  cli.stop();
}

void iniciarWiFi() {
  if (WiFi.status() == WL_NO_MODULE) { Serial.println("{\"t\":\"ev\",\"msg\":\"Modulo WiFi no encontrado\"}"); return; }

  if (strlen(SECRET_SSID) == 0) {
    // Modo punto de acceso: el Arduino crea su propia red
    if (WiFi.beginAP("SeguridadR4", "seguridad123") == WL_AP_LISTENING) {
      wifiOk = true; delay(1000);
      servidor.begin();
      registrar("WiFi AP 'SeguridadR4' -> http://" + WiFi.localIP().toString());
    }
    return;
  }

  for (int i = 0; i < 3 && WiFi.status() != WL_CONNECTED; i++) {
    WiFi.begin(SECRET_SSID, SECRET_PASS);
    delay(3000);
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiOk = true;
    servidor.begin();
    for (int i = 0; i < 5 && epochInicio == 0; i++) { uint32_t e = WiFi.getTime(); if (e > 1600000000UL) epochInicio = e - millis() / 1000; else delay(1000); }
    registrar("WiFi conectado -> http://" + WiFi.localIP().toString());
  } else {
    registrar("WiFi no conectado (solo USB)");
  }
}

// ================== SETUP / LOOP ==================
void setup() {
  Serial.begin(115200);
#if USAR_ESP32CAM
  Serial1.begin(115200);
#endif

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_RECEPTOR, INPUT);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LASER, OUTPUT);
  pinMode(PIN_MOTOR_ENA, OUTPUT); pinMode(PIN_MOTOR_IN1, OUTPUT); pinMode(PIN_MOTOR_IN2, OUTPUT);
  motorParar();
  digitalWrite(PIN_LASER, HIGH);

  maxIniciar();
  maxTexto("HOLA    ");

#if USAR_RTC_DS3231
  if (!rtc.begin()) Serial.println("{\"t\":\"ev\",\"msg\":\"RTC no encontrado\"}");
  else if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
#endif
#if USAR_MICROSD
  if (!SD.begin(PIN_SD_CS)) Serial.println("{\"t\":\"ev\",\"msg\":\"microSD no encontrada\"}");
#endif

  tone(PIN_BUZZER, 1500, 100);
  registrar("Sistema iniciado");
  iniciarWiFi();
}

uint32_t msUltimoEnvio = 0;

void loop() {
  leerSerial();
  leerSensores();
  actualizarEstado();
  actualizarMotor();
  actualizarDisplay();
  atenderWeb();

  if (millis() - msUltimoEnvio >= 300) {  // estado al simulador por USB
    msUltimoEnvio = millis();
    enviarEstado();
  }
}
