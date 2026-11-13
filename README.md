# Sistema de Seguridad con Arduino UNO R4 WiFi

Alarma con sensor de movimiento (PIR), sensor de distancia (ultrasonido), barrera láser,
cerradura motorizada, buzzer y display de cuenta regresiva. Se controla desde:

- **El simulador web** (carpeta `docs/`, publicado en GitHub Pages). Puede funcionar solo
  (simulación) o **controlar el Arduino real por USB** con el botón *Conectar Arduino (USB)*
  (Chrome o Edge).
- **El panel WiFi** que sirve el propio Arduino (desde el celular o la PC).

## Cómo funciona

| Estado | Qué pasa | Display |
|---|---|---|
| DESARMADO | Nada vigila. Cerradura abierta. | `OFF` |
| ARMANDO | 10 s para salir, pitido cada segundo. Cierra la cerradura. | `ArnA  10` |
| ARMADO | Vigila PIR, ultrasonido (< umbral) y haz láser. | `On   123` (distancia) |
| INTRUSION | Se detectó algo: 10 s para poner el PIN. | `Ent   10` |
| ALARMA | Sirena, cerradura cerrada. Se apaga con el PIN (o sola a los 120 s). | `ALAr-ALA` parpadeando |

PIN por defecto: **1234** (cámbialo en `arduino_secrets.h`).

## Conexiones

| Componente | Pin del módulo | Arduino R4 WiFi |
|---|---|---|
| PIR HC-SR501 | VCC / OUT / GND | 5V / **D2** / GND |
| Buzzer | + / − | **D3** / GND |
| HC-SR04 | VCC / TRIG / ECHO / GND | 5V / **D4** / **D5** / GND |
| L298N | ENA / IN1 / IN2 | **D6** / **D7** / **D8** |
| L298N | GND | GND del Arduino **y** (−) de la batería |
| L298N | 12V | (+) de la batería del motor (7–12 V) |
| L298N | OUT1 / OUT2 | Los dos cables del motor DC |
| Láser KY-008 | S / − | **D9** / GND (el pin del medio no se usa) |
| MAX7219 | VCC / GND / DIN / CS / CLK | 5V / GND / **A1** / **A2** / **A3** |

### Componentes que faltan (pines ya reservados)

| Componente | Pin del módulo | Arduino R4 WiFi | Activar en el código |
|---|---|---|---|
| Receptor láser | VCC / OUT / GND | 5V / **A0** / GND | `USAR_RECEPTOR_LASER 1` |
| RTC DS3231 | VCC / GND / SDA / SCL | 5V / GND / **A4** / **A5** | `USAR_RTC_DS3231 1` + librería *RTClib* |
| microSD | VCC / GND / CS / MOSI / MISO / SCK | 5V / GND / **D10** / **D11** / **D12** / **D13** | `USAR_MICROSD 1` |
| ESP32-CAM | 5V / GND / U0R / U0T | 5V (fuente aparte) / GND / **D1** / **D0** | `USAR_ESP32CAM 1` |

### Importante al conectar

1. **GND común**: todas las tierras (Arduino, L298N, batería, sensores) deben ir juntas.
2. **El motor NO se alimenta del Arduino.** Usa una batería o fuente de 7–12 V en el L298N.
   Deja puesto el jumper **5V-EN** del L298N y **quita el jumper de ENA** (ENA va a D6).
3. **No uses el pin 5V del L298N para alimentar el Arduino** mientras esté conectado por USB.
4. El **PIR** tarda unos 30 s en estabilizarse al encender; el programa lo ignora ese tiempo.
   En el PIR pon el jumper en **H** (repetición) y gira el potenciómetro de tiempo al mínimo.
5. La **ESP32-CAM** consume mucho: aliméntala con una fuente de 5 V aparte (GND común).
   Su TX es 3.3 V; para la línea Arduino D1 → ESP32 U0R usa un divisor (1 kΩ + 2 kΩ).

## Subir el programa

1. Abre `firmware/SistemaSeguridad/SistemaSeguridad.ino` en Arduino IDE.
2. Copia `arduino_secrets.example.h` como `arduino_secrets.h` y, si quieres, pon tu WiFi.
   - Sin WiFi configurado el Arduino crea su propia red **SeguridadR4** (clave `seguridad123`);
     conéctate y abre **http://192.168.4.1**.
   - Con WiFi configurado, la IP aparece en el simulador al conectar por USB.
3. Placa: **Arduino UNO R4 WiFi** → Subir.

## Comandos (USB a 115200 o `http://IP/api/cmd?c=...`)

`ARMAR` · `DESARMAR 1234` · `PANICO` · `CERRAR` · `ABRIR` · `LASER ON` · `LASER OFF` ·
`UMBRAL 50` · `ESTADO` · `EVENTOS`
