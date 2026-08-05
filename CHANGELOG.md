# Änderungen

## 1.6.0 — 5. August 2026

**Einzelne Panels auf der Weboberfläche.** Die Statusseite zeigt jetzt zusätzlich
die Leistung der vier MPPT-Eingänge. Damit ist auf einen Blick erkennbar, ob ein
Panel verschattet ist oder ausfällt — die Gesamtleistung allein verrät das nicht.

Die Werte lagen schon vor: `parseParamInfo` liest die Felder `0xc6` bis `0xc9`
seit der ersten MQTT-Fassung mit, hat sie aber nur ins Log geschrieben. Auf dem
Display erscheinen sie bewusst nicht, dort ist auf 240×240 Pixeln kein Platz
dafür.

## 1.5.0 — 4. August 2026

**Netzzähler-Teiler richtet sich nach dem Gerät.** Der Teiler war fest auf 100
verdrahtet — passend zu einem Shelly EM3, der Hundertstel-Watt meldet. Ein
Nutzer mit einem Anker-Smartmeter bekam dadurch Werte um Faktor 100 zu klein:
dessen Rohwert 250 entspricht 250 W, nicht 2,5 W. Die Einheit ist also
modellabhängig, eine feste Konstante kann nicht für alle stimmen.

Der Teiler wird jetzt aus `device_pn` abgeleitet (`SHEM*` → 100, sonst → 1) und
lässt sich auf der Statusseite mit einem Klick auf 1, 10, 100 oder 1000
umstellen. Die Einstellung liegt im NVS und überlebt Neustarts. Damit kann
jeder seinen Zähler selbst geradeziehen, ohne dass jedes Modell bekannt sein
muss — die Automatik erspart nur den meisten Leuten den Klick.

Die Statusseite zeigt außerdem den erkannten Zählertyp und den aktiven Teiler
an. Ohne diese Anzeige sieht man nicht, worauf das Gerät gerade steht.

## 1.4.2 — 4. August 2026

**Anlage wird nach Erreichbarkeit gewählt.** Bisher nahm die automatische
Auswahl stumpf die erste Anlage der Liste. Bei mehreren Anlagen traf das leicht
die falsche — im Testkonto stand dort eine Solarbank, die offline war, und das
Display blieb ohne Werte. Jetzt fragt der Sketch `get_relate_and_bind_devices`
ab und nimmt die erste Anlage, in der ein Gerät mit `wifi_online: true` steht.
Ist nichts erreichbar, fällt er auf die erste Anlage zurück und schreibt den
Grund ins Log.

**Wartebildschirm schlägt keinen Alarm mehr.** Während des Verbindungsaufbaus
stand rot „KEIN SIGNAL", obwohl nichts kaputt war. Steht die MQTT-Verbindung
und fehlen nur noch die Daten, heißt es jetzt orange „Decodiere Daten". Rot
bleibt dem Fall vorbehalten, in dem tatsächlich keine Verbindung besteht.
Darunter steht die IP-Adresse, über die die Weboberfläche erreichbar ist.

**Hinweistext bereinigt.** Nach der Einrichtung wurde noch der Pfad `/sites`
genannt. Den gibt es seit 1.4.0 als Schaltfläche auf der Startseite, deshalb
steht dort jetzt nur die IP-Adresse.

## 1.4.0 — 3. August 2026

**Weboberfläche im laufenden Betrieb.** Der Webserver lief bisher nur im
Einrichtungsmodus; danach sprang der Sketch in die MQTT-Schleife, in der
`server.handleClient()` nie aufgerufen wurde. Das Gerät war im Heimnetz also
gar nicht erreichbar, und ein Anlagenwechsel ging nur über Reset und
Neueinrichtung.

Unter der IP des Displays gibt es jetzt eine Statusseite mit den aktuellen
Messwerten, die sich alle zehn Sekunden selbst aktualisiert, sowie Zugang zur
Anlagenauswahl und zum Zugangsdaten-Formular. Die Anlagenliste wird dabei
frisch geladen — sie wurde bisher nur beim Einrichten gefüllt und wäre im
Normalbetrieb leer gewesen.

Nebenbei behoben: Der Puffer der Statusseite war mit 1400 Byte kleiner als die
Vorlage samt CSS, `snprintf` hätte die Seite mitten im HTML abgeschnitten.

## 1.3.0 — 3. August 2026

**Einrichtung ist einstufig.** Vorher: speichern, dann im Heimnetz `/sites`
aufrufen und die Anlage auswählen. Jetzt übernimmt das Gerät eine Anlage selbst
und startet neu. `/sites` bleibt erreichbar, um nachträglich zu wechseln.

## 1.2.9 — 3. August 2026

**Anmeldeseite öffnet sich zuverlässiger.** Der Zugangspunkt wurde gestartet,
bevor er konfiguriert war. Der DHCP-Server lief dadurch kurz mit Standardwerten
und verteilte nicht zwingend `192.168.4.1` als DNS-Server. Ohne den
Platzhalter-DNS erreichen die Prüfanfragen der Betriebssysteme den ESP32 gar
nicht erst, und das Anmeldefenster bleibt aus. Jetzt erst `softAPConfig`, dann
`softAP`.

Dazu: `handleNotFound` schickte ein 302 ohne Rumpf und ohne Inhaltstyp, was
manche Systeme nicht als Anmeldeseite werten — es kommt nun eine echte Seite
mit Code 200 zurück. Und eigene Handler für die neun bekannten Prüfadressen von
iOS, Android, Windows, Firefox und Ubuntu.

Neuere iOS- und Android-Fassungen prüfen zusätzlich über HTTPS. Das kann ein
Zugangspunkt nicht umlenken, ohne dass es nach einem Angriff aussieht — deshalb
bleibt das Fenster manchmal trotzdem aus. Die IP-Adresse steht dann auf dem
Display.

## 1.2.8 — 3. August 2026

Erste veröffentlichte Fassung.

**Echtzeitdaten über MQTT.** Die REST-API liefert nur 5-Minuten-Cachedaten, und
ihr verschlüsselter Endpunkt (`algo_ecdh`) ist bis heute nicht nachgebaut. Die
Werte kommen deshalb über Ankers MQTT-Broker: `get_user_mqtt_info` liefert ein
zehn Jahre gültiges Client-Zertifikat, damit TLS zu `aiot-mqtt-eu.anker.com`,
und ein Echtzeit-Trigger bringt die Solarbank auf einen 3-Sekunden-Takt.

Angezeigt werden Solarleistung, Akkustand, Akkuleistung mit Richtung,
Netzbezug und Hausverbrauch. Der Netzbezug stammt vom Zähler, der als eigenes
Gerät auf einem eigenen Topic sendet.

Enthält die Feldbelegung des Binärformats für A17C5 und SHEM3 in
[docs/mqtt-protokoll.md](docs/mqtt-protokoll.md) sowie den Web-Installer.
