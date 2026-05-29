# Architecture pixfrog

Document de référence pour l'architecture logicielle de **pixfrog**, un contrôleur LED 8 canaux haute performance basé sur ESP32-P4, piloté par ArtNet sur Ethernet.

> Ce document est normatif : tout module doit le respecter ou amender ce fichier avant tout changement de design.

---

## 1. Vue d'ensemble

pixfrog est conçu autour d'un principe central : **découpler la réception réseau (jitter, bursts) du rendu LED (rigueur temporelle)** via plusieurs étages double-bufferisés. Chaque étage tourne sur un cœur dédié, ne partage que des pointeurs atomiques, et n'alloue jamais en chemin chaud.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              ESP32-P4 dual-core                              │
│                                                                              │
│  Core 0 (NETWORK + UI)                Core 1 (RENDER + DMA)                  │
│  ┌────────────────────────┐           ┌────────────────────────┐             │
│  │ lwIP / Ethernet RMII   │           │ render_task            │             │
│  │   │                    │           │   prio 20              │             │
│  │   ▼                    │           │   │                    │             │
│  │ artnet_rx_task         │           │   │ 1. swap universes  │             │
│  │   prio 10              │           │   │ 2. encode FULL     │             │
│  │   │ writes             │           │   │    frame into      │             │
│  │   ▼                    │           │   │    fb_back (PSRAM) │             │
│  │ universe_pool[2][N]    │ ───────►  │   │ 3. esp_cache_msync │             │
│  │ (PSRAM, swap atomic)   │           │   │ 4. draw_bitmap →   │             │
│  │                        │           │   │    swap fb_active  │             │
│  │ ui_task                │           │   │ 5. wait done_sem   │             │
│  │   prio 4               │           │   │                    │             │
│  │   (idle when no event) │           │   ▼                    │             │
│  │                        │           │ frame_buf[2] (PSRAM)   │             │
│  │ config_store           │           │   ▲                    │             │
│  │ (NVS, called from UI)  │           │   │ GDMA pulls bytes   │             │
│  │                        │           │   │ at PCLK rate       │             │
│  │                        │           │ LCD_CAM peripheral     │             │
│  │                        │           │   │ → 16 GPIOs         │             │
│  └────────────────────────┘           └────────────────────────┘             │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Principe clé** : la frame N+1 s'encode dans `frame_buf[back]` (PSRAM) pendant que GDMA pulse la frame N depuis `frame_buf[front]`. Le swap est un échange de pointeur après `esp_cache_msync` et `esp_lcd_panel_draw_bitmap`. Aucun risque d'underrun car l'encodeur n'a pas de contrainte temps-réel par-chunk : il a toute la durée d'émission de la frame courante pour produire la suivante.

---

## 2. Choix du framework de build

**ESP-IDF v5.3+ natif (CMake + `idf.py`).**

Justifications :

1. **Support ESP32-P4 stable** : PlatformIO a historiquement plusieurs mois de retard sur le support des nouvelles cibles. ESP32-P4 (HiFive E24 + RV64) requiert un toolchain RISC-V récent que seule l'IDF officielle maintient.
2. **LCD_CAM bas niveau** : nous accédons à des registres non couverts par les drivers HAL et toute couche d'abstraction PlatformIO complique le débogage.
3. **`sdkconfig` granulaire** : tuning lwIP, FreeRTOS, PSRAM octal, IRAM pour ISR — tous accessibles via `idf.py menuconfig`.
4. **Composants tiers** : ESP-IDF Component Registry (`espressif/esp_eth_driver_*`, `espressif/esp_lcd_panel_io`) est natif IDF.
5. **CI reproductible** : un Dockerfile officiel `espressif/idf:v5.3` rend les builds bit-perfect.

Conséquence : l'arborescence respecte la convention IDF (`main/`, `components/`, `sdkconfig.defaults`, `CMakeLists.txt` à la racine).

---

## 3. Topologie des tâches FreeRTOS

