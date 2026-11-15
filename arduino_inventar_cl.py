#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Arduino-Inventar: durchsucht ein Arduino-Verzeichnis und erstellt eine
Übersicht (Markdown) aller Projekte auf Deutsch.

Aufruf:
    python arduino_inventar.py "C:\\Users\\Name\\Documents\\Arduino"
    python arduino_inventar.py ~/Arduino -o inventar.md

Pro Projekt (= Ordner mit mindestens einer .ino-Datei) wird erfasst:
  - Ordnername und Pfad
  - letzte Änderung
  - Dateien im Ordner (mit Größe)
  - Kopfkommentar der .ino-Datei (meist die beste Beschreibung)
  - verwendete Bibliotheken (#include)
  - Hinweise auf Hardware (WiFi, Servo, Display, Sensoren usw.)
"""

import argparse
import re
import sys
from datetime import datetime
from pathlib import Path

# Stichwörter -> Hardware-/Funktionshinweis (Deutsch)
HINWEISE = {
    r"WiFi|ESP8266|ESP32|WebServer": "WLAN / Netzwerk",
    r"Bluetooth|BLE|SoftwareSerial": "Bluetooth / serielle Verbindung",
    r"Servo": "Servomotor",
    r"Stepper|AccelStepper": "Schrittmotor",
    r"LiquidCrystal|LCD": "LCD-Display",
    r"Adafruit_SSD1306|U8g2|OLED": "OLED-Display",
    r"DHT|BME280|BMP280|DS18B20|OneWire": "Temperatur-/Feuchtesensor",
    r"NeoPixel|FastLED|WS2812": "LED-Streifen (NeoPixel/WS2812)",
    r"IRremote": "Infrarot-Fernbedienung",
    r"MFRC522|RFID": "RFID-Leser",
    r"Keypad": "Tastenfeld",
    r"RTClib|DS3231|DS1307": "Echtzeituhr (RTC)",
    r"SD\.h|SPI\.h": "SD-Karte / SPI",
    r"Wire\.h": "I2C-Geräte",
    r"MQTT|PubSubClient": "MQTT (Smart Home)",
    r"Ultraschall|HC-SR04|pulseIn": "Ultraschall-Entfernungsmessung",
    r"EEPROM": "Speicherung im EEPROM",
}

IGNORIERE = {".git", "node_modules", "__pycache__", "libraries", ".vscode", "build"}


def lese_text(pfad: Path) -> str:
    for kodierung in ("utf-8", "latin-1"):
        try:
            return pfad.read_text(encoding=kodierung)
        except (UnicodeDecodeError, OSError):
            continue
    return ""


def kopfkommentar(text: str, max_zeilen: int = 8) -> str:
    """Holt den ersten Kommentarblock am Dateianfang."""
    text = text.lstrip("\ufeff").strip()
    zeilen = []
    if text.startswith("/*"):
        block = text[2:].split("*/", 1)[0]
        for z in block.splitlines():
            z = z.strip().lstrip("*").strip()
            if z:
                zeilen.append(z)
    else:
        for z in text.splitlines():
            if z.strip().startswith("//"):
                zeilen.append(z.strip().lstrip("/").strip())
            elif z.strip():
                break
    return " ".join(zeilen[:max_zeilen])


def groesse(byte: int) -> str:
    if byte < 1024:
        return f"{byte} B"
    return f"{byte / 1024:.1f} KB"


def finde_projekte(wurzel: Path):
    projekte = []
    for ino in wurzel.rglob("*.ino"):
        if any(teil in IGNORIERE for teil in ino.parts):
            continue
        ordner = ino.parent
        if ordner not in projekte:
            projekte.append(ordner)
    return sorted(projekte, key=lambda p: str(p).lower())


def analysiere(ordner: Path, wurzel: Path) -> str:
    ino_dateien = sorted(ordner.glob("*.ino"))
    alle_dateien = sorted(f for f in ordner.iterdir() if f.is_file())
    code = "\n".join(lese_text(f) for f in ino_dateien)

    letzte = max((f.stat().st_mtime for f in alle_dateien), default=0)
    datum = datetime.fromtimestamp(letzte).strftime("%d.%m.%Y") if letzte else "unbekannt"

    beschreibung = kopfkommentar(lese_text(ino_dateien[0])) if ino_dateien else ""
    if not beschreibung:
        readme = next((f for f in alle_dateien if f.name.lower().startswith("readme")), None)
        if readme:
            beschreibung = kopfkommentar("/*" + lese_text(readme)[:300] + "*/")
    if not beschreibung:
        beschreibung = "_(keine Beschreibung im Code – bitte ergänzen)_"

    includes = sorted(set(re.findall(r'#include\s*[<"]([^>"]+)[>"]', code)))
    hinweise = [text for muster, text in HINWEISE.items() if re.search(muster, code)]

    unterordner = sorted(d.name for d in ordner.iterdir() if d.is_dir() and d.name not in IGNORIERE)

    rel = ordner.relative_to(wurzel) if ordner != wurzel else Path(".")
    zeilen = [
        f"### {ordner.name}",
        f"- **Pfad:** `{rel}`",
        f"- **Zuletzt geändert:** {datum}",
        f"- **Beschreibung:** {beschreibung}",
        f"- **Hauptdateien:** {', '.join(f.name for f in ino_dateien)}",
    ]
    if hinweise:
        zeilen.append(f"- **Vermutlich beteiligt:** {', '.join(hinweise)}")
    if includes:
        zeilen.append(f"- **Bibliotheken:** {', '.join(includes)}")
    if unterordner:
        zeilen.append(f"- **Unterordner:** {', '.join(unterordner)}")
    weitere = [f for f in alle_dateien if f.suffix.lower() != ".ino"]
    if weitere:
        zeilen.append(
            "- **Weitere Dateien:** "
            + ", ".join(f"{f.name} ({groesse(f.stat().st_size)})" for f in weitere[:15])
            + (" …" if len(weitere) > 15 else "")
        )
    zeilen.append("- **Notiz / Wofür gedacht?:** _(bitte ergänzen)_")
    return "\n".join(zeilen)


def main():
    parser = argparse.ArgumentParser(description="Erstellt ein Inventar eines Arduino-Verzeichnisses.")
    parser.add_argument("verzeichnis", help="Pfad zum Arduino-Verzeichnis")
    parser.add_argument("-o", "--ausgabe", default="arduino_inventar.md", help="Ausgabedatei (Markdown)")
    args = parser.parse_args()

    wurzel = Path(args.verzeichnis).expanduser().resolve()
    if not wurzel.is_dir():
        sys.exit(f"Verzeichnis nicht gefunden: {wurzel}")

    projekte = finde_projekte(wurzel)
    ausgabe = [
        "# Arduino-Inventar",
        f"_Erstellt am {datetime.now().strftime('%d.%m.%Y %H:%M')} aus `{wurzel}`_",
        f"\n**Anzahl Projekte:** {len(projekte)}\n",
        "## Übersicht",
        "| Projekt | Zuletzt geändert | Kurzbeschreibung |",
        "|---|---|---|",
    ]

    details = []
    for p in projekte:
        text = analysiere(p, wurzel)
        details.append(text)
        datum = re.search(r"Zuletzt geändert:\*\* (.+)", text).group(1)
        besch = re.search(r"Beschreibung:\*\* (.+)", text).group(1)
        ausgabe.append(f"| {p.name} | {datum} | {besch[:90].replace('|', '/')} |")

    ausgabe += ["", "## Details", ""] + ["\n\n".join(details)]

    Path(args.ausgabe).write_text("\n".join(ausgabe), encoding="utf-8")
    print(f"Fertig: {len(projekte)} Projekte -> {args.ausgabe}")


if __name__ == "__main__":
    main()
