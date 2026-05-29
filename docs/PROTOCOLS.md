# Protocoles LED — pixfrog

Référence des timings, formules d'horloge et schémas d'encodage DMA pour tous les protocoles supportés par pixfrog.

> Toute déviation par rapport aux valeurs de ce document doit être justifiée et validée à l'oscilloscope.

---

## 1. Protocoles supportés

| Protocole | Type      | Bit rate / CLOCK max | Order par défaut | Bits par pixel | Note                            |
|-----------|-----------|----------------------|------------------|---------------:|---------------------------------|
| WS2815    | 1-fil NRZ | 800 kbps             | GRB              | 24             | Cible prioritaire, signal robuste |
| WS2812B   | 1-fil NRZ | 800 kbps             | GRB              | 24             | Quasi identique WS2815          |
| WS2811    | 1-fil NRZ | 400 ou 800 kbps      | RGB              | 24             | Variante slow ou fast           |
| SK6812    | 1-fil NRZ | 800 kbps             | GRB ou GRBW      | 24 ou 32       | Variante RGBW courante          |
| WS2814    | 1-fil NRZ | 800 kbps             | RGBW             | 32             | RGBW + canal blanc dédié        |
| APA102    | SPI-like  | 1–30 MHz CLOCK       | BGR              | 32 (start+brightness+BGR) | Brightness 5-bit intégré |
| SK9822    | SPI-like  | 1–30 MHz CLOCK       | BGR              | 32                       | Compatible APA102 timing |
| LPD8806   | SPI-like  | 1–20 MHz CLOCK       | GRB              | 24                       | MSB de chaque octet = 1  |

---

## 2. Timings 1-fil NRZ (référence datasheet)

Valeurs typiques en nanosecondes. T0H = niveau haut pour coder un '0', T1H = haut pour '1', etc. TRESET = temps bas requis entre frames pour latch.

| Protocole | T0H  | T0L  | T1H  | T1L  | TDATA (T0H+T0L = T1H+T1L) | TRESET   |
|-----------|-----:|-----:|-----:|-----:|--------------------------:|---------:|
| WS2815    | 300  | 950  | 950  | 300  | 1 250                     | ≥ 280 µs |
| WS2812B   | 350  | 800  | 700  | 600  | 1 250                     | ≥ 50 µs  |
| WS2811-fast | 350 | 800  | 700  | 600  | 1 250                     | ≥ 50 µs  |
| WS2811-slow | 700 | 1 600 | 1 200 | 1 300 | ~2 500                  | ≥ 50 µs  |
| SK6812    | 300  | 900  | 600  | 600  | 1 200                     | ≥ 80 µs  |
| WS2814    | 300  | 950  | 950  | 300  | 1 250                     | ≥ 280 µs |

Tolérance constructeur : typiquement ±150 ns sur chaque sous-temps. pixfrog vise un centrage **à mi-tolérance** pour rester compatible cross-batch.

---

## 3. Formule d'horloge LCD_CAM

### 3.1 Chaîne d'horloges

```
PLL160M (source) ──► CLK_DIV (entier) ──► PCLK ──► DMA samples ──► 16 GPIOs
```

Formules :

```
f_PCLK   = f_PLL_source / CLK_DIV         avec CLK_DIV ∈ {2, 3, …, 256}
T_PCLK   = 1 / f_PCLK                     période d'un sample DMA
T_bit    = samples_per_bit × T_PCLK       durée d'un bit DATA pour un protocole 1-fil
```

Pour un protocole clocké, on dérive en plus :

```
T_clock_cycle = samples_per_clock × T_PCLK
f_clock_out   = 1 / T_clock_cycle = f_PCLK / samples_per_clock
```

où `samples_per_clock` doit être un entier **pair** ≥ 2 (moitié low, moitié high).

### 3.2 Choix PCLK pour pixfrog

