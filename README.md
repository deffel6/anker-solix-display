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

[pages]: https://deffel6.github.io/anker-solix-display-beta/

### Aus dem Quelltext

Arduino IDE mit dem [ESP32-Core][core] (getestet mit 3.3.10) und diesen
Bibliotheken:

| Bibliothek | Zweck |
|---|---|
| [LovyanGFX][gfx] | Display-Ansteuerung |
| [ArduinoJson][json] | API-Antworten |
| [PubSubClient][mqtt] | MQTT |

Dann `anker_display/anker_display.ino` öffnen, Board **ESP32C3 Dev Module**
wählen und unter *Werkzeuge → Partition Scheme* auf
**Huge APP (3MB No OTA/1MB SPIFFS)** stellen — mit der Voreinstellung passt das
Programm nicht in den Flash. Danach hochladen.

[core]: https://github.com/espressif/arduino-esp32
[gfx]: https://github.com/lovyan03/LovyanGFX
[json]: https://arduinojson.org/
[mqtt]: https://github.com/knolleary/pubsubclient

## Einrichtung

Beim ersten Start öffnet der ESP32 ein WLAN namens **Anker-Display-Setup**.
Damit verbinden, im Browser `192.168.4.1` aufrufen und dort WLAN-Zugang und
Anker-Konto eintragen. Das Gerät wählt anschließend selbst eine Anlage und
startet neu — bei nur einer Anlage ist die Einrichtung damit fertig.

Bei **mehreren** Anlagen zeigt das Display stattdessen eine IP-Adresse mit dem
Pfad `/sites` — dort wird ausgewählt. Automatisch entschieden wird bewusst
nicht: Jede Regel dafür wäre beliebig, und schlimmer noch, ihr Ergebnis könnte
sich später von allein ändern, wenn ein Gerät in einer anderen Anlage online
geht.

## Weboberfläche

Im laufenden Betrieb ist das Display im Heimnetz über seine IP-Adresse
erreichbar. Sie steht beim Booten kurz auf dem Bildschirm und im seriellen
Monitor. Die Seite zeigt die aktuellen Messwerte und aktualisiert sich alle
zehn Sekunden — darunter auch die **Leistung der vier MPPT-Eingänge einzeln**
samt **Tagesertrag in kWh**, an dem sich ein verschattetes oder ausgefallenes
Panel erkennen lässt. Auf dem Display selbst erscheinen sie nicht, dort fehlt
der Platz.

Der Tagesertrag wird aus den Momentanleistungen aufsummiert, weil die Solarbank
keine Zählerstände je Panel liefert. Um Mitternacht beginnt er neu. Es sind
Schätzwerte, keine geeichten Zählerstände — nach längeren Verbindungslücken
fehlt der Teil, den das Gerät nicht gesehen hat.

Eine Unterseite zeigt die **Akkupacks einzeln**: Ladestand, Zellspannungen,
Temperaturen und Seriennummer, dazu die Spreizung zwischen höchster und
niedrigster Zelle — an ihr erkennt man eine schwächelnde Zelle deutlich früher
als am Ladestand.

Darüber lässt sich außerdem

- die **Anlage wechseln**, falls die automatische Wahl nicht die gewünschte
  getroffen hat,
- die **Zugangsdaten ändern**, etwa nach einem WLAN-Wechsel,
- die **Akkukapazität** setzen, falls Erweiterungsbatterien verbaut sind,
- und die **Anzeige drehen** (0/90/180/270 Grad), falls das Gehäuse anders
  steht als gedacht.

Alles davon ohne Reset und ohne neu zu flashen.

Das Anker-Passwort wird verschlüsselt an Anker übertragen (ECDH plus AES, so wie
die App es macht) und liegt lokal im NVS-Flash des ESP32. Es verlässt das Gerät
nur Richtung Anker.

## Wenn der Netzwert nicht stimmt

Zeigt das Display beim Netz einen Wert, der um Faktor 100 oder 1000 daneben
liegt, ist der Teiler für deinen Zähler falsch. Ursache: Die Zähler melden ihre
Rohwerte in unterschiedlichen Einheiten, und wir kennen bisher nur zwei
Modelle.

Zu beheben ohne neu zu flashen: die IP des Displays im Browser aufrufen. Unten
auf der Statusseite steht der erkannte Zählertyp mit dem aktiven Teiler, dazu
die Auswahl **1 · 10 · 100 · 1000**. Der richtige Wert ist der, bei dem die
Anzeige zu deinem Stromzähler passt. Die Einstellung überlebt Neustarts.

