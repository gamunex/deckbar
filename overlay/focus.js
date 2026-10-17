// KWin-Skript (lädt deckbar-overlay beim Start, entlädt es beim Beenden): meldet jedes aktive
// Fenster an das Overlay, damit es sich ausblendet, wenn AION 2 nicht im Fokus ist.
// Nur Strings über D-Bus, Zahlen kämen je nach KWin als int oder double an.
function report(w) {
    callDBus("org.deckbar.Overlay", "/Focus", "org.deckbar.Overlay", "activeWindow",
             w ? String(w.resourceClass) : "", w ? String(w.caption) : "", w ? String(w.pid) : "");
}

workspace.windowActivated.connect(report);
report(workspace.activeWindow);
