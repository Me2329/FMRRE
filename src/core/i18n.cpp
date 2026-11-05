#include "core/i18n.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <unordered_map>

namespace tw::i18n {

namespace {

struct Entry { const char* en; const char* tr; };

// Romanian. Keys are the exact English UI strings; "{}" stands for a part that changes (a number,
// a name, a slot). Screens are written in English, so a missing entry just shows English.
const Entry kRo[] = {
    // navigation, shell, common
    {"Back", "Înapoi"}, {"Home", "Acasă"}, {"Console", "Consolă"}, {"Task", "Sarcină"},
    {"Cancel", "Anulează"}, {"Delete", "Șterge"}, {"Done", "Gata"}, {"Options", "Opțiuni"},
    {"Settings", "Setări"}, {"Advanced", "Avansat"}, {"Install", "Instalare"}, {"Wipe", "Ștergere"},
    {"Backup", "Backup"}, {"Restore", "Restaurare"}, {"Mount", "Montare"}, {"Reboot", "Repornire"},
    {"Files", "Fișiere"}, {"Terminal", "Terminal"}, {"Root", "Root"}, {"Success", "Reușit"}, {"Failed", "Eșuat"},
    {"Completed successfully", "Finalizat cu succes"}, {"Finished with errors", "Terminat cu erori"},
    {"In progress  \xE2\x80\xA2  please don't reboot", "În curs  \xE2\x80\xA2  nu reporni"},
    {"Please wait for the operation to finish", "Așteaptă să se termine operația"},
    {"Another operation is still running", "O altă operație încă rulează"},
    {"An operation is still running!", "O operație încă rulează!"},
    {"Reboot to system?", "Repornești în sistem?"}, {"Your device will restart now.", "Telefonul repornește acum."},
    {"Swipe to reboot", "Glisează pentru repornire"}, {"Enter PIN to unlock FMRRE", "Introdu PIN-ul pentru FMRRE"},
    {"Log saved to {*}", "Jurnal salvat în {*}"}, {"Couldn't save the log", "Jurnalul nu a putut fi salvat"},
    {"Screenshot saved", "Captură salvată"}, {"Screenshot failed", "Captura a eșuat"}, {"Unlocked", "Deblocat"},
    {"FMR Recovery Environment \xE2\x80\xA2 {*}", "Mediul de recuperare FMR \xE2\x80\xA2 {*}"},
    {"FMR Recovery Environment", "Mediul de recuperare FMR"}, {"FMR Recovery Environment  \xE2\x80\xA2  built {*}", "Mediul de recuperare FMR  \xE2\x80\xA2  compilat {*}"}, {"Status", "Stare"}, {"None", "Niciunul"},
    {"All", "Toate"}, {"Off", "Oprit"}, {"Clear", "Golește"}, {"View", "Vezi"}, {"Copy", "Copiază"}, {"Move", "Mută"},
    {"Rename", "Redenumește"}, {"Size", "Mărime"}, {"Name", "Nume"}, {"Storage", "Stocare"}, {"Partitions", "Partiții"},
    {"Slot", "Slot"}, {"Device", "Dispozitiv"}, {"Model", "Model"}, {"Kernel", "Kernel"}, {"Build", "Build"},
    {"Not ready", "Nu este gata"}, {"Not running", "Nu rulează"}, {"Starting...", "Pornește..."}, {"Connected", "Conectat"},
    {"Commands", "Comenzi"}, {"Steps", "Pași"}, {"Result", "Rezultat"}, {"Logs", "Jurnale"}, {"Tools", "Unelte"},

    // home
    {"Zips & images", "Zip-uri și imagini"}, {"Factory reset", "Resetare din fabrică"}, {"Save partitions", "Salvează partițiile"},
    {"Bring it back", "Adu-le înapoi"}, {"Browse & manage", "Răsfoiește și organizează"}, {"Power tools", "Unelte avansate"},
    {"Root shell", "Shell root"}, {"Magisk & modules", "Magisk și module"}, {"Flash system images", "Instalează imagini de sistem"},
    {"Themes, display, security & more", "Teme, ecran, securitate și altele"},
    {"System, bootloader, fastbootd, EDL...", "Sistem, bootloader, fastbootd, EDL..."},
    {"Internal storage  {*}", "Memorie internă  {*}"}, {"Data not mounted", "Data nu este montată"},
    {"Data is encrypted \xE2\x80\xA2 internal storage unavailable", "Data este criptată \xE2\x80\xA2 memoria internă nu e disponibilă"},

    // install
    {"Flash zips or images", "Instalează zip-uri sau imagini"}, {"{} zip(s) queued", "{} zip-uri în coadă"},
    {"Zip", "Zip"}, {"Image", "Imagine"}, {"Sideload", "Sideload"}, {"Queue", "Coadă"},
    {"No zips queued", "Niciun zip în coadă"}, {"Add ROMs, kernels, Magisk, GApps...", "Adaugă ROM-uri, kerneluri, Magisk, GApps..."},
    {"Select zip", "Alege un zip"}, {"Add more", "Adaugă"}, {"Added {*}", "Adăugat {*}"}, {"Zip path", "Calea zip-ului"},
    {"File not found", "Fișierul nu există"}, {"Verify zip integrity", "Verifică integritatea zip-ului"},
    {"CRC-check every entry before flashing", "Verificare CRC a fiecărui fișier înainte de instalare"},
    {"Wipe cache & dalvik after", "Șterge cache și dalvik după"}, {"Recommended after ROM updates", "Recomandat după actualizări de ROM"},
    {"Reboot when done", "Repornește la final"}, {"Automatically boot to system on success", "Pornește automat sistemul dacă reușește"},
    {"Queue a zip to flash", "Adaugă un zip de instalat"}, {"Swipe to flash", "Glisează pentru instalare"},
    {"Swipe to flash all", "Glisează pentru a le instala pe toate"}, {"Flashing {*}", "Se instalează {*}"},
    {"Select image", "Alege imaginea"}, {"Choose image", "Alege imaginea"}, {"Can't find your file?", "Nu-ți găsești fișierul?"},
    {"Type the path, see why it's hidden, get a ready dd command", "Scrie calea, vezi de ce e ascuns, primește comanda dd"},
    {"Images are flashed from the next screen", "Imaginile se instalează din ecranul următor"},
    {"Raw, sparse, .xz, .gz, .zst or zipped images", "Imagini raw, sparse, .xz, .gz, .zst sau în zip"},
    {"Generic System Images, done properly", "Imagini GSI, instalate corect"}, {"ADB sideload from a computer", "ADB sideload de pe calculator"},
    {"ADB sideload", "ADB sideload"}, {"Swipe to start sideload", "Glisează pentru a porni sideload"},
    {"Flash image", "Instalează imaginea"}, {"Target partition", "Partiția țintă"}, {"Verify after flashing", "Verifică după instalare"},
    {"Read back and compare SHA-256", "Recitește și compară SHA-256"}, {"Check archive first", "Verifică întâi arhiva"},
    {"Swipe to flash {}", "Glisează pentru a instala {}"}, {"This looks like a GSI", "Pare a fi un GSI"},
    {"Use the GSI wizard: resizes system, disables verity", "Folosește asistentul GSI: redimensionează system, oprește verity"},
    {"Rebooting in 3 seconds...", "Repornire în 3 secunde..."},

    // GSI wizard
    {"GSI wizard", "Asistent GSI"}, {"Flash any Treble GSI in one swipe", "Instalează orice GSI Treble dintr-o glisare"},
    {"Choose GSI", "Alege GSI"}, {"Select GSI", "Alege GSI"}, {"Choose a GSI first", "Alege întâi un GSI"},
    {"No image selected", "Nicio imagine aleasă"}, {"Dynamic partitions", "Partiții dinamice"}, {"System partition", "Partiția system"},
    {"GSI needs", "GSI-ul are nevoie de"}, {"Free in super", "Liber în super"}, {"Free the other slot", "Eliberează celălalt slot"},
    {"Disable AVB verification", "Dezactivează verificarea AVB"}, {"Factory reset (keeps internal storage)", "Resetare din fabrică (păstrează memoria internă)"},
    {"Needed when coming from another ROM", "Necesar când vii de pe alt ROM"}, {"Read back and compare SHA-256", "Recitește și compară SHA-256"},
    {"Reboot to system when done", "Pornește sistemul la final"}, {"Swipe to flash GSI", "Glisează pentru a instala GSI"},
    {"Flashing GSI", "Se instalează GSI"}, {"Unlocked bootloader required", "Necesită bootloader deblocat"},
    {"Super partitions", "Partiții super"}, {"fits as is", "încape așa"}, {"doesn't fit", "nu încape"},

    // A/B OTA
    {"A/B ROM / OTA", "ROM / OTA A/B"}, {"What happens", "Ce se întâmplă"}, {"Current slot", "Slotul curent"},
    {"Written to", "Scris în"}, {"slot {}", "slotul {}"}, {"Switch to slot {} afterwards", "Trece pe slotul {} la final"},
    {"Verify every partition", "Verifică fiecare partiție"}, {"Check each partition's SHA-256 after writing", "Verifică SHA-256 al fiecărei partiții după scriere"},
    {"Boot the new system right away", "Pornește imediat noul sistem"}, {"Swipe to install to slot {}", "Glisează pentru a instala în slotul {}"},
    {"Extract images (boot.img for Magisk...)", "Extrage imagini (boot.img pentru Magisk...)"}, {"Extract images", "Extrage imagini"},
    {"Extracting images", "Se extrag imaginile"}, {"Install A/B ROM / OTA", "Instalează ROM / OTA A/B"},
    {"Extract images from payload", "Extrage imagini din payload"}, {"For root", "Pentru root"}, {"Select partitions", "Alege partițiile"},
    {"Can't install on this phone", "Nu se poate instala pe acest telefon"}, {"Ignore the device check", "Ignoră verificarea dispozitivului"},
    {"Data is not wiped. Coming from another ROM? Format Data afterwards.", "Datele nu se șterg. Vii de pe alt ROM? Formatează Data după."},
    {"Slot {} becomes active: reboot to start the new system.", "Slotul {} devine activ: repornește ca să pornești noul sistem."},

    // wipe
    {"Factory reset & advanced wipe", "Resetare și ștergere avansată"}, {"Data, Cache and ART/Dalvik cache", "Data, Cache și cache ART/Dalvik"},
    {"Apps and settings are removed. Internal storage (photos, downloads, backups) is kept.",
     "Aplicațiile și setările se șterg. Memoria internă (poze, descărcări, backup-uri) rămâne."},
    {"Swipe to factory reset", "Glisează pentru resetare"}, {"Factory reset complete.", "Resetare finalizată."},
    {"Factory reset finished with errors.", "Resetarea s-a terminat cu erori."}, {"Format Data", "Formatează Data"},
    {"Erase everything incl. internal storage & encryption", "Șterge tot, inclusiv memoria internă și criptarea"},
    {"Wipe ART / Dalvik cache", "Șterge cache-ul ART / Dalvik"}, {"Fixes boot loops after updates", "Rezolvă bootloop-uri după actualizări"},
    {"Wipe ART cache?", "Ștergi cache-ul ART?"}, {"Apps will be re-optimised on the next boot.", "Aplicațiile vor fi reoptimizate la următoarea pornire."},
    {"Swipe to wipe", "Glisează pentru ștergere"}, {"Wiping ART cache", "Se șterge cache-ul ART"}, {"Advanced wipe", "Ștergere avansată"},
    {"Wipe selected?", "Ștergi selecția?"}, {"This erases EVERYTHING", "Asta șterge TOT"}, {"Type yes", "Scrie yes"},
    {"Format Data now", "Formatează Data acum"}, {"Format failed.", "Formatarea a eșuat."}, {"not mounted", "nemontat"},
    {"Apps, settings, internal storage (photos, music, backups) and file-based encryption are removed.",
     "Se șterg aplicațiile, setările, memoria internă (poze, muzică, backup-uri) și criptarea."},

    // backup / restore
    {"Backup name", "Numele backup-ului"}, {"Compression", "Compresie"}, {"gzip - smaller backups, slightly slower", "gzip - backup-uri mai mici, puțin mai lent"},
    {"SHA-256 digests", "Sume SHA-256"}, {"Verify integrity before restoring", "Verifică integritatea înainte de restaurare"},
    {"Select at least one partition", "Alege cel puțin o partiție"}, {"Swipe to back up", "Glisează pentru backup"},
    {"Restore these", "Restaurează acestea"}, {"Verify digests first", "Verifică întâi sumele"}, {"Abort if the backup is corrupt", "Oprește dacă backup-ul e corupt"},
    {"Verify only", "Doar verifică"}, {"Delete backup?", "Ștergi backup-ul?"}, {"Swipe to delete", "Glisează pentru ștergere"},
    {"Backup deleted", "Backup șters"}, {"Nothing selected", "Nimic selectat"}, {"Swipe to restore", "Glisează pentru restaurare"},
    {"No backups yet", "Încă nu există backup-uri"}, {"{} backup(s) found", "{} backup-uri găsite"}, {"Backups", "Backup-uri"},

    // mount
    {"Tap to mount / unmount, hold for file system tools", "Atinge pentru montare / demontare, ține apăsat pentru unelte"},
    {"Filesystems", "Sisteme de fișiere"}, {"Mount system as read/write", "Montează system ca citire/scriere"},
    {"Off keeps dm-verity happy (recommended)", "Oprit păstrează dm-verity mulțumit (recomandat)"}, {"Mount data", "Montează data"},
    {"Unmount all", "Demontează tot"}, {"All partitions unmounted", "Toate partițiile demontate"}, {"Rescan USB / SD", "Caută din nou USB / SD"},
    {"No USB or SD card detected", "Niciun USB sau card SD detectat"}, {"FS tools", "Unelte FS"}, {"Refreshed", "Actualizat"},

    // reboot
    {"Choose where to go next", "Alege unde pornești"}, {"System", "Sistem"}, {"Boot Android", "Pornește Android"},
    {"Recovery", "Recovery"}, {"Restart FMRRE", "Repornește FMRRE"}, {"Bootloader", "Bootloader"}, {"Fastboot mode", "Mod fastboot"},
    {"Fastbootd", "Fastbootd"}, {"Userspace fastboot", "Fastboot în userspace"}, {"Download", "Download"},
    {"Samsung Odin mode", "Mod Odin Samsung"}, {"EDL", "EDL"}, {"Qualcomm emergency mode", "Mod de urgență Qualcomm"},
    {"Power off", "Oprește"}, {"Shut the device down", "Închide telefonul"}, {"Reboot to {*}", "Repornire în {*}"},
    {"Unmounts partitions and restarts now.", "Demontează partițiile și repornește acum."}, {"Swipe to power off", "Glisează pentru oprire"},
    {"See you on the other side", "Ne vedem de partea cealaltă"}, {"Tap to switch to the other slot", "Atinge ca să treci pe celălalt slot"},
    {"Active slot: {*}", "Slot activ: {*}"}, {"Swipe to switch", "Glisează pentru schimbare"},

    // advanced
    {"Tools for power users", "Unelte pentru utilizatori avansați"}, {"Partitions & boot", "Partiții și boot"}, {"Diagnostics", "Diagnoză"},
    {"Fastboot mode (fastbootd)", "Mod fastboot (fastbootd)"}, {"fastboot flash from a computer, super resized for you", "fastboot flash de pe calculator, super redimensionat automat"},
    {"Install FMRRE permanently", "Instalează FMRRE permanent"}, {"Keep it after fastboot boot - stock is saved first", "Păstrează-l după fastboot boot - originalul se salvează întâi"},
    {"Magisk / KernelSU / APatch, stock backup, modules", "Magisk / KernelSU / APatch, backup original, module"},
    {"Resize / delete dynamic partitions (system, product...)", "Redimensionează / șterge partiții dinamice (system, product...)"},
    {"Flash a Generic System Image the right way", "Instalează corect un GSI"}, {"Verified boot (vbmeta)", "Pornire verificată (vbmeta)"},
    {"Disable / enable dm-verity and AVB", "Dezactivează / activează dm-verity și AVB"}, {"File system tools", "Unelte pentru sisteme de fișiere"},
    {"Repair, resize, change file system", "Repară, redimensionează, schimbă sistemul de fișiere"}, {"Find & flash a file", "Găsește și instalează un fișier"},
    {"Manual path, diagnostics, dd command", "Cale manuală, diagnoză, comandă dd"}, {"Bootloop rescue", "Salvare din bootloop"},
    {"Why the last boot failed, with one-tap fixes", "De ce a eșuat ultima pornire, cu reparații dintr-o atingere"},
    {"System monitor", "Monitor de sistem"}, {"Live CPU, memory, battery & thermals", "CPU, memorie, baterie și temperaturi în timp real"},
    {"Partition info", "Informații partiții"}, {"Block devices, sizes and flags", "Dispozitive bloc, mărimi și atribute"},
    {"Input & touch test", "Test taste și touch"}, {"Check keys and touch calibration", "Verifică tastele și calibrarea touch-ului"},
    {"File manager", "Manager de fișiere"}, {"Browse, copy, move, chmod", "Răsfoiește, copiază, mută, chmod"},
    {"Run shell commands as root", "Rulează comenzi shell ca root"}, {"Copy log to storage", "Copiază jurnalul în memorie"},
    {"Save recovery.log for bug reports", "Salvează recovery.log pentru raportări"}, {"Take screenshot", "Fă o captură de ecran"},
    {"Saved to Pictures/Screenshots", "Salvată în Pictures/Screenshots"}, {"Fix storage contexts", "Repară contextele memoriei"},
    {"restorecon on internal storage", "restorecon pe memoria internă"}, {"Restart ADB", "Repornește ADB"}, {"Restart the adbd service", "Repornește serviciul adbd"},
    {"Reload partitions", "Reîncarcă partițiile"}, {"Re-read fstab and block devices", "Recitește fstab și dispozitivele bloc"},
    {"Switch boot slot", "Schimbă slotul de boot"}, {"Make the other A/B slot active", "Activează celălalt slot A/B"},

    // fastbootd
    {"fastbootd built into FMRRE", "fastbootd inclus în FMRRE"}, {"Waiting for a computer", "Se așteaptă un calculator"},
    {"Fastboot mode stopped", "Modul fastboot s-a oprit"}, {"Flash partitions from a computer with fastboot", "Instalează partiții de pe calculator cu fastboot"},
    {"Plug in the USB cable, then run: fastboot devices", "Conectează cablul USB, apoi rulează: fastboot devices"},
    {"On the computer", "Pe calculator"}, {"Exit fastboot mode", "Ieși din modul fastboot"}, {"Start fastboot mode", "Pornește modul fastboot"},
    {"Start again", "Pornește din nou"}, {"Leaving fastboot mode", "Se iese din modul fastboot"}, {"Over the network (TCP 5554)", "Prin rețea (TCP 5554)"},
    {"For emulators and USB networking: fastboot -s tcp:<ip>", "Pentru emulatoare și rețea prin USB: fastboot -s tcp:<ip>"},

    // rescue
    {"Why the last boot failed, and what to do", "De ce a eșuat ultima pornire și ce poți face"}, {"Last boot", "Ultima pornire"},
    {"Boot reason", "Motivul pornirii"}, {"Kernel log", "Jurnal kernel"}, {"What went wrong", "Ce nu a mers"},
    {"Nothing suspicious found", "Nimic suspect"}, {"Previous boot's kernel log", "Jurnalul kernel al pornirii anterioare"},
    {"Save logs for a bug report", "Salvează jurnalele pentru o raportare"}, {"Check again", "Verifică din nou"}, {"Re-read the logs", "Recitește jurnalele"},
    {"Checked again", "Verificat din nou"}, {"not reported", "neraportat"}, {"none kept", "niciunul păstrat"},
    {"Not available on this phone", "Indisponibil pe acest telefon"}, {"Disable all modules", "Dezactivează toate modulele"},
    {"Disable verified boot", "Dezactivează pornirea verificată"}, {"Boot the other slot", "Pornește celălalt slot"},
    {"Flash a system image", "Instalează o imagine de sistem"}, {"Restore stock boot", "Restaurează boot-ul original"},
    {"Disable all modules?", "Dezactivezi toate modulele?"}, {"Swipe to disable", "Glisează pentru dezactivare"},
    {"Boot the other slot?", "Pornești celălalt slot?"}, {"The other A/B slot becomes active for the next boot.", "Celălalt slot A/B devine activ la următoarea pornire."},
    {"Verified boot rejected a partition", "Pornirea verificată a respins o partiție"},
    {"The system partition didn't mount", "Partiția system nu s-a montat"}, {"The SELinux policy didn't load", "Politica SELinux nu s-a încărcat"},
    {"Android couldn't decrypt or mount /data", "Android nu a putut decripta sau monta /data"}, {"The kernel crashed (panic)", "Kernelul s-a blocat (panic)"},
    {"The kernel hit a fault (oops)", "Kernelul a dat o eroare (oops)"}, {"Android gave up after repeated crashes (Rescue Party)", "Android a renunțat după blocări repetate (Rescue Party)"},
    {"The Android framework keeps crashing (zygote / system_server)", "Sistemul Android se tot blochează (zygote / system_server)"},
    {"init hit a fatal error", "init a dat o eroare fatală"}, {"The system froze (watchdog reset)", "Sistemul a înghețat (resetare watchdog)"},
    {"Magisk / KernelSU modules are enabled", "Modulele Magisk / KernelSU sunt active"}, {"No log from the previous boot", "Niciun jurnal de la pornirea anterioară"},

    {"The system (or vendor) image doesn't match what vbmeta expects - normal after flashing a GSI or a modified image. Disable verification in vbmeta, or flash the vbmeta that belongs to the image.",
     "Imaginea system (sau vendor) nu se potrivește cu ce așteaptă vbmeta - normal după instalarea unui GSI sau a unei imagini modificate. Dezactivează verificarea în vbmeta sau instalează vbmeta-ul care aparține imaginii."},
    {"System and vendor don't fit together (for example a GSI that is too new or too old for this vendor). Try another GSI build, or go back to the stock system.",
     "System și vendor nu se potrivesc (de exemplu un GSI prea nou sau prea vechi pentru acest vendor). Încearcă alt build de GSI sau revino la sistemul original."},
    {"Usually an image for the wrong CPU type (arm64 vs 32-bit 'a64'), a non-A/B image on an A/B phone, or a damaged download. Flash the right image - for GSIs, arm64-ab (vndklite if vendor is read-only).",
     "De obicei o imagine pentru alt tip de procesor (arm64 sau „a64” pe 32 de biți), o imagine non-A/B pe un telefon A/B sau o descărcare coruptă. Instalează imaginea potrivită - pentru GSI-uri, arm64-ab (vndklite dacă vendor e doar pentru citire)."},
    {"Typical after switching ROMs or GSIs: the old encryption keys don't fit any more. Format Data fixes it but erases internal storage - copy your files to a computer or SD card first.",
     "Tipic după schimbarea ROM-ului sau a GSI-ului: vechile chei de criptare nu se mai potrivesc. Formatarea Data rezolvă, dar șterge memoria internă - copiază-ți întâi fișierele pe calculator sau pe un card SD."},
    {"See the line below. If this began after flashing a kernel, a boot image or a Magisk/KernelSU patch, restore the stock boot image.",
     "Vezi linia de mai jos. Dacă a început după instalarea unui kernel, a unei imagini boot sau a unui patch Magisk/KernelSU, restaurează imaginea boot originală."},
    {"A driver or kernel bug. If it started after flashing a kernel or boot image, restore the stock one.",
     "O eroare de driver sau de kernel. Dacă a apărut după instalarea unui kernel sau a unei imagini boot, restaureaz-o pe cea originală."},
    {"An app or the framework crashed over and over. Disable Magisk modules first; if that doesn't help, Format Data.",
     "O aplicație sau sistemul s-a blocat iar și iar. Dezactivează întâi modulele Magisk; dacă nu ajută, formatează Data."},
    {"Common with leftover data from another ROM (Format Data), a GSI that doesn't suit this vendor, or a broken Magisk / Xposed module (disable modules).",
     "Frecvent din cauza datelor rămase de la alt ROM (formatează Data), a unui GSI nepotrivit pentru acest vendor sau a unui modul Magisk / Xposed stricat (dezactivează modulele)."},
    {"Android's first process stopped the boot. The line below names the cause; often the system image or its vendor doesn't fit.",
     "Primul proces Android a oprit pornirea. Linia de mai jos arată cauza; adesea imaginea system sau vendor-ul ei nu se potrivesc."},
    {"Usually a kernel or driver hang. Try the other slot, or the stock boot image if you changed the kernel.",
     "De obicei un kernel sau un driver blocat. Încearcă celălalt slot sau imaginea boot originală, dacă ai schimbat kernelul."},
    {"If the bootloop began after adding or updating a module, disable all of them - they can be turned back on in the Magisk app.",
     "Dacă bootloop-ul a început după adăugarea sau actualizarea unui modul, dezactivează-le pe toate - le poți reactiva din aplicația Magisk."},
    {"This phone keeps no kernel log across reboots (no pstore / last_kmsg), or the previous boot was a clean shutdown. Boot once more, then come back here right after the bootloop.",
     "Acest telefon nu păstrează jurnalul kernel între reporniri (fără pstore / last_kmsg) sau pornirea anterioară s-a încheiat normal. Pornește încă o dată, apoi revino aici imediat după bootloop."},
    {"The bootloader recorded a kernel panic as the reason for the last reset. If it started after flashing a kernel or boot image, restore the stock one.",
     "Bootloader-ul a înregistrat un kernel panic drept cauză a ultimei resetări. Dacă a apărut după instalarea unui kernel sau a unei imagini boot, restaureaz-o pe cea originală."},
    {"The last reset came from the watchdog: something hung. Try the other slot.", "Ultima resetare a venit de la watchdog: ceva s-a blocat. Încearcă celălalt slot."},
    {"The previous boot's log shows none of the known failure patterns. Open it below, or save the logs and ask for help with them.",
     "Jurnalul pornirii anterioare nu arată niciuna dintre problemele cunoscute. Deschide-l mai jos sau salvează jurnalele și cere ajutor cu ele."},
    {"Kernel logs, recovery.log and this diagnosis to FMRRE/logs", "Jurnalele kernel, recovery.log și această diagnoză în FMRRE/logs"},
    {"Every Magisk / KernelSU module is disabled (not removed). Turn them back on in the Magisk app.",
     "Toate modulele Magisk / KernelSU sunt dezactivate (nu șterse). Le poți reactiva din aplicația Magisk."},
    {"Disabled {} module(s)", "Dezactivate {} module"}, {"Saved to {*}", "Salvat în {*}"}, {"Could not save the logs", "Jurnalele nu au putut fi salvate"},
    {"Booted slot {}", "Pornit de pe slotul {}"}, {"Not an A/B phone", "Telefonul nu este A/B"},
    {"system_server crashed (Java exception)", "system_server s-a blocat (excepție Java)"},
    {"The core Android process threw the exception below - usually a ROM/GSI that doesn't fit this vendor, a broken Xposed / Magisk module, or leftover data from another ROM.",
     "Procesul principal Android a aruncat excepția de mai jos - de obicei un ROM/GSI nepotrivit pentru acest vendor, un modul Xposed / Magisk stricat sau date rămase de la alt ROM."},
    {"Previous boot's Android log", "Jurnalul Android al pornirii anterioare"}, {"Warnings, errors and crashes (from pstore)", "Avertismente, erori și blocări (din pstore)"},
    {"Encrypt with a password", "Criptează cu o parolă"}, {"AES-256 - the password is needed to restore", "AES-256 - parola e necesară la restaurare"},
    {"Backup password", "Parola backup-ului"}, {"Repeat the password", "Repetă parola"}, {"The passwords don't match", "Parolele nu se potrivesc"},
    {"Use at least 4 characters", "Folosește cel puțin 4 caractere"}, {"At least 4 characters", "Cel puțin 4 caractere"},
    {"Wrong password", "Parolă greșită"}, {"Encrypted - you'll be asked for the password", "Criptat - ți se va cere parola"},
    {"Recent changes", "Modificări recente"},
    // settings
    {"Appearance", "Aspect"}, {"Accent colour", "Culoare de accent"}, {"Background style", "Stilul fundalului"}, {"Animations", "Animații"},
    {"Transitions, ripples and motion", "Tranziții, unde și mișcare"}, {"Display", "Ecran"}, {"Screen timeout", "Stingerea ecranului"},
    {"24-hour clock", "Ceas de 24 de ore"}, {"Fahrenheit", "Fahrenheit"}, {"Temperature unit in the status bar", "Unitatea de temperatură din bara de stare"},
    {"For upside-down panels", "Pentru ecrane montate invers"}, {"Security & feedback", "Securitate și vibrații"}, {"Haptic feedback", "Vibrații la atingere"},
    {"Vibrate on touch", "Vibrează la atingere"}, {"Set PIN lock", "Setează un PIN"}, {"Remove PIN lock", "Elimină PIN-ul"},
    {"Protect this recovery with a PIN", "Protejează acest recovery cu un PIN"}, {"Recovery asks for the PIN on boot", "Recovery cere PIN-ul la pornire"},
    {"Lock when screen turns off", "Blochează la stingerea ecranului"}, {"Needs a PIN", "Necesită un PIN"}, {"Touch calibration", "Calibrare touch"},
    {"Swap X / Y", "Inversează X / Y"}, {"Flip X", "Oglindește X"}, {"Flip Y", "Oglindește Y"}, {"About FMRRE", "Despre FMRRE"},
    {"Version, device & credits", "Versiune, dispozitiv și mulțumiri"}, {"Reset appearance", "Resetează aspectul"}, {"About", "Despre"},
    {"Credits", "Mulțumiri"}, {"Language", "Limbă"}, {"Animation speed  {}", "Viteza animațiilor  {}"}, {"Brightness  {}", "Luminozitate  {}"}, {"New PIN", "PIN nou"}, {"PIN set", "PIN setat"}, {"PIN removed", "PIN eliminat"},
    {"PIN must be 4-12 digits", "PIN-ul trebuie să aibă 4-12 cifre"}, {"Make it yours", "Personalizează-l"},
    {"Midnight", "Miezul nopții"}, {"Light", "Luminos"}, {"Ocean", "Ocean"}, {"Sunset", "Apus"}, {"Rose", "Trandafir"}, {"Violet", "Violet"},
    {"Lime", "Lime"}, {"Amber", "Chihlimbar"}, {"Ice", "Gheață"}, {"Teal", "Turcoaz"},

    // root
    {"Install / remove", "Instalare / eliminare"}, {"Root installer (zip / APK)", "Instalator root (zip / APK)"},
    {"Install root from zip or APK", "Instalează root din zip sau APK"}, {"Flash a patched image", "Instalează o imagine modificată"},
    {"Patched image", "Imagine modificată"}, {"Stock backup", "Backup original"}, {"No stock backup yet", "Încă nu există un backup original"},
    {"Flash a saved stock image back", "Reinstalează o imagine originală salvată"}, {"Modules ({})", "Module ({})"},
    {"No modules installed", "Niciun modul instalat"}, {"Bootloop? Disable all modules", "Bootloop? Dezactivează toate modulele"},
    {"Re-enable", "Reactivează"}, {"Disable", "Dezactivează"}, {"Swipe to unroot", "Glisează pentru a elimina root-ul"},
    {"No root manager data in /data/adb", "Nicio aplicație root în /data/adb"}, {"Can't read /data/adb (encrypted)", "/data/adb nu poate fi citit (criptat)"},
    {"Data is encrypted - root files can't be checked", "Data este criptată - fișierele root nu pot fi verificate"},

    // super partitions
    {"Groups", "Grupuri"}, {"Logical partitions  (tap for actions)", "Partiții logice  (atinge pentru acțiuni)"},
    {"Create logical partition", "Creează o partiție logică"}, {"Delete partition", "Șterge partiția"}, {"Back up metadata", "Salvează metadatele"},
    {"Restore metadata backup", "Restaurează metadatele salvate"}, {"Free the other slot?", "Eliberezi celălalt slot?"},
    {"Swipe to free", "Glisează pentru eliberare"}, {"Swipe to shrink", "Glisează pentru micșorare"}, {"Map read-write", "Mapează pentru scriere"},
    {"Resize to fit an image...", "Redimensionează pentru o imagine..."}, {"Custom size (MB)...", "Mărime personalizată (MB)..."},
    {"Shrink to 0 (keep entry)", "Micșorează la 0 (păstrează intrarea)"}, {"No dynamic partitions", "Nicio partiție dinamică"},
    {"Super is busy (OTA pending)", "Super este ocupat (OTA în curs)"}, {"Metadata backup", "Backup metadate"},
    {"Undo resizes/deletes from a saved copy", "Anulează redimensionări/ștergeri dintr-o copie salvată"},
    {"Save geometry + all metadata slots", "Salvează geometria și toate sloturile de metadate"},

    // verified boot
    {"Verified boot", "Pornire verificată"}, {"Disable verified boot?", "Dezactivezi pornirea verificată?"},
    {"Verification enabled (stock)", "Verificare activă (original)"}, {"Disable verity & verification", "Dezactivează verity și verificarea"},
    {"Flash a vbmeta.img", "Instalează un vbmeta.img"}, {"Select vbmeta image", "Alege imaginea vbmeta"},
    {"Same as fastboot --disable-verity --disable-verification", "Ca fastboot --disable-verity --disable-verification"},

    // file system tools
    {"Check & repair", "Verifică și repară"}, {"Grow file system to fill partition", "Extinde sistemul de fișiere la toată partiția"},
    {"Change file system...", "Schimbă sistemul de fișiere..."}, {"File system info", "Informații despre sistemul de fișiere"},
    {"Check, repair, resize or change the file system", "Verifică, repară, redimensionează sau schimbă sistemul de fișiere"},
    {"Reformat as ext4, f2fs, exFAT or FAT32 (erases it)", "Reformatează ca ext4, f2fs, exFAT sau FAT32 (o șterge)"},

    // terminal, manual flash
    {"Run now", "Rulează acum"}, {"Open in terminal", "Deschide în terminal"}, {"Save script", "Salvează scriptul"},
    {"File path", "Calea fișierului"}, {"Type the path of your image", "Scrie calea imaginii"}, {"File found", "Fișier găsit"},
    {"Show the dd command instead", "Arată comanda dd în schimb"}, {"Type its path - get a checked dd command", "Scrie calea - primești o comandă dd verificată"},
    {"Command ready below - check it, then press the run button.", "Comanda e mai jos - verific-o, apoi apasă butonul de rulare."},

    // FMRRE install
    {"Install FMRRE", "Instalează FMRRE"}, {"Install FMRRE?", "Instalezi FMRRE?"}, {"Choose FMRRE image", "Alege imaginea FMRRE"},
    {"Make the recovery permanent", "Fă recovery-ul permanent"}, {"Stock backups (FMRRE/stock)", "Backup-uri originale (FMRRE/stock)"},
    {"Swipe to flash with FMRRE", "Glisează pentru a instala cu FMRRE"}, {"From a computer", "De pe un calculator"},
    {"Samsung - Download mode", "Samsung - mod Download"}, {"Installing FMRRE", "Se instalează FMRRE"},

    // monitor, input test, scripts
    {"Thermal zones", "Zone termice"}, {"Core frequencies", "Frecvențele nucleelor"}, {"Voltage / current", "Tensiune / curent"},
    {"Health", "Stare"}, {"Input test", "Test de intrare"}, {"Press keys, draw on the screen", "Apasă taste, desenează pe ecran"},
    {"Press any hardware key...", "Apasă orice tastă fizică..."}, {"Running these commands automatically", "Aceste comenzi rulează automat"},
    {"Script cancelled", "Script anulat"},

    // files
    {"Flash .img inside zip", "Instalează .img din zip"}, {"Cut - open a folder and paste", "Tăiat - deschide un folder și lipește"},
    {"Copied - open a folder and paste", "Copiat - deschide un folder și lipește"}, {"Permissions set to 755", "Permisiuni setate la 755"},
    {"Permissions set to 644", "Permisiuni setate la 644"}, {"Pick any file", "Alege orice fișier"}, {"No matching files here", "Niciun fișier potrivit aici"},
    {"{} other file(s) hidden by the type filter - tap to show all", "{} alte fișiere ascunse de filtru - atinge pentru a le vedea pe toate"},
    {"Sorted by {*}", "Sortat după {*}"}, {"{} item(s) here", "{} elemente aici"}, {"{} selected", "{} selectate"},
    {"{} partition(s)", "{} partiții"}, {"Estimated {*}", "Estimat {*}"}, {"Browse all files", "Toate fișierele"},
    {"Couldn't create folder", "Folderul nu a putut fi creat"}, {"New name", "Nume nou"},

    // battery guard
    {"Battery is low", "Bateria este descărcată"}, {"Swipe to continue anyway", "Glisează ca să continui oricum"},
    {"Battery at {} and not charging. If the phone dies while flashing, it may not boot any more - plug in the charger first.",
     "Baterie la {} și nu se încarcă. Dacă telefonul se oprește în timpul instalării, s-ar putea să nu mai pornească - pune-l întâi la încărcat."},

    // files, dialogs, misc
    {"This folder is empty", "Folderul este gol"}, {"New folder", "Folder nou"}, {"Folder name", "Numele folderului"},
    {"Folder created", "Folder creat"}, {"Delete file?", "Ștergi fișierul?"}, {"This cannot be undone.", "Nu se poate anula."},
    {"Install zip", "Instalează zip"}, {"SHA-256 checksum", "Sumă de control SHA-256"}, {"dd command for this file", "Comanda dd pentru acest fișier"},
    {"Flash as GSI (system)", "Instalează ca GSI (system)"}, {"Copying files", "Se copiază fișierele"}, {"Moving files", "Se mută fișierele"},
    {"Deleting files", "Se șterg fișierele"}, {"Renamed", "Redenumit"}, {"Rename failed", "Redenumirea a eșuat"}, {"Deleted", "Șters"},
    {"All files", "Toate fișierele"}, {"Copy", "Copiază"}, {"Paste {*}", "Lipește {*}"},
    {"Battery", "Baterie"}, {"Memory", "Memorie"}, {"Temperature", "Temperatură"}, {"Uptime {*}", "Pornit de {*}"},

    // auto repair
    {"Auto repair", "Reparare automată"}, {"Finds out why the phone doesn't start, and fixes it", "Află de ce nu pornește telefonul și îl repară"},
    {"Android couldn't start and asked for a factory reset. Try the fixes below first - nothing is erased unless you choose Format Data.",
     "Android nu a putut porni și a cerut o resetare din fabrică. Încearcă întâi reparațiile de mai jos - nu se șterge nimic dacă nu alegi Formatează Data."},
    {"Here is what was found, and what will be done about it.", "Iată ce s-a găsit și ce se va face."},
    {"If the phone still doesn't start, tell FMRRE below - many phones keep no log of a failed boot.",
     "Dacă telefonul tot nu pornește, spune-i lui FMRRE mai jos - multe telefoane nu păstrează niciun jurnal al unei porniri eșuate."},
    {"Last change:", "Ultima modificare:"}, {"My phone doesn't start (bootloop)", "Telefonul meu nu pornește (bootloop)"},
    {"Plan the usual bootloop fixes even without a log", "Planifică reparațiile obișnuite chiar și fără jurnal"}, {"Checked", "Verificat"},
    {"Nothing to repair", "Nimic de reparat"}, {"Repair plan (safest first)", "Planul de reparare (întâi cele mai sigure)"},
    {"No automatic fix applies. See the advice below, or open Bootloop rescue for the full logs.",
     "Nicio reparare automată nu se aplică. Vezi sfaturile de mai jos sau deschide Salvare din bootloop pentru jurnalele complete."},
    {"Everything checked looks fine. If the phone still doesn't start, switch on \"My phone doesn't start\" above.",
     "Tot ce s-a verificat arată bine. Dacă telefonul tot nu pornește, activează „Telefonul meu nu pornește” de mai sus."},
    {"Safe", "Sigur"}, {"Can be undone", "Se poate anula"}, {"Erases data", "Șterge datele"}, {"Recommended", "Recomandat"},
    {"Tried already", "Încercat deja"}, {"Start Android right after the repair", "Pornește Android imediat după reparare"},
    {"Details and logs", "Detalii și jurnale"}, {"Bootloop rescue: the previous boot's logs, explained", "Salvare din bootloop: jurnalele pornirii anterioare, explicate"},
    {"Just try starting Android again", "Încearcă doar să pornești Android din nou"}, {"Repairing automatically in {} s", "Reparare automată în {} s"},
    {"Repair now", "Repară acum"}, {"Select fixes above", "Alege reparațiile de mai sus"},
    {"Swipe to repair and ERASE data", "Glisează pentru a repara și a ȘTERGE datele"}, {"Swipe to repair", "Glisează pentru a repara"},
    {"Swipe to run {} fixes", "Glisează pentru a rula {} reparații"}, {"The last boot failed", "Ultima pornire a eșuat"},
    {"Android reported {*}", "Android a raportat {*}"},
    {"no log was kept, so the usual bootloop fixes are planned", "nu s-a păstrat niciun jurnal, așa că sunt planificate reparațiile obișnuite"},
    {"Found {} problem(s)", "S-au găsit {} probleme"},
    {"No failed boot recorded, and everything checked looks fine", "Nicio pornire eșuată înregistrată și tot ce s-a verificat arată bine"},
    {"Boot image ({})", "Imagine de boot ({})"}, {"Verified boot ({})", "Pornire verificată ({})"},
    {"Dynamic partitions (super)", "Partiții dinamice (super)"}, {"System (slot {})", "Sistem (slotul {})"}, {"Other slot ({})", "Celălalt slot ({})"},
    {"System update", "Actualizare de sistem"}, {"Storage space (/data)", "Spațiu de stocare (/data)"},
    {"Magisk / KernelSU modules", "Module Magisk / KernelSU"}, {"damaged - {*}", "deteriorat - {*}"},
    {"verification off", "verificare oprită"}, {"verification on", "verificare pornită"}, {"OK, {} partitions", "OK, {} partiții"},
    {"unreadable - {*}", "ilizibil - {*}"}, {"missing - this slot has no system", "lipsește - acest slot nu are sistem"}, {"present", "prezent"},
    {"complete - boot and system present", "complet - boot și sistem prezente"}, {"incomplete", "incomplet"},
    {"{*} free", "{*} libere"}, {"{*} free - too full to boot", "{*} libere - prea plin ca să pornească"},
    {"encrypted - not readable here", "criptat - nu poate fi citit aici"}, {"{} enabled", "{} activate"}, {"{}, charging", "{}, se încarcă"},
    {"Disable all Magisk / KernelSU modules", "Dezactivează toate modulele Magisk / KernelSU"},
    {"A module is the most common cause of a bootloop on a rooted phone. They are only disabled: turn them back on in the Magisk app.",
     "Un modul este cea mai frecventă cauză a unui bootloop pe un telefon cu root. Sunt doar dezactivate: le poți reactiva din aplicația Magisk."},
    {"Clear the app caches (ART / Dalvik and /cache)", "Golește cache-ul aplicațiilor (ART / Dalvik și /cache)"},
    {"Frees space on a full /data. Android rebuilds the caches on the next boot (the first boot takes longer).",
     "Eliberează spațiu pe un /data plin. Android reface cache-ul la următoarea pornire (prima pornire durează mai mult)."},
    {"Stale compiled app code after an update can crash Android at boot. It is rebuilt automatically; no apps or files are lost.",
     "Codul compilat vechi al aplicațiilor poate bloca Android la pornire după o actualizare. Se reface automat; nu se pierd aplicații sau fișiere."},
    {"Disable verified boot (vbmeta)", "Dezactivează pornirea verificată (vbmeta)"},
    {"Verified boot stopped the last boot: the system image is modified or not the one vbmeta expects (normal after a GSI).",
     "Pornirea verificată a oprit ultima pornire: imaginea de sistem este modificată sau nu e cea așteptată de vbmeta (normal după un GSI)."},
    {"Restore the saved vbmeta header", "Restaurează antetul vbmeta salvat"},
    {"vbmeta is damaged; FMRRE saved its header before changing it.", "vbmeta este deteriorat; FMRRE i-a salvat antetul înainte de a-l modifica."},
    {"Restore the last dynamic partition table", "Restaurează ultimul tabel de partiții dinamice"},
    {"The partition table in super is unreadable; FMRRE saved a copy.", "Tabelul de partiții din super nu poate fi citit; FMRRE a salvat o copie."},
    {"Restore the original {}", "Restaurează {} original"},
    {"The {} image is damaged; FMRRE saved the original.", "Imaginea {} este deteriorată; FMRRE a salvat originalul."},
    {"The phone stopped starting after {} was changed; this puts the original back.",
     "Telefonul nu mai pornește de când s-a modificat {}; asta pune originalul la loc."},
    {"The kernel crashed; if a kernel, boot image or root patch caused it, the original image undoes it.",
     "Kernelul s-a blocat; dacă un kernel, o imagine de boot sau un patch de root a cauzat asta, imaginea originală anulează schimbarea."},
    {"This also removes FMRRE from {}.", "Asta elimină și FMRRE din {}."}, {"Start the other slot ({})", "Pornește celălalt slot ({})"},
    {"This slot has no system; the other one is complete.", "Acest slot nu are sistem; celălalt este complet."},
    {"A new system was installed to this slot and it doesn't start; the other slot still has the previous one.",
     "În acest slot s-a instalat un sistem nou care nu pornește; celălalt slot îl are încă pe cel anterior."},
    {"The other slot still has a complete system - usually the version that worked before.",
     "Celălalt slot are încă un sistem complet - de obicei versiunea care mergea înainte."},
    {"Check and repair the /data file system", "Verifică și repară sistemul de fișiere /data"},
    {"Android couldn't mount or unlock /data. A file system check fixes damage from a crash or a full disk without erasing anything.",
     "Android nu a putut monta sau debloca /data. O verificare a sistemului de fișiere repară deteriorările de la o blocare sau de la un disc plin, fără să șteargă nimic."},
    {"Format Data (factory reset)", "Formatează Data (resetare din fabrică)"},
    {"Fixes encryption errors and leftovers from another ROM - the last resort. Erases apps, settings and internal storage: copy your files to a computer or SD card first.",
     "Repară erorile de criptare și resturile altui ROM - ultima soluție. Șterge aplicațiile, setările și memoria internă: copiază-ți întâi fișierele pe un calculator sau pe un card SD."},
    {"The system image doesn't fit this phone: flash the ROM or GSI again (for GSIs, the arm64 A/B build), or go back to the stock firmware.",
     "Imaginea de sistem nu se potrivește cu acest telefon: instalează din nou ROM-ul sau GSI-ul (pentru GSI, varianta arm64 A/B) sau revino la firmware-ul original."},
    {"The safe fixes have been tried. What's left erases data: Format Data (copy your files first), or flash the stock firmware - from a computer with the FMRRE PC rescue tool if needed.",
     "Reparațiile sigure au fost încercate. Ce a rămas șterge datele: Formatează Data (copiază-ți întâi fișierele) sau instalează firmware-ul original - de pe un calculator, cu unealta FMRRE PC rescue, dacă e nevoie."},
    {"No safe automatic fix applies. Format Data (copy your files first), or flash the ROM / stock firmware again - from a computer with the FMRRE PC rescue tool if needed.",
     "Nicio reparare automată sigură nu se aplică. Formatează Data (copiază-ți întâi fișierele) sau instalează din nou ROM-ul / firmware-ul original - de pe un calculator, cu unealta FMRRE PC rescue, dacă e nevoie."},
    {"Last boot failed \xE2\x80\xA2 tap here for auto repair", "Ultima pornire a eșuat \xE2\x80\xA2 atinge aici pentru reparare automată"},
    {"Auto repair - find and fix it for me", "Reparare automată - găsește și repară în locul meu"}, {"Android said", "Android a spus"},
    {"Doesn't start? Finds the problem and fixes it", "Nu pornește? Găsește problema și o repară"},
    {"Repair automatically after a failed boot", "Repară automat după o pornire eșuată"},
    {"Runs the safe fixes after a 15 s countdown, then reboots. Never erases data", "Rulează reparațiile sigure după 15 s, apoi repornește. Nu șterge niciodată datele"},
    {"Android sent the phone to recovery", "Android a trimis telefonul în recovery"},
    {"Locked", "Blocat"}, {"unlocked", "deblocat"}, {"Replaces partitions", "Înlocuiește partiții"},
    {"MTP - files on a computer", "MTP - fișiere pe calculator"},
    {"Copy files to and from a computer", "Copiază fișiere de pe și pe calculator"}, {"Connected to {*}", "Conectat la {*}"},
    {"On - waiting for a computer ({*})", "Pornit - se așteaptă un calculator ({*})"},
    {"MTP is on", "MTP este pornit"}, {"MTP could not start", "MTP nu a putut porni"}, {"MTP is off", "MTP este oprit"},
    {"Restore the system from the backup {*}", "Restaurează sistemul din backup-ul {*}"}, {"Puts back: {*}", "Pune la loc: {*}"},
    {"Returns the system to how it was when the backup was made - for when the safe fixes didn't help. Apps and files on /data are kept.",
     "Readuce sistemul cum era când s-a făcut backup-ul - pentru când reparațiile sigure nu au ajutat. Aplicațiile și fișierele din /data rămân."},
    {"locked - modified images are refused", "blocat - imaginile modificate sunt refuzate"},
    {"Verified boot stopped the boot, but the bootloader is locked: it only starts the official images. Flash the stock firmware, or unlock the bootloader (that erases the phone).",
     "Pornirea verificată a oprit pornirea, dar bootloader-ul este blocat: pornește doar imaginile oficiale. Instalează firmware-ul original sau deblochează bootloader-ul (asta șterge telefonul)."},
    {"Android stopped the boot itself and asked for recovery, with the reason below. Auto repair tries the safe fixes first.",
     "Android a oprit singur pornirea și a cerut recovery, cu motivul de mai jos. Reparare automată încearcă întâi reparațiile sigure."},

    // zip signatures
    {"Signature", "Semnătură"}, {"Checking the signature...", "Se verifică semnătura..."}, {"Not signed", "Nesemnat"},
    {"The signature doesn't match the file", "Semnătura nu se potrivește cu fișierul"},
    {"The signature couldn't be checked", "Semnătura nu a putut fi verificată"},
    {"Signed with the installed system's key", "Semnat cu cheia sistemului instalat"},
    {"Signed with a key this recovery trusts", "Semnat cu o cheie în care acest recovery are încredere"},
    {"Signed with a key you trusted", "Semnat cu o cheie în care ai încredere"},
    {"Signed with the public AOSP test key", "Semnat cu cheia publică de test AOSP"},
    {"Signed with a key FMRRE doesn't know", "Semnat cu o cheie pe care FMRRE nu o cunoaște"},
    {"Unsigned", "Nesemnat"}, {"BAD", "INVALID"}, {"\xE2\x9C\x93 your ROM", "\xE2\x9C\x93 ROM-ul tău"},
    {"\xE2\x9C\x93 trusted", "\xE2\x9C\x93 de încredere"}, {"Test key", "Cheie de test"}, {"Other key", "Altă cheie"},
    {"Signed by", "Semnat de"}, {"Organisation", "Organizație"}, {"Key", "Cheie"}, {"Key ID", "ID cheie"},
    {"Trusted by", "De încredere prin"}, {"Found", "Problema"}, {"Trust this key", "Am încredere în această cheie"},
    {"Key saved to FMRRE/keys", "Cheie salvată în FMRRE/keys"},
    {"Its zips then count as trusted, also for \"only trusted zips\"",
     "Zip-urile semnate cu ea devin de încredere, și pentru „doar zip-uri de încredere”"},
    {"Most custom ROMs, GApps and Magisk zips aren't signed this way - that's normal. Official OTAs and many ROM releases are.",
     "Cele mai multe ROM-uri custom, GApps și zip-uri Magisk nu sunt semnate așa - e normal. OTA-urile oficiale și multe ROM-uri sunt."},
    {"The zip was changed after it was signed, or it is damaged. Download it again; a modified ROM has to be signed again by whoever changed it.",
     "Zip-ul a fost modificat după ce a fost semnat sau este deteriorat. Descarcă-l din nou; un ROM modificat trebuie semnat din nou de cine l-a modificat."},
    {"The file couldn't be read to the end.", "Fișierul nu a putut fi citit până la capăt."},
    {"The system installed now takes updates signed with this key: this is an update of your ROM. Your data can stay.",
     "Sistemul instalat acum acceptă actualizări semnate cu această cheie: este o actualizare a ROM-ului tău. Datele pot rămâne."},
    {"This recovery trusts this key - the phone maker's or the ROM's.",
     "Acest recovery are încredere în această cheie - a producătorului telefonului sau a ROM-ului."},
    {"You added this key to FMRRE/keys yourself.", "Ai adăugat singur această cheie în FMRRE/keys."},
    {"Anyone can sign with the AOSP test key: it shows an unofficial or test build, not who made it.",
     "Oricine poate semna cu cheia de test AOSP: arată un build neoficial sau de test, nu cine l-a făcut."},
    {"The installed system's keys couldn't be read, so FMRRE can't tell whether this is an update of your ROM.",
     "Cheile sistemului instalat nu au putut fi citite, deci FMRRE nu poate spune dacă este o actualizare a ROM-ului tău."},
    {"The system installed now takes updates signed with another key: this is another ROM, or another build of it signed by someone else. Switching ROMs? Wipe data after flashing, or Android usually stops at the boot animation.",
     "Sistemul instalat acum acceptă actualizări semnate cu altă cheie: este alt ROM sau alt build al lui, semnat de altcineva. Schimbi ROM-ul? Șterge datele după instalare, altfel Android rămâne de obicei la animația de pornire."},
    {"Not the installed system's key - normal for add-ons (GApps, kernels, Magisk), which their makers sign.",
     "Nu este cheia sistemului instalat - normal pentru suplimente (GApps, kerneluri, Magisk), pe care le semnează autorii lor."},
    {"there is no signature at the end of the zip", "nu există semnătură la sfârșitul zip-ului"},
    {"the signature footer is damaged", "finalul semnăturii este deteriorat"},
    {"the file is shorter than its signature says", "fișierul este mai scurt decât spune semnătura"},
    {"the end of the zip doesn't match its signature", "sfârșitul zip-ului nu se potrivește cu semnătura"},
    {"something was added after the signed end of the zip", "ceva a fost adăugat după sfârșitul semnat al zip-ului"},
    {"the signature block can't be read: {*}", "blocul semnăturii nu poate fi citit: {*}"},
    {"it is signed with an unsupported hash", "este semnat cu un hash nesuportat"},
    {"it was changed after it was signed, or it is damaged", "a fost modificat după semnare sau este deteriorat"},
    {"the file can't be opened", "fișierul nu poate fi deschis"}, {"the file can't be read", "fișierul nu poate fi citit"},
    {"the installed system has no otacerts.zip", "sistemul instalat nu are otacerts.zip"},
    {"the system partition couldn't be mounted", "partiția system nu a putut fi montată"},
    {"there is no system partition", "nu există partiție system"},
    {"Another ROM than the one installed", "Alt ROM decât cel instalat"},
    {"Signed with another key than the system on the phone now. Wipe data after flashing it, or Android usually stops at the boot animation.",
     "Semnat cu altă cheie decât sistemul de acum de pe telefon. Șterge datele după ce îl instalezi, altfel Android rămâne de obicei la animația de pornire."},
    {"Data is encrypted: after a ROM switch a factory reset is often not enough - Format Data erases internal storage too (copy your files off first, e.g. over MTP).",
     "Datele sunt criptate: după o schimbare de ROM resetarea din fabrică adesea nu ajunge - Format Data șterge și memoria internă (copiază-ți întâi fișierele, de exemplu prin MTP)."},
    {"Format Data...", "Formatează Data..."}, {"Factory reset after flashing", "Resetare din fabrică după instalare"},
    {"Factory reset after installing", "Resetare din fabrică după instalare"},
    {"Apps and settings go; internal storage stays", "Aplicațiile și setările se șterg; memoria internă rămâne"},
    {"Check signatures", "Verifică semnăturile"},
    {"Who signed it; spots a ROM switch", "Cine l-a semnat; vede schimbarea ROM-ului"},
    {"Another ROM than the one installed (another signing key): wipe data after installing it, or Android usually stops at the boot animation.",
     "Alt ROM decât cel instalat (altă cheie de semnare): șterge datele după instalare, altfel Android rămâne de obicei la animația de pornire."},
    {"Another ROM than the one installed (another signing key). Data is encrypted: after a ROM switch only Format Data reliably helps - it erases internal storage too (Wipe).",
     "Alt ROM decât cel instalat (altă cheie de semnare). Datele sunt criptate: după o schimbare de ROM doar Format Data ajută sigur - șterge și memoria internă (Ștergere)."},
    {"A different ROM was installed without wiping data", "Un alt ROM a fost instalat fără ștergerea datelor"},
    {"The last ROM is signed with another key than the system it replaced, and data wasn't wiped since. Android usually can't start on another ROM's apps and settings: a factory reset fixes it (internal storage stays).",
     "Ultimul ROM e semnat cu altă cheie decât sistemul pe care l-a înlocuit, iar datele nu au fost șterse de atunci. Android de obicei nu pornește pe aplicațiile și setările altui ROM: o resetare din fabrică rezolvă (memoria internă rămâne)."},
    {"The last ROM is signed with another key than the system it replaced, and data wasn't wiped since. With encrypted data only Format Data reliably helps - it erases internal storage too.",
     "Ultimul ROM e semnat cu altă cheie decât sistemul pe care l-a înlocuit, iar datele nu au fost șterse de atunci. Cu date criptate doar Format Data ajută sigur - șterge și memoria internă."},
    {"Factory reset (keeps internal storage)", "Resetare din fabrică (păstrează memoria internă)"},
    {"Another ROM was installed without wiping data, and Android can't start on the old ROM's apps and settings. Erases apps and settings; photos, downloads and other files in internal storage stay.",
     "Un alt ROM a fost instalat fără ștergerea datelor, iar Android nu poate porni pe aplicațiile și setările ROM-ului vechi. Șterge aplicațiile și setările; pozele, descărcările și celelalte fișiere din memoria internă rămân."},
    {"Last ROM install", "Ultima instalare de ROM"}, {"another ROM - data not wiped since", "alt ROM - datele nu au fost șterse de atunci"},
    {"Run adb sideload <file.zip> on your computer: the package is read over USB while it installs - no free space needed - checked (CRC, signature) and flashed here, and adb shows the result. FMRRE speaks adb itself - no minadbd needed.",
     "Rulează adb sideload <fișier.zip> pe calculator: pachetul este citit prin USB în timp ce se instalează - nu e nevoie de spațiu liber -, verificat (CRC, semnătură) și instalat aici, iar adb arată rezultatul. FMRRE vorbește singur adb - nu e nevoie de minadbd."},
    {"Decompress to {*}", "Decomprimă în {*}"}, {"Extract {*}", "Extrage {*}"}, {"Unpacking {*}", "Se despachetează {*}"},
    {"Flash history", "Istoric de instalări"}, {"Every flash, wipe, restore and repair, with the time", "Fiecare instalare, ștergere, restaurare și reparație, cu ora"},
    {"What was flashed, wiped, restored and repaired", "Ce s-a instalat, șters, restaurat și reparat"}, {"Nothing yet", "Nimic încă"},
    {"Every zip, image, OTA, wipe, restore and repair is noted here.", "Fiecare zip, imagine, OTA, ștergere, restaurare și reparație e notată aici."},
    {"Payload signature", "Semnătura payload-ului"}, {"Its signature is damaged", "Semnătura lui este deteriorată"},
    {"Signed with the key that signed the zip", "Semnat cu cheia care a semnat zip-ul"},
    {"Only install zips signed with a trusted key", "Instalează doar zip-uri semnate cu o cheie de încredere"},
    {"The installed system's keys, this recovery's and FMRRE/keys - like a stock recovery",
     "Cheile sistemului instalat, ale acestui recovery și din FMRRE/keys - ca un recovery original"},
};

bool isPattern(const std::string& s) { return s.find("{}") != std::string::npos || s.find("{*}") != std::string::npos; }

std::unordered_map<std::string, std::string>* buildExact(const Entry* e, size_t n) {
    auto* m = new std::unordered_map<std::string, std::string>();
    for (size_t i = 0; i < n; i++)
        if (!isPattern(e[i].en)) (*m)[e[i].en] = e[i].tr;
    return m;
}

// "{}" stands for one word (no spaces: a number, a slot, a partition); "{*}" for any text.
struct Pattern { std::string pre, post, trPre, trPost; bool anyText; };
std::vector<Pattern> buildPatterns(const Entry* e, size_t n) {
    std::vector<Pattern> out;
    for (size_t i = 0; i < n; i++) {
        std::string en = e[i].en, tr = e[i].tr;
        bool any = en.find("{*}") != std::string::npos;
        const char* ph = any ? "{*}" : "{}";
        size_t a = en.find(ph), b = tr.find(ph), len = any ? 3 : 2;
        if (a == std::string::npos || b == std::string::npos) continue;
        out.push_back({en.substr(0, a), en.substr(a + len), tr.substr(0, b), tr.substr(b + len), any});
    }
    // Longest fixed text first, so specific patterns win over general ones.
    std::stable_sort(out.begin(), out.end(), [](const Pattern& x, const Pattern& y) {
        return x.pre.size() + x.post.size() > y.pre.size() + y.post.size();
    });
    return out;
}

thread_local int g_raw = 0;

std::string g_lang = "en";
const std::unordered_map<std::string, std::string>* g_exact = nullptr;
const std::vector<Pattern>* g_patterns = nullptr;

}  // namespace

std::vector<Language> languages() { return {{"en", "English"}, {"ro", "Rom\xC3\xA2n\xC4\x83"}}; }

void setLanguage(const std::string& code) {
    static auto* ro = buildExact(kRo, sizeof(kRo) / sizeof(kRo[0]));
    static auto* roP = new std::vector<Pattern>(buildPatterns(kRo, sizeof(kRo) / sizeof(kRo[0])));
    if (code == "ro") {
        g_lang = "ro";
        g_exact = ro;
        g_patterns = roP;
    } else {
        g_lang = "en";
        g_exact = nullptr;
        g_patterns = nullptr;
    }
}

const std::string& language() { return g_lang; }

NoTranslate::NoTranslate() { g_raw++; }
NoTranslate::~NoTranslate() { g_raw--; }

const std::string& tr(const std::string& s) {
    if (!g_exact || g_raw || s.empty() || s.size() > 400) return s;
    auto it = g_exact->find(s);
    if (it != g_exact->end()) return it->second;
    // "{}" patterns. Results live in a small ring: callers use them right away (or copy them).
    static thread_local std::string ring[64];
    static thread_local unsigned next = 0;
    for (auto& p : *g_patterns) {
        if (s.size() <= p.pre.size() + p.post.size()) continue;
        if (s.compare(0, p.pre.size(), p.pre) != 0 || s.compare(s.size() - p.post.size(), p.post.size(), p.post) != 0) continue;
        std::string mid = s.substr(p.pre.size(), s.size() - p.pre.size() - p.post.size());
        if (!p.anyText && mid.find(' ') != std::string::npos) continue;
        std::string& out = ring[next++ % 64];
        out = p.trPre + mid + p.trPost;
        return out;
    }
    return s;
}

std::string upper(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
        else { out += s.substr(i, 1); i++; continue; }  // longer sequences (bullets, arrows) are copied byte by byte
        uint32_t u = cp;
        if (cp >= 'a' && cp <= 'z') u = cp - 32;
        else if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) u = cp - 32;                // à..þ (not ÷)
        else if (cp >= 0x100 && cp <= 0x17F && cp != 0x138 && cp != 0x149 && cp != 0x17F) {
            bool oddUpper = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
            if (oddUpper ? (cp % 2 == 0) : (cp % 2 == 1)) u = cp - 1;
        } else if (cp == 0x219 || cp == 0x21B) u = cp - 1;                           // ș ț
        if (u < 0x80) out += char(u);
        else { out += char(0xC0 | (u >> 6)); out += char(0x80 | (u & 0x3F)); }
        i += len;
    }
    return out;
}

std::string longDate(int weekday, int day, int month) {
    static const char* enD[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char* enM[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    static const char* roD[] = {"Duminică", "Luni", "Marți", "Miercuri", "Joi", "Vineri", "Sâmbătă"};
    static const char* roM[] = {"ianuarie", "februarie", "martie", "aprilie", "mai", "iunie", "iulie", "august", "septembrie", "octombrie", "noiembrie", "decembrie"};
    weekday = ((weekday % 7) + 7) % 7;
    month = ((month % 12) + 12) % 12;
    char d[8];
    snprintf(d, sizeof(d), "%02d", day);
    bool ro = g_lang == "ro";
    return std::string(ro ? roD[weekday] : enD[weekday]) + ", " + d + " " + (ro ? roM[month] : enM[month]);
}

std::vector<std::pair<std::string, std::string>> entries(const std::string& code) {
    std::vector<std::pair<std::string, std::string>> v;
    if (code == "ro")
        for (auto& e : kRo) v.push_back({e.en, e.tr});
    return v;
}

}  // namespace tw::i18n
