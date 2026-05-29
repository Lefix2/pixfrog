# Hardware pixfrog

Cibles : ESP32-P4 DevKitC officiel Espressif pour la v0 (prototypage), carte custom pour la v1 (à venir).

> Tous les pinouts sont **modifiables** : seule contrainte stricte, respecter les capacités matérielles du SoC (cf. §6).

---

## 1. Carte cible v0 : ESP32-P4 Function EV Board

Référence Espressif officielle : `ESP32-P4-Function-EV-Board`.

Caractéristiques pertinentes :

- ESP32-P4 dual-core RISC-V 400 MHz
- 32 Mo PSRAM octal soudée
- 16 Mo flash externe
- **PHY Ethernet IP101GRI déjà soudé** avec connecteur RJ45 magnétique
- Header GPIO accessible

Cette carte couvre tous nos besoins pour la v0 sans ajout matériel autre que :

- 1 OLED I2C SSD1306 128×64
- 1 encodeur Adafruit seesaw #4991
- 8 niveaux de level shifters (74HCT245 ou similaire) pour passer 3.3V → 5V vers les strips
- 8 + 8 = 16 GPIOs LED sortis sur connecteurs

---

## 2. Pinout (ESP32-P4 DevKitC v0)

Validé contre le pinout du connecteur header officiel de la Function EV Board. Toutes les broches LED, encodeur INT et status sont sur le connecteur user-facing ; les broches RMII Ethernet (MDC/MDIO + REF_CLK + TX/RX) sont câblées en interne vers le PHY IP101GRI et ne sont pas exposées.

### 2.1 Sorties LED 16-bit parallèle (LCD_CAM)

Le contrôleur LCD_CAM impose 16 GPIOs mappables arbitrairement via le GPIO Matrix. On choisit des pins disponibles sur le connecteur, en évitant les broches de strap (GPIO 0 = boot, GPIO 6 = download/normal mode, GPIO 35 = SDIO).

| Bit | Signal       | GPIO | Rôle                                  |
|----:|--------------|-----:|---------------------------------------|
| 0   | CH1_DATA     | 2    | DATA strip 1                          |
| 1   | CH1_CLOCK    | 3    | CLOCK strip 1 (protocoles clockés)    |
| 2   | CH2_DATA     | 4    | DATA strip 2                          |
| 3   | CH2_CLOCK    | 5    | CLOCK strip 2                         |
| 4   | CH3_DATA     | 22   | DATA strip 3                          |
| 5   | CH3_CLOCK    | 23   | CLOCK strip 3                         |
| 6   | CH4_DATA     | 24   | DATA strip 4                          |
| 7   | CH4_CLOCK    | 25   | CLOCK strip 4                         |
| 8   | CH5_DATA     | 26   | DATA strip 5                          |
| 9   | CH5_CLOCK    | 16   | CLOCK strip 5                         |
| 10  | CH6_DATA     | 32   | DATA strip 6                          |
| 11  | CH6_CLOCK    | 33   | CLOCK strip 6                         |
| 12  | CH7_DATA     | 47   | DATA strip 7                          |
| 13  | CH7_CLOCK    | 48   | CLOCK strip 7                         |
| 14  | CH8_DATA     | 53   | DATA strip 8                          |
| 15  | CH8_CLOCK    | 54   | CLOCK strip 8                         |

**Note** : la PCLK du LCD_CAM (free-running clock du bus parallèle) n'est pas routée vers l'extérieur (`pclk_gpio_num = -1`) — elle reste interne au SoC. Les CLOCK des protocoles clockés (APA102 etc.) sont encodées dans les bits 1, 3, 5… du bus, pas tirées de PCLK. Cf. `docs/PROTOCOLS.md` §3.

### 2.2 Ethernet RMII (interne au board)

Les signaux RMII vers le PHY IP101GRI sont câblés sur la carte EV ; les broches MDC/MDIO sont configurées côté firmware. Aucune n'apparaît sur le connecteur header, donc aucun conflit avec les LED ci-dessus.

| Signal RMII           | GPIO P4   |
|-----------------------|-----------|
| EMAC_REF_CLK          | interne   |
| EMAC_TX_EN / TXD[0:1] | interne   |
| EMAC_RXD[0:1] / CRS_DV | interne  |
| MDC                   | GPIO 29   |
| MDIO                  | GPIO 30   |
| PHY reset             | -1 (RC POR sur PHY) |

### 2.3 I2C (OLED + encodeur)

| Signal | GPIO | Note                                                  |
|--------|-----:|-------------------------------------------------------|
| SDA    | 7    | Marquée `SDA(GPIO7)` sur le silkscreen header         |
| SCL    | 8    | Marquée `SCL(GPIO8)`                                  |
| INT    | 21   | IRQ encodeur seesaw, active LOW                       |

Adresses I2C :
- SSD1306 : `0x3C` ou `0x3D` (selon module)
- seesaw #4991 : `0x36` par défaut