| Tâche                | Cœur | Priorité | Pile  | Source de réveil              | Rôle                                              |
|----------------------|:----:|:--------:|:-----:|-------------------------------|---------------------------------------------------|
| `lwip_tcpip_thread`  | 0    | 18       | 4 ko  | mailbox lwIP                  | Stack TCP/IP IDF (fourni)                         |
| `eth_rx_task`        | 0    | 19       | 2 ko  | IRQ Ethernet                  | RMII → lwIP (fourni)                              |
| `artnet_rx_task`     | 0    | 10       | 4 ko  | `lwip_recvfrom` bloquant      | Parse paquets, écrit `universe_pool[back]`        |
| `render_task`        | 1    | 20       | 8 ko  | Timer 60 Hz + `ArtSync`       | Swap universes, encode frame complète → `fb_back` (PSRAM), kick LCD_CAM, attend `done_sem` |
| `ui_task`            | 0    | 4        | 4 ko  | IRQ encodeur (INT) + timer 1s | Lit encodeur, met à jour OLED, écrit NVS          |
| `idle_0` / `idle_1`  | 0/1  | 0        | 1 ko  | (FreeRTOS)                    | Power-save hooks                                  |

> **Note** : il n'y a **pas** de tâche `lcd_cam_refill_task`. Le frame buffer complet vit en PSRAM ; GDMA balaie ce buffer en une transaction, l'ISR `on_color_trans_done` libère `done_sem` à la fin. L'encodeur n'a aucune contrainte temps-réel sous-trame.

**Règles d'affinité strictes** :

- Tout ce qui touche au DMA LCD_CAM ou à un buffer DMA → cœur 1.
- Tout ce qui touche à lwIP ou à un syscall réseau → cœur 0.
- L'UI est sur le cœur 0 mais en priorité basse — elle ne bloque jamais le réseau parce qu'elle attend une IRQ ou un timer.

**Aucune tâche ne fait `vTaskDelay` dans le chemin chaud**. Les attentes sont toujours sur des primitives de synchronisation IDF (sémaphores, event groups, queues).

---

## 4. Cycle de vie d'une frame

Une « frame » est l'intervalle entre deux rendus LED (33.33 ms à 30 Hz, 16.67 ms à 60 Hz).

```
t = 0 ms       : render_task est réveillé par le timer
                 a) swap atomique universe_front ↔ universe_back
                 b) pour chaque canal :
                    décode universes → pixels (DMX offset, color order, brightness,
                    grouping, invert) dans son pixel_buf interne,
                    puis encode pixels → samples directement dans fb_back (PSRAM)
                 c) esp_cache_msync(fb_back, ..., DIR_C2M)   // writeback cache
                 d) esp_lcd_panel_draw_bitmap(panel, fb_back) // déclenche swap + GDMA
                 e) xSemaphoreTake(done_sem)                  // attend fin émission précédente
t = 0..encode  : core 1 encode la frame entière (~5-15 ms typique selon pixel count)
t = encode..   : GDMA pull continu depuis PSRAM vers LCD_CAM FIFO → 16 GPIOs
                 Pendant ce temps, core 1 peut être réveillé pour la frame N+1 du timer suivant.
t = encode + T_dma : ISR on_color_trans_done → xSemaphoreGiveFromISR(done_sem)
                 GPIO reste figé sur les derniers samples (zéros = TRESET pour 1-fil)

Pendant toute la frame, en parallèle sur core 0 :
   artnet_rx_task accumule des paquets dans universe_pool[back]
   ArtSync (optionnel) → note_sync() → swap anticipé au prochain tick
```

**Latence totale réseau → photons** : 2 frames typique (1 frame d'attente + 1 frame d'émission). À 30 Hz = ~66 ms, à 60 Hz = ~33 ms — acceptable pour du lighting.

**Garantie de cohérence** : un `ArtDmx` reçu pendant l'encodage de la frame N est intégré à la frame N+1, jamais panaché.

---

## 5. Budget mémoire

### SRAM interne (768 ko, dont ~512 ko utilisables)

| Poste                                    | Taille      | Justification                                  |
|------------------------------------------|------------:|------------------------------------------------|
| FreeRTOS + lwIP + drivers IDF            | ~120 ko     | Mesuré sur projets similaires                  |
| `pixel_buf[8]` (1 buffer par canal)      | 8 × 4 ko    | 1024 px × 4 octets RGBW, intermédiaire encode  |
| Stacks toutes tâches                     | ~30 ko      | Cf. table §3                                   |
| Heap général                             | ~330 ko     | Marge pour init Ethernet, NVS, OLED            |

`pixel_buf` est un buffer scratch interne au `render_task` pour stocker les pixels décodés (après application DMX offset / color order / brightness) avant l'encodage final dans le frame buffer PSRAM. Pas besoin de double-bufferiser : c'est éphémère par frame.

