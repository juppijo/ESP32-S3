import os
import time
from pathlib import Path
import sys

# Lokales Benutzerverzeichnis zum Pfad hinzufügen
user_site = Path.home() / ".local" / "lib" / "python3.10" / "site-packages"
if str(user_site) not in sys.path:
    sys.path.append(str(user_site))

try:
    from deep_translator import GoogleTranslator
    TRANSLATOR_VERFUEGBAR = True
except ImportError:
    TRANSLATOR_VERFUEGBAR = False

ARDUINO_VERZEICHNIS = "/media/jo-ssd/SSD_1T_J1/ARDUINO_2"

def lese_ino_beschreibung(ino_pfad):
    """Liest den ersten Kommentarblock einer .ino-Datei aus und übersetzt ihn ins Deutsche."""
    beschreibung = "Keine Beschreibung gefunden"
    try:
        with open(ino_pfad, 'r', encoding='utf-8', errors='ignore') as datei:
            inhalt = datei.read(2000)
            
            if inhalt.strip().startswith("/*"):
                end_idx = inhalt.find("*/")
                if end_idx != -1:
                    beschreibung = inhalt[2:end_idx].strip()
            elif inhalt.strip().startswith("//"):
                zeilen = []
                for zeile in inhalt.splitlines():
                    if zeile.strip().startswith("//"):
                        zeilen.append(zeile.strip().lstrip("/").strip())
                    else:
                        break
                if zeilen:
                    beschreibung = " ".join(zeilen)
    except Exception as fehler:
        beschreibung = f"Fehler beim Lesen: {fehler}"

    # Übersetzung ins Deutsche mit eingebautem Schutz vor zu vielen Anfragen
    if beschreibung != "Keine Beschreibung gefunden" and not beschreibung.startswith("Fehler"):
        if TRANSLATOR_VERFUEGBAR:
            try:
                # Kurze Pause einbauen, um Google-Sperren zu verhindern (0.6 Sekunden = max ~1.6 Anfragen/Sek.)
                time.sleep(0.6)
                uebersetzt = GoogleTranslator(source='auto', target='de').translate(beschreibung[:500])
                if uebersetzt:
                    beschreibung = uebersetzt
            except Exception as e:
                # Bei zu vielen Anfragen kurz warten und einmalig einen Retry versuchen
                try:
                    time.sleep(2.0)
                    uebersetzt = GoogleTranslator(source='auto', target='de').translate(beschreibung[:500])
                    if uebersetzt:
                        beschreibung = uebersetzt
                except Exception:
                    pass # Falls es immer noch fehlschlägt, bleibt der Originaltext erhalten
        
    return beschreibung

def erstelle_inventar():
    basis_pfad = Path(ARDUINO_VERZEICHNIS).resolve()
    projekte = []

    print("Durchsuche Verzeichnis nach Arduino-Projekten und übersetze Beschreibungen...")
    for ino_datei in basis_pfad.glob("**/*.ino"):
        if ino_datei.parent.name == ino_datei.stem:
            projekt_name = ino_datei.parent.name
            relativer_pfad = ino_datei.parent.relative_to(basis_pfad)
            print(f"Verarbeite Projekt: {projekt_name}...")
            beschreibung = lese_ino_beschreibung(ino_datei)
            projekte.append({
                "name": projekt_name,
                "pfad": str(relativer_pfad),
                "beschreibung": beschreibung.replace("\n", " ")
            })

    ausgabe_datei = basis_pfad / "arduino_inventar_bericht.md"
    with open(ausgabe_datei, 'w', encoding='utf-8') as datei:
        datei.write("# Arduino Projekt-Inventar\n\n")
        datei.write(f"Automatisch generiert für Verzeichnis: `{basis_pfad}`\n\n")
        datei.write("| Projektname | Pfad | Kurzbeschreibung |\n")
        datei.write("| :--- | :--- | :--- |\n")
        for p in sorted(projekte, key=lambda x: x['name'].lower()):
            datei.write(f"| **{p['name']}** | `{p['pfad']}` | {p['beschreibung']} |\n")
            
    print(f"\nInventar erfolgreich erstellt: {ausgabe_datei}")

if __name__ == "__main__":
    erstelle_inventar()
