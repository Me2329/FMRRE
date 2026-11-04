"""Romanian for FMRRE PC rescue. Keys are the exact English messages; "{}" stands for a part that changes
(it is translated too when it is a known message), "{w}" for one word. Used by fmrre_rescue.T()."""

RO = {
    # ---- modes, status
    "not found": "negăsit",
    "Android is running (adb)": "Android pornit (adb)",
    "recovery, waiting for 'adb sideload'": "recovery, așteaptă 'adb sideload'",
    "Android rescue mode (adb)": "modul de salvare Android (adb)",
    "connected - but USB debugging isn't allowed for this computer yet":
        "conectat - dar depanarea USB nu este încă permisă pentru acest calculator",
    "connected - but adb sees it as offline": "conectat - dar adb îl vede offline",
    "fastbootd (userspace fastboot)": "fastbootd (fastboot din Android)",
    "Samsung Download mode (Heimdall)": "modul Download Samsung (Heimdall)",
    "Phone": "Telefon", "Mode": "Mod", "Serial": "Serie", "Slot": "Slot", "Note": "Notă", "serial": "serie", "slot": "slot",
    "Unknown phone": "Telefon necunoscut",
    "not FMRRE (stock or another recovery)": "nu este FMRRE (original sau alt recovery)",
    "UNLOCKED": "DEBLOCAT", "LOCKED": "BLOCAT", "CHECK THE SCREEN": "VERIFICĂ ECRANUL", "UNKNOWN": "NECUNOSCUT",
    "unbootable": "nu pornește", "booted fine": "a pornit bine", "never booted fully": "nu a pornit niciodată complet",
    "{w} tries left": "{} încercări rămase", "{w} try left": "{} încercare rămasă",
    "this bootloader doesn't report it": "acest bootloader nu raportează asta",
    "Download mode doesn't tell the computer - it is on the phone's screen":
        "modul Download nu îi spune calculatorului - e pe ecranul telefonului",
    "the phone reports no lock state": "telefonul nu raportează starea de blocare",
    "{w} phones in fastboot - using {w} (choose one with --serial)": "{} telefoane în fastboot - se folosește {} (alege unul cu --serial)",
    "{w} phones on adb - using {w} (choose one with --serial)": "{} telefoane pe adb - se folosește {} (alege unul cu --serial)",
    "Samsung Knox warranty bit: tripped (0x1)": "Bitul de garanție Knox Samsung: declanșat (0x1)",

    # ---- bootloader lock
    "Repairs from this computer are possible.": "Reparațiile de pe acest calculator sunt posibile.",
    "Nothing can be flashed while the bootloader is locked. To unlock (it ERASES the phone): {} Without unlocking: {}":
        "Nu se poate instala nimic cât timp bootloader-ul este blocat. Deblocare (ȘTERGE telefonul): {} Fără deblocare: {}",
    "OEM unlocking is already allowed in Developer options. {}": "Deblocarea OEM este deja permisă în Opțiuni dezvoltator. {}",
    "OEM unlocking is OFF in Developer options. {}": "Deblocarea OEM este OPRITĂ în Opțiuni dezvoltator. {}",
    "Can't ask the phone in this mode.": "Telefonul nu poate fi întrebat în acest mod.",
    "The phone doesn't report it ({}). If you know it is unlocked, pass --unlocked.":
        "Telefonul nu raportează asta ({}). Dacă știi că e deblocat, folosește --unlocked.",
    "Samsung Download mode doesn't tell the computer. On the phone's screen: 'OEM LOCK: OFF (U)' (or just OFF) means unlocked, "
    "'ON (L)' means locked. 'KG STATE: Prenormal' (or 'RMM STATE: Prenormal') refuses custom images until the phone has been "
    "online for a while. Checked? Tick the box (or pass --unlocked).":
        "Modul Download Samsung nu îi spune calculatorului. Pe ecranul telefonului: 'OEM LOCK: OFF (U)' (sau doar OFF) înseamnă "
        "deblocat, 'ON (L)' înseamnă blocat. 'KG STATE: Prenormal' (sau 'RMM STATE: Prenormal') refuză imaginile modificate până "
        "când telefonul a stat o vreme online. Ai verificat? Bifează căsuța (sau folosește --unlocked).",
    # unlock help, per maker
    "Settings > About phone: tap Build number 7 times. Developer options: turn on OEM unlocking. Then 'adb reboot bootloader' and "
    "'fastboot flashing unlock' (confirm with the volume keys).":
        "Setări > Despre telefon: atinge de 7 ori Numărul versiunii. Opțiuni dezvoltator: activează Deblocarea OEM. Apoi "
        "'adb reboot bootloader' și 'fastboot flashing unlock' (confirmă cu tastele de volum).",
    "Developer options: turn on OEM unlocking (if it is missing, this model or region can't be unlocked - e.g. US models). Power "
    "off, hold Volume Up + Volume Down and plug in the cable, then long-press Volume Up to unlock. This trips the Knox warranty "
    "bit for good.":
        "Opțiuni dezvoltator: activează Deblocarea OEM (dacă lipsește, modelul sau regiunea nu se poate debloca - de ex. modelele "
        "din SUA). Oprește telefonul, ține Volum Sus + Volum Jos și conectează cablul, apoi apasă lung Volum Sus pentru deblocare. "
        "Bitul de garanție Knox se declanșează definitiv.",
    "Developer options: turn on OEM unlocking and add your Mi account under Mi Unlock status, then use Xiaomi's official Mi "
    "Unlock tool on Windows (there is a waiting period; HyperOS phones need approval in the Xiaomi Community app first).":
        "Opțiuni dezvoltator: activează Deblocarea OEM și adaugă contul Mi la Mi Unlock status, apoi folosește unealta oficială "
        "Mi Unlock pe Windows (există o perioadă de așteptare; telefoanele cu HyperOS au nevoie întâi de aprobare în aplicația "
        "Xiaomi Community).",
    "Developer options: turn on OEM unlocking, then 'fastboot flashing unlock' (older models: 'fastboot oem unlock').":
        "Opțiuni dezvoltator: activează Deblocarea OEM, apoi 'fastboot flashing unlock' (modelele mai vechi: 'fastboot oem unlock').",
    "Developer options: turn on OEM unlocking, then 'fastboot flashing unlock'.":
        "Opțiuni dezvoltator: activează Deblocarea OEM, apoi 'fastboot flashing unlock'.",
    "Get an unlock code from Motorola's bootloader unlock website with the output of 'fastboot oem get_unlock_data', then "
    "'fastboot oem unlock <code>'.":
        "Cere un cod de deblocare pe site-ul Motorola de deblocare a bootloader-ului, cu rezultatul comenzii 'fastboot oem "
        "get_unlock_data', apoi 'fastboot oem unlock <cod>'.",
    "Get an unlock code from Sony's developer website (it needs the IMEI), then 'fastboot oem unlock 0x<code>'.":
        "Cere un cod de deblocare pe site-ul Sony pentru dezvoltatori (are nevoie de IMEI), apoi 'fastboot oem unlock 0x<cod>'.",
    "Huawei stopped giving out unlock codes in 2018: most Huawei phones can't be unlocked.":
        "Huawei nu mai dă coduri de deblocare din 2018: majoritatea telefoanelor Huawei nu pot fi deblocate.",
    "Honor doesn't give out unlock codes: most Honor phones can't be unlocked.":
        "Honor nu dă coduri de deblocare: majoritatea telefoanelor Honor nu pot fi deblocate.",
    "Only with the official Deep Testing app, on some models and regions.":
        "Doar cu aplicația oficială Deep Testing, pe unele modele și în unele regiuni.",
    "Developer options: turn on OEM unlocking, then 'fastboot flashing unlock' (some makers: 'fastboot oem unlock'). Check the "
    "maker's website - some need an unlock code.":
        "Opțiuni dezvoltator: activează Deblocarea OEM, apoi 'fastboot flashing unlock' (la unii producători: 'fastboot oem "
        "unlock'). Verifică site-ul producătorului - unii cer un cod de deblocare.",
    # official restore tools
    "Google's Pixel Update and Software Repair tool (pixelrepair.withgoogle.com) reinstalls the official software on a locked "
    "Pixel; the stock recovery's 'Apply update from ADB' with a full OTA from developers.google.com works too.":
        "Unealta Google Pixel Update and Software Repair (pixelrepair.withgoogle.com) reinstalează software-ul oficial pe un "
        "Pixel blocat; merge și 'Apply update from ADB' din recovery-ul original, cu un OTA complet de pe developers.google.com.",
    "Smart Switch on a computer (Emergency Software Recovery), or Odin with the official firmware (BL, AP, CP, CSC) - official "
    "firmware flashes on a locked phone.":
        "Smart Switch pe calculator (Emergency Software Recovery) sau Odin cu firmware-ul oficial (BL, AP, CP, CSC) - firmware-ul "
        "oficial se instalează și pe un telefon blocat.",
    "A Xiaomi service center, or Mi Flash with an unlocked bootloader; the stock recovery can sideload the official package.":
        "Un service Xiaomi sau Mi Flash cu bootloader deblocat; recovery-ul original poate instala pachetul oficial prin sideload.",
    "Motorola's Software Fix (Rescue and Smart Assistant) on a computer.":
        "Software Fix de la Motorola (Rescue and Smart Assistant) pe calculator.",
    "The maker's official repair tool, or the stock recovery: 'Apply update from ADB' and then 'adb sideload <official full "
    "OTA>.zip' reinstalls the system on a locked phone, keeping data where possible.":
        "Unealta oficială de reparare a producătorului, sau recovery-ul original: 'Apply update from ADB' și apoi 'adb sideload "
        "<OTA oficial complet>.zip' reinstalează sistemul pe un telefon blocat, păstrând datele când se poate.",

    # ---- connecting
    "No phone found. {}": "Niciun telefon găsit. {}",
    "Plug it in with a USB cable that carries data, then put it in one of these modes:\n"
    "  - bootloader / fastboot (most phones): power off, then hold Volume Down + Power\n"
    "  - Samsung Download mode: power off, hold Volume Up + Volume Down and plug the cable in, then press Volume Up\n"
    "  - Android or a recovery with USB debugging on (adb)\n"
    "A bootlooping phone: hold the key combination while it restarts.\n"
    "Windows needs the phone's USB driver (Google USB driver for fastboot / adb, Samsung USB driver for Download mode).":
        "Conectează-l cu un cablu USB de date, apoi pune-l într-unul din aceste moduri:\n"
        "  - bootloader / fastboot (majoritatea telefoanelor): oprește-l, apoi ține Volum Jos + Pornire\n"
        "  - modul Download Samsung: oprește-l, ține Volum Sus + Volum Jos și conectează cablul, apoi apasă Volum Sus\n"
        "  - Android sau un recovery cu depanarea USB activă (adb)\n"
        "Un telefon în bootloop: ține combinația de taste în timp ce repornește.\n"
        "Pe Windows e nevoie de driverul USB al telefonului (Google USB driver pentru fastboot / adb, driverul USB Samsung pentru "
        "modul Download).",
    "{}\nMissing on this computer: {} - {}": "{}\nLipsește de pe acest calculator: {} - {}",
    "install Android platform-tools: https://developer.android.com/tools/releases/platform-tools (unzipping it next to this "
    "script works too)":
        "instalează Android platform-tools: https://developer.android.com/tools/releases/platform-tools (merge și dezarhivat "
        "lângă acest script)",
    "install Heimdall (Linux: sudo apt install heimdall-flash, macOS: brew install heimdall), or use Odin on Windows":
        "instalează Heimdall (Linux: sudo apt install heimdall-flash, macOS: brew install heimdall) sau folosește Odin pe Windows",
    "{w} is not installed - {}": "{} nu este instalat - {}",

    # ---- findings
    "No phone found": "Niciun telefon găsit",
    "USB debugging isn't allowed for this computer": "Depanarea USB nu este permisă pentru acest calculator",
    "Unlock the phone's screen and tap Allow on 'Allow USB debugging?' (tick 'Always allow'). No prompt? Developer options > "
    "Revoke USB debugging authorizations, then plug in again.":
        "Deblochează ecranul telefonului și atinge Permite la 'Permiți depanarea USB?' (bifează 'Permite întotdeauna'). Nu apare? "
        "Opțiuni dezvoltator > Revocă autorizările pentru depanarea USB, apoi conectează-l din nou.",
    "adb sees the phone as offline": "adb vede telefonul offline",
    "Unplug and plug in again (another cable or port helps), or restart adb: 'adb kill-server'.":
        "Deconectează-l și conectează-l din nou (ajută alt cablu sau alt port) sau repornește adb: 'adb kill-server'.",
    "The bootloader is locked": "Bootloader-ul este blocat",
    "Slot {w} {}; slot {w} booted fine before": "Slotul {} {}; slotul {} a pornit bine înainte",
    "is marked unbootable": "este marcat că nu pornește",
    "has no boot attempts left": "nu mai are încercări de pornire",
    "never finished booting": "nu a terminat niciodată pornirea",
    "Switching to slot {w} goes back to the system that worked - often the one before an update.":
        "Trecerea pe slotul {} întoarce sistemul care mergea - de obicei cel de dinaintea unei actualizări.",
    "Both slots are marked unbootable": "Ambele sloturi sunt marcate că nu pornesc",
    "Start FMRRE from this computer to look closer, or reinstall the firmware.":
        "Pornește FMRRE de pe acest calculator ca să vezi mai bine, sau reinstalează firmware-ul.",
    "A system update is still being merged": "O actualizare de sistem încă se aplică (merge)",
    "Don't flash or wipe anything until Android finishes it. If it can't start to finish it, the maker's official firmware tool "
    "is the safe way.":
        "Nu instala și nu șterge nimic până nu termină Android. Dacă nu poate porni ca s-o termine, unealta oficială de firmware "
        "a producătorului este calea sigură.",
    "A system update is installed but not finished": "O actualizare de sistem este instalată, dar neterminată",
    "If the bootloop began with this update, cancelling it (and starting the other slot) goes back to the previous version. "
    "fastbootd also refuses to flash system partitions until it is cancelled.":
        "Dacă bootloop-ul a început cu această actualizare, anularea ei (și pornirea celuilalt slot) revine la versiunea "
        "anterioară. fastbootd refuză să instaleze partițiile de sistem până nu este anulată.",
    "The battery is low": "Bateria este descărcată",
    "Charge it for half an hour before flashing anything.": "Încarcă-l o jumătate de oră înainte să instalezi ceva.",
    "FMRRE can be started from this computer": "FMRRE poate fi pornit de pe acest calculator",
    "'fastboot boot' runs it from RAM - nothing is written - and its Auto repair checks the logs, boot images, vbmeta, both "
    "slots and /data from the inside.":
        "'fastboot boot' îl rulează din RAM - nu se scrie nimic - iar Repararea automată verifică din interior jurnalele, "
        "imaginile de boot, vbmeta, ambele sloturi și /data.",
    "This image can't be started with fastboot boot": "Această imagine nu poate fi pornită cu fastboot boot",
    "{} isn't a boot/recovery image (vendor_boot images can only be flashed).":
        "{} nu este o imagine boot/recovery (imaginile vendor_boot pot fi doar instalate).",
    "For the most thorough check: an FMRRE image for this phone": "Pentru cea mai temeinică verificare: o imagine FMRRE pentru acest telefon",
    "With --recovery fmrre-recovery.img (built from this phone's own recovery or boot image) it is started from RAM and Auto "
    "repair looks at the phone from the inside. Without one: the recovery installed on the phone is tried.":
        "Cu --recovery fmrre-recovery.img (făcută din imaginea recovery sau boot a acestui telefon) pornește din RAM și "
        "Repararea automată verifică telefonul din interior. Fără ea: se încearcă recovery-ul instalat pe telefon.",
    "Samsung Download mode": "Modul Download Samsung",
    "Install FMRRE (recovery + vbmeta with verification disabled) and let its Auto repair fix the phone, or flash the stock "
    "boot / vbmeta / recovery from your firmware's AP file to undo a bad change.":
        "Instalează FMRRE (recovery + vbmeta cu verificarea oprită) și lasă Repararea automată să repare telefonul, sau "
        "instalează boot / vbmeta / recovery originale din fișierul AP al firmware-ului ca să anulezi o modificare greșită.",
    "Android starts": "Android pornește",
    "When it misbehaves: the tool can reboot to recovery and run FMRRE's Auto repair (when FMRRE is the installed recovery).":
        "Când nu merge bine: unealta poate reporni în recovery și rula Repararea automată din FMRRE (când FMRRE este "
        "recovery-ul instalat).",
    "Problems: {}": "Probleme: {}", "Planned fixes: {}": "Reparații planificate: {}", "Nothing to repair.": "Nimic de reparat.",
    "FMRRE Auto repair": "Reparare automată FMRRE",
    "A recovery other than FMRRE is running": "Rulează un alt recovery decât FMRRE",
    "A stock recovery can reinstall the official system without unlocking: 'Apply update from ADB', then 'adb sideload "
    "<official OTA>.zip'. To use FMRRE: reboot to the bootloader and start it from here.":
        "Un recovery original poate reinstala sistemul oficial fără deblocare: 'Apply update from ADB', apoi 'adb sideload "
        "<OTA oficial>.zip'. Pentru FMRRE: repornește în bootloader și pornește-l de aici.",
    "Waiting for adb sideload": "Se așteaptă adb sideload",
    "'adb sideload <rom-or-ota>.zip' installs it; cancel sideload on the phone to get back.":
        "'adb sideload <rom-sau-ota>.zip' îl instalează; anulează sideload pe telefon ca să revii.",

    # ---- repairs: questions, refusals, progress
    "{} yes (--yes)": "{} da (--yes)",
    "waiting for the phone ({w})...": "se așteaptă telefonul ({})...",
    "Not {}: the bootloader is LOCKED.\n{}": "Nu se face {}: bootloader-ul este BLOCAT.\n{}",
    "Not {}: the phone doesn't say whether its bootloader is unlocked.\n{}":
        "Nu se face {}: telefonul nu spune dacă bootloader-ul este deblocat.\n{}",
    "switching slots": "schimbarea slotului", "cancelling the update": "anularea actualizării", "changing vbmeta": "modificarea vbmeta",
    "flashing": "instalarea", "wiping": "ștergerea", "starting FMRRE": "pornirea FMRRE", "installing FMRRE": "instalarea FMRRE",
    "repairing": "repararea",
    "This needs the phone in {} mode - it is in: {}.": "Pentru asta telefonul trebuie să fie în modul {} - acum este în: {}.",
    "or": "sau",
    "A system update is still being merged: flashing or wiping now could leave the phone unable to start.":
        "O actualizare de sistem încă se aplică: o instalare sau o ștergere acum ar putea lăsa telefonul fără să mai pornească.",
    "Can't get to the bootloader from {} - power off, then hold Volume Down + Power.":
        "Nu se poate ajunge în bootloader din {} - oprește telefonul, apoi ține Volum Jos + Pornire.",
    "The phone didn't come back in the bootloader. Hold Volume Down + Power, then try again.":
        "Telefonul nu a revenit în bootloader. Ține Volum Jos + Pornire, apoi încearcă din nou.",
    "This phone has no A/B slots.": "Acest telefon nu are sloturi A/B.",
    "Make slot {w} active (the other system, can be switched back)?": "Activezi slotul {} (celălalt sistem, se poate schimba înapoi)?",
    "Cancel the pending system update?": "Anulezi actualizarea de sistem în așteptare?",
    "Flash vbmeta with verification disabled ({})?": "Instalezi vbmeta cu verificarea oprită ({})?",
    "both slots": "ambele sloturi",
    "No such file: {}": "Fișierul nu există: {}",
    "Flash {} to {w} with Heimdall?": "Instalezi {} în {} cu Heimdall?",
    "Flash {} to {w}?": "Instalezi {} în {}?",
    "Factory reset: ERASES apps, settings, photos and every file on the phone's internal storage.":
        "Resetare din fabrică: ȘTERGE aplicațiile, setările, fotografiile și toate fișierele din memoria internă a telefonului.",
    "Type {w} to continue:": "Tastează {} ca să continui:",
    "Not wiped (ERASE wasn't typed).": "Nu s-a șters (nu s-a tastat ERASE).",
    "fastboot can't reboot to {w}": "fastboot nu poate reporni în {}",
    "Download mode: hold Volume Down + Power for 7 seconds to restart (add Volume Up right after the screen goes black for "
    "recovery).":
        "Modul Download: ține Volum Jos + Pornire 7 secunde ca să repornească (adaugă Volum Sus imediat ce ecranul se "
        "înnegrește, pentru recovery).",
    "{} can't be started with 'fastboot boot' ({}).": "{} nu poate fi pornit cu 'fastboot boot' ({}).",
    "a vendor_boot image": "o imagine vendor_boot", "not a boot/recovery image": "nu este o imagine boot/recovery",
    "Start FMRRE from this computer (fastboot boot - nothing is written)?":
        "Pornești FMRRE de pe acest calculator (fastboot boot - nu se scrie nimic)?",
    "The bootloader refused to start the image. On some phones 'fastboot boot' is disabled - flash it instead (scripts/install.sh "
    "--permanent).":
        "Bootloader-ul a refuzat să pornească imaginea. Pe unele telefoane 'fastboot boot' este dezactivat - instaleaz-o în "
        "schimb (scripts/install.sh --permanent).",
    "FMRRE didn't show up on adb. If its screen is on the phone, check the cable / USB driver.":
        "FMRRE nu a apărut pe adb. Dacă ecranul lui este pe telefon, verifică cablul / driverul USB.",
    "A recovery started, but it isn't FMRRE.": "A pornit un recovery, dar nu este FMRRE.",
    "FMRRE is running.": "FMRRE rulează.",
    "(dry run: FMRRE would now check the phone and repair it)": "(simulare: acum FMRRE ar verifica telefonul și l-ar repara)",
    "The running recovery isn't FMRRE.": "Recovery-ul care rulează nu este FMRRE.",
    "FMRRE Auto repair - what it found:": "Reparare automată FMRRE - ce a găsit:",
    "Auto repair may Format Data if it finds that it is needed - that ERASES the phone.":
        "Repararea automată poate formata Data dacă găsește că e nevoie - asta ȘTERGE telefonul.",
    "Not allowing Format Data (ERASE wasn't typed).": "Formatarea Data nu este permisă (nu s-a tastat ERASE).",
    "Nothing for Auto repair to fix. Details: 'fmrre_rescue.py logs' saves the phone's logs.":
        "Nimic de reparat pentru Repararea automată. Detalii: 'fmrre_rescue.py logs' salvează jurnalele telefonului.",
    "Run these repairs on the phone and restart it?": "Rulezi aceste reparații pe telefon și îl repornești?",
    "Run these repairs on the phone?": "Rulezi aceste reparații pe telefon?",
    "Samsung needs a recovery.img (build it from the stock recovery.img in your firmware's AP file: scripts/install.sh "
    "--build-from recovery.img).":
        "Samsung are nevoie de un recovery.img (fă-l din recovery.img original din fișierul AP al firmware-ului: "
        "scripts/install.sh --build-from recovery.img).",
    "Flash FMRRE to RECOVERY and a vbmeta with verification disabled to VBMETA?":
        "Instalezi FMRRE în RECOVERY și un vbmeta cu verificarea oprită în VBMETA?",
    "Heimdall failed - see above. (Odin on Windows: scripts/install.sh --odin-only builds the AP package.)":
        "Heimdall a eșuat - vezi mai sus. (Odin pe Windows: scripts/install.sh --odin-only face pachetul AP.)",
    "Flashed. Now start STRAIGHT into recovery, or stock Android replaces FMRRE on its first boot:\n"
    "   1. hold Volume Down + Power until the screen goes black\n"
    "   2. at once switch to Volume Up + Power (+ Bixby on older models) and hold until FMRRE appears":
        "Instalat. Acum pornește DIRECT în recovery, altfel Android-ul original înlocuiește FMRRE la prima pornire:\n"
        "   1. ține Volum Jos + Pornire până se înnegrește ecranul\n"
        "   2. treci imediat la Volum Sus + Pornire (+ Bixby pe modelele vechi) și ține până apare FMRRE",
    "{} has none of: {}": "{} nu conține niciuna dintre: {}",
    "Flash the stock {} from {}?": "Instalezi {} originale din {}?",
    "No logs to save in this mode ({}).": "Nu sunt jurnale de salvat în acest mod ({}).",
    "Saved: {}": "Salvat: {}",
    "The phone is waiting in {} - cancel it on the phone first.": "Telefonul așteaptă în {} - anulează întâi pe telefon.",
    "Restart to recovery and run FMRRE's Auto repair there?": "Repornești în recovery și rulezi acolo Repararea automată FMRRE?",
    "adb couldn't restart the phone.": "adb nu a putut reporni telefonul.",
    "This recovery isn't FMRRE. Give an FMRRE image (--recovery fmrre-recovery.img) to start it from this computer, or use "
    "this recovery's 'Apply update from ADB' with the official firmware.":
        "Acest recovery nu este FMRRE. Dă o imagine FMRRE (--recovery fmrre-recovery.img) ca să-l pornești de pe acest "
        "calculator, sau folosește 'Apply update from ADB' din acest recovery cu firmware-ul oficial.",
    "Waiting for FMRRE (start it with the key combination above)...":
        "Se așteaptă FMRRE (pornește-l cu combinația de taste de mai sus)...",
    "In Download mode the tool can:\n"
    "  - install FMRRE and let it repair the phone:   fix --recovery fmrre-recovery.img\n"
    "    (build the image from the stock recovery.img in your firmware: scripts/install.sh --build-from recovery.img)\n"
    "  - flash the stock boot / vbmeta / recovery from your firmware, undoing root or a bad flash:\n"
    "                                                    fix --ap AP_<model>_....tar.md5\n"
    "  The whole firmware (BL, AP, CP, CSC) is best flashed with Odin or Smart Switch.":
        "În modul Download unealta poate:\n"
        "  - instala FMRRE și să-l lase să repare telefonul:   fix --recovery fmrre-recovery.img\n"
        "    (fă imaginea din recovery.img original din firmware: scripts/install.sh --build-from recovery.img)\n"
        "  - instala boot / vbmeta / recovery originale din firmware, anulând root-ul sau o instalare greșită:\n"
        "                                                    fix --ap AP_<model>_....tar.md5\n"
        "  Firmware-ul întreg (BL, AP, CP, CSC) se instalează cel mai bine cu Odin sau Smart Switch.",
    "Slot {w} {}, slot {w} booted fine before: going back to slot {w}.": "Slotul {} {}, slotul {} a pornit bine înainte: se revine la slotul {}.",
    "Done. If it still doesn't start, run fix again: the next step is starting FMRRE to repair from the inside.":
        "Gata. Dacă tot nu pornește, rulează din nou fix: pasul următor este pornirea FMRRE ca să repare din interior.",
    "Try starting the recovery installed on the phone (FMRRE, if it is installed)?":
        "Încerci să pornești recovery-ul instalat pe telefon (FMRRE, dacă este instalat)?",
    "The installed recovery isn't FMRRE. Give an FMRRE image (--recovery fmrre-recovery.img) to start it from this "
    "computer, or use the stock recovery's 'Apply update from ADB' with the official firmware.":
        "Recovery-ul instalat nu este FMRRE. Dă o imagine FMRRE (--recovery fmrre-recovery.img) ca să-l pornești de pe "
        "acest calculator, sau folosește 'Apply update from ADB' din recovery-ul original cu firmware-ul oficial.",
    "The phone's recovery didn't start (or this bootloader can't 'reboot recovery'). Give an FMRRE image: fix --recovery "
    "fmrre-recovery.img":
        "Recovery-ul telefonului nu a pornit (sau acest bootloader nu poate face 'reboot recovery'). Dă o imagine FMRRE: fix "
        "--recovery fmrre-recovery.img",
    "Nothing is clearly broken as far as the bootloader can tell. Give an FMRRE image (--recovery) so it can look inside, or "
    "choose a repair: switch-slot, disable-vbmeta, cancel-update, flash, factory, wipe-data.":
        "Din ce vede bootloader-ul, nimic nu este clar stricat. Dă o imagine FMRRE (--recovery) ca să se uite în interior, "
        "sau alege o reparație: switch-slot, disable-vbmeta, cancel-update, flash, factory, wipe-data.",
    "not a vbmeta image (no AVB0 header)": "nu este o imagine vbmeta (lipsește antetul AVB0)",
    "{} in {} is damaged ({}) - download the firmware again.": "{} din {} este deteriorat ({}) - descarcă din nou firmware-ul.",
    "{} is damaged: its MD5 doesn't match - download the firmware again.": "{} este deteriorat: MD5-ul nu se potrivește - descarcă din nou firmware-ul.",
    "checking the MD5 of {} ({} MB)...": "se verifică MD5-ul pentru {} ({} MB)...",
    "Unexpected error - nothing more was done. Please report it with the details below:":
        "Eroare neașteptată - nu s-a mai făcut nimic. Te rog raporteaz-o, cu detaliile de mai jos:",
    "{} is not a firmware tar: {}": "{} nu este un tar de firmware: {}",
    "{} doesn't look like a {w} image (header {}) - not flashing it": "{} nu pare o imagine {} (antet {}) - nu se instalează",
    "unpacking {} ({} MB)": "se despachetează {} ({} MB)",
    "{} is not a factory image zip: {}": "{} nu este un zip cu imagine de fabrică: {}",
    "{} has no image-*.zip inside: not a factory image (flash an OTA zip with adb sideload instead).":
        "{} nu conține image-*.zip: nu este o imagine de fabrică (un zip OTA se instalează cu adb sideload).",
    "This factory image is for {}, but the phone is {}: not flashing it (the wrong bootloader can make a phone unusable).":
        "Această imagine de fabrică este pentru {}, dar telefonul este {}: nu se instalează (un bootloader greșit poate face "
        "telefonul inutilizabil).",
    "(the image doesn't name its phone - check it is the one for {})": "(imaginea nu își numește telefonul - verifică dacă e cea pentru {})",
    "this phone": "acest telefon",
    "Reinstall and factory reset: ERASES apps, settings, photos and every file on the phone's internal storage.":
        "Reinstalare și resetare din fabrică: ȘTERGE aplicațiile, setările, fotografiile și toate fișierele din memoria internă.",
    "Reinstall the official firmware from {} and ERASE the phone?": "Reinstalezi firmware-ul oficial din {} și ȘTERGI telefonul?",
    "Reinstall the official firmware from {} (apps and files are kept)?": "Reinstalezi firmware-ul oficial din {} (aplicațiile și fișierele rămân)?",
    "Flashing the {w} failed - see above. Don't restart the phone: try again, or use the maker's official tool.":
        "Instalarea {} a eșuat - vezi mai sus. Nu reporni telefonul: încearcă din nou sau folosește unealta oficială a producătorului.",
    "The phone didn't come back in the bootloader after flashing the {w}.": "Telefonul nu a revenit în bootloader după instalarea {}.",
    "unpacking {}": "se despachetează {}",
    "reinstalling the firmware": "reinstalarea firmware-ului",
    "Factory image zip for this phone": "Zip-ul cu imaginea de fabrică pentru acest telefon",
    "Reinstall firmware...": "Reinstalează firmware...",
    "==> done": "==> gata", "==> not done": "==> nu s-a făcut", "cancelled": "anulat",

    # ---- window
    "Looking for a phone...": "Se caută un telefon...", "Plug the phone in": "Conectează telefonul",
    "Bootloader: {}": "Bootloader: {}",
    "I checked on the phone: the bootloader is unlocked (Samsung: OEM LOCK: OFF)":
        "Am verificat pe telefon: bootloader-ul este deblocat (Samsung: OEM LOCK: OFF)",
    "What was found": "Ce s-a găsit", "FMRRE image:": "Imagine FMRRE:", "Browse...": "Alege...",
    "Samsung AP file:": "Fișier AP Samsung:", "Fix it for me": "Repară-l în locul meu", "Start FMRRE": "Pornește FMRRE",
    "Auto repair (FMRRE)": "Reparare automată (FMRRE)", "Other slot": "Celălalt slot", "Disable vbmeta": "Dezactivează vbmeta",
    "Cancel update": "Anulează actualizarea", "Flash image...": "Instalează imagine...", "Wipe data...": "Șterge datele...",
    "Samsung:": "Samsung:", "Install FMRRE": "Instalează FMRRE",
    "Stock boot / vbmeta / recovery from AP": "boot / vbmeta / recovery originale din AP", "Restart to:": "Repornire în:",
    "System": "Sistem", "Save logs...": "Salvează jurnalele...", "Refresh": "Reîmprospătează",
    "FMRRE image for this phone": "Imagine FMRRE pentru acest telefon", "Samsung firmware AP file": "Fișierul AP al firmware-ului Samsung",
    "Flash an image": "Instalează o imagine",
    "Partition (boot, init_boot, vendor_boot, recovery, vbmeta, dtbo...):": "Partiția (boot, init_boot, vendor_boot, recovery, vbmeta, dtbo...):",
    "Image for {}": "Imagine pentru {}", "Save the phone's logs to": "Salvează jurnalele telefonului în",
    "FMRRE PC rescue {} - tools: {}": "FMRRE PC rescue {} - unelte: {}",
}
