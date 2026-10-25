// ===================================================
// JUNCTION A — FINAL CLEAN
// MAC : 88:57:21:B2:48:10
// Peer: B0:CB:D8:CF:BA:4C (JN-B)
// WiFi: TrafficSystem / 12345678
// Dashboard: http://192.168.4.1
// ===================================================

#include <WiFi.h>
#include <esp_now.h>
#include <WebServer.h>

uint8_t peerMAC[] = {0xB0, 0xCB, 0xD8, 0xCF, 0xBA, 0x4C};

WebServer server(80);

#define R1_RED    13
#define R1_YELLOW 12
#define R1_GREEN  14
#define R2_RED    27
#define R2_YELLOW 26
#define R2_GREEN  25
#define R3_RED    33
#define R3_YELLOW 15
#define R3_GREEN  23
#define R4_RED    22
#define R4_YELLOW 21
#define R4_GREEN  19
#define PED_BLUE_LED 18
#define PED_NS    32
#define PED_EW    35
#define MUX_S0    4
#define MUX_S1    16
#define MUX_S2    17
#define MUX_S3    5
#define MUX_SIG   34

#define BASE_GREEN_TIME   4000
#define EXTRA_PER_VEH     2000
#define MAX_GREEN_TIME    12000
#define YELLOW_TIME       2000
#define PED_TIME          5000
#define PRINT_INTERVAL    3000
#define STARVE_LIMIT      20000
#define EMG_CORRIDOR_TIME 10000
#define EMG_TIMEOUT       8000
#define EMG_RETRY_TIME    3000

enum Phase       { GREEN_PHASE, YELLOW_PHASE, PED_PHASE };
enum SystemState { NORMAL, EMG_WAIT, EMG_ACTIVE, EMG_DELAY };

Phase       currentPhase = GREEN_PHASE;
SystemState sysState     = NORMAL;

int           currentRoad     = 1;
unsigned long phaseStart      = 0;
unsigned long pedStart        = 0;
unsigned long lastPrint       = 0;
unsigned long lockedGreenTime = 0;
unsigned long emgSentTime     = 0;
unsigned long emgStartTime    = 0;
unsigned long delayRetryTime  = 0;

bool pedActive      = false;
bool pedNSRequested = false;
bool pedEWRequested = false;

int           trafficCount[4] = {0};
unsigned long lastServed[4]   = {0};

char jnbRoad[4]   = "?";
char jnbPhase[8]  = "?";
char jnbPed[10]   = "?";
char jnbState[12] = "?";

volatile bool newMsgFlag = false;
char          recvBuf[64] = {0};

void onDataRecv(const esp_now_recv_info_t *info,
                const uint8_t *data, int len)
{
  int n = len < 63 ? len : 63;
  memcpy(recvBuf, data, n);
  recvBuf[n] = '\0';
  newMsgFlag = true;
}

void espSend(const char* msg)
{
  esp_now_send(peerMAC, (uint8_t*)msg, strlen(msg));
}

void parseJNBStatus(String msg)
{
  int p1 = msg.indexOf('|');
  int p2 = msg.indexOf('|', p1+1);
  int p3 = msg.indexOf('|', p2+1);
  int p4 = msg.indexOf('|', p3+1);
  if (p1<0||p2<0||p3<0||p4<0) return;
  msg.substring(p1+1, p2).toCharArray(jnbRoad,  sizeof(jnbRoad));
  msg.substring(p2+1, p3).toCharArray(jnbPhase, sizeof(jnbPhase));
  msg.substring(p3+1, p4).toCharArray(jnbPed,   sizeof(jnbPed));
  msg.substring(p4+1).toCharArray(jnbState,     sizeof(jnbState));
}

void selectMux(int ch)
{
  digitalWrite(MUX_S0,  ch & 1);
  digitalWrite(MUX_S1, (ch >> 1) & 1);
  digitalWrite(MUX_S2, (ch >> 2) & 1);
  digitalWrite(MUX_S3, (ch >> 3) & 1);
}

int readMux(int ch)
{
  selectMux(ch);
  delayMicroseconds(5);
  return digitalRead(MUX_SIG);
}

void readTraffic()
{
  memset(trafficCount, 0, sizeof(trafficCount));
  for (int ch=0; ch<8; ch++)
    if (readMux(ch)==LOW)
      trafficCount[ch/2]++;
}

