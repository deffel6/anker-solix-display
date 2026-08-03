# Anker Solix Display

Echtzeit-Anzeige für Anker SOLIX Solarbank auf einem ESP32-C3 mit rundem
240×240-Display. Solarleistung, Akkustand, Akkuleistung, Netzbezug und
Hausverbrauch — **aktualisiert alle 3 Sekunden**.

<!-- Screenshot des laufenden Displays hier einfügen -->

## Warum das interessant ist

Die offizielle REST-API von Anker liefert Daten aus einem Cache, der sich nur
alle fünf Minuten erneuert. Wer häufiger fragt, bekommt dieselben Zahlen noch
einmal. Die App selbst umgeht das über einen verschlüsselten Endpunkt
(`X-Encryption-Info: algo_ecdh`), der bis heute nicht nachgebaut werden konnte —
die Referenz-Bibliothek [anker-solix-api][asa] markiert ihren Verschlüsselungs-
Code ausdrücklich als nicht funktionierend, und auch mit vollständigen
Proxy-Mitschnitten hat niemand die Header-Formeln rekonstruiert.

Dieses Projekt geht deshalb einen anderen Weg: **MQTT statt REST.** Die Geräte
sprechen über AWS IoT mit Ankers Cloud und lassen sich zu Echtzeit-Updates
im 3-Sekunden-Takt bewegen. Die Zugangsdaten dafür gibt die API auf einem
ganz normalen, unverschlüsselten Endpunkt heraus.

Die dabei entstandene [Protokoll-Dokumentation](docs/mqtt-protokoll.md)
beschreibt das Binärformat der Solarbank 3 Pro (A17C5) samt Feldbelegung.
Nach meiner Kenntnis ist das bislang nirgends öffentlich beschrieben.

[asa]: https://github.com/thomluther/anker-solix-api

## Installation

### Web-Installer (am einfachsten)

ESP32-C3 per USB anschließen und auf der [Installationsseite][pages] auf
*Installieren* klicken. Läuft in Chrome oder Edge, es wird nichts installiert.

[pages]: https://deffel6.github.io/anker-solix-display/

### Aus dem Quelltext

Arduino IDE mit dem [ESP32-Core][core] (getestet mit 3.3.10) und diesen
Bibliotheken:

| Bibliothek | Zweck |
|---|---|
| [LovyanGFX][gfx] | Display-Ansteuerung |
| [ArduinoJson][json] | API-Antworten |
| [PubSubClient][mqtt] | MQTT |

Dann `anker_display/anker_display.ino` öffnen, Board **ESP32C3 Dev Module**
wählen und hochladen.

[core]: https://github.com/espressif/arduino-esp32
[gfx]: https://github.com/lovyan03/LovyanGFX
[json]: https://arduinojson.org/
[mqtt]: https://github.com/knolleary/pubsubclient

## Einrichtung

Beim ersten Start öffnet der ESP32 ein WLAN namens **Anker-Display-Setup**.
Damit verbinden, im Browser `192.168.4.1` aufrufen und dort WLAN-Zugang und
Anker-Konto eintragen. Das Gerät übernimmt anschließend die erste Anlage des
Kontos und startet neu — bei nur einer Anlage ist die Einrichtung damit fertig.

## Weboberfläche

Im laufenden Betrieb ist das Display im Heimnetz über seine IP-Adresse
erreichbar. Sie steht beim Booten kurz auf dem Bildschirm und im seriellen
Monitor. Die Seite zeigt die aktuellen Messwerte und aktualisiert sich alle
zehn Sekunden. Darüber lässt sich außerdem

- die **Anlage wechseln** — nötig, wenn das Konto mehrere hat, denn automatisch
  wird immer die erste genommen,
- und die **Zugangsdaten ändern**, etwa nach einem WLAN-Wechsel.

Beides ohne Reset und ohne neu zu flashen.

Das Anker-Passwort wird verschlüsselt an Anker übertragen (ECDH plus AES, so wie
die App es macht) und liegt lokal im NVS-Flash des ESP32. Es verlässt das Gerät
nur Richtung Anker.

## Hardware

Ein ESP32-C3 mit rundem GC9A01A-Display, wie er als fertiges Modul erhältlich
ist. Die Pinbelegung steht in der `LGFX`-Klasse ganz oben im Sketch:

| Signal | GPIO |
|---|---|
| SCLK | 6 |
| MOSI | 7 |
| DC | 2 |
| CS | 10 |
| RST | 1 |
| Backlight | 3 |

Bei abweichender Verdrahtung dort anpassen.

## Wie es funktioniert

```
Login (passport/login)          ->  auth_token
app/devicemanage/get_user_mqtt_info  ->  Client-Zertifikat + Schlüssel
power_service/v1/site/get_site_detail ->  Seriennummern
                    |
                    v
   aiot-mqtt-eu.anker.com:8883  (TLS mit Client-Zertifikat)
                    |
     dt/anker_power/A17C5/<seriennummer>/param_info    <- alle 3 s
     cmd/anker_power/A17C5/<seriennummer>/req          -> Echtzeit-Trigger
```

Ohne den Trigger sendet die Solarbank nur alle paar Minuten. Der Trigger wird
alle zwei Minuten erneuert, damit der Datenstrom nicht abreißt.

Das Client-Zertifikat ist **zehn Jahre gültig** und hängt am Konto, nicht an
einer Sitzung.

## Was noch offen ist

- Der Netzbezug wird mit dem Faktor `GRID_SCALE` (100) skaliert. Das passt zu
  meinen Messungen, ist aber nur an einem Zählertyp geprüft.
- `"battery"` aus `state_info` sieht nach Ladestand aus, ist aber keiner —
  der echte Wert steckt in Feld `0xa3` der `param_info`. Siehe
  [Protokoll-Dokumentation](docs/mqtt-protokoll.md).
- Getestet ausschließlich mit **Solarbank 3 Pro E2700 (A17C5)** und einem
  **Shelly EM3 (SHEM3)** als Netzzähler. Andere Modelle senden mit hoher
  Wahrscheinlichkeit andere Feldbelegungen.

Rückmeldungen zu anderen Modellen sind willkommen — am hilfreichsten ist die
`[NETZ]`- beziehungsweise `[INT]`-Ausgabe aus dem seriellen Monitor zusammen mit
den Werten, die die App zur selben Zeit anzeigt.

## Lizenz

MIT, siehe [LICENSE](LICENSE).

Kein offizielles Anker-Projekt. „Anker" und „SOLIX" sind Marken ihrer jeweiligen
Inhaber. Die verwendeten Schnittstellen sind nicht dokumentiert und können sich
jederzeit ändern.