Im seriellen Monitor hilft die Zeile mit dem Rohwert beim Rechnen:

```
[NETZ] roh a8=90925 a9=0 -> 909 W Bezug | Haus 1345 W
```

Über eine Rückmeldung mit Zählermodell und passendem Teiler freue ich mich —
dann kann die Automatik das Modell künftig selbst erkennen.

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

- Die Einheit des Netzzählers ist modellabhängig. Geprüft sind zwei Fälle:
  Shelly EM3 meldet Hundertstel-Watt, der Anker-Smartmeter ganze Watt. Der
  Sketch leitet den Teiler aus dem Gerätetyp ab und lässt ihn in der
  Weboberfläche überschreiben — ob es Zähler mit noch anderen Einheiten gibt,
  ist offen.
- Zwei Fallen beim Ladestand: `"battery"` aus `state_info` sieht nach einem aus,
  steht aber konstant auf 100. Und `0xa3` aus der `param_info` ist der Wert der
  **Kopfstation**, nicht des Systems — bei mehreren Speichern weicht er von der
  App ab. Der Systemwert entsteht aus den Packblöcken. Siehe
  [Protokoll-Dokumentation](docs/mqtt-protokoll.md).
- Getestet ausschließlich mit **Solarbank 3 Pro E2700 (A17C5)** und einem
  **Shelly EM3 (SHEM3)** als Netzzähler. Andere Modelle senden mit hoher
  Wahrscheinlichkeit andere Feldbelegungen.
- **Der Firmwarestand der Solarbank spielt mit.** Die Feldzuordnung wurde an
  einem Gerät mit `device_sw_version` v1.0.7.3 ermittelt. Anker veröffentlicht
  das Protokoll nicht und versioniert es nicht — ein Update der Solarbank kann
  Felder verschieben, ohne dass es angekündigt würde. Zeigt die Anzeige nach
  einem Geräte-Update Unsinn, liegt es vermutlich daran; die
  [Protokoll-Dokumentation](docs/mqtt-protokoll.md) beschreibt, wie sich das
  mit der Summenprobe schnell nachweisen lässt.

Rückmeldungen zu anderen Modellen sind willkommen — am hilfreichsten ist die
`[NETZ]`- beziehungsweise `[INT]`-Ausgabe aus dem seriellen Monitor zusammen mit
den Werten, die die App zur selben Zeit anzeigt.

## Kontakt

Fragen, Rückmeldungen oder ein Zählermodell, das noch nicht erkannt wird:
**esp32.display@gmail.com**

Fehlerberichte gerne auch als
[Issue](https://github.com/deffel6/anker-solix-display-beta/issues).

Das Projekt entsteht in der Freizeit und bleibt kostenlos. Wer etwas dazugeben
möchte, schreibt einfach an dieselbe Adresse.

## Änderungen

Siehe [CHANGELOG.md](CHANGELOG.md).

## Lizenz

[PolyForm Noncommercial 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0/)

Nutzung, Änderung und Weitergabe sind für **nicht-kommerzielle Zwecke** frei —
privat, in Forschung und Lehre, durch gemeinnützige Einrichtungen. Pull Requests
sind also ausdrücklich willkommen. Nicht erlaubt ist die kommerzielle
Verwertung: die Software zu verkaufen, in ein kommerzielles Produkt einzubauen
oder im Rahmen eines Geschäftsbetriebs einzusetzen.

Wer sie kommerziell nutzen möchte, schreibt an esp32.display@gmail.com.
Der vollständige Wortlaut steht in [LICENSE](LICENSE), eine Zusammenfassung auf
Deutsch in [LIZENZ-KURZ.md](LIZENZ-KURZ.md).

SPDX-Kennung: `PolyForm-Noncommercial-1.0.0`. GitHub zeigt oben trotzdem
„Other" an — die dortige Erkennung umfasst nur Lizenzen von
choosealicense.com, und da PolyForm die kommerzielle Nutzung ausschließt, ist
sie definitionsgemäß keine Open-Source-Lizenz und dort nicht gelistet.

Kein offizielles Anker-Projekt. „Anker" und „SOLIX" sind Marken ihrer jeweiligen
Inhaber. Die verwendeten Schnittstellen sind nicht dokumentiert und können sich
jederzeit ändern.