Fréquence I2C : **400 kHz** (Fast Mode), suffisante pour rafraîchir l'OLED à ~10 Hz.

### 2.4 Autres

| Signal       | GPIO | Note                                       |
|--------------|-----:|--------------------------------------------|
| STATUS_LED   | 1    | LED externe (heartbeat)                    |
| DEBUG_TX     | 37   | UART0 console (marquée TXD(GPIO37))        |
| DEBUG_RX     | 38   | UART0 (marquée RXD(GPIO38))                |

---

## 3. Question Q1 — LCD_CAM 16-bit avec horloge arbitraire ?

**Oui.** Le contrôleur LCD_CAM de l'ESP32-P4 hérite du périphérique LCD_CAM ESP32-S3 et le complète. Il offre :

- Mode **i80 (Intel 8080)** : 8/16/24-bit + WR/RD strobes, idéal pour LCD, **non utilisé ici** (overhead CMD/data inutile).
- Mode **RGB (DPI)** : 16/24-bit avec HSYNC/VSYNC, **idéal pour notre usage** — la sortie est continue, pilotée par DMA, sans frame interne au protocole (on désactive HSYNC/VSYNC).

Notre mode opératoire :

1. Configuration LCD_CAM en mode RGB sans HSYNC/VSYNC (signaux ignorés)
2. PCLK généré en interne, divisé depuis `LCD_CLK_SRC_PLL160M` ou `LCD_CLK_SRC_XTAL` selon la précision désirée
3. DMA GDMA pousse en continu les samples 16-bit vers les 16 GPIOs choisis
4. Le bus est strictement free-running tant que la chaîne DMA n'est pas vide

API IDF utilisée : `esp_lcd_panel_io_dpi_*` ou accès direct au registre `LCD_CAM_LCD_USER_REG` selon le niveau de contrôle requis. Pour la PoC : `esp_lcd_panel_io_dpi` avec callbacks DMA EOF custom.

Horloges PCLK accessibles (formule en `docs/PROTOCOLS.md` §3) : de quelques kHz à ~80 MHz. Pour pixfrog la plage cible est **3-20 MHz**.

---

## 4. Question Q2 — Quel PHY Ethernet ?

**Recommandation : IP101GRI** (déjà soudé sur le ESP32-P4 Function EV Board).

Comparatif :

| PHY         | REF_CLK     | Niveau | Avantages                              | Inconvénients                          |
|-------------|-------------|--------|----------------------------------------|----------------------------------------|
| **IP101GRI** | 50 MHz int. ou ext. | 3.3V   | Officiellement supporté Espressif, drivers stables IDF, déjà sur EV Board | Boîtier QFN32, sourcing irrégulier |
| LAN8720A    | 50 MHz ext. uniquement | 3.3V | Très répandu, modules tout faits dispo | Nécessite oscillateur 50 MHz dédié, capricieux sur le reset |
| KSZ8081RNB  | 25 MHz ext. + PLL interne | 3.3V | Robuste, doc Microchip excellente | Plus cher, MDIO peut nécessiter pull-up |
| RTL8201F    | 50 MHz int./ext. | 3.3V | Très bon marché, intégré clock         | Driver IDF moins testé sur P4          |

**Décision** : IP101GRI pour la v0 (zéro effort), réévaluer en v1 selon dispo/sourcing.

Driver IDF : `esp_eth_phy_new_ip101()`. La carte EV expose le PHY sur des GPIOs spécifiques (cf. §2.2).

---

## 5. Question Q3 — CLOCK comme bit du bus LCD_CAM ?

**Oui, et c'est exactement la stratégie retenue.**

Le bus 16-bit du LCD_CAM est piloté par DMA : à chaque tick PCLK, les 16 GPIOs sont mis à jour simultanément depuis l'octet/mot DMA courant. Cela implique :

1. **Synchronisme matériel garanti** entre DATA et CLOCK d'un même canal — ils sortent au même PCLK edge.
2. La forme du signal CLOCK est **encodée dans les bits 1/3/5/.../15 du buffer DMA**, comme n'importe quel DATA.
3. Pour les protocoles 1-fil (WS2815…), le bit CLOCK est simplement laissé à 0 sur tous les samples — le signal en sortie est un niveau LOW constant, sans impact (le strip ignore le pin CLOCK puisqu'il n'en a pas).
4. Pour les protocoles clockés (APA102…), le bit CLOCK alterne selon le ratio PCLK / freq_clock_souhaitée.

**Conséquence formelle** : le débit DMA est identique quel que soit le protocole (PCLK fixé), seul l'encodage des samples diffère.

Encodage typique (cf. `docs/PROTOCOLS.md` §4) :

- 1-fil NRZ : `samples_per_bit` samples par bit de DATA, CLOCK = 0
- Clocké SPI : `samples_per_clock_cycle` samples par cycle CLOCK (la moitié low, la moitié high), DATA stable pendant chaque cycle

