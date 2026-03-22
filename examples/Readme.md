# Example configuration

[Combi 4 / 4D / 6 / 6D](combi.yaml)

[Combi 4E / 4DE / 6E / 6DE](combi_E.yaml)

## OTA (Over-the-Air Update)

Die Beispiele enthalten einen vorausgefüllten `ota`-Block mit `platform: esphome` und einem Passwort.
Damit lassen sich Firmware-Updates direkt über WLAN einspielen, ohne den ESP32 physisch anschließen zu müssen.

**Wichtig:** Das Passwort in den Beispieldateien ist ein Platzhalter — bitte vor dem Einsatz durch ein eigenes, langes Passwort ersetzen und sicher aufbewahren. Wer das Passwort vergisst, muss den ESP32 wieder per USB flashen.
