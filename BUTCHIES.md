# Butchies Media Center (Grundlage: Jellyfin)

Eigene Kopie des freien Medienservers **Jellyfin** (GPL-2.0, siehe `LICENSE`), angepasst für den
WD MyCloud EX2 Ultra (32-Bit-ARM, ~1 GB RAM). Offiziell unterstützt Jellyfin ab 10.11 kein 32-Bit-ARM mehr –
dieser Zweig baut es selbst dafür.

- Grundlage: `jellyfin/jellyfin` **v10.11.11**, Weboberfläche `jellyfin/jellyfin-web` v10.11.11
- Bau: `.github/workflows/build-armhf.yml` → GitHub-Release `armhf-…` mit `butchies-media-center-*-armhf.tar.xz`
- Test: Start im ARMv7-Emulator, Abfrage `/System/Info/Public` und `/web/`
- Installation auf dem NAS: siehe `nas/jellyfin/` im Repo `butchies-raucherstube`
- Lizenz: GPL-2.0 – bei Weitergabe Quellcode mitgeben. Keine Emby-Dateien übernehmen.

## Plan
1. ✅ Quelle übernommen, ARMv7-Build + Starttest (Weboberfläche lief im Emulator nach ~200 s)
2. Auf dem NAS testen (Speicher, Zusammenspiel mit der Website)
3. (später) Butchies-Look / Gold Lounge
4. Sparsamer Betrieb (Speicher, 4K-Sperre), eigene Erweiterungen
