#pragma once
#include "gfx/canvas.h"

namespace tw {

// Vector icon set rendered from signed-distance shapes on a 24x24 grid, so it
// stays crisp at any display density. Masks are cached per (icon, size).
enum class Icon {
    None = 0,
    Install, Wipe, Backup, Restore, Mount, Folder, File, Zip, Image, Terminal,
    Advanced, Settings, Power, Reboot, Back, Forward, Home, Console, Check, Close,
    Lock, Battery, Thermo, Cpu, Memory, Sd, Usb, Up, Down, Sort, Plus, Trash,
    Copy, Move, Edit, Info, Warning, Sun, Palette, Timer, Vibrate, Key, Refresh,
    Bolt, Shield, Chart, Swap, Search, Phone, Storage, Hash, Touch, Camera,
    Keyboard, Moon, Wrench, Chip, Download, Eye, Count
};

namespace icons {
void draw(Canvas& c, Icon icon, float x, float y, float size, Color col);
inline void drawCentered(Canvas& c, Icon icon, float cx, float cy, float size, Color col) {
    draw(c, icon, cx - size / 2, cy - size / 2, size, col);
}
}  // namespace icons

}  // namespace tw