### PSRAM octal externe (32 Mo)

| Poste                              | Taille      | Justification                                        |
|------------------------------------|------------:|------------------------------------------------------|
| `frame_buf[2]` (PSRAM)             | 2 × ~2.6 Mo | 1024 px × 32 bits × 40 samples × 2 octets (worst case WS2811-slow RGBW) |
| `universe_pool[2][N]`              | 2 × 48 ko   | 48 univers × 512 octets × 2 buffers                  |
| Logs circulaires                   | 64 ko       | Debug post-mortem                                    |
| Marge applicative                  | ~27 Mo      | Disponible pour évolutions (sequencer, FX engine…)   |

**GDMA depuis PSRAM** : supporté nativement par `esp_lcd_new_rgb_panel(flags.fb_in_psram=true)` sur ESP32-P4. Le bus DMA partagé tolère 32 MB/s sustained sur le PSRAM octal 200 MHz (peak ~200 MB/s, partagé avec cache et autres maîtres DMA — marge confortable). Une `esp_cache_msync(DIR_C2M)` est requise après écriture CPU et avant kick DMA.

Pour des configs plus modestes (par exemple aucun canal en WS2811-slow / RGBW), `frame_buf` peut être taillé plus petit dynamiquement — cf. `lcd::init()`.

### Configuration NVS

| Poste                              | Taille      |
|------------------------------------|------------:|
| Config globale (IP, ArtNet, names) | ~256 octets |
| Config par canal × 8               | 8 × 64 octets ≈ 512 octets |
| Total                              | < 1 ko      |

Partition NVS standard 24 ko largement suffisante.

---

## 6. Stratégie de synchronisation

Pixfrog évite délibérément les mutex dans le chemin chaud. Toutes les synchronisations data-plane reposent sur **swap atomique de pointeurs**.

### 6.1 Swap pointeur (universe_pool)

```cpp
// shared state
std::atomic<UniverseBank*> universe_front;  // lu par render_task
UniverseBank* universe_back;                // écrit par artnet_rx_task

// render_task, à chaque tick :
UniverseBank* new_front = universe_back;
universe_back = universe_front.exchange(new_front, std::memory_order_acq_rel);
// new_front est maintenant la lecture cohérente pour cette frame
```

Aucun verrou. Le « back » étant écrit par un seul writer (`artnet_rx_task`) et lu par un seul reader (`render_task`) après swap, la cohérence est garantie par l'ordre acquire/release.

### 6.2 Sémaphore binaire (fin d'émission DMA)

- ISR `on_color_trans_done` LCD_CAM → `xSemaphoreGiveFromISR(done_sem)`
- `render_task` → `xSemaphoreTake(done_sem, timeout)` avant de kicker la frame suivante

Si le `take` time out, on incrémente `dma_underruns` mais on continue avec la frame d'après — pas de cascade d'erreurs. Le swap de `frame_buf` est géré par le périphérique LCD_CAM lui-même via `esp_lcd_panel_draw_bitmap` qui retourne quasi-immédiatement (il enregistre juste le pointeur pour la prochaine émission).

### 6.3 Event groups (config change pending)

L'UI signale un changement via un bit d'event group. Le `render_task` lit le bit en début de tick ; s'il est positionné, il reconfigure le canal concerné (potentiellement reset du protocole, recalcul des samples par bit) entre deux frames. Le bit est consommé par `xEventGroupClearBits`.

```
       ui_task          render_task
         │                  │
   set CHANNEL_3_DIRTY      │
         │   ───────────►   │ (au prochain tick)
                            │ reconfigure channel 3
                            │ clear CHANNEL_3_DIRTY
```

### 6.4 NVS

Accès NVS strictement depuis `ui_task` (cœur 0, basse prio). Une écriture NVS peut bloquer plusieurs ms — c'est acceptable parce que l'UI tolère la latence.

Le `render_task` lit la config via un cache RAM mis à jour par l'UI lors d'un commit (cf. §6.3).

---

## 7. ISR et IRAM

Toutes les ISR sont marquées `IRAM_ATTR` :

- LCD_CAM `on_color_trans_done` ISR (uniquement `xSemaphoreGiveFromISR`)
- IRQ encodeur seesaw (idem)
- Ethernet RX (géré par IDF, déjà en IRAM)

Conséquence : `CONFIG_LCD_CAM_ISR_IRAM_SAFE=y`, `CONFIG_GDMA_ISR_IRAM_SAFE=y`, `CONFIG_FREERTOS_INTERRUPT_BACKTRACE=y`.