void allRed()
{
  digitalWrite(R1_RED,HIGH); digitalWrite(R2_RED,HIGH);
  digitalWrite(R3_RED,HIGH); digitalWrite(R4_RED,HIGH);
  digitalWrite(R1_GREEN,LOW); digitalWrite(R2_GREEN,LOW);
  digitalWrite(R3_GREEN,LOW); digitalWrite(R4_GREEN,LOW);
  digitalWrite(R1_YELLOW,LOW); digitalWrite(R2_YELLOW,LOW);
  digitalWrite(R3_YELLOW,LOW); digitalWrite(R4_YELLOW,LOW);
}

void setGreen(int r)
{
  allRed();
  if(r==1){digitalWrite(R1_GREEN,HIGH);digitalWrite(R1_RED,LOW);}
  if(r==2){digitalWrite(R2_GREEN,HIGH);digitalWrite(R2_RED,LOW);}
  if(r==3){digitalWrite(R3_GREEN,HIGH);digitalWrite(R3_RED,LOW);}
  if(r==4){digitalWrite(R4_GREEN,HIGH);digitalWrite(R4_RED,LOW);}
  lastServed[r-1]=millis();
}

void setYellow(int r)
{
  allRed();
  if(r==1){digitalWrite(R1_YELLOW,HIGH);digitalWrite(R1_RED,LOW);}
  if(r==2){digitalWrite(R2_YELLOW,HIGH);digitalWrite(R2_RED,LOW);}
  if(r==3){digitalWrite(R3_YELLOW,HIGH);digitalWrite(R3_RED,LOW);}
  if(r==4){digitalWrite(R4_YELLOW,HIGH);digitalWrite(R4_RED,LOW);}
}

void setEWGreen()
{
  allRed();
  digitalWrite(R2_GREEN,HIGH); digitalWrite(R2_RED,LOW);
  digitalWrite(R4_GREEN,HIGH); digitalWrite(R4_RED,LOW);
}

bool isNSRoad(int r){ return (r==1||r==3); }
bool isEWRoad(int r){ return (r==2||r==4); }

unsigned long computeGreenTime(int road)
{
  unsigned long t = BASE_GREEN_TIME + trafficCount[road-1]*EXTRA_PER_VEH;
  if (t>MAX_GREEN_TIME) t=MAX_GREEN_TIME;
  return t;
}

int chooseNextRoad()
{
  unsigned long now = millis();
  for (int i=0;i<4;i++)
    if (now-lastServed[i]>STARVE_LIMIT) return i+1;
  int best=0, bestScore=-1;
  for (int i=0;i<4;i++)
    if (trafficCount[i]>bestScore)
    { bestScore=trafficCount[i]; best=i+1; }
  if (bestScore==0) return (currentRoad%4)+1;
  return best;
}

void resumeNormal()
{
  sysState        = NORMAL;
  currentPhase    = GREEN_PHASE;
  lockedGreenTime = computeGreenTime(currentRoad);
  setGreen(currentRoad);
  phaseStart      = millis();
  Serial.println("[JN-A] Returned to NORMAL");
}

void sendToken()
{
  espSend("TOKEN:EW");
  emgSentTime = millis();
  sysState    = EMG_WAIT;
  Serial.println("[JN-A] TOKEN sent — waiting for JN-B...");
}

void handleData()
{
  const char* phaseStr =
    currentPhase==GREEN_PHASE  ? "GREEN"  :
    currentPhase==YELLOW_PHASE ? "YELLOW" : "PED";
  const char* stateStr =
    sysState==NORMAL     ? "NORMAL"     :
    sysState==EMG_WAIT   ? "EMG_WAIT"   :
    sysState==EMG_ACTIVE ? "EMG_ACTIVE" : "EMG_DELAY";
  const char* pedAStr =
    pedActive      ? "ACTIVE"  :
    pedNSRequested ? "NS_WAIT" :
    pedEWRequested ? "EW_WAIT" : "NONE";

  char json[512];
  snprintf(json, sizeof(json),
    "{\"jna\":{\"road\":\"R%d\",\"phase\":\"%s\","
    "\"state\":\"%s\",\"ped\":\"%s\","
    "\"t1\":%d,\"t2\":%d,\"t3\":%d,\"t4\":%d},"
    "\"jnb\":{\"road\":\"%s\",\"phase\":\"%s\","
    "\"ped\":\"%s\",\"state\":\"%s\"}}",
    currentRoad, phaseStr, stateStr, pedAStr,
    trafficCount[0], trafficCount[1],
    trafficCount[2], trafficCount[3],
    jnbRoad, jnbPhase, jnbPed, jnbState
  );

  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200, "application/json", json);
}