---

## 6. Level shifters

Le SoC ESP32-P4 sort en logique **3.3V**. Les strips LED 5V ont des seuils variables :

| Strip               | Vih threshold @ Vcc=5V       | 3.3V direct OK ?         |
|---------------------|------------------------------|--------------------------|
| WS2815              | typique 0.7 × Vcc = 3.5V     | **Non** — limite, à éviter |
| WS2812B             | typique 0.7 × Vcc = 3.5V     | Non (idem)               |
| WS2811              | 0.7 × Vcc = 3.5V             | Non                      |
| SK6812              | 0.65 × Vcc = 3.25V           | Marginal, à éviter prod  |
| APA102 / SK9822     | 0.7 × Vcc = 3.5V             | Non                      |

**Décision** : level shifter obligatoire pour la prod. Pour la PoC v0, on peut tester avec un seul strip court (< 30 LED) près du PSU, le premier WS2815 régénérera le signal et on peut s'en sortir parfois.

**Composant retenu** : **74HCT245** (8 bits en un boîtier), 2 boîtiers couvrent les 16 GPIOs. Le `HCT` (pas HC) est crucial : seuil Vih = 2.0V (CMOS-to-TTL), garantit la commutation depuis du 3.3V CMOS vers du 5V CMOS.

Câblage type 74HCT245 :
```
Vcc       : 5V (côté strip)
GND       : commun MCU/strip
DIR        : haut (A → B fixe)
/OE        : bas (toujours actif)
A0..A7     : entrées 3.3V depuis ESP32-P4
B0..B7     : sorties 5V vers strips
```

> Alternative compacte : **SN74LVC1T45** unitaire pour chaque GPIO, ou **TXS0108E** (mais éviter — l'auto-direction TXS est trop lente pour les protocoles clockés rapides).

---

## 7. Câblage encodeur Adafruit seesaw #4991

L'encodeur seesaw expose tout en I2C :

```
seesaw #4991      ESP32-P4
─────────────     ──────────
Vin (3-5V)   ───► 3.3V
GND          ───► GND
SDA          ───► GPIO 7 (I2C SDA)
SCL          ───► GPIO 8 (I2C SCL)
INT          ───► GPIO 9 (active LOW, pull-up interne du seesaw)
ADR (option) ───► laisser flottant → adresse 0x36
SS (option)  ───► ignoré (seul I2C utilisé)
```

L'INT est configurée côté seesaw pour s'activer sur :
- Rotation encodeur (chaque cran)
- Bouton (press et release distincts)

Implémentation côté firmware :

1. ISR sur falling edge du GPIO 9 → `xSemaphoreGiveFromISR(ui_wakeup_sem)`
2. `ui_task` consomme le sémaphore, fait un `seesaw_read_irq_flags()` (I2C), met à jour son état
3. Pas de polling I2C en boucle — économie d'énergie et de bus

Référence schéma : [Adafruit Learn — I2C QT Rotary Encoder](https://learn.adafruit.com/adafruit-i2c-qt-rotary-encoder).

---

## 8. Câblage OLED SSD1306

```
SSD1306 128×64    ESP32-P4
──────────────    ──────────
VCC (3-5V)   ───► 3.3V
GND          ───► GND
SDA          ───► GPIO 7 (partagé avec encodeur)
SCL          ───► GPIO 8 (partagé)
```

Les pull-ups I2C 4.7 kΩ ne sont nécessaires qu'une fois sur le bus (vérifier que le module ne les a pas déjà).

Rafraîchissement OLED : **10 Hz max** pour économiser la bande passante I2C, le contenu étant principalement statique. L'écran HOME se rafraîchit toutes les 100 ms pour mettre à jour FPS / paquets/s.

---

## 9. Alimentation

- ESP32-P4 + logique 3.3V : LDO ou DC-DC 5V→3.3V (selon EV Board)
- Strips LED 5V : alimentation séparée **avec masse commune** (jamais d'alim flottante)
- Capacité de découplage 1000 µF côté strips, idéalement par segment de 1 m
- Résistance série 470 Ω entre level shifter et premier LED DATA (atténuation de réflexions)

⚠ **Ne jamais alimenter un strip de plus de quelques LEDs depuis le 5V de la carte ESP32** — courant insuffisant et bruit injecté dans le SoC.

---

## 10. Évolutions hardware envisagées (v1 custom)

- PCB custom dual-layer minimum, plan de masse continu
- LDO 1A 3.3V dédié au SoC
- DC-DC isolé pour level shifters si distance > 30 cm aux strips
- 8 connecteurs JST-XH 3 broches (DATA, CLOCK, GND) par canal
- Connecteur Ethernet en façade
- Boîtier alu avec dissipation passive (PSRAM octal chauffe à plein débit)
