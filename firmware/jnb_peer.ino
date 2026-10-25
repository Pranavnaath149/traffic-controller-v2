// ===================================================
// JUNCTION B — FINAL CLEAN
// MAC : B0:CB:D8:CF:BA:4C
// Peer: 88:57:21:B2:48:11 (JN-A AP MAC)
// ===================================================

#include <WiFi.h>
#include <esp_now.h>

uint8_t peerMAC[] = {0x88, 0x57, 0x21, 0xB2, 0x48, 0x11};

#define R1_RED    13
#define R1_YELLOW 12
#define R1_GREEN  14
#define R2_RED    27
#define R2_YELLOW 26
#define R2_GREEN  25
#define R3_RED    33
#define R3_YELLOW 32
#define R3_GREEN  23
#define R4_RED    22
#define R4_YELLOW 21
#define R4_GREEN  19
#define PED_BLUE_LED 18
#define PED_NS    4
#define PED_EW    16

#define GREEN_TIME        6000
#define YELLOW_TIME       2000
#define PED_TIME          5000
#define PRINT_INTERVAL    3000
#define STATUS_INTERVAL   2000
#define EMG_CORRIDOR_TIME 10000

enum Phase       { GREEN_PHASE, YELLOW_PHASE, PED_PHASE };
enum SystemState { NORMAL, EMG_ACTIVE };

Phase       currentPhase = GREEN_PHASE;
SystemState sysState     = NORMAL;

int           currentRoad  = 1;
unsigned long phaseStart   = 0;
unsigned long pedStart     = 0;
unsigned long lastPrint    = 0;
unsigned long lastStatus   = 0;
unsigned long emgStartTime = 0;

bool pedActive      = false;
bool pedNSRequested = false;
bool pedEWRequested = false;

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

void resumeNormal()
{
  sysState     = NORMAL;
  currentPhase = GREEN_PHASE;
  setGreen(currentRoad);
  phaseStart   = millis();
  Serial.println("[JN-B] Returned to NORMAL");
}

void sendStatusToJNA()
{
  if (millis() - lastStatus < STATUS_INTERVAL) return;
  lastStatus = millis();

  const char* phaseStr =
    currentPhase==GREEN_PHASE  ? "GREEN"  :
    currentPhase==YELLOW_PHASE ? "YELLOW" : "PED";
  const char* stateStr =
    sysState==NORMAL ? "NORMAL" : "EMG_ACTIVE";
  const char* pedStr =
    pedActive      ? "ACTIVE"  :
    pedNSRequested ? "NS_WAIT" :
    pedEWRequested ? "EW_WAIT" : "NONE";

  char msg[64];
  snprintf(msg, sizeof(msg), "ST|R%d|%s|%s|%s",
           currentRoad, phaseStr, pedStr, stateStr);
  espSend(msg);
}

void evaluateToken()
{
  Serial.println("[JN-B] TOKEN received — evaluating...");

  if (pedActive || pedNSRequested || pedEWRequested)
  {
    Serial.println("[JN-B] Decision: DELAY");
    espSend("DELAY");
    return;
  }

  Serial.println("[JN-B] Decision: ACCEPT");
  espSend("ACCEPT");
  sysState     = EMG_ACTIVE;
  emgStartTime = millis();
  setEWGreen();
  Serial.println("[JN-B] EW GREEN — Corridor ACTIVE");
}

void printStatus()
{
  Serial.println("======== JN-B STATUS ========");
  Serial.print("  Road  : R"); Serial.println(currentRoad);
  Serial.print("  Phase : ");
  Serial.println(currentPhase==GREEN_PHASE?"GREEN":
                 currentPhase==YELLOW_PHASE?"YELLOW":"PED");
  Serial.print("  Ped   : ");
  Serial.println(pedActive?"ACTIVE":pedNSRequested?"NS_WAIT":
                 pedEWRequested?"EW_WAIT":"NONE");
  Serial.print("  State : ");
  Serial.println(sysState==NORMAL?"NORMAL":"EMG_ACTIVE");
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
    PED_BLUE_LED
  };
  for (int p : outPins) pinMode(p, OUTPUT);

  pinMode(PED_NS, INPUT_PULLUP);
  pinMode(PED_EW, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.begin("TrafficSystem", "12345678");

  Serial.print("[JN-B] Connecting to JN-A hotspot");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20)
  {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED)
    Serial.println("\n[JN-B] Connected");
  else
    Serial.println("\n[JN-B] Connection failed");

  if (esp_now_init() != ESP_OK)
  {
    Serial.println("[JN-B] ESP-NOW FAILED");
    return;
  }
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, peerMAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx   = WIFI_IF_STA;
  esp_now_add_peer(&peer);

  Serial.println("[JN-B] READY");
  setGreen(currentRoad);
  phaseStart = millis();
}

void loop()
{
  unsigned long now = millis();

  if (digitalRead(PED_NS)==LOW) pedNSRequested = true;
  if (digitalRead(PED_EW)==LOW) pedEWRequested = true;

  if (newMsgFlag)
  {
    newMsgFlag = false;
    String msg = String(recvBuf);

    if (msg=="TOKEN:EW" && sysState==NORMAL)
      evaluateToken();
    else if (msg=="TOKEN:EW" && sysState==EMG_ACTIVE)
      espSend("DELAY");
    else if (msg=="CLEAR")
    {
      Serial.println("[JN-B] CLEAR received");
      resumeNormal();
    }
  }

  sendStatusToJNA();

  if (sysState==EMG_ACTIVE)
  {
    setEWGreen();
    if (now-emgStartTime>=EMG_CORRIDOR_TIME+2000)
    {
      Serial.println("[JN-B] Auto-clear");
      resumeNormal();
    }
    goto status_print;
  }

  if (pedActive)
  {
    currentPhase=PED_PHASE;
    allRed();
    digitalWrite(PED_BLUE_LED,HIGH);
    if (now-pedStart>=PED_TIME)
    {
      pedActive=false;
      digitalWrite(PED_BLUE_LED,LOW);
      setGreen(currentRoad);
      phaseStart=millis();
      currentPhase=GREEN_PHASE;
    }
    goto status_print;
  }

  if (currentPhase==GREEN_PHASE && now-phaseStart>=GREEN_TIME)
  {
    currentPhase=YELLOW_PHASE;
    setYellow(currentRoad);
  }

  if (currentPhase==YELLOW_PHASE &&
      now-phaseStart>=GREEN_TIME+YELLOW_TIME)
  {
    bool isNS=(currentRoad==1||currentRoad==3);
    bool isEW=(currentRoad==2||currentRoad==4);

    if (pedNSRequested && isEW)
    {
      pedActive=true; pedStart=millis();
      pedNSRequested=false;
      allRed(); digitalWrite(PED_BLUE_LED,HIGH);
      goto status_print;
    }
    if (pedEWRequested && isNS)
    {
      pedActive=true; pedStart=millis();
      pedEWRequested=false;
      allRed(); digitalWrite(PED_BLUE_LED,HIGH);
      goto status_print;
    }

    currentRoad=(currentRoad%4)+1;
    setGreen(currentRoad);
    phaseStart=millis();
    currentPhase=GREEN_PHASE;
  }

status_print:
  if (now-lastPrint>=PRINT_INTERVAL)
  {
    lastPrint=now;
    printStatus();
  }
}