Critère 1 : couvrir précisément le timing WS2815 (T0H = 300 ns, T1H = 950 ns) avec une granularité fine. T_PCLK doit diviser 300 ns et 950 ns avec un reste acceptable.

Critère 2 : permettre des CLOCK rates utiles pour APA102 (au moins 1 MHz, idéalement 6+ MHz).

Critère 3 : rester sous une charge DMA raisonnable (< 30 % bus PSRAM).

**Choix retenu** : `f_PCLK = 16 MHz` (donc `T_PCLK = 62.5 ns`).

Vérification 1-fil :

| Cible (ns) | Samples idéaux | Samples retenus | Réalisé (ns) | Erreur     |
|------------|---------------:|----------------:|-------------:|-----------:|
| T0H = 300  | 4.8            | 5               | 312.5        | +12.5 ns   |
| T0L = 950  | 15.2           | 15              | 937.5        | −12.5 ns   |
| T1H = 950  | 15.2           | 15              | 937.5        | −12.5 ns   |
| T1L = 300  | 4.8            | 5               | 312.5        | +12.5 ns   |
| TDATA      | 20             | 20 (= T0H+T0L)  | 1 250        | 0          |

→ `samples_per_bit = 20` pour le WS2815, total bit = 1 250 ns exact. ✓

Vérification clocké :

```
samples_per_clock = 2   → f_clock = 8 MHz   (APA102 à 8 MHz, OK)
samples_per_clock = 4   → f_clock = 4 MHz
samples_per_clock = 8   → f_clock = 2 MHz
samples_per_clock = 16  → f_clock = 1 MHz
```

Plage CLOCK exploitable : **125 kHz à 8 MHz** par pas correspondant aux diviseurs entiers. Suffisant pour tous les usages réels (APA102 stable à 4-8 MHz sur ~100 LEDs).

### 3.3 Autres protocoles 1-fil avec PCLK=16 MHz

| Protocole | T0H cible | T1H cible | Samples T0H | Samples T1H | Samples bit | Erreur max |
|-----------|----------:|----------:|------------:|------------:|------------:|-----------:|
| WS2815    | 300       | 950       | 5           | 15          | 20          | ±13 ns     |
| WS2812B   | 350       | 700       | 6           | 11          | 20          | ±25 ns     |
| WS2811-fast | 350    | 700       | 6           | 11          | 20          | ±25 ns     |
| WS2811-slow | 700    | 1 200     | 11          | 19          | 40          | ±25 ns     |
| SK6812    | 300       | 600       | 5           | 10          | 19 (~1 187 ns) | ±13 ns  |
| WS2814    | 300       | 950       | 5           | 15          | 20          | ±13 ns     |

Toutes les erreurs sont sous le seuil ±150 ns datasheet → ✓.

---

## 4. Encodage DMA (1-fil vs clocké)

Le buffer DMA est une séquence de samples 16-bit. Pour chaque sample, chaque bit représente l'état d'un GPIO à cet instant PCLK.

```
sample[t] = (CH8_CLOCK << 15) | (CH8_DATA << 14) | … | (CH1_CLOCK << 1) | CH1_DATA
```

### 4.1 Encodage 1-fil NRZ (exemple WS2815, samples_per_bit=20)

Pour encoder le bit `1` sur la voie CH1_DATA :

```
sample[0]  : CH1_DATA = 1
sample[1]  : CH1_DATA = 1
…
sample[14] : CH1_DATA = 1   // 15 samples HIGH = 937.5 ns ≈ T1H
sample[15] : CH1_DATA = 0
…
sample[19] : CH1_DATA = 0   // 5 samples LOW = 312.5 ns ≈ T1L
```

Pour encoder le bit `0` :

```
sample[0]  : CH1_DATA = 1
…
sample[4]  : CH1_DATA = 1   // 5 samples HIGH = 312.5 ns ≈ T0H
sample[5]  : CH1_DATA = 0
…
sample[19] : CH1_DATA = 0   // 15 samples LOW = 937.5 ns ≈ T0L
```

