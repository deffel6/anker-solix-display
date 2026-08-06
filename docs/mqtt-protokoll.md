# Anker SOLIX MQTT-Protokoll

Alles hier stammt aus eigenen Mitschnitten mit einer **Solarbank 3 Pro E2700
(A17C5)** und einem **Shelly EM3 (SHEM3)** als Netzzähler. Anker dokumentiert
nichts davon. Andere Modelle senden vermutlich andere Feldbelegungen.

Belegte Felder sind mit ✔ markiert — sie wurden gegen die Anker-App oder über
eine Summenprobe geprüft. Alles andere ist eine Vermutung.

> ### Gilt nur für diesen Firmwarestand
>
> Die Zuordnung der Feldnummern wurde an einem Gerät mit **`device_sw_version`
> v1.0.7.3** und **`version` v0.3.3.0** (aus `state_info`) ermittelt.
>
> Es gibt keine Zusage von Anker, dass diese Nummern stabil bleiben. Das
> Protokoll ist unveröffentlicht und wird nicht versioniert — ein
> Firmware-Update der Solarbank kann Felder verschieben, umdeuten oder
> entfernen, ohne dass es irgendwo angekündigt würde. Dasselbe gilt für die
> verwendeten API-Endpunkte.
>
> Praktisch heißt das: Zeigt die Anzeige nach einem Geräte-Update plötzlich
> Unsinn, liegt der Fehler mit hoher Wahrscheinlichkeit hier und nicht im
> Sketch. Die Summenprobe ist dann der schnellste Test — ergeben `0xc6` bis
> `0xc9` nicht mehr zusammen `0xab`, hat sich die Belegung geändert.

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

## Solarbank A17C5 — `state_info`, Typ 0500

Diese Nachricht trägt die Daten der einzelnen Akkupacks. Sie kommt selten, etwa
zwei- bis dreimal in fünf Minuten, und in wechselnden Größen — nicht jede
enthält alle Packs. Ein Parser muss die Blöcke deshalb nach Index **einsortieren
und über mehrere Nachrichten sammeln**, statt die Liste je Nachricht neu zu
füllen.

Aufbau einer beobachteten Nachricht mit drei Packs:

```
len=711  a0:2/t01  a1:1/t32  a2:107/t04  a3:94/t04
         a4:148/t04  a5:164/t04  a6:165/t04  aa:4/t04
```

`a4`, `a5`, `a6` … sind die Packblöcke, erkennbar an Typ `04` und einer Länge
über 32 Byte. `a2` und `a3` enthalten Systemdaten, die noch nicht entschlüsselt
sind.

### Aufbau eines Packblocks

Feste Offsets, an mehreren Aufnahmen und drei Packs geprüft:

| Offset | Inhalt | |
|---|---|---|
| 0 | laufender Index (1, 2, 3 …) | ✔ |
| 12–13 | u16, je Pack **konstant** — Bedeutung offen | |
| 14–23 | fünf Zellspannungen, je u16 in Millivolt | ✔ |
| 24–31 | vier Temperaturen, je i16 in Zehntelgrad | ✔ |
| 36–37 | **Ladestand in Zehntelprozent** | ✔ |
| Ende | Seriennummer als ASCII, mit Längenbyte davor | ✔ |

Der Abgleich mit der App, alle drei Packs gleichzeitig:

| App | Offset 36 | Temperatur App / Block |
|---|---|---|
| Host E2700 Pro: 25 % | 252 → 25,2 % | 25 °C / 25,4 °C |
| BP1600: 26 % | 260 → 26,0 % | 23 °C / 23,0 °C |
| BP2700: 26 % | 258 → 25,8 % | 23 °C / 23,7 °C |

**Vorsicht bei Offset 34.** Dort steht ein sehr ähnlicher Wert — 248 / 252 / 254
in derselben Aufnahme —, der bei zwei von drei Packs auf den falschen
Prozentwert rundet. Ohne den Abgleich mit allen drei Packs gleichzeitig ist der
Unterschied nicht zu erkennen.

**Und Vorsicht bei Offset 12.** Die Werte dort (89 / 70 / 60 im Testgerät) sehen
wie Ladestände aus und ergeben kapazitätsgewichtet sogar ungefähr den Wert, den
die App anzeigt. Sie sind aber über Stunden konstant, während sich die Akkus
entladen. Ein zufällig passender Mittelwert ist kein Beleg — erst die zeitliche
Veränderung entscheidet.

### Systemwert

`0xa3` in der `param_info` ist der Ladestand der **Kopfstation**, nicht des
Systems. Bei mehreren Speichern weicht er von dem ab, was die App zeigt. Der
Systemwert entspricht dem Mittel über die Packs; kapazitätsgewichtet wäre
genauer, aber die Kapazität der einzelnen Packs steht nicht im Datenstrom.

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

### Die Einheit hängt vom Zählermodell ab

Das ist die wichtigste Stolperfalle in diesem Abschnitt. Zwei geprüfte Fälle:

| Zähler | Rohwert `a8` | tatsächlich | Teiler |
|---|---|---|---|
| Shelly EM3 (`SHEM3`) | 90925 | ≈ 909 W | 100 |
| Anker-Smartmeter | 250 | ≈ 250 W | 1 |

Der Shelly meldet also Hundertstel-Watt, der Anker-Zähler ganze Watt. Ein fest
eingebauter Teiler liefert damit zwangsläufig bei einem Teil der Nutzer Werte,
die um Faktor 100 danebenliegen — genau das ist in den ersten Fassungen
passiert.

Der Sketch leitet den Teiler deshalb aus `device_pn` ab (`SHEM*` → 100, sonst
→ 1) und lässt ihn in der Weboberfläche überschreiben. Ob es Zähler mit noch
anderen Einheiten gibt, ist offen; die Umschaltmöglichkeit ist deshalb der
eigentliche Verlass, die Automatik nur eine Bequemlichkeit.

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