void handleRoot()
{
  String html = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DATMISCS — Traffic Control</title>
<link href="https://fonts.googleapis.com/css2?family=Orbitron:wght@400;700;900&family=Share+Tech+Mono&display=swap" rel="stylesheet">
<style>
:root {
  --bg: #040608;
  --surface: #080d12;
  --border: #0f2233;
  --accent: #00e5ff;
  --accent2: #ff6b35;
  --green: #00ff88;
  --yellow: #ffd600;
  --red: #ff1744;
  --text: #cfe8f3;
  --muted: #3a5a6e;
  --grid: rgba(0,229,255,0.03);
}

* { margin:0; padding:0; box-sizing:border-box; }

body {
  background: var(--bg);
  color: var(--text);
  font-family: 'Share Tech Mono', monospace;
  min-height: 100vh;
  overflow-x: hidden;
  position: relative;
}

/* Animated grid background */
body::before {
  content:'';
  position:fixed;
  inset:0;
  background-image:
    linear-gradient(var(--grid) 1px, transparent 1px),
    linear-gradient(90deg, var(--grid) 1px, transparent 1px);
  background-size: 40px 40px;
  pointer-events:none;
  z-index:0;
}

/* Scanline overlay */
body::after {
  content:'';
  position:fixed;
  inset:0;
  background: repeating-linear-gradient(
    0deg,
    transparent,
    transparent 2px,
    rgba(0,0,0,0.15) 2px,
    rgba(0,0,0,0.15) 4px
  );
  pointer-events:none;
  z-index:1;
}

.wrap {
  position:relative;
  z-index:2;
  max-width:1100px;
  margin:0 auto;
  padding:20px 16px 40px;
}

/* ── HEADER ── */
.header {
  text-align:center;
  margin-bottom:32px;
  padding-bottom:24px;
  border-bottom: 1px solid var(--border);
  position:relative;
}

.header::after {
  content:'';
  position:absolute;
  bottom:-1px; left:50%;
  transform:translateX(-50%);
  width:200px; height:1px;
  background: linear-gradient(90deg, transparent, var(--accent), transparent);
}

.badge {
  display:inline-block;
  font-size:0.6rem;
  letter-spacing:4px;
  color:var(--accent);
  border:1px solid var(--accent);
  padding:3px 12px;
  margin-bottom:12px;
  text-transform:uppercase;
  animation: badgePulse 3s ease-in-out infinite;
}

@keyframes badgePulse {
  0%,100% { box-shadow: 0 0 6px rgba(0,229,255,0.3); }
  50%      { box-shadow: 0 0 18px rgba(0,229,255,0.7); }
}

.title {
  font-family: 'Orbitron', monospace;
  font-size: clamp(1rem, 3.5vw, 1.8rem);
  font-weight:900;
  letter-spacing:4px;
  color:#fff;
  line-height:1.2;
  margin-bottom:6px;
}

.title span { color:var(--accent); }

.subtitle {
  font-size:0.65rem;
  letter-spacing:3px;
  color:var(--muted);
  text-transform:uppercase;
}

/* ── EMERGENCY BANNER ── */
.emg-bar {
  display:flex;
  align-items:center;
  justify-content:space-between;
  padding:12px 20px;
  background: var(--surface);
  border: 1px solid var(--border);
  border-left: 3px solid var(--green);
  margin-bottom:24px;
  position:relative;
  overflow:hidden;
  transition: border-color 0.3s;
}

.emg-bar::before {
  content:'';
  position:absolute;
  top:0; left:0; right:0; bottom:0;
  background: linear-gradient(90deg, rgba(0,255,136,0.05), transparent);
  transition: background 0.3s;
}

.emg-bar.active {
  border-left-color: var(--red);
  animation: emgFlash 0.8s ease-in-out infinite;
}

.emg-bar.active::before {
  background: linear-gradient(90deg, rgba(255,23,68,0.1), transparent);
}

@keyframes emgFlash {
  0%,100% { box-shadow: inset 0 0 0 1px rgba(255,23,68,0.2); }
  50%      { box-shadow: inset 0 0 20px rgba(255,23,68,0.3); }
}

.emg-left { display:flex; align-items:center; gap:12px; }

.emg-dot {
  width:10px; height:10px;
  border-radius:50%;
  background:var(--green);
  box-shadow: 0 0 10px var(--green);
  animation: dotPulse 2s infinite;
  transition: background 0.3s, box-shadow 0.3s;
}

.emg-dot.on {
  background:var(--red);
  box-shadow: 0 0 15px var(--red);
  animation: dotFlash 0.5s infinite;
}

@keyframes dotPulse { 0%,100%{opacity:1} 50%{opacity:0.4} }
@keyframes dotFlash { 0%,100%{opacity:1} 50%{opacity:0.2} }

.emg-label {
  font-family:'Orbitron', monospace;
  font-size:0.65rem;
  letter-spacing:3px;
  color:var(--muted);
}

.emg-status {
  font-size:0.9rem;
  font-weight:bold;
  color:var(--green);
  transition: color 0.3s;
  letter-spacing:1px;
}

.emg-status.on { color:var(--red); }

.emg-detail {
  font-size:0.65rem;
  color:var(--muted);
  text-align:right;
  letter-spacing:1px;
}

/* ── JUNCTION GRID ── */
.junctions {
  display:grid;
  grid-template-columns:1fr 1fr;
  gap:16px;
  margin-bottom:20px;
}

@media(max-width:600px) {
  .junctions { grid-template-columns:1fr; }
}

/* ── JUNCTION CARD ── */
.jcard {
  background:var(--surface);
  border:1px solid var(--border);
  padding:0;
  overflow:hidden;
  position:relative;
}

.jcard::before {
  content:'';
  position:absolute;
  top:0; left:0; right:0;
  height:2px;
  background: linear-gradient(90deg, var(--accent), transparent);
}

.jcard-head {
  padding:14px 18px 10px;
  border-bottom:1px solid var(--border);
  display:flex;
  justify-content:space-between;
  align-items:center;
}

.jcard-title {
  font-family:'Orbitron', monospace;
  font-size:0.6rem;
  letter-spacing:3px;
  color:var(--muted);
  text-transform:uppercase;
  display:flex;
  align-items:center;
  gap:8px;
}

.live-dot {
  width:6px; height:6px;
  border-radius:50%;
  background:var(--green);
  box-shadow:0 0 6px var(--green);
  animation:dotPulse 2s infinite;
}

.jcard-type {
  font-size:0.55rem;
  letter-spacing:2px;
  color:var(--accent);
  border:1px solid rgba(0,229,255,0.3);
  padding:2px 8px;
}

.jcard-body { padding:16px 18px; }

.road-display {
  display:flex;
  align-items:baseline;
  gap:12px;
  margin-bottom:12px;
}

.road-num {
  font-family:'Orbitron', monospace;
  font-size:3.5rem;
  font-weight:900;
  color:#fff;
  line-height:1;
  text-shadow: 0 0 30px rgba(255,255,255,0.2);
}

.road-info { display:flex; flex-direction:column; gap:4px; }

.phase-badge {
  display:inline-block;
  font-size:0.65rem;
  letter-spacing:2px;
  font-weight:bold;
  padding:3px 10px;
  border:1px solid;
  transition: all 0.3s;
}

.phase-badge.GREEN  { color:var(--green);  border-color:var(--green);  background:rgba(0,255,136,0.08); box-shadow:0 0 10px rgba(0,255,136,0.2); }
.phase-badge.YELLOW { color:var(--yellow); border-color:var(--yellow); background:rgba(255,214,0,0.08);  box-shadow:0 0 10px rgba(255,214,0,0.2); }
.phase-badge.PED    { color:var(--accent); border-color:var(--accent); background:rgba(0,229,255,0.08); box-shadow:0 0 10px rgba(0,229,255,0.2); }
.phase-badge.EMG    { color:var(--red);    border-color:var(--red);    background:rgba(255,23,68,0.08);  box-shadow:0 0 10px rgba(255,23,68,0.2); }

.state-tag {
  font-size:0.6rem;
  letter-spacing:1px;
  color:var(--muted);
}

.stats-row {
  display:grid;
  grid-template-columns:1fr 1fr;
  gap:6px;
  margin-top:12px;
}

.stat {
  background:rgba(255,255,255,0.02);
  border:1px solid var(--border);
  padding:8px 10px;
  display:flex;
  flex-direction:column;
  gap:2px;
}

.stat-lbl { font-size:0.55rem; letter-spacing:2px; color:var(--muted); text-transform:uppercase; }
.stat-val { font-size:0.85rem; color:#fff; letter-spacing:1px; }

/* ── TRAFFIC BARS (JN-A only) ── */
.traffic-section {
  margin-top:14px;
  padding-top:12px;
  border-top:1px solid var(--border);
}

.traffic-label {
  font-size:0.55rem;
  letter-spacing:3px;
  color:var(--muted);
  margin-bottom:10px;
  text-transform:uppercase;
}

.bars {
  display:grid;
  grid-template-columns:repeat(4,1fr);
  gap:8px;
}

.bwrap { display:flex; flex-direction:column; align-items:center; gap:4px; }

.bbg {
  width:100%;
  height:52px;
  background:rgba(255,255,255,0.03);
  border:1px solid var(--border);
  display:flex;
  align-items:flex-end;
  overflow:hidden;
  position:relative;
}

.bfill {
  width:100%;
  background: linear-gradient(180deg, var(--accent), rgba(0,229,255,0.3));
  transition:height 0.6s cubic-bezier(0.4,0,0.2,1);
  min-height:2px;
  position:relative;
}

.bfill::after {
  content:'';
  position:absolute;
  top:0; left:0; right:0;
  height:2px;
  background:var(--accent);
  box-shadow:0 0 8px var(--accent);
}

.blbl { font-size:0.6rem; color:var(--muted); letter-spacing:1px; }
.bcnt { font-size:0.75rem; color:var(--accent); font-weight:bold; }

/* ── SIGNAL LIGHTS VISUAL ── */
.lights {
  display:flex;
  gap:6px;
  margin-top:10px;
}

.light {
  width:12px; height:12px;
  border-radius:50%;
  border:1px solid var(--border);
  transition: all 0.3s;
}

.light.r { background:#1a0005; border-color:#ff1744; }
.light.y { background:#1a1000; border-color:#ffd600; }
.light.g { background:#001a0a; border-color:#00ff88; }
.light.r.on { background:var(--red);    box-shadow:0 0 12px var(--red);    border-color:var(--red); }
.light.y.on { background:var(--yellow); box-shadow:0 0 12px var(--yellow); border-color:var(--yellow); }
.light.g.on { background:var(--green);  box-shadow:0 0 12px var(--green);  border-color:var(--green); }

/* ── FOOTER ── */
.footer {
  display:flex;
  justify-content:space-between;
  align-items:center;
  padding-top:16px;
  border-top:1px solid var(--border);
  font-size:0.6rem;
  color:var(--muted);
  letter-spacing:2px;
}

.pulse-ring {
  display:inline-block;
  width:8px; height:8px;
  border-radius:50%;
  background:var(--accent);
  box-shadow:0 0 6px var(--accent);
  animation:dotPulse 1.5s infinite;
  margin-right:6px;
}

/* corner decorations */
.jcard::after {
  content:'';
  position:absolute;
  bottom:0; right:0;
  width:20px; height:20px;
  border-bottom:1px solid var(--accent);
  border-right:1px solid var(--accent);
  opacity:0.3;
}
</style>
</head>
<body>
<div class="wrap">

  <!-- HEADER -->
  <div class="header">
    <div class="badge">ESP32 · ESP-NOW · Real-Time P2P Network</div>
    <div class="title">DECENTRALIZED AUTONOMOUS<br><span>MULTI-INTERSECTION</span> TRAFFIC CONTROL</div>
    <div class="subtitle">Safe Emergency Vehicle Preemption System · ESP-NOW P2P Network</div>
  </div>

  <!-- EMERGENCY BANNER -->
  <div class="emg-bar" id="emgBox">
    <div class="emg-left">
      <div class="emg-dot" id="emgDot"></div>
      <div>
        <div class="emg-label">Emergency Corridor Status</div>
        <div class="emg-status" id="emgSt">● INACTIVE — Normal Operation</div>
      </div>
    </div>
    <div class="emg-detail" id="emgDt">All junctions nominal</div>
  </div>

  <!-- JUNCTIONS -->
  <div class="junctions">

    <!-- JN-A -->
    <div class="jcard">
      <div class="jcard-head">
        <div class="jcard-title"><div class="live-dot"></div>Junction A</div>
        <div class="jcard-type">Adaptive · IR Sensing</div>
      </div>
      <div class="jcard-body">
        <div class="road-display">
          <div class="road-num" id="aRoad">—</div>
          <div class="road-info">
            <span class="phase-badge GREEN" id="aPhase">—</span>
            <span class="state-tag" id="aSt">—</span>
          </div>
        </div>
        <div class="lights">
          <div class="light r" id="aLR"></div>
          <div class="light y" id="aLY"></div>
          <div class="light g" id="aLG"></div>
        </div>
        <div class="stats-row">
          <div class="stat">
            <div class="stat-lbl">Pedestrian</div>
            <div class="stat-val" id="aPed">—</div>
          </div>
          <div class="stat">
            <div class="stat-lbl">Network</div>
            <div class="stat-val" style="color:var(--green)">ESP-NOW</div>
          </div>
        </div>
        <div class="traffic-section">
          <div class="traffic-label">Vehicle Density Per Road</div>
          <div class="bars">
            <div class="bwrap"><div class="bbg"><div class="bfill" id="b1" style="height:4px"></div></div><div class="blbl">R1</div><div class="bcnt" id="c1">0</div></div>
            <div class="bwrap"><div class="bbg"><div class="bfill" id="b2" style="height:4px"></div></div><div class="blbl">R2</div><div class="bcnt" id="c2">0</div></div>
            <div class="bwrap"><div class="bbg"><div class="bfill" id="b3" style="height:4px"></div></div><div class="blbl">R3</div><div class="bcnt" id="c3">0</div></div>
            <div class="bwrap"><div class="bbg"><div class="bfill" id="b4" style="height:4px"></div></div><div class="blbl">R4</div><div class="bcnt" id="c4">0</div></div>
          </div>
        </div>
      </div>
    </div>

    <!-- JN-B -->
    <div class="jcard">
      <div class="jcard-head">
        <div class="jcard-title"><div class="live-dot"></div>Junction B</div>
        <div class="jcard-type">Fixed Cycle · Timed</div>
      </div>
      <div class="jcard-body">
        <div class="road-display">
          <div class="road-num" id="bRoad">—</div>
          <div class="road-info">
            <span class="phase-badge GREEN" id="bPhase">—</span>
            <span class="state-tag" id="bSt">—</span>
          </div>
        </div>
        <div class="lights">
          <div class="light r" id="bLR"></div>
          <div class="light y" id="bLY"></div>
          <div class="light g" id="bLG"></div>
        </div>
        <div class="stats-row">
          <div class="stat">
            <div class="stat-lbl">Pedestrian</div>
            <div class="stat-val" id="bPed">—</div>
          </div>
          <div class="stat">
            <div class="stat-lbl">Cycle Time</div>
            <div class="stat-val" style="color:var(--accent)">6s / 2s</div>
          </div>
        </div>
        <div class="stats-row" style="margin-top:6px">
          <div class="stat" style="grid-column:1/-1">
            <div class="stat-lbl">Token Negotiation Protocol</div>
            <div class="stat-val" id="bNeg" style="color:var(--muted)">Standby</div>
          </div>
        </div>
      </div>
    </div>

  </div>

  <!-- FOOTER -->
  <div class="footer">
    <div><span class="pulse-ring"></span>Live · Refresh 1s</div>
    <div id="upd">Connecting...</div>
    <div>192.168.4.1</div>
  </div>

</div>

<script>
function setLights(prefix, phase) {
  document.getElementById(prefix+'LR').className = 'light r' + (phase==='RED'||phase==='YELLOW'||phase==='PED' ? ' on':'');
  document.getElementById(prefix+'LY').className = 'light y' + (phase==='YELLOW' ? ' on':'');
  document.getElementById(prefix+'LG').className = 'light g' + (phase==='GREEN' ? ' on':'');
}

function refresh() {
  fetch('/data')
  .then(r => r.json())
  .then(d => {
    const a = d.jna, b = d.jnb;

    // JN-A
    document.getElementById('aRoad').textContent = a.road;
    const ap = document.getElementById('aPhase');
    const phA = a.state !== 'NORMAL' ? 'EMG' : a.phase;
    ap.textContent = a.state !== 'NORMAL' ? a.state : a.phase;
    ap.className = 'phase-badge ' + phA;
    document.getElementById('aSt').textContent = 'STATE: ' + a.state;
    document.getElementById('aPed').textContent = a.ped;
    setLights('a', a.phase);

    [1,2,3,4].forEach(i => {
      const v = a['t'+i] || 0;
      document.getElementById('b'+i).style.height = Math.max(4, (v/2)*100) + '%';
      document.getElementById('c'+i).textContent = v;
    });

    // JN-B
    document.getElementById('bRoad').textContent = b.road;
    const bp = document.getElementById('bPhase');
    const phB = b.state !== 'NORMAL' ? 'EMG' : b.phase;
    bp.textContent = b.state !== 'NORMAL' ? b.state : b.phase;
    bp.className = 'phase-badge ' + phB;
    document.getElementById('bSt').textContent = 'STATE: ' + b.state;
    document.getElementById('bPed').textContent = b.ped;
    setLights('b', b.phase);

    const neg = document.getElementById('bNeg');
    if (b.state === 'EMG_ACTIVE') {
      neg.textContent = '⚡ CORRIDOR ACTIVE — EW GREEN';
      neg.style.color = 'var(--red)';
    } else if (a.state === 'EMG_WAIT') {
      neg.textContent = '◈ TOKEN SENT — AWAITING RESPONSE';
      neg.style.color = 'var(--yellow)';
    } else if (a.state === 'EMG_DELAY') {
      neg.textContent = '⏳ DELAYED — RETRY PENDING';
      neg.style.color = 'var(--yellow)';
    } else {
      neg.textContent = 'Standby';
      neg.style.color = 'var(--muted)';
    }

    // Emergency banner
    const emg = a.state !== 'NORMAL' || b.state === 'EMG_ACTIVE';
    const box = document.getElementById('emgBox');
    const dot = document.getElementById('emgDot');
    const st  = document.getElementById('emgSt');
    const dt  = document.getElementById('emgDt');
    if (emg) {
      box.className = 'emg-bar active';
      dot.className = 'emg-dot on';
      st.className  = 'emg-status on';
      st.textContent = '⚠ ACTIVE — EW CORRIDOR PREEMPTION';
      dt.textContent = 'JN-A: ' + a.state + ' · JN-B: ' + b.state;
    } else {
      box.className = 'emg-bar';
      dot.className = 'emg-dot';
      st.className  = 'emg-status';
      st.textContent = '● INACTIVE — Normal Operation';
      dt.textContent = 'All junctions nominal';
    }

    document.getElementById('upd').textContent =
      'Updated: ' + new Date().toLocaleTimeString();
  })
  .catch(() => {
    document.getElementById('upd').textContent = 'Reconnecting...';
  });
}

setInterval(refresh, 1000);
refresh();
</script>
</body>
</html>
)rawhtml";
  server.send(200,"text/html",html);
}

void printStatus()
{
  const char* phaseStr =
    currentPhase==GREEN_PHASE  ? "GREEN"  :
    currentPhase==YELLOW_PHASE ? "YELLOW" : "PED";
  const char* stateStr =
    sysState==NORMAL     ? "NORMAL"     :
    sysState==EMG_WAIT   ? "EMG_WAIT"   :
    sysState==EMG_ACTIVE ? "EMG_ACTIVE" : "EMG_DELAY";

  Serial.println("======== JN-A STATUS ========");
  Serial.print("  Road  : R"); Serial.println(currentRoad);
  Serial.print("  Phase : "); Serial.println(phaseStr);
  Serial.print("  State : "); Serial.println(stateStr);
  Serial.print("  Traffic: R1="); Serial.print(trafficCount[0]);
  Serial.print(" R2="); Serial.print(trafficCount[1]);
  Serial.print(" R3="); Serial.print(trafficCount[2]);
  Serial.print(" R4="); Serial.println(trafficCount[3]);
  Serial.print("  JN-B  : ");
  Serial.print(jnbRoad); Serial.print(" ");
  Serial.print(jnbPhase); Serial.print(" Ped=");
  Serial.println(jnbPed);
  Serial.println("  [E = Emergency]");
  Serial.println("=============================");
}

void setup()
{
  Serial.begin(115200);

  int outPins[] = {
    R1_RED,R1_YELLOW,R1_GREEN,
    R2_RED,R2_YELLOW,R2_GREEN,
    R3_RED,R3_YELLOW,R3_GREEN,
    R4_RED,R4_YELLOW,R4_GREEN,
    PED_BLUE_LED,
    MUX_S0,MUX_S1,MUX_S2,MUX_S3
  };
  for (int p : outPins) pinMode(p, OUTPUT);

  pinMode(PED_NS,  INPUT);
  pinMode(PED_EW,  INPUT);
  pinMode(MUX_SIG, INPUT_PULLUP);

  WiFi.mode(WIFI_AP);
  WiFi.softAP("TrafficSystem", "12345678", 1);
  Serial.print("[JN-A] AP IP: ");
  Serial.println(WiFi.softAPIP());

  if (esp_now_init() != ESP_OK)
  {
    Serial.println("[JN-A] ESP-NOW FAILED");
    return;
  }
  esp_now_register_recv_cb(onDataRecv);

  esp_now_del_peer(peerMAC);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, peerMAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx   = WIFI_IF_AP;
  esp_now_add_peer(&peer);

  server.on("/",     handleRoot);
  server.on("/data", handleData);
  server.begin();

  Serial.println("[JN-A] WiFi: TrafficSystem / 12345678");
  Serial.println("[JN-A] Open: http://192.168.4.1");

  lockedGreenTime = computeGreenTime(currentRoad);
  setGreen(currentRoad);
  phaseStart = millis();
}

void loop()
{
  unsigned long now = millis();

  if (newMsgFlag)
  {
    newMsgFlag = false;
    String msg = String(recvBuf);

    if (msg.startsWith("ST|"))
      parseJNBStatus(msg);
    else if (sysState==EMG_WAIT || sysState==EMG_DELAY)
    {
      if (msg=="ACCEPT")
      {
        sysState=EMG_ACTIVE; emgStartTime=millis();
        setEWGreen();
        Serial.println("[JN-A] ACCEPT — Corridor ACTIVE");
      }
      else if (msg=="DELAY")
      {
        sysState=EMG_DELAY; delayRetryTime=millis();
        setEWGreen();
        Serial.println("[JN-A] DELAY — retrying in 3s");
      }
      else if (msg=="REJECT")
      {
        sysState=EMG_ACTIVE; emgStartTime=millis();
        setEWGreen();
        Serial.println("[JN-A] REJECT — local only");
      }
    }
  }

  if (Serial.available())
  {
    char c = Serial.read();
    if ((c=='e'||c=='E') && sysState==NORMAL)
      sendToken();
  }

  if (sysState==EMG_WAIT && now-emgSentTime>EMG_TIMEOUT)
  {
    Serial.println("[JN-A] Timeout — proceeding locally");
    sysState=EMG_ACTIVE; emgStartTime=millis();
    setEWGreen();
  }

  if (sysState==EMG_DELAY && now-delayRetryTime>EMG_RETRY_TIME)
  {
    Serial.println("[JN-A] Retrying TOKEN...");
    espSend("TOKEN:EW");
    sysState=EMG_WAIT; emgSentTime=millis();
  }

  if (sysState==EMG_ACTIVE)
  {
    setEWGreen();
    if (now-emgStartTime>=EMG_CORRIDOR_TIME)
    {
      Serial.println("[JN-A] Corridor done — CLEAR sent");
      espSend("CLEAR");
      resumeNormal();
    }
    goto status_print;
  }

  readTraffic();
  if (digitalRead(PED_NS)==LOW) pedNSRequested = true;
  if (digitalRead(PED_EW)==LOW) pedEWRequested = true;

  if (pedActive)
  {
    currentPhase=PED_PHASE;
    allRed();
    digitalWrite(PED_BLUE_LED,HIGH);
    if (now-pedStart>=PED_TIME)
    {
      pedActive=false;
      digitalWrite(PED_BLUE_LED,LOW);
      lockedGreenTime=computeGreenTime(currentRoad);
      setGreen(currentRoad);
      phaseStart=millis();
      currentPhase=GREEN_PHASE;
    }
    goto status_print;
  }

  if (currentPhase==GREEN_PHASE &&
      now-phaseStart>=lockedGreenTime)
  {
    currentPhase=YELLOW_PHASE;
    setYellow(currentRoad);
  }

  if (currentPhase==YELLOW_PHASE &&
      now-phaseStart>=lockedGreenTime+YELLOW_TIME)
  {
    if (pedNSRequested && isEWRoad(currentRoad))
    {
      pedActive=true; pedStart=millis();
      pedNSRequested=false;
      allRed(); digitalWrite(PED_BLUE_LED,HIGH);
      goto status_print;
    }
    if (pedEWRequested && isNSRoad(currentRoad))
    {
      pedActive=true; pedStart=millis();
      pedEWRequested=false;
      allRed(); digitalWrite(PED_BLUE_LED,HIGH);
      goto status_print;
    }
    currentRoad=chooseNextRoad();
    lockedGreenTime=computeGreenTime(currentRoad);
    setGreen(currentRoad);
    phaseStart=millis();
    currentPhase=GREEN_PHASE;
  }

status_print:
  server.handleClient();
  if (now-lastPrint>=PRINT_INTERVAL)
  {
    lastPrint=now;
    printStatus();
  }
}