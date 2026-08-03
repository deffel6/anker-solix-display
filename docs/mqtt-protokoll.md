# Anker SOLIX MQTT-Protokoll

Alles hier stammt aus eigenen Mitschnitten mit einer **Solarbank 3 Pro E2700
(A17C5, Firmware v1.0.7.3)** und einem **Shelly EM3 (SHEM3)** als Netzzähler.
Anker dokumentiert nichts davon. Andere Modelle senden vermutlich andere
Feldbelegungen.

Belegte Felder sind mit ✔ markiert — sie wurden gegen die Anker-App oder über
eine Summenprobe geprüft. Alles andere ist eine Vermutung.

## Zugang

### 1. Anmelden

```
POST https://ankerpower-api-eu.anker.com/passport/login
```

Passwort mit AES-256-CBC verschlüsselt, Schlüssel aus ECDH (secp256r1) gegen
diesen fest eingebauten Server-Schlüssel:

```
04c5c00c4f8d1197cc7c3167c52bf7acb054d722f0ef08dcd7e0883236e0d72a3
868d9750cb47fa4619248f3d83f0f662671dadc6e2d31c2f41db0161651c7c076
```

Liefert `auth_token` und `user_id`.

### 2. MQTT-Zugangsdaten holen

```
POST https://ankerpower-api-eu.anker.com/app/devicemanage/get_user_mqtt_info
```

**Ohne** `power_service/v1`-Präfix — mit Präfix antwortet der Server 404. Der
Aufruf ist unverschlüsselt und braucht nur den `auth_token`.

Antwort (rund 8 KB):

| Feld | Inhalt |
|---|---|
| `endpoint_addr` | `aiot-mqtt-eu.anker.com` |
| `thing_name` | `<user_id>-anker_power` |
| `certificate_pem` | Client-Zertifikat, **10 Jahre gültig** |
| `private_key` | RSA-2048, PKCS#1 |
| `aws_root_ca1_pem` | Wurzelzertifikat |
| `certificate_id` | für die `client_id` beim Publish |

Im JSON stehen die Zeilenumbrüche der PEM-Blöcke als `\n` (zwei Zeichen) und
müssen vor der Übergabe an die TLS-Bibliothek in echte Umbrüche gewandelt
werden.

### 3. Verbinden

Broker `aiot-mqtt-eu.anker.com:8883`, TLS mit gegenseitiger Authentifizierung.
Client-ID: `<thing_name>_<5 Ziffern>`.

| Richtung | Topic |
|---|---|
| Empfangen | `dt/anker_power/<produktcode>/<seriennummer>/#` |
| Senden | `cmd/anker_power/<produktcode>/<seriennummer>/req` |

Seriennummer und Produktcode liefert
`POST power_service/v1/site/get_site_detail` mit `{"site_id":"..."}` —
die Solarbank steht unter `solarbank_list`, der Netzzähler unter `grid_list`.

## Echtzeit-Trigger

Ohne ihn sendet die Solarbank nur alle paar Minuten. Danach kommen Daten alle
drei Sekunden. Sinnvollerweise alle ein bis zwei Minuten wiederholen.

Veröffentlicht auf `cmd/.../req`:

```json
{
  "head": {
    "version": "1.0.0.1",
    "client_id": "android-anker_power-<user_id>-<certificate_id>",
    "sess_id": "1234-5678",
    "msg_seq": 1,
    "seed": 1,
    "timestamp": 1785745314,
    "cmd_status": 2,
    "cmd": 17,
    "sign_code": 1,
    "device_pn": "A17C5",
    "device_sn": "APCD..."
  },
  "payload": "{\"device_sn\":\"APCD...\",\"account_id\":\"<user_id>\",\"data\":\"<base64>\"}"
}
```

Beachten: `client_id` hat beim Senden ein **anderes Format** als beim Verbinden.

Die Binärdaten in `data` (vor der Base64-Kodierung):

```
ff 09 19 00 03 00 0f 00 57 a1 01 22 a2 01 01 a3 02 2c 01 fe 04 <zeit 4B LE>
                        ^^^^^ Typ 0057 = Echtzeit-Trigger
                                          ^^^^^^^^ a3 = Zeitfenster in Sekunden
```

## Nachrichtenaufbau

Der `payload` der empfangenen Nachrichten ist ein JSON-**String**. Enthält er
ein Feld `data`, steckt darin Base64-kodiertes Binär:

```
ff 09 | länge (2 B, LE) | 03 01 0f | typ (2 B) | felder...
```

Jedes Feld:

```
tag (1 B) | länge (1 B) | typ (1 B) | wert (länge-1 B)
```

Ausnahme: Bei Länge 1 folgt kein Typ-Byte, das eine Byte ist der Wert.