> Le frame buffer en PSRAM passe par le cache CPU, mais la chaîne DMA elle-même tape directement le PSRAM via le contrôleur cache (sans CPU). Ce qui est IRAM-sensible est uniquement le code ISR ; le buffer peut résider en PSRAM sans contrainte particulière.

Aucune ISR ne fait d'appel pouvant déclencher un cache miss flash : pas de `printf`, pas de logique fancy, juste give-semaphore et retour.

---

## 8. Stratégie de reconfiguration à chaud (Q4)

Changer le protocole d'un canal modifie potentiellement :

1. Le nombre de samples par bit (encodage NRZ vs clocké)
2. La fréquence PCLK requise (PCLK est globale au LCD_CAM)
3. La taille des buffers pixels (RGB vs RGBW)

**Solution adoptée** : la fréquence PCLK est **fixée au boot** à une valeur compromis (cf. `docs/PROTOCOLS.md` §3). Le nombre de samples par bit est dérivé par canal sans recompiler la PCLK. Les protocoles clockés tournent à PCLK/N où N est calculé pour viser la fréquence CLOCK demandée par l'UI.

Conséquence : **changer de protocole à chaud ne nécessite jamais de réinitialiser le LCD_CAM**. Le `render_task` met simplement à jour l'encodeur du canal concerné entre deux frames :

```
1. UI valide une nouvelle config (event group set + commit NVS)
2. render_task voit le bit dirty en début de tick
3. render_task termine la frame courante normalement (déjà queued)
4. render_task remplace l'encodeur du canal (changement de fonction ptr)
5. render_task réalloue le pixel_buf si la taille a changé (depuis pool pré-alloué)
6. render_task efface le bit dirty
```

Le pool pixel_buf est dimensionné pour le pire cas (1024 px × 4 octets RGBW) — aucune `malloc` n'est faite à chaud.

---

## 9. Modules et frontières

| Composant            | Responsabilité                                     | Dépend de                |
|----------------------|----------------------------------------------------|--------------------------|
| `lcd_cam_output`     | Driver LCD_CAM 16-bit, GDMA, ISR refill            | (HAL IDF uniquement)     |
| `led_protocols`      | Encodeurs par protocole (NRZ, SPI-like)            | (libre)                  |
| `artnet`             | Parser UDP ArtNet, ArtPollReply, écrit pool        | `lwip`                   |
| `dmx_manager`        | Pool d'univers + mapping canal → univers + offset  | `artnet`, `config_store` |
| `config_store`       | Wrappers NVS + cache RAM                           | IDF NVS                  |
| `ui`                 | OLED + encodeur seesaw + menu state machine        | IDF I2C, `config_store`  |
| `boards/<hw>.h`      | Pinout, capacités hardware                         | (header only)            |

**Règle de dépendance** : `lcd_cam_output` ne connaît rien d'ArtNet, et `artnet` ne connaît rien du LCD_CAM. Ils se rencontrent dans `main.cpp` via `dmx_manager` qui orchestre les pointeurs.

---

## 10. Logging et télémétrie

Tags `ESP_LOG` par module (`ARTNET`, `LCD_CAM`, `UI`, `DMX`, `CFG`, `MAIN`). Niveau par défaut `INFO`, ajustable à la compilation.

Compteurs internes exposés via `dmx_manager_get_stats()` :

```cpp
struct DmxStats {
    uint64_t frames_emitted;
    uint64_t artnet_packets_rx;
    uint64_t artnet_bad_packets;
    uint32_t dma_underruns;
    uint32_t current_fps;
};
```

Lus par l'écran HOME. Pas de serveur web, pas de Prometheus — la philosophie est zéro surface réseau hors ArtNet.

---

## 11. Sécurité / robustesse

- Pas de WiFi → pas de surface RF
- ArtNet : aucun écrit possible sur la config depuis le réseau, uniquement réception DMX
- NVS chiffré désactivé volontairement (pas d'enjeu de secret)
- Watchdog matériel activé, kick depuis `render_task` (la tâche temps réel critique)

---

## 12. Évolutions hors scope v1

- Sequencer interne (FX engine)
- sACN E1.31 (envisageable, structure univers identique)
- Web UI (nécessiterait WiFi via co-MCU)
- Sync trame inter-contrôleurs (PTP)