CH1_CLOCK reste à 0 sur les 20 samples (le pin CLOCK existe mais n'est jamais connecté à un strip 1-fil).

Optimisation : on précalcule deux templates 16-bit (`tpl_bit0`, `tpl_bit1`) par protocole et on les copie par mémo, plus rapide qu'un `if` par sample.

### 4.2 Encodage clocké SPI-like (exemple APA102, samples_per_clock=4 → 4 MHz)

Pour transmettre 1 bit DATA cadencé sur 1 cycle CLOCK :

```
sample[0]  : CH1_CLOCK = 0, CH1_DATA = bit  // setup
sample[1]  : CH1_CLOCK = 0, CH1_DATA = bit  // continue setup
sample[2]  : CH1_CLOCK = 1, CH1_DATA = bit  // rising edge — strip latches DATA
sample[3]  : CH1_CLOCK = 1, CH1_DATA = bit  // hold
```

Pour la séquence d'octets APA102 :

```
Start frame : 0x00 0x00 0x00 0x00                   (4 octets, 32 bits → 32 cycles CLOCK)
Pixel       : 0xE0|brightness  +  B  +  G  +  R     (4 octets par pixel)
End frame   : 0xFF × ceil(N/2)/8                    (padding pour propagation, cf datasheet)
```

### 4.3 Latch / Reset

Pour les protocoles 1-fil, après le dernier pixel, on doit maintenir DATA à 0 pendant TRESET (≥ 280 µs pour WS2815). À 16 MHz, c'est 4 480 samples bas. On les insère en tail du buffer DMA et on stoppe la chaîne ensuite.

Pour les protocoles clockés, pas de latch — on laisse simplement le bus inactif après l'end frame.

---

## 5. Stratégie multi-canaux mixtes

Tous les canaux partagent le même PCLK = 16 MHz. Conséquences :

- Si **tous les canaux sont 1-fil**, la trame DMA est dimensionnée pour le canal au plus grand pixel count.
- Si **un canal est clocké** et les autres 1-fil, la durée DMA est dictée par le plus long des deux :
   - Durée 1-fil = `pixels × 24 (ou 32) × samples_per_bit × T_PCLK`
   - Durée clockée = `(start + pixels × pixel_bits + end) × samples_per_clock × T_PCLK`
- Les canaux courts émettent des `0` après leur dernier bit utile (sans effet sur les strips déjà latchés).

Exemple chiffré : 8 canaux WS2815 × 600 px = 8 × 600 × 24 × 20 / 16e6 = **14.4 ms** de DMA pure, plus 280 µs de reset → ~14.7 ms par frame. À 60 Hz on a 16.67 ms budget → marge ~1.97 ms (12 %).

→ **Limite pratique** : 1024 px par canal en WS2815 à 60 Hz est faisable mais sans marge. À 30 Hz le confort est total.

---

## 6. Vérification

Tests unitaires (`components/led_protocols/test/`) :

1. Pour chaque protocole 1-fil : encoder un buffer de 1 pixel `(0xFF, 0x80, 0x00)`, vérifier que les samples produits respectent les timings cible à ±0 sample près.
2. Pour APA102 : encoder un buffer de 1 pixel `(R=0xFF, G=0x80, B=0x00, brightness=31)`, vérifier la structure start+frame+end et la valeur de `samples_per_clock`.
3. Test de bornes : pixel count = 1, pixel count = 1024 — vérifier que la taille DMA ne dépasse pas la borne configurée.

Tests d'intégration (`components/lcd_cam_output/test/`) :

1. Émission d'une frame de calibration (DATA = pattern carré 1 kHz) sur chacun des 16 GPIOs, mesure à l'oscilloscope.
2. Vérifier que CLOCK est strictement synchrone avec DATA sur le même canal (delta < 5 ns).
3. Vérifier l'absence de glitch sur des transitions back-to-back.
