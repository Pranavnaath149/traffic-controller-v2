# traffic-controller-v2
V2: Decentralized dual-intersection traffic control using ESP-NOW P2P token negotiation, LM324 op-amp conditioning, and emergency preemption.

# Decentralized Multi-Intersection Traffic Control System (P2P Token Negotiation) 🚦

A fault-tolerant, decentralized traffic signal automation architecture that coordinates adjacent intersections without a central server or cloud dependency. Built using dual ESP32 nodes communicating via connectionless **ESP-NOW**, analog multiplexer-based hardware prioritization, and a structured Finite State Machine (FSM) token protocol.

Documented in research co-authored at SRM Institute of Science and Technology:  
*"A Decentralised Autonomous Multi-Intersection Traffic Control System Using Sensor Input with MUX Implementation and Peer-to-Peer Token Negotiation"* (Pranav Naath B, Shubham Prasad, Dr. K Vaishnavi).

---

## ⚡ Key Architectural Highlights

* **Decentralized Coordination:** Eliminates single points of failure by replacing cloud servers with direct peer-to-peer (P2P) wireless token negotiation over **ESP-NOW**.
* **Hardware-Level Prioritization:** Integrates a **74HC4051 8:1 analog multiplexer** and **LM324 op-amp comparators** to evaluate 8 IR vehicle detection channels simultaneously, eliminating sequential polling latency.
* **Token Negotiation FSM:** When a junction requests a green phase change, it transmits a token request; the adjacent junction evaluates local safety conditions and responds with **ACCEPT**, **DELAY**, or **REJECT**.
* **Safety-First Preemption:**
  * **Emergency Corridors:** Detects priority vehicles and clears an East-West green corridor across both intersections within ~3.1 seconds.
  * **Pedestrian Interlocks:** Prevents emergency preemption from overriding active pedestrian crossing phases, issuing a temporary `DELAY` response until crosswalks are clear.
* **Fault Tolerance & Zero Deadlock:** Over 500 tested negotiation cycles recorded zero deadlocks, utilizing an 8-second implicit timeout fallback to prevent gridlock during RF interference.
* **Self-Hosted Dark-Mode Dashboard:** Junction A serves an embedded web interface (`http://192.168.4.1`) over SoftAP for live phase monitoring and manual corridor overrides.

---

## 🛠️ Hardware Stack & Interfacing

| Subsystem | Components Used | Role |
| :--- | :--- | :--- |
| **Microcontrollers** | 2x ESP32 WROOM-32 | Core decision logic, SoftAP server, ESP-NOW transceiver |
| **Signal Conditioning** | 2x LM324 Quad Op-Amps | Compares IR sensor analog voltages against calibrated thresholds |
| **Multiplexing** | CD74HC4051 / 74HC4067 | 8-to-1 hardware sensor multiplexing to single ADC/GPIO |
| **Signal Amplification** | ULN2803A Darlington Arrays | Sinks current (up to 500mA) for high-brightness R/Y/G LEDs |
| **Vehicle / Pedestrian Detection** | 8x Active-Low IR Modules | Lane occupancy and approach sensing |

---

## 📊 Experimental Results & Benchmarks

* **Average ESP-NOW Round-Trip Latency:** **8 ms** (measured at 15m physical separation).
* **Emergency Preemption Latency:** **3.1 seconds** (dominated by the mandatory safe yellow vehicle clearance interval).
* **Protocol Reliability:** **92.6% immediate ACCEPT**, **4.2% timeout-fallback**, **0 deadlocks** across 500 automated cycles.

---

## 💻 Firmware Modules

* `/firmware/junction_a_master.ino`: Master adaptive junction, SoftAP host, HTTP JSON endpoint (`/data`), and primary scheduler.
* `/firmware/junction_b_peer.ino`: Peer junction client, state broadcaster (`ST|...`), and pedestrian safety interlock evaluator.
