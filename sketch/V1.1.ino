/*************************************************************
 *                      INCLUDES
 *************************************************************/
 #include <ESP8266WiFi.h>
 #include <ESP8266WebServer.h>
 #include <FastLED.h>
 #include <AccelStepper.h>
 #include <EEPROM.h>
 
 /*************************************************************
  *                      WIFI CONFIG
  *************************************************************/
 const char* ssid     = "***";
 const char* password = "***";
 
 /*************************************************************
  *                      LED CONFIG
  *************************************************************/
 #define LED_PIN D2
 #define NUM_LEDS 27
 CRGB leds[NUM_LEDS];
 
 int mode = 0;   // 0=static, 1=animation, 2=stage
 
 // Farb-/Helligkeitssteuerung
 uint8_t hue = 0;
 // WICHTIG: Brightness NUR global via FastLED.setBrightness steuern
 uint8_t brightness = 75;

 // Custom RGB color (entered via double-click on the color card)
 bool useCustomColor = false;
 CRGB customColor = CRGB(255, 255, 255);
 
 // Animationen
 // 0 = ColorWave (kombiniert Wave+Pulse, nutzt Hue); 1 = Rainbow; 2 = DualWave
 int currentAnimation = 0;
 uint8_t waveOffset = 0;
 int animSpeed = 20;            // 1..50 (kleiner = schneller tick)
 unsigned long lastAnimTick = 0;
 
 int stageSize = 1;
 int stagePosition = 1;
 
 /*************************************************************
  *                      MOTOR CONFIG
  *************************************************************/
 #define IN1 D5
 #define IN2 D6
 #define IN3 D7
 #define IN4 D8
 
 AccelStepper stepper(AccelStepper::HALF4WIRE, IN1, IN3, IN2, IN4);
 
 bool motorRunning = false;
 int motorDirection = 1;   // -1 links, +1 rechts
 int motorSpeed = 1000;    // Schritte/Sek (intern) - max speed from reference
 
 float currentMotorSpeed = 0;     // für Ramping
 const int rampDuration = 500;    // 0.5 s total
 const int rampInterval = 10;     // ms: wie oft wir rampen
 
 // Basis-Takt für loop(), damit Motor-Aufrufe in allen Modi gleich häufig sind
 unsigned long lastLoopTick = 0;
 
 // Motor timing - completely independent from LED timing
 unsigned long lastMotorTick = 0;
 const unsigned long motorInterval = 1;  // 1ms for max speed (matches reference code)
 
 /*************************************************************
  *                      EEPROM STRUCT
  *************************************************************/
 struct Settings {
   int mode;
   uint8_t hue;
   uint8_t brightness;
   int anim;
   int animSpeed;
   int stageSize;
   int stagePos;
   int motorSpeed;
   int motorDir;

  bool customMode;
  uint8_t customR;
  uint8_t customG;
  uint8_t customB;
  uint32_t magic = 0xABCDEF01;
};
 
 Settings S;
 
 /*************************************************************
  *                      WEB SERVER
  *************************************************************/
 ESP8266WebServer server(80);
 
 /*************************************************************
  *                      EEPROM FUNCTIONS
  *************************************************************/
 void saveSettings() {
   EEPROM.put(0, S);
   EEPROM.commit();
 }
 
 void loadSettings() {
   EEPROM.get(0, S);
   if (S.magic != 0xABCDEF01) {
     // Defaults beim Erststart
     S.mode = 0;          // Static
     S.hue = 127;         // Mitte der "Weiß-Zone"
     S.brightness = 51;   // ca. 20% Helligkeit
     S.anim = 0;
     S.animSpeed = 20;
     S.stageSize = 1;
     S.stagePos = 0;
     S.motorSpeed = 500;
     S.motorDir = 1;
     S.customMode = false;
     S.customR = 255;
     S.customG = 255;
     S.customB = 255;
     saveSettings();
   }
 
   // Anwenden
   mode           = S.mode;
   hue            = S.hue;
   brightness     = S.brightness;
   currentAnimation = S.anim;
   animSpeed      = S.animSpeed;
   stageSize      = S.stageSize;
   stagePosition  = S.stagePos;
   motorSpeed     = S.motorSpeed;
   motorDirection = S.motorDir;
   useCustomColor = S.customMode;
   customColor = CRGB(S.customR, S.customG, S.customB);
 }
 
 /*************************************************************
  *                      LED ANIMATIONS
  *************************************************************/
 // Nutzt immer HSV(..., 255, 255) und lässt Brightness ausschließlich
 // über FastLED.setBrightness() regeln. Ausnahme: Puls (V schwankt),
 // globales Brightness skaliert trotzdem noch mal sauber.
 void animColorWave() {
   for (int i = 0; i < NUM_LEDS; i++) {
     uint8_t v = sin8((i * 255 / NUM_LEDS) + waveOffset); // Puls (0..255)
     // In der "Weiß-Zone" des Hue-Sliders: entsättigte Welle (Weiß-Grau)
     if (hue >= 120 && hue <= 135) {
       leds[i] = CHSV(0, 0, v);
     } else {
       leds[i] = CHSV(hue, 255, v);
     }
   }
 }
 
 void animRainbowWave() {
   for (int i = 0; i < NUM_LEDS; i++) {
     uint8_t h = (i * 255 / NUM_LEDS) + waveOffset;
     leds[i] = CHSV(h, 255, 255);
   }
 }
 
 void animDualWave() {
   for (int i = 0; i < NUM_LEDS; i++) {
     uint8_t w1 = sin8((i * 255 / NUM_LEDS) + waveOffset);
     uint8_t w2 = sin8((i * 255 / NUM_LEDS) - waveOffset);
     leds[i] = CRGB(w1, 0, w2);
   }
 }
 
 // Animations-Takt vollständig entkoppelt vom Motor
 // animSpeed wird im UI als "langsamer -> kleiner Wert, schneller -> größer Wert" verstanden.
 // Wir mappen daher 1..50 (UI) auf 50..1 ms Delay.
 void runAnimation() {
   uint8_t delayMs = 51 - constrain(animSpeed, 1, 50);
   if (millis() - lastAnimTick < (unsigned long)delayMs) return;
   lastAnimTick = millis();
 
   switch (currentAnimation) {
     case 0: animColorWave(); break;
     case 1: animRainbowWave(); break;
     case 2: animDualWave();   break;
   }
   waveOffset += 3;
   FastLED.show();
 }
 
 void runStatic() {
   if (useCustomColor) {
     fill_solid(leds, NUM_LEDS, customColor);
   } else if (hue >= 120 && hue <= 135) {
     fill_solid(leds, NUM_LEDS, CRGB(255, 255, 200));  // Noch weniger Blau für wärmeres, weißlicheres Licht
   } else {
     fill_solid(leds, NUM_LEDS, CHSV(hue, 255, 255));
   }
   FastLED.show();
 }
 
 void runStage() {
   fill_solid(leds, NUM_LEDS, CRGB::Black);
   for (int i = 0; i < stageSize; i++) {
     if (useCustomColor) {
       leds[(stagePosition + i) % NUM_LEDS] = customColor;
     } else if (hue >= 120 && hue <= 135) {
       leds[(stagePosition + i) % NUM_LEDS] = CRGB(255, 255, 230);  // Warm white (less blue)
     } else {
       leds[(stagePosition + i) % NUM_LEDS] = CHSV(hue, 255, 255);
     }
   }
   FastLED.show();
 }
 
 /*************************************************************
  *                MOTOR RAMPING FUNCTION (0.5 s)
  *************************************************************/
 void updateMotorRamping() {
   static unsigned long lastRampUpdate = 0;
   if (millis() - lastRampUpdate < rampInterval) {
     return;
   }
   lastRampUpdate = millis();
 
   if (!motorRunning) {
     if (currentMotorSpeed > 0) {
       float step = (float)motorSpeed / (rampDuration / rampInterval);
       currentMotorSpeed -= step;
       if (currentMotorSpeed < 0) currentMotorSpeed = 0;
     }
   } else {
     if (currentMotorSpeed < motorSpeed) {
       float step = (float)motorSpeed / (rampDuration / rampInterval);
       currentMotorSpeed += step;
       if (currentMotorSpeed > motorSpeed) currentMotorSpeed = motorSpeed;
     }
   }
 
   stepper.setSpeed(currentMotorSpeed * motorDirection);
 }
 
 // Completely independent motor control - runs at max speed when enabled
 void updateMotorIndependent() {
   if (!motorRunning) {
     // Stop motor immediately when not running
     stepper.stop();
     currentMotorSpeed = 0;
     return;
   }
   
   // Update ramping
   updateMotorRamping();
   
   // Run stepper at the exact timing from reference code (1ms intervals)
   if (millis() - lastMotorTick >= motorInterval) {
     stepper.runSpeed();
     lastMotorTick = millis();
   }
 }
 
 /*************************************************************
  *                      FULL iOS UI (schmal)
  *************************************************************/
 String page;
 
 void buildPage() {
   page = R"HTML(
 <!DOCTYPE html>
 <html>
 <head>
 <meta name="viewport" content="width=device-width, initial-scale=1">
 <meta name="fw-version" content="1.1">
 
 <style>
 body{
   background:#f5f5f7; margin:0; padding:0;
   display:flex; justify-content:center;
   font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto;
 }
 .container{ width:100%; max-width:430px; padding:18px; }
 h2{ font-weight:700; text-align:center; margin:0 0 16px 0; }
 h3{ margin:0 0 10px 0; }
 .card{
   background:#fff; border-radius:22px; padding:18px; margin:14px 0;
   box-shadow:0 4px 25px rgba(0,0,0,0.08);
 }
 .icon-grid{ display:flex; gap:12px; }
 .icon-tile{
   flex:1; background:#fff; border-radius:18px; padding:16px; text-align:center;
   box-shadow:0 2px 10px rgba(0,0,0,0.05); user-select:none;
 }
 .icon-selected{ outline:3px solid #007aff; }
 .btn{
   width:100%; padding:14px; border:none; border-radius:14px;
   font-size:17px; font-weight:600;
 }
 #motorBtn.start{ background:#34c759; color:#fff; }
 #motorBtn.stop{  background:#ff3b30; color:#fff; }
 input[type=range]{
   width:100%;
   -webkit-appearance:none;
   height:14px;
   border-radius:999px;
   background:#d1d1d6;
 }
 input[type=range]::-webkit-slider-thumb{
   -webkit-appearance:none;
   width:22px; height:22px;
   border-radius:50%;
   background:#ffffff;
   border:1px solid #d1d1d6;
   box-shadow:0 2px 6px rgba(0,0,0,0.3);
   margin-top:-4px;
 }
 input[type=range]::-moz-range-thumb{
   width:22px; height:22px;
   border-radius:50%;
   background:#ffffff;
   border:1px solid #d1d1d6;
   box-shadow:0 2px 6px rgba(0,0,0,0.3);
 }
 .hue-slider::-webkit-slider-runnable-track{
   height:14px;
   border-radius:999px;
   background:linear-gradient(to right,
     #ff0000 0%,
     #ffff00 14%,
     #00ff00 28%,
     #00ffff 42%,
     #ffffff 48%,
     #ffffff 52%,
     #0000ff 58%,
     #ff00ff 72%,
     #ff0000 100%
   );
 }
 .hue-slider::-moz-range-track{
   height:14px;
   border-radius:999px;
   background:linear-gradient(to right,
     #ff0000 0%,
     #ffff00 14%,
     #00ff00 28%,
     #00ffff 42%,
     #ffffff 48%,
     #ffffff 52%,
     #0000ff 58%,
     #ff00ff 72%,
     #ff0000 100%
   );
 }
 .bright-slider::-webkit-slider-runnable-track{
   height:14px;
   border-radius:999px;
   background:linear-gradient(to right,#000000,#ffffff);
 }
 .bright-slider::-moz-range-track{
   height:14px;
   border-radius:999px;
   background:linear-gradient(to right,#000000,#ffffff);
 }
 .hidden{ display:none; }
 select{ width:100%; padding:12px; border-radius:12px; border:1px solid #d1d1d6; }
 </style>
 </head>
 <body>
 <div class="container">
 
 <h2>Rotary Table</h2>
 <div id="versionInfo" style="text-align:center;font-size:13px;color:#8e8e93;margin-top:4px;"></div>
 
 <!-- MODE -->
 <div class="card">
   <h3>Mode</h3>
   <div class="icon-grid">
     <div class="icon-tile" onclick="setMode(0)" id="m0">Static</div>
     <div class="icon-tile" onclick="setMode(1)" id="m1">Animation</div>
     <div class="icon-tile" onclick="setMode(2)" id="m2">Stage</div>
   </div>
 </div>
 
 <!-- COLOR -->
 <div class="card" id="colorCard" ondblclick="openRgbModal()">
   <h3>LED Color</h3>
   <small style="color:#8e8e93;">Doppelklick: RGB-Werte eingeben</small>
   <br>
   Hue
   <input id="hue" class="hue-slider" type="range" min="0" max="255" onchange="setHue(this.value)">
   <br><br>
   Brightness
   <input id="bright" class="bright-slider" type="range" min="0" max="255" onchange="setBrightness(this.value)">
 </div>
 
 <!-- RGB Modal -->
 <div id="rgbModal" style="display:none; position:fixed; inset:0; background:rgba(0,0,0,0.5); align-items:center; justify-content:center;">
   <div style="background:#fff; border-radius:16px; padding:18px; width:280px; box-shadow:0 8px 30px rgba(0,0,0,0.3);">
     <h3 style="margin-top:0;">RGB-Farbe</h3>
     <div style="display:grid; gap:10px;">
       <label>R <input id="rgbR" type="number" min="0" max="255" style="width:100%; padding:8px; border-radius:10px; border:1px solid #d1d1d6;"></label>
       <label>G <input id="rgbG" type="number" min="0" max="255" style="width:100%; padding:8px; border-radius:10px; border:1px solid #d1d1d6;"></label>
       <label>B <input id="rgbB" type="number" min="0" max="255" style="width:100%; padding:8px; border-radius:10px; border:1px solid #d1d1d6;"></label>
     </div>
     <div style="display:flex; justify-content:flex-end; gap:10px; margin-top:16px;">
       <button onclick="closeRgbModal()" style="padding:10px 14px; border:none; border-radius:14px; background:#d1d1d6;">Abbrechen</button>
       <button onclick="saveRgb()" style="padding:10px 14px; border:none; border-radius:14px; background:#34c759; color:#fff;">Speichern</button>
     </div>
   </div>
 </div>
 
 <!-- ANIMATION -->
 <div class="card hidden" id="animCard">
   <h3>Animation</h3>
   <select id="animSel" onchange="setAnim(this.value)">
     <option value="0">Color Wave</option>
     <option value="1">Rainbow</option>
     <option value="2">Dual Wave</option>
   </select>
   <br><br>
   Speed
   <input id="animspd" type="range" min="1" max="50" onchange="setAnimSpeed(this.value)">
 </div>
 
 <!-- STAGE -->
 <div class="card hidden" id="stageCard">
   <h3>Stage Mode</h3>
   <div style="display:flex;justify-content:center;margin:10px 0 8px 0;">
     <canvas id="stageCanvas" width="260" height="260" style="background:#f5f5f7;border-radius:24px;"></canvas>
   </div>
   <small style="display:block;text-align:center;color:#8e8e93;">
     Drag the two points on the circumference.
   </small>
   <!-- versteckte Slider für Kompatibilität -->
   <input id="stagesz" type="range" min="1" max="27" onchange="setStageSize(this.value)" style="display:none;">
   <input id="stagepos" type="range" min="0" max="26" onchange="setStagePos(this.value)" style="display:none;">
 </div>
 
 <!-- MOTOR -->
 <div class="card">
   <h3>Motor</h3>
   <button id="motorBtn" class="btn start" onclick="toggleMotor()">Start</button>
   <br><br>
   <div style="display:flex;gap:10px;">
     <button class="btn" onclick="fetch('/motor/right')">&#8592; Left</button>
     <button class="btn" onclick="fetch('/motor/left')">Right &#8594;</button>
   </div>
   <br>
   Speed
   <input id="motorspd" type="range" min="100" max="1000" onchange="setMotorSpeed(this.value)">
 </div>
 
 <div style="text-align:center; margin-top:20px; padding:10px; color:#8e8e93; font-size:12px; border-top:1px solid #d1d1d6;">
   <small>For bug reports or feature requests, please contact: 
     <a href="mailto:sales.piodeer@gmail.com?subject=Rotating%20Table%20(Bug/Fix/Extension)" 
        style="color:#34c759; text-decoration:none;">sales.piodeer@gmail.com</a>
   </small>
 </div>
 
 <script>
 let motorState = 0;
 let currentMode = 0;
 const LED_COUNT = )HTML";
 // NUM_LEDS wird vom C++-Compiler als String eingefügt
 page += String(NUM_LEDS);
 page += R"HTML(;
 let useCustomColor = )HTML";
 page += useCustomColor ? "true" : "false";
 page += R"HTML(;
 let customRgb = { r: )HTML";
 page += String(customColor.r);
 page += R"HTML(, g: )HTML";
 page += String(customColor.g);
 page += R"HTML(, b: )HTML";
 page += String(customColor.b);
 page += R"HTML( };
 
 function setMode(m){
   currentMode = m;
   fetch("/mode/" + (m==0?"static":m==1?"anim":"stage"));
   ["m0","m1","m2"].forEach(id=>document.getElementById(id).classList.remove("icon-selected"));
   document.getElementById("m"+m).classList.add("icon-selected");
   document.getElementById("animCard").classList.add("hidden");
   document.getElementById("stageCard").classList.add("hidden");
   if(m==1) document.getElementById("animCard").classList.remove("hidden");
   if(m==2) document.getElementById("stageCard").classList.remove("hidden");
 }
 
 function toggleMotor(){
   let b=document.getElementById("motorBtn");
   if(motorState==0){
     motorState=1; fetch("/motor/start");
     b.classList.remove("start"); b.classList.add("stop");
     b.innerHTML="Stop";
   } else {
     motorState=0; fetch("/motor/stop");
     b.classList.remove("stop"); b.classList.add("start");
     b.innerHTML="Start";
   }
 }
 
 function setHue(v){ fetch("/hue?v="+v); }
 function setBrightness(v){ fetch("/brightness?v="+v); }
 function setAnim(v){ fetch("/anim?a="+v); }
 function setAnimSpeed(v){ fetch("/animspeed?v="+v); }
 function setStageSize(v){ fetch("/stagesize?v="+v); }
 function setStagePos(v){ fetch("/stagepos?v="+v); }
 function setMotorSpeed(v){ fetch("/motorspeed?v="+v); }
 
 function openRgbModal(){
   document.getElementById('rgbR').value = customRgb.r;
   document.getElementById('rgbG').value = customRgb.g;
   document.getElementById('rgbB').value = customRgb.b;
   document.getElementById('rgbModal').style.display = 'flex';
 }
 
 function closeRgbModal(){
   document.getElementById('rgbModal').style.display = 'none';
 }
 
 function saveRgb(){
   const r = parseInt(document.getElementById('rgbR').value, 10);
   const g = parseInt(document.getElementById('rgbG').value, 10);
   const b = parseInt(document.getElementById('rgbB').value, 10);
   if ([r,g,b].some(n=>isNaN(n) || n<0 || n>255)) {
     alert('Werte müssen 0–255 sein');
     return;
   }
   customRgb = { r, g, b };
   useCustomColor = true;
   fetch(`/rgb?r=${r}&g=${g}&b=${b}`);
   closeRgbModal();
 }
 
 // --- Stage Mode Canvas (wie dein Beispiel) ---
 const STAGE_POSITIONS = LED_COUNT;
 let stagePoints = [
   { index: 0, x: 0, y: 0 },
   { index: Math.floor(STAGE_POSITIONS/2), x: 0, y: 0 }
 ];
 let stageDragPoint = null;
 
 function stageUpdatePointCoords(canvas, center, radius) {
   stagePoints.forEach(p => {
     const angle = (p.index / STAGE_POSITIONS) * 2 * Math.PI - Math.PI/2;
     p.x = center.x + radius * Math.cos(angle);
     p.y = center.y + radius * Math.sin(angle);
   });
 }
 
 function stageDraw(canvas){
   const ctx = canvas.getContext('2d');
   const center = { x: canvas.width/2, y: canvas.height/2 };
   const radius = Math.min(canvas.width, canvas.height)/2 - 16;
 
   stageUpdatePointCoords(canvas, center, radius);
 
   ctx.clearRect(0,0,canvas.width,canvas.height);
 
   // Alle LED-Positionen als kleine Punkte zeichnen
   for(let i = 0; i < STAGE_POSITIONS; i++){
     const angle = (i / STAGE_POSITIONS) * 2 * Math.PI - Math.PI/2;
     const x = center.x + radius * Math.cos(angle);
     const y = center.y + radius * Math.sin(angle);
     ctx.beginPath();
     ctx.arc(x, y, 3, 0, 2*Math.PI);
     ctx.fillStyle = '#d1d1d6';
     ctx.fill();
   }
 
   // Aktive LEDs (blau) hervorheben
   let start = stagePoints[0].index;
   let end = stagePoints[1].index;
   let count = (end - start + STAGE_POSITIONS) % STAGE_POSITIONS + 1;
   for(let i = 0; i < count; i++){
     let idx = (start + i) % STAGE_POSITIONS;
     let angle = (idx / STAGE_POSITIONS) * 2 * Math.PI - Math.PI/2;
     let x = center.x + radius * Math.cos(angle);
     let y = center.y + radius * Math.sin(angle);
     ctx.beginPath();
     ctx.arc(x, y, 4, 0, 2*Math.PI);
     ctx.fillStyle = '#007aff';
     ctx.fill();
   }
 
   // Drag-Punkte (schwarz, größer)
   ctx.fillStyle = '#111111';
   stagePoints.forEach((p, idx)=>{
     ctx.beginPath();
     ctx.arc(p.x, p.y, 8, 0, 2*Math.PI);
     ctx.fill();
     // Index-Label
     ctx.fillStyle = '#ffffff';
     ctx.font = 'bold 10px -apple-system,sans-serif';
     ctx.textAlign = 'center';
     ctx.textBaseline = 'middle';
     ctx.fillText(p.index, p.x, p.y);
     ctx.fillStyle = '#111111';
   });
 }
 
 function stageGetClosestPoint(x,y){
   return stagePoints.find(p => Math.hypot(p.x - x, p.y - y) < 12) || null;
 }
 
 function stageApplyToBackend(){
   // Segmentdefinition wie im Canvas: start = points[0], end = points[1] (vorwärts, inkl. Wrap)
   const start = stagePoints[0].index;
   const end = stagePoints[1].index;
   const size = (end - start + STAGE_POSITIONS) % STAGE_POSITIONS + 1;
 
   // Slider für Kompatibilität
   document.getElementById("stagepos").value = start;
   document.getElementById("stagesz").value = size;
 
   setStagePos(start);
   setStageSize(size);
 }
 
 function initStageCanvas(){
   const canvas = document.getElementById('stageCanvas');
   if(!canvas) return;
   const rect = canvas.getBoundingClientRect();
   const center = { x: canvas.width/2, y: canvas.height/2 };
   const radius = Math.min(canvas.width, canvas.height)/2 - 16;
 
   stageUpdatePointCoords(canvas, center, radius);
   stageDraw(canvas);
 
   const getLocalCoords = (clientX, clientY) => {
     const r = canvas.getBoundingClientRect();
     return {
       x: clientX - r.left,
       y: clientY - r.top
     };
   };
 
   const handlePointerDown = (clientX, clientY) => {
     const pos = getLocalCoords(clientX, clientY);
     stageDragPoint = stageGetClosestPoint(pos.x, pos.y);
   };
 
   const handlePointerMove = (clientX, clientY) => {
     if(!stageDragPoint) return;
     const pos = getLocalCoords(clientX, clientY);
     const dx = pos.x - center.x;
     const dy = pos.y - center.y;
     const angle = Math.atan2(dy, dx);
     let index = Math.round(((angle + Math.PI/2 + 2*Math.PI) % (2*Math.PI)) / (2*Math.PI) * STAGE_POSITIONS);
     stageDragPoint.index = (index + STAGE_POSITIONS) % STAGE_POSITIONS;
     stageDraw(canvas);
   };
 
   const handlePointerUp = () => {
     if(stageDragPoint){
       stageApplyToBackend();
     }
     stageDragPoint = null;
   };
 
   // Maus-Events
   canvas.addEventListener('mousedown', e=>{
     handlePointerDown(e.clientX, e.clientY);
   });
   canvas.addEventListener('mousemove', e=>{
     handlePointerMove(e.clientX, e.clientY);
   });
   ['mouseup','mouseleave'].forEach(evName=>{
     canvas.addEventListener(evName, ()=>{
       handlePointerUp();
     });
   });
 
   // Touch-Events (für Mobile)
   canvas.addEventListener('touchstart', e=>{
     e.preventDefault();
     if(e.touches.length>0){
       const t = e.touches[0];
       handlePointerDown(t.clientX, t.clientY);
     }
   }, {passive:false});
 
   canvas.addEventListener('touchmove', e=>{
     e.preventDefault();
     if(e.touches.length>0){
       const t = e.touches[0];
       handlePointerMove(t.clientX, t.clientY);
     }
   }, {passive:false});
 
   canvas.addEventListener('touchend', e=>{
     e.preventDefault();
     handlePointerUp();
   }, {passive:false});
 }
 
 window.addEventListener("load", ()=>{
   initStageCanvas();
 
   // Versions-Check
   const vi = document.getElementById("versionInfo");
   const meta = document.querySelector('meta[name="fw-version"]');
   const localVersion = meta ? (meta.getAttribute("content") || "").trim() : "";
   if (vi && localVersion) {
     vi.textContent = "Running firmware version " + localVersion + " … checking for updates …";
     fetch("https://raw.githubusercontent.com/piodeer/RotaryTable_LEDeffects/main/files/version.txt")
       .then(r => r.text())
       .then(txt => {
         const remote = (txt || "").trim();
         const localNum  = parseFloat(localVersion) || 0;
         const remoteNum = parseFloat(remote) || 0;
         if (!remote) {
           vi.textContent = "Version " + localVersion + " (could not check for updates)";
           return;
         }
         if (localNum >= remoteNum) {
           vi.innerHTML = '<div style="display:inline-block;padding:4px 10px;border-radius:999px;background:#34c759;color:#ffffff;font-weight:500;">On newest version (' 
             + localVersion + ')</div>';
         } else {
           vi.innerHTML = 
             "Installed version " + localVersion + " - newest is " + remote + ". " +
             '<a href="https://github.com/piodeer/RotaryTable_LEDeffects/tree/main/sketch" ' +
             'style="color:#007aff;text-decoration:none;" target="_blank" rel="noopener">Get the newest version here</a>.';
         }
       })
       .catch(() => {
         vi.textContent = "Version " + localVersion + " (could not check for updates)";
       });
   }
 });
 </script>
 
   </div>
 </body>
 </html>
 )HTML";
}
 
 /*************************************************************
  *                      WEB HANDLERS
  *************************************************************/
 void handleRoot(){ server.send(200,"text/html",page); }
 
 void setModeStatic(){ mode=0; S.mode=0; saveSettings(); server.send(200,"OK"); }
 void setModeAnim(){   mode=1; S.mode=1; saveSettings(); server.send(200,"OK"); }
 void setModeStage(){  mode=2; S.mode=2; saveSettings(); server.send(200,"OK"); }
 
 void setHueHandler(){
   hue = (uint8_t)server.arg("v").toInt();
   S.hue = hue;
   useCustomColor = false;
   S.customMode = false;
   saveSettings();
   server.send(200,"OK");
 }
 
 void setBrightnessHandler(){
   brightness = (uint8_t)server.arg("v").toInt();
   S.brightness = brightness;
   FastLED.setBrightness(brightness);
   saveSettings();
   server.send(200,"OK");
 }
 
 void setRGBHandler(){
   int r = server.arg("r").toInt();
   int g = server.arg("g").toInt();
   int b = server.arg("b").toInt();
   if (r < 0) r = 0; if (r > 255) r = 255;
   if (g < 0) g = 0; if (g > 255) g = 255;
   if (b < 0) b = 0; if (b > 255) b = 255;
   useCustomColor = true;
   customColor = CRGB(r, g, b);
   S.customMode = true;
   S.customR = r;
   S.customG = g;
   S.customB = b;
   saveSettings();
   server.send(200,"OK");
 }
 
 void setAnimHandler(){
   currentAnimation = server.arg("a").toInt();
   if(currentAnimation<0) currentAnimation=0;
   if(currentAnimation>2) currentAnimation=2;
   S.anim = currentAnimation; saveSettings();
   server.send(200,"OK");
 }
 
 void setAnimSpeedHandler(){
   animSpeed = server.arg("v").toInt();
   if(animSpeed<1) animSpeed=1;
   if(animSpeed>50) animSpeed=50;
   S.animSpeed = animSpeed; saveSettings();
   server.send(200,"OK");
 }
 
 void setStageSizeHandler(){
   stageSize = server.arg("v").toInt();
   if(stageSize<1) stageSize=1;
   if(stageSize>NUM_LEDS) stageSize=NUM_LEDS;
   S.stageSize = stageSize; saveSettings();
   server.send(200,"OK");
 }
 
 void setStagePosHandler(){
   int uiPos = server.arg("v").toInt();
   if(uiPos<0) uiPos=0;
   if(uiPos>=NUM_LEDS) uiPos=NUM_LEDS-1;
   stagePosition = uiPos;
   S.stagePos = stagePosition; saveSettings();
   server.send(200,"OK");
 }
 
 void motorStart(){ motorRunning = true; server.send(200,"OK"); }
 void motorStop(){  motorRunning = false; server.send(200,"OK"); }
 void motorLeft(){  motorDirection=-1; S.motorDir=-1; saveSettings(); server.send(200,"OK"); }
 void motorRight(){ motorDirection= 1; S.motorDir= 1; saveSettings(); server.send(200,"OK"); }
 
 void setMotorSpeedHandler(){
   motorSpeed = server.arg("v").toInt();
   if(motorSpeed<100) motorSpeed=100;
   if(motorSpeed>1000) motorSpeed=3000;
   S.motorSpeed = motorSpeed; 
 
   // Wenn der Motor läuft, neue Zielgeschwindigkeit sofort spürbar machen
   if (motorRunning) {
     currentMotorSpeed = motorSpeed;
     stepper.setSpeed(currentMotorSpeed * motorDirection);
   }
 
   saveSettings();
   server.send(200,"OK");
 }
 
 /*************************************************************
  *                      SETUP
  *************************************************************/
 void setup(){
   Serial.begin(115200);
 
   EEPROM.begin(512);
   loadSettings();
 
   FastLED.addLeds<WS2812B, LED_PIN, RGB>(leds, NUM_LEDS);
   FastLED.setBrightness(brightness);
 
   stepper.setMaxSpeed(1200);
   stepper.setAcceleration(800);
 
   Serial.print("Verbinde mit WLAN: ");
   Serial.println(ssid);
   WiFi.begin(ssid, password);
   unsigned long wifiStart = millis();
   while(WiFi.status() != WL_CONNECTED) {
     if(millis() - wifiStart > 15000) {
       Serial.println("FEHLER: WLAN-Verbindung fehlgeschlagen!");
       break;
     }
     delay(200);
     Serial.print(".");
   }
   if(WiFi.status() == WL_CONNECTED) {
     Serial.println();
     Serial.print("WLAN verbunden! IP-Adresse: ");
     Serial.println(WiFi.localIP());
   }
 
   server.on("/", handleRoot);
 
   server.on("/mode/static", setModeStatic);
   server.on("/mode/anim",   setModeAnim);
   server.on("/mode/stage",  setModeStage);
 
   server.on("/hue",         setHueHandler);
   server.on("/brightness",  setBrightnessHandler);
   server.on("/anim",        setAnimHandler);
   server.on("/animspeed",   setAnimSpeedHandler);
   server.on("/stagesize",   setStageSizeHandler);
   server.on("/stagepos",    setStagePosHandler);
   server.on("/rgb",          setRGBHandler);
 
   server.on("/motor/start", motorStart);
   server.on("/motor/stop",  motorStop);
   server.on("/motor/left",  motorLeft);
   server.on("/motor/right", motorRight);
   server.on("/motorspeed",  setMotorSpeedHandler);
 
   buildPage();
   server.begin();
 }
 
 /*************************************************************
  *                      LOOP
  *************************************************************/
 void loop(){
   server.handleClient();
 
   // LED timing - independent from motor
   unsigned long now = millis();
   if (now - lastLoopTick >= 4) {  // ~250 Hz LED updates
     lastLoopTick = now;
     
     switch(mode){
       case 0: runStatic();    break;
       case 1: runAnimation(); break;
       case 2: runStage();     break;
     }
   }
 
   // Motor runs completely independently at max speed
   updateMotorIndependent();
 }
