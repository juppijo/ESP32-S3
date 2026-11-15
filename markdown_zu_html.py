import re
from pathlib import Path

def markdown_tabelle_zu_html(md_pfad, html_pfad):
    md_datei = Path(md_pfad)
    if not md_datei.exists():
        print(f"Fehler: Die Datei '{md_pfad}' wurde nicht gefunden.")
        return

    with open(md_datei, 'r', encoding='utf-8') as f:
        zeilen = f.readlines()

    # Filtere alle Zeilen heraus, die zu einer Markdown-Tabelle gehören (beginnen mit '|')
    tabellen_zeilen = [zeile.strip() for zeile in zeilen if zeile.strip().startswith('|')]

    if not tabellen_zeilen:
        print("Keine Tabelle in der Markdown-Datei gefunden.")
        return

    # Kopfzeile extrahieren (erste Zeile)
    kopf_zellen = [z.strip() for z in tabellen_zeilen[0].split('|')[1:-1]]

    # Datenzeilen extrahieren (ab Zeile 3, da Zeile 2 die Trennlinie |---| ist)
    daten_zeilen = []
    for zeile in tabellen_zeilen[2:]:
        zellen = [z.strip() for z in zeile.split('|')[1:-1]]
        if zellen:
            daten_zeilen.append(zellen)

    # HTML-Struktur mit optimiertem Spaltenlayout und Textumbruch
    html_inhalt = f"""<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Arduino Projekt-Inventar</title>
    <style>
        :root {{
            --bg-color: #0f172a;
            --card-bg: #1e293b;
            --text-color: #f8fafc;
            --text-muted: #94a3b8;
            --border-color: #334155;
            --accent-color: #3b82f6;
            --accent-hover: #2563eb;
        }}
        body {{
            font-family: system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background-color: var(--bg-color);
            color: var(--text-color);
            margin: 0;
            padding: 2rem;
        }}
        .container {{
            max-width: 1400px;
            margin: 0 auto;
        }}
        header {{
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 2rem;
            background-color: var(--card-bg);
            padding: 1.5rem 2rem;
            border-radius: 0.75rem;
            border: 1px solid var(--border-color);
            box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1);
        }}
        h1 {{
            margin: 0;
            font-size: 1.5rem;
            font-weight: 600;
        }}
        .btn-vollbild {{
            background-color: var(--accent-color);
            color: white;
            border: none;
            padding: 0.6rem 1.2rem;
            border-radius: 0.5rem;
            cursor: pointer;
            font-weight: 600;
            font-size: 0.95rem;
            transition: background-color 0.2s, transform 0.1s;
        }}
        .btn-vollbild:hover {{
            background-color: var(--accent-hover);
        }}
        .btn-vollbild:active {{
            transform: scale(0.98);
        }}
        .tabelle-wrapper {{
            background-color: var(--card-bg);
            border-radius: 0.75rem;
            box-shadow: 0 10px 15px -3px rgba(0, 0, 0, 0.1);
            overflow-x: auto;
            border: 1px solid var(--border-color);
        }}
        table {{
            width: 100%;
            border-collapse: collapse;
            text-align: left;
            table-layout: fixed; /* Wichtig: Zwingt Spalten sich an Breiten zu halten */
        }}
        /* Spaltenbreiten festlegen: Projektname (25%), Pfad (25%), Kurzbeschreibung (50%) */
        th:nth-child(1), td:nth-child(1) {{ width: 25%; }}
        th:nth-child(2), td:nth-child(2) {{ width: 25%; }}
        th:nth-child(3), td:nth-child(3) {{ width: 50%; }}

        th, td {{
            padding: 1rem 1.25rem;
            border-bottom: 1px solid var(--border-color);
            vertical-align: top;
            word-wrap: break-word;
            overflow-wrap: break-word;
        }}
        th {{
            background-color: rgba(51, 65, 85, 0.4);
            font-weight: 600;
            color: var(--text-color);
            font-size: 0.9rem;
            text-transform: uppercase;
            letter-spacing: 0.05em;
        }}
        tr:last-child td {{
            border-bottom: none;
        }}
        tr:hover {{
            background-color: rgba(51, 65, 85, 0.2);
        }}
        td code {{
            background-color: rgba(15, 23, 42, 0.6);
            padding: 0.2rem 0.4rem;
            border-radius: 0.25rem;
            font-family: monospace;
            font-size: 0.9em;
            color: #38bdf8;
            word-break: break-all;
        }}
        .beschreibung-text {{
            color: var(--text-muted);
            font-size: 0.95rem;
            line-height: 1.5;
        }}
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>Arduino Projekt-Inventar</h1>
            <button id="fullscreen-btn" class="btn-vollbild">Vollbild Modus</button>
        </header>
        
        <div class="tabelle-wrapper">
            <table>
                <thead>
                    <tr>
                        {"".join(f"<th>{kopf}</th>" for kopf in kopf_zellen)}
                    </tr>
                </thead>
                <tbody>
"""

    for zeile in daten_zeilen:
        html_inhalt += "                    <tr>\n"
        for i, zelle in enumerate(zeile):
            if i == len(zeile) - 1 and len(zeile) >= 3:
                html_inhalt += f'                        <td><div class="beschreibung-text">{zelle}</div></td>\n'
            else:
                html_inhalt += f'                        <td>{zelle}</td>\n'
        html_inhalt += "                    </tr>\n"

    html_inhalt += f"""                </tbody>
            </table>
        </div>
    </div>

    <script>
        const btn = document.getElementById('fullscreen-btn');
        
        btn.addEventListener('click', () => {{
            if (!document.fullscreenElement) {{
                document.documentElement.requestFullscreen().catch(err => {{
                    alert(`Fehler beim Umschalten in den Vollbildmodus: ${{err.message}}`);
                }});
            }} else {{
                if (document.exitFullscreen) {{
                    document.exitFullscreen();
                }}
            }}
        }});

        document.addEventListener('fullscreenchange', () => {{
            if (document.fullscreenElement) {{
                btn.textContent = 'Vollbild beenden';
            }} else {{
                btn.textContent = 'Vollbild Modus';
            }}
        }});
    </script>
</body>
</html>
"""

    output_pfad = Path(html_pfad)
    with open(output_pfad, 'w', encoding='utf-8') as f:
        f.write(html_inhalt)
        
    print(f"HTML-Seite erfolgreich aktualisiert: {output_pfad.resolve()}")

if __name__ == "__main__":
    markdown_tabelle_zu_html("arduino_inventar_bericht.md", "arduino_inventar.html")