| Typ | Bedeutung |
|---|---|
| `00` | Zeichenkette |
| `01` | uint8 |
| `02` | int16, little endian |
| `03` | uint32, little endian |
| `05` | float32, little endian |

Solarbank und Netzzähler benutzen **unterschiedliche Typen für dieselbe Art von
Messwert** — die Solarbank `float32`, der Zähler `uint32`. Ein Parser, der nur
einen davon kennt, findet beim jeweils anderen Gerät gar nichts.

## Solarbank A17C5 — `param_info`, Typ 0405

Die 749 Byte große Variante. Es gibt eine kürzere mit 425 Byte ohne `0xab`,
deren Inhalt ich nicht entschlüsselt habe.

| Tag | Typ | Bedeutung | |
|---|---|---|---|
| `a2` | str | Seriennummer | ✔ |
| `a3` | u8 | **Ladestand in %** | ✔ |
| `ab` | f32 | **Solarleistung gesamt (W)** | ✔ |
| `ac` | f32 | **Akkuleistung (W)**, negativ = Entladen | ✔ |
| `ad` | f32 | **Ausgangsleistung (W)** = `ab` + `ac` | ✔ |
| `ae` | f32 | wie `ad` | |
| `b0` | f32 | Energiezähler (kWh), steigt langsam | |
| `b3` | f32 | Energiezähler (kWh) | |
| `b7` | u8 | 90 — vermutlich Ladeobergrenze | |
| `bd` | i16 | 1200 — vermutlich Leistungsgrenze | |
| `be` | i16 | 800 — Einspeisegrenze | |
| `c2` | f32 | Spiegel von `ab` | ✔ |
| `c6` | f32 | **PV-String 1 (W)** | ✔ |
| `c7` | f32 | **PV-String 2 (W)** | ✔ |
| `c8` | f32 | **PV-String 3 (W)** | ✔ |
| `c9` | f32 | **PV-String 4 (W)** | ✔ |
| `fe` | u32 | Unix-Zeitstempel | ✔ |

Die PV-Strings sind belegt, weil `c6 + c7 + c8 + c9` in jeder geprüften
Nachricht **exakt** `ab` ergibt:

| Aufnahme | c6 | c7 | c8 | c9 | Summe | ab |
|---|---|---|---|---|---|---|
| 1 | 150 | 207 | 442 | 150 | 949 | 949 |
| 2 | 137 | 183 | 395 | 140 | 855 | 855 |
| 3 | 119 | 148 | 344 | 125 | 736 | 736 |
| 4 | 153 | 198 | 440 | 151 | 942 | 942 |

Ebenso gilt durchgehend `ab + ac = ad`: Die Anlage regelt die Batterie so, dass
die Ausgangsleistung dem Bedarf folgt.

### Falle: `state_info`

Die `state_info`-Nachricht enthält ein Klartext-JSON mit einem Feld `battery`:

```json
{"version":"v0.3.3.0","battery":100,"rssi":-63,"ssid":"...","ip":"..."}
```

**Das ist nicht der Ladestand.** Der Wert stand konstant auf 100, während der
Akku real bei 9 % lag und sich weiter entlud. Der Ladestand steht in `0xa3`
der `param_info`.

## Netzzähler SHEM3 — `param_info`, Typ 0405

435 Byte. Alle Messwerte als `uint32`.

| Tag | Typ | Bedeutung | |
|---|---|---|---|
| `a8` | u32 | **Netzbezug**, Einheit siehe unten | ✔ |
| `a9` | u32 | **Einspeisung** | |
| `aa` | u32 | Energiezähler, achtstellig | |
| `ab` | u32 | Energiezähler, achtstellig | |
| `fe` | u32 | Unix-Zeitstempel | ✔ |

Weil `uint32` kein Vorzeichen kennt, liegen beide Richtungen in getrennten
Feldern. Die tatsächliche Netzleistung ist `a8 - a9`.

**Einheit:** Rohwert 90925 entsprach rund 909 W, also Hundertstel-Watt. Das ist
an einem einzigen Zähler geprüft — im Sketch steht der Faktor als
`GRID_SCALE`, falls er woanders abweicht.

## Fehlercodes der REST-API

| Code | Bedeutung |
|---|---|
| 462 | Wiederholung erkannt — `X-Request-Once` muss je Anfrage eindeutig sein |
| 463 | Verschlüsselte Anfrage ohne gültigen Schlüsselaustausch |

Zu 463: Der Endpunkt `POST /v1/openapi/oauth/key/exchange` existiert und
antwortet mit konkreten Fehlermeldungen (`field "X-Signature" is not set`), aber
wie `X-Key-Ident` und `X-Signature` berechnet werden, ist ungeklärt. Selbst mit
echten Mitschnitten hat das bisher niemand rekonstruiert. Der MQTT-Weg umgeht
das Problem vollständig.
