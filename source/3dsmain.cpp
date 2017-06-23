#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include <iostream>
#include <sstream>

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <3ds.h>

#include <dirent.h>
#include "snes9x.h"
#include "memmap.h"
#include "apu.h"
#include "gfx.h"
#include "snapshot.h"
#include "cheats.h"
#include "movie.h"
#include "display.h"
#include "soundux.h"

#include "3dsexit.h"
#include "3dsgpu.h"
#include "3dsopt.h"
#include "3dssound.h"
#include "3dsmenu.h"
#include "3dsui.h"
#include "3dsfont.h"
#include "3dsconfig.h"
#include "3dsfiles.h"
#include "3dsinput.h"
#include "3dssettings.h"
#include "3dsimpl.h"
#include "3dsimpl_tilecache.h"
#include "3dsimpl_gpu.h"

#include "lodepng.h"

inline std::string operator "" s(const char* s, unsigned int length) {
    return std::string(s, length);
}

S9xSettings3DS settings3DS;


#define TICKS_PER_SEC (268123480)
#define TICKS_PER_FRAME_NTSC (4468724)
#define TICKS_PER_FRAME_PAL (5362469)

int frameCount60 = 60;
u64 frameCountTick = 0;
int framesSkippedCount = 0;
char romFileName[_MAX_PATH];
char romFileNameLastSelected[_MAX_PATH];


void LoadDefaultSettings() {
    settings3DS.PaletteFix = 0;
    settings3DS.SRAMSaveInterval = 0;
    settings3DS.ForceSRAMWriteOnPause = 0;

    settings3DS.ButtonMappingsSnes[static_cast<size_t>(SnesButtons::A     )].SetSingleMapping(KEY_A);
    settings3DS.ButtonMappingsSnes[static_cast<size_t>(SnesButtons::B     )].SetSingleMapping(KEY_B);
    settings3DS.ButtonMappingsSnes[static_cast<size_t>(SnesButtons::Y     )].SetSingleMapping(KEY_Y);
    settings3DS.ButtonMappingsSnes[static_cast<size_t>(SnesButtons::X     )].SetSingleMapping(KEY_X);
    settings3DS.ButtonMappingsSnes[static_cast<size_t>(SnesButtons::L     )].SetSingleMapping(KEY_L);
    settings3DS.ButtonMappingsSnes[static_cast<size_t>(SnesButtons::R     )].SetSingleMapping(KEY_R);
    settings3DS.ButtonMappingsSnes[static_cast<size_t>(SnesButtons::Up    )].SetDoubleMapping(KEY_DUP,    KEY_CPAD_UP);
    settings3DS.ButtonMappingsSnes[static_cast<size_t>(SnesButtons::Down  )].SetDoubleMapping(KEY_DDOWN,  KEY_CPAD_DOWN);
    settings3DS.ButtonMappingsSnes[static_cast<size_t>(SnesButtons::Left  )].SetDoubleMapping(KEY_DLEFT,  KEY_CPAD_LEFT);
    settings3DS.ButtonMappingsSnes[static_cast<size_t>(SnesButtons::Right )].SetDoubleMapping(KEY_DRIGHT, KEY_CPAD_RIGHT);
    settings3DS.ButtonMappingsSnes[static_cast<size_t>(SnesButtons::Start )].SetSingleMapping(KEY_START);
    settings3DS.ButtonMappingsSnes[static_cast<size_t>(SnesButtons::Select)].SetSingleMapping(KEY_SELECT);
}


//----------------------------------------------------------------------
// Checks if file exists.
//----------------------------------------------------------------------
bool IsFileExists(const char * filename) {
    if (FILE * file = fopen(filename, "r")) {
        fclose(file);
        return true;
    }
    return false;
}

//-------------------------------------------------------
// Clear top screen with logo.
//-------------------------------------------------------
void clearTopScreenWithLogo()
{
	unsigned char* image;
	unsigned width, height;

    int error = lodepng_decode32_file(&image, &width, &height, "romfs:/snes9x_3ds_top.png");

    if (!error && width == 400 && height == 240)
    {
        // lodepng outputs big endian rgba so we need to convert
        for (int i = 0; i < 2; i++)
        {
            u8* src = image;
            uint32* fb = (uint32 *) gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
            for (int y = 0; y < 240; y++)
                for (int x = 0; x < 400; x++)
                {
                    uint32 r = *src++;
                    uint32 g = *src++;
                    uint32 b = *src++;
                    uint32 a = *src++;

                    uint32 c = ((r << 24) | (g << 16) | (b << 8) | 0xff);
                    fb[x * 240 + (239 - y)] = c;
                }
            gfxSwapBuffers();
        }

        free(image);
    }
}




//----------------------------------------------------------------------
// Menu options
//----------------------------------------------------------------------

namespace {
    template <typename T>
    bool CheckAndUpdate( T& oldValue, const T& newValue, bool& changed ) {
        if ( oldValue != newValue ) {
            oldValue = newValue;
            changed = true;
            return true;
        }
        return false;
    }

    void AddMenuAction(std::vector<SMenuItem>& items, const std::string& text, std::function<void(int)> callback) {
        items.emplace_back(callback, MenuItemType::Action, text, ""s);
    }

    void AddMenuDialogOption(std::vector<SMenuItem>& items, int value, const std::string& text, const std::string& description = ""s) {
        items.emplace_back(nullptr, MenuItemType::Action, text, description, value);
    }

    void AddMenuDisabledOption(std::vector<SMenuItem>& items, const std::string& text) {
        items.emplace_back(nullptr, MenuItemType::Disabled, text, ""s);
    }

    void AddMenuHeader1(std::vector<SMenuItem>& items, const std::string& text) {
        items.emplace_back(nullptr, MenuItemType::Header1, text, ""s);
    }

    void AddMenuHeader2(std::vector<SMenuItem>& items, const std::string& text) {
        items.emplace_back(nullptr, MenuItemType::Header2, text, ""s);
    }

    void AddMenuCheckbox(std::vector<SMenuItem>& items, const std::string& text, int value, std::function<void(int)> callback) {
        items.emplace_back(callback, MenuItemType::Checkbox, text, ""s, value);
    }

    void AddMenuGauge(std::vector<SMenuItem>& items, const std::string& text, int min, int max, int value, std::function<void(int)> callback) {
        items.emplace_back(callback, MenuItemType::Gauge, text, ""s, value, min, max);
    }

    void AddMenuPicker(std::vector<SMenuItem>& items, const std::string& text, const std::string& description, const std::vector<SMenuItem>& options, int value, int backgroundColor, bool showSelectedOptionInMenu, std::function<void(int)> callback) {
        items.emplace_back(callback, MenuItemType::Picker, text, ""s, value, showSelectedOptionInMenu ? 1 : 0, 0, description, options, backgroundColor);
    }
}

void exitEmulatorOptionSelected( int val ) {
    if ( val == 1 ) {
        GPU3DS.emulatorState = EMUSTATE_END;
        appExiting = 1;
    }
}

std::vector<SMenuItem> makeOptionsForNoYes() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, 0, "No"s, ""s);
    AddMenuDialogOption(items, 1, "Yes"s, ""s);
    return items;
}

std::vector<SMenuItem> makeOptionsForOk() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, 0, "OK"s, ""s);
    return items;
}

std::vector<SMenuItem> makeEmulatorMenu(std::vector<SMenuTab>& menuTab, int& currentMenuTab, bool& closeMenu) {
    std::vector<SMenuItem> items;
    AddMenuHeader2(items, "Resume"s);
    items.emplace_back([&closeMenu](int val) {
        closeMenu = true;
    }, MenuItemType::Action, "  Resume Game"s, ""s);
    AddMenuHeader2(items, ""s);

    AddMenuHeader2(items, "Savestates"s);
    for (int slot = 1; slot <= 5; ++slot) {
        std::ostringstream optionText;
        optionText << "  Save Slot #" << slot;
        items.emplace_back([slot, &menuTab, &currentMenuTab](int val) {
            SMenuTab dialogTab;
            bool isDialog = false;
            bool result;

            {
                std::ostringstream oss;
                oss << "Saving into slot #" << slot << "...";
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Savestates", oss.str(), DIALOGCOLOR_CYAN, std::vector<SMenuItem>());
                result = impl3dsSaveStateSlot(slot);
                menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);
            }

            if (!result) {
                std::ostringstream oss;
                oss << "Unable to save into #" << slot << "!";
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Savestate failure", oss.str(), DIALOGCOLOR_RED, makeOptionsForOk());
                menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);
            }
        }, MenuItemType::Action, optionText.str(), ""s);
    }
    AddMenuHeader2(items, ""s);
    
    for (int slot = 1; slot <= 5; ++slot) {
        std::ostringstream optionText;
        optionText << "  Load Slot #" << slot;
        items.emplace_back([slot, &menuTab, &currentMenuTab, &closeMenu](int val) {
            bool result = impl3dsLoadStateSlot(slot);
            if (!result) {
                SMenuTab dialogTab;
                bool isDialog = false;
                std::ostringstream oss;
                oss << "Unable to load slot #" << slot << "!";
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Savestate failure", oss.str(), DIALOGCOLOR_RED, makeOptionsForOk());
                menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);
            } else {
                closeMenu = true;
            }
        }, MenuItemType::Action, optionText.str(), ""s);
    }
    AddMenuHeader2(items, ""s);

    AddMenuHeader2(items, "Others"s);
    items.emplace_back([&menuTab, &currentMenuTab](int val) {
        SMenuTab dialogTab;
        bool isDialog = false;
        menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Screenshot", "Now taking a screenshot...\nThis may take a while.", DIALOGCOLOR_CYAN, std::vector<SMenuItem>());

        char ext[256];
        const char *path = NULL;

        // Loop through and look for an non-existing
        // file name.
        //
        int i = 1;
        while (i <= 999)
        {
            snprintf(ext, 255, ".b%03d.bmp", i);
            path = S9xGetFilename(ext);
            if (!IsFileExists(path))
                break;
            path = NULL;
            i++;
        }

        bool success = false;
        if (path)
        {
            success = menu3dsTakeScreenshot(path);
        }
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);

        if (success)
        {
            char text[600];
            snprintf(text, 600, "Done! File saved to %s", path);
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Screenshot", text, DIALOGCOLOR_GREEN, makeOptionsForOk());
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);
        }
        else 
        {
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Screenshot", "Oops. Unable to take screenshot!", DIALOGCOLOR_RED, makeOptionsForOk());
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);
        }
    }, MenuItemType::Action, "  Take Screenshot"s, ""s);

    items.emplace_back([&menuTab, &currentMenuTab, &closeMenu](int val) {
        SMenuTab dialogTab;
        bool isDialog = false;
        int result = menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Reset Console", "Are you sure?", DIALOGCOLOR_RED, makeOptionsForNoYes());
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);

        if (result == 1) {
            impl3dsResetConsole();
            closeMenu = true;
        }
    }, MenuItemType::Action, "  Reset Console"s, ""s);

    AddMenuPicker(items, "  Exit"s, "Leaving so soon?", makeOptionsForNoYes(), 0, DIALOGCOLOR_RED, false, exitEmulatorOptionSelected);

    return items;
}

std::vector<SMenuItem> makeOptionsForFont() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, 0, "Tempesta"s, ""s);
    AddMenuDialogOption(items, 1, "Ronda"s,    ""s);
    AddMenuDialogOption(items, 2, "Arial"s,    ""s);
    return items;
}

std::vector<SMenuItem> makeOptionsForStretch() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, 0, "No Stretch"s,              "'Pixel Perfect'"s);
    AddMenuDialogOption(items, 7, "Expand to Fit"s,           "'Pixel Perfect' fit"s);
    AddMenuDialogOption(items, 6, "TV-style"s,                "Stretch width only to 292px"s);
    AddMenuDialogOption(items, 5, "4:3"s,                     "Stretch width only"s);
    AddMenuDialogOption(items, 1, "4:3 Fit"s,                 "Stretch to 320x240"s);
    AddMenuDialogOption(items, 2, "Fullscreen"s,              "Stretch to 400x240"s);
    AddMenuDialogOption(items, 3, "Cropped 4:3 Fit"s,         "Crop & Stretch to 320x240"s);
    AddMenuDialogOption(items, 4, "Cropped Fullscreen"s,      "Crop & Stretch to 400x240"s);
    return items;
}

std::vector<SMenuItem> makeOptionsForButtonMapping() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, 0,                                  "Not Mapped"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_A),            "3DS A Button"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_B),            "3DS B Button"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_Y),            "3DS Y Button"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_X),            "3DS X Button"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_L),            "3DS L Button"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_R),            "3DS R Button"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_START),        "3DS Start Button"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_SELECT),       "3DS Select Button"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_DUP),          "3DS D-Pad Up"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_DDOWN),        "3DS D-Pad Down"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_DLEFT),        "3DS D-Pad Left"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_DRIGHT),       "3DS D-Pad Right"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_UP),      "3DS Circle Pad Up"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_DOWN),    "3DS Circle Pad Down"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_LEFT),    "3DS Circle Pad Left"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_RIGHT),   "3DS Circle Pad Right"s);
    /*
    // doesn't work for some reason, see #37
    AddMenuDialogOption(items, static_cast<int>(KEY_ZL),           "New 3DS ZL Button"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_ZR),           "New 3DS ZR Button"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_CSTICK_UP),    "New 3DS C-Stick Up"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_CSTICK_DOWN),  "New 3DS C-Stick Down"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_CSTICK_LEFT),  "New 3DS C-Stick Left"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_CSTICK_RIGHT), "New 3DS C-Stick Right"s);
    */
    return items;
}

std::vector<SMenuItem> makeOptionsForFrameskip() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, 0, "Disabled"s,                ""s);
    AddMenuDialogOption(items, 1, "Enabled (max 1 frame)"s,   ""s);
    AddMenuDialogOption(items, 2, "Enabled (max 2 frames)"s,   ""s);
    AddMenuDialogOption(items, 3, "Enabled (max 3 frames)"s,   ""s);
    AddMenuDialogOption(items, 4, "Enabled (max 4 frames)"s,   ""s);
    return items;
};

std::vector<SMenuItem> makeOptionsForFrameRate() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, 0, "Default based on ROM region"s,    ""s);
    AddMenuDialogOption(items, 1, "50 FPS"s,                  ""s);
    AddMenuDialogOption(items, 2, "60 FPS"s,                  ""s);
    return items;
};

std::vector<SMenuItem> makeOptionsForAutoSaveSRAMDelay() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, 1, "1 second"s,    ""s);
    AddMenuDialogOption(items, 2, "10 seconds"s,  ""s);
    AddMenuDialogOption(items, 3, "60 seconds"s,  ""s);
    AddMenuDialogOption(items, 4, "Disabled"s,    "Touch bottom screen to save"s);
    return items;
};

std::vector<SMenuItem> makeOptionsForInFramePaletteChanges() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, 1, "Enabled"s,          "Best (not 100% accurate); slower"s);
    AddMenuDialogOption(items, 2, "Disabled Style 1"s, "Faster than \"Enabled\""s);
    AddMenuDialogOption(items, 3, "Disabled Style 2"s, "Faster than \"Enabled\""s);
    return items;
};

std::vector<SMenuItem> makeEmulatorNewMenu() {
    std::vector<SMenuItem> items;
    AddMenuPicker(items, "  Exit"s, "Leaving so soon?", makeOptionsForNoYes(), 0, DIALOGCOLOR_RED, false, exitEmulatorOptionSelected);
    return items;
}

std::vector<SMenuItem> makeOptionMenu() {
    std::vector<SMenuItem> items;

    AddMenuHeader1(items, "GLOBAL SETTINGS"s);
    AddMenuPicker(items, "  Screen Stretch"s, "How would you like the final screen to appear?"s, makeOptionsForStretch(), settings3DS.ScreenStretch, DIALOGCOLOR_CYAN, true,
                  []( int val ) { CheckAndUpdate( settings3DS.ScreenStretch, val, settings3DS.Changed ); });
    AddMenuPicker(items, "  Font"s, "The font used for the user interface."s, makeOptionsForFont(), settings3DS.Font, DIALOGCOLOR_CYAN, true,
                  []( int val ) { if ( CheckAndUpdate( settings3DS.Font, val, settings3DS.Changed ) ) { ui3dsSetFont(val); } });
    AddMenuCheckbox(items, "  Hide text in bottom screen"s, settings3DS.HideUnnecessaryBottomScrText,
                    []( int val ) { CheckAndUpdate( settings3DS.HideUnnecessaryBottomScrText, val, settings3DS.Changed ); });
    AddMenuDisabledOption(items, ""s);

    AddMenuCheckbox(items, "  Automatically save state on exit and load state on start"s, settings3DS.AutoSavestate,
                         []( int val ) { CheckAndUpdate( settings3DS.AutoSavestate, val, settings3DS.Changed ); });
    AddMenuDisabledOption(items, ""s);

    AddMenuHeader1(items, "GAME-SPECIFIC SETTINGS"s);
    AddMenuHeader2(items, "Graphics"s);
    AddMenuPicker(items, "  Frameskip"s, "Try changing this if the game runs slow. Skipping frames helps it run faster, but less smooth."s, makeOptionsForFrameskip(), settings3DS.MaxFrameSkips, DIALOGCOLOR_CYAN, true,
                  []( int val ) { CheckAndUpdate( settings3DS.MaxFrameSkips, val, settings3DS.Changed ); });
    AddMenuPicker(items, "  Framerate"s, "Some games run at 50 or 60 FPS by default. Override if required."s, makeOptionsForFrameRate(), settings3DS.ForceFrameRate, DIALOGCOLOR_CYAN, true,
                  []( int val ) { CheckAndUpdate( settings3DS.ForceFrameRate, val, settings3DS.Changed ); });
    AddMenuPicker(items, "  In-Frame Palette Changes"s, "Try changing this if some colours in the game look off."s, makeOptionsForInFramePaletteChanges(), settings3DS.PaletteFix, DIALOGCOLOR_CYAN, true,
                  []( int val ) { CheckAndUpdate( settings3DS.PaletteFix, val, settings3DS.Changed ); });
    AddMenuDisabledOption(items, ""s);

    AddMenuHeader2(items, "Audio"s);
    AddMenuGauge(items, "  Volume Amplification"s, 0, 8, settings3DS.Volume,
                 []( int val ) { CheckAndUpdate( settings3DS.Volume, val, settings3DS.Changed ); });
    AddMenuDisabledOption(items, ""s);
    AddMenuHeader2(items, "Turbo (Auto-Fire) Buttons"s);
    AddMenuCheckbox(items, "  Button A"s, settings3DS.Turbo[0], []( int val ) { CheckAndUpdate( settings3DS.Turbo[0], val, settings3DS.Changed ); });
    AddMenuCheckbox(items, "  Button B"s, settings3DS.Turbo[1], []( int val ) { CheckAndUpdate( settings3DS.Turbo[1], val, settings3DS.Changed ); });
    AddMenuCheckbox(items, "  Button X"s, settings3DS.Turbo[2], []( int val ) { CheckAndUpdate( settings3DS.Turbo[2], val, settings3DS.Changed ); });
    AddMenuCheckbox(items, "  Button Y"s, settings3DS.Turbo[3], []( int val ) { CheckAndUpdate( settings3DS.Turbo[3], val, settings3DS.Changed ); });
    AddMenuCheckbox(items, "  Button L"s, settings3DS.Turbo[4], []( int val ) { CheckAndUpdate( settings3DS.Turbo[4], val, settings3DS.Changed ); });
    AddMenuCheckbox(items, "  Button R"s, settings3DS.Turbo[5], []( int val ) { CheckAndUpdate( settings3DS.Turbo[5], val, settings3DS.Changed ); });
    AddMenuDisabledOption(items, ""s);

    AddMenuHeader2(items, "SRAM (Save Data)"s);
    AddMenuPicker(items, "  SRAM Auto-Save Delay"s, "Try setting to 60 seconds or Disabled this if the game saves SRAM (Save Data) to SD card too frequently."s, makeOptionsForAutoSaveSRAMDelay(), settings3DS.SRAMSaveInterval, DIALOGCOLOR_CYAN, true,
                  []( int val ) { CheckAndUpdate( settings3DS.SRAMSaveInterval, val, settings3DS.Changed ); });
    AddMenuCheckbox(items, "  Force SRAM Write on Pause"s, settings3DS.ForceSRAMWriteOnPause,
                    []( int val ) { CheckAndUpdate( settings3DS.ForceSRAMWriteOnPause, val, settings3DS.Changed ); });
    return items;
};

std::vector<SMenuItem> makeControlsMenu() {
    std::vector<SMenuItem> items;

    std::array<std::string, static_cast<size_t>( SnesButtons::Count )> snesButtonNames;
    snesButtonNames[static_cast<int>( SnesButtons::A      )] = "A Button"s;
    snesButtonNames[static_cast<int>( SnesButtons::B      )] = "B Button"s;
    snesButtonNames[static_cast<int>( SnesButtons::Y      )] = "Y Button"s;
    snesButtonNames[static_cast<int>( SnesButtons::X      )] = "X Button"s;
    snesButtonNames[static_cast<int>( SnesButtons::L      )] = "L Button"s;
    snesButtonNames[static_cast<int>( SnesButtons::R      )] = "R Button"s;
    snesButtonNames[static_cast<int>( SnesButtons::Up     )] = "D-Pad Up"s;
    snesButtonNames[static_cast<int>( SnesButtons::Down   )] = "D-Pad Down"s;
    snesButtonNames[static_cast<int>( SnesButtons::Left   )] = "D-Pad Left"s;
    snesButtonNames[static_cast<int>( SnesButtons::Right  )] = "D-Pad Right"s;
    snesButtonNames[static_cast<int>( SnesButtons::Start  )] = "Start Button"s;
    snesButtonNames[static_cast<int>( SnesButtons::Select )] = "Select Button"s;

    AddMenuHeader1(items, "BUTTON CONFIGURATION"s);
    for (size_t i = 0; i < settings3DS.ButtonMappingsSnes.size(); ++i) {
        for (size_t j = 0; j < settings3DS.ButtonMappingsSnes[i].MappingBitmasks.size(); ++j) {
            std::ostringstream optionName;
            optionName << "SNES " << snesButtonNames[i] << " (";
            switch (j) {
                case 0: optionName << "Primary"; break;
                case 1: optionName << "Secondary"; break;
                case 2: optionName << "Tertiary"; break;
                default: optionName << (j+1) << "th"; break;
            }
            optionName << ")";

            AddMenuPicker( items, optionName.str(), ""s, makeOptionsForButtonMapping(), settings3DS.ButtonMappingsSnes[i].MappingBitmasks[j], DIALOGCOLOR_CYAN, true,
                [i, j]( int val ) {
                    uint32 v = static_cast<uint32>(val);
                    CheckAndUpdate( settings3DS.ButtonMappingsSnes[i].MappingBitmasks[j], v, settings3DS.Changed );
                }
            );
        }
    }

    return items;
}

void menuSetupCheats(std::vector<SMenuItem>& cheatMenu);

std::vector<SMenuItem> makeCheatMenu() {
    std::vector<SMenuItem> items;
    AddMenuHeader2(items, "Cheats"s);
    menuSetupCheats(items);
    return items;
};


//----------------------------------------------------------------------
// Update settings.
//----------------------------------------------------------------------

bool settingsUpdateAllSettings(bool updateGameSettings = true)
{
    bool settingsChanged = false;

    // update screen stretch
    //
    if (settings3DS.ScreenStretch == 0)
    {
        settings3DS.StretchWidth = 256;
        settings3DS.StretchHeight = -1;    // Actual height
        settings3DS.CropPixels = 0;
    }
    else if (settings3DS.ScreenStretch == 1)
    {
        // Added support for 320x240 (4:3) screen ratio
        settings3DS.StretchWidth = 320;
        settings3DS.StretchHeight = 240;
        settings3DS.CropPixels = 0;
    }
    else if (settings3DS.ScreenStretch == 2)
    {
        settings3DS.StretchWidth = 400;
        settings3DS.StretchHeight = 240;
        settings3DS.CropPixels = 0;
    }
    else if (settings3DS.ScreenStretch == 3)
    {
        settings3DS.StretchWidth = 320;
        settings3DS.StretchHeight = 240;
        settings3DS.CropPixels = 8;
    }
    else if (settings3DS.ScreenStretch == 4)
    {
        settings3DS.StretchWidth = 400;
        settings3DS.StretchHeight = 240;
        settings3DS.CropPixels = 8;
    }
    else if (settings3DS.ScreenStretch == 5)
    {
        settings3DS.StretchWidth = 04030000;       // Stretch width only to 4/3
        settings3DS.StretchHeight = -1;
        settings3DS.CropPixels = 0;
    }
    else if (settings3DS.ScreenStretch == 6)    // TV
    {
        settings3DS.StretchWidth = 292;       
        settings3DS.StretchHeight = -1;
        settings3DS.CropPixels = 0;
    }
    else if (settings3DS.ScreenStretch == 7)    // Stretch h/w but keep 1:1 ratio
    {
        settings3DS.StretchWidth = 01010000;       
        settings3DS.StretchHeight = 240;
        settings3DS.CropPixels = 0;
    }

    // Update the screen font
    //
    ui3dsSetFont(settings3DS.Font);

    if (updateGameSettings)
    {
        // Update frame rate
        //
        if (Settings.PAL)
            settings3DS.TicksPerFrame = TICKS_PER_FRAME_PAL;
        else
            settings3DS.TicksPerFrame = TICKS_PER_FRAME_NTSC;

        if (settings3DS.ForceFrameRate == 1)
            settings3DS.TicksPerFrame = TICKS_PER_FRAME_PAL;

        else if (settings3DS.ForceFrameRate == 2)
            settings3DS.TicksPerFrame = TICKS_PER_FRAME_NTSC;

        // update global volume
        //
        if (settings3DS.Volume < 0)
            settings3DS.Volume = 0;
        if (settings3DS.Volume > 8)
            settings3DS.Volume = 8;
        Settings.VolumeMultiplyMul4 = (settings3DS.Volume + 4);
        //printf ("vol: %d\n", Settings.VolumeMultiplyMul4);

        // update in-frame palette fix
        //
        if (settings3DS.PaletteFix == 1)
            SNESGameFixes.PaletteCommitLine = -2;
        else if (settings3DS.PaletteFix == 2)
            SNESGameFixes.PaletteCommitLine = 1;
        else if (settings3DS.PaletteFix == 3)
            SNESGameFixes.PaletteCommitLine = -1;
        else
        {
            if (SNESGameFixes.PaletteCommitLine == -2)
                settings3DS.PaletteFix = 1;
            else if (SNESGameFixes.PaletteCommitLine == 1)
                settings3DS.PaletteFix = 2;
            else if (SNESGameFixes.PaletteCommitLine == -1)
                settings3DS.PaletteFix = 3;
            settingsChanged = true;
        }

        if (settings3DS.SRAMSaveInterval == 1)
            Settings.AutoSaveDelay = 60;
        else if (settings3DS.SRAMSaveInterval == 2)
            Settings.AutoSaveDelay = 600;
        else if (settings3DS.SRAMSaveInterval == 3)
            Settings.AutoSaveDelay = 3600;
        else if (settings3DS.SRAMSaveInterval == 4)
            Settings.AutoSaveDelay = -1;
        else
        {
            if (Settings.AutoSaveDelay == 60)
                settings3DS.SRAMSaveInterval = 1;
            else if (Settings.AutoSaveDelay == 600)
                settings3DS.SRAMSaveInterval = 2;
            else if (Settings.AutoSaveDelay == 3600)
                settings3DS.SRAMSaveInterval = 3;
            settingsChanged = true;
        }
        
        // Fixes the Auto-Save timer bug that causes
        // the SRAM to be saved once when the settings were
        // changed to Disabled.
        //
        if (Settings.AutoSaveDelay == -1)
            CPU.AutoSaveTimer = -1;
        else
            CPU.AutoSaveTimer = 0;
    }

    return settingsChanged;
}

namespace {
    void config3dsReadWriteBitmask(const char* name, uint32* bitmask) {
        int tmp = static_cast<int>(*bitmask);
        config3dsReadWriteInt32(name, &tmp, std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
        *bitmask = static_cast<uint32>(tmp);
    }
}

//----------------------------------------------------------------------
// Read/write all possible game specific settings.
//----------------------------------------------------------------------
bool settingsReadWriteFullListByGame(bool writeMode)
{
    if (!writeMode) {
        // set default values first.
        LoadDefaultSettings();
    }

    bool success = config3dsOpenFile(S9xGetFilename(".cfg"), writeMode);
    if (!success)
        return false;

    config3dsReadWriteInt32("#v1\n", NULL, 0, 0);
    config3dsReadWriteInt32("# Do not modify this file or risk losing your settings.\n", NULL, 0, 0);

    config3dsReadWriteInt32("Frameskips=%d\n", &settings3DS.MaxFrameSkips, 0, 4);
    config3dsReadWriteInt32("Framerate=%d\n", &settings3DS.ForceFrameRate, 0, 2);
    config3dsReadWriteInt32("TurboA=%d\n", &settings3DS.Turbo[0], 0, 1);
    config3dsReadWriteInt32("TurboB=%d\n", &settings3DS.Turbo[1], 0, 1);
    config3dsReadWriteInt32("TurboX=%d\n", &settings3DS.Turbo[2], 0, 1);
    config3dsReadWriteInt32("TurboY=%d\n", &settings3DS.Turbo[3], 0, 1);
    config3dsReadWriteInt32("TurboL=%d\n", &settings3DS.Turbo[4], 0, 1);
    config3dsReadWriteInt32("TurboR=%d\n", &settings3DS.Turbo[5], 0, 1);
    config3dsReadWriteInt32("Vol=%d\n", &settings3DS.Volume, 0, 8);
    config3dsReadWriteInt32("PalFix=%d\n", &settings3DS.PaletteFix, 0, 3);
    config3dsReadWriteInt32("SRAMInterval=%d\n", &settings3DS.SRAMSaveInterval, 0, 4);
    config3dsReadWriteInt32("ForceSRAMWrite=%d\n", &settings3DS.ForceSRAMWriteOnPause, 0, 1);

    for (size_t i = 0; i < settings3DS.ButtonMappingsSnes.size(); ++i) {
        for (size_t j = 0; j < settings3DS.ButtonMappingsSnes[i].MappingBitmasks.size(); ++j) {
            std::ostringstream oss;
            oss << "ButtonMapping_" << i << "_" << j << "=%d\n";
            config3dsReadWriteBitmask(oss.str().c_str(), &settings3DS.ButtonMappingsSnes[i].MappingBitmasks[j]);
        }
    }

    // All new options should come here!

    config3dsCloseFile();
    return true;
}


//----------------------------------------------------------------------
// Read/write all possible game specific settings.
//----------------------------------------------------------------------
bool settingsReadWriteFullListGlobal(bool writeMode)
{
    bool success = config3dsOpenFile("./snes9x_3ds.cfg", writeMode);
    if (!success)
        return false;
    
    config3dsReadWriteInt32("#v1\n", NULL, 0, 0);
    config3dsReadWriteInt32("# Do not modify this file or risk losing your settings.\n", NULL, 0, 0);

    config3dsReadWriteInt32("ScreenStretch=%d\n", &settings3DS.ScreenStretch, 0, 7);
    config3dsReadWriteInt32("HideUnnecessaryBottomScrText=%d\n", &settings3DS.HideUnnecessaryBottomScrText, 0, 1);
    config3dsReadWriteInt32("Font=%d\n", &settings3DS.Font, 0, 2);

    // Fixes the bug where we have spaces in the directory name
    config3dsReadWriteString("Dir=%s\n", "Dir=%1000[^\n]s\n", file3dsGetCurrentDir());
    config3dsReadWriteString("ROM=%s\n", "ROM=%1000[^\n]s\n", romFileNameLastSelected);

    config3dsReadWriteInt32("AutoSavestate=%d\n", &settings3DS.AutoSavestate, 0, 1);

    // All new options should come here!

    config3dsCloseFile();
    return true;
}

//----------------------------------------------------------------------
// Save settings by game.
//----------------------------------------------------------------------
bool settingsSave(bool includeGameSettings = true)
{
    consoleClear();
    ui3dsDrawRect(50, 140, 270, 154, 0x000000);
    ui3dsDrawStringWithNoWrapping(50, 140, 270, 154, 0x3f7fff, HALIGN_CENTER, "Saving settings to SD card...");

    if (includeGameSettings)
        settingsReadWriteFullListByGame(true);

    settingsReadWriteFullListGlobal(true);
    ui3dsDrawRect(50, 140, 270, 154, 0x000000);

    settings3DS.Changed = false;
    return true;
}

//----------------------------------------------------------------------
// Load settings by game.
//----------------------------------------------------------------------
bool settingsLoad(bool includeGameSettings = true)
{
    settings3DS.Changed = false;
    bool success = settingsReadWriteFullListGlobal(false);
    if (!success)
        return false;
    settingsUpdateAllSettings(false);

    if (includeGameSettings)
    {
        success = settingsReadWriteFullListByGame(false);
        if (success)
        {
            if (settingsUpdateAllSettings())
                settingsSave();
            return true;
        }
        else
        {
            // If we can't find the saved settings, always
            // set the frame rate to be based on the ROM's region.
            // For the rest of the settings, we use whatever has been
            // set in the previous game.
            //
            settings3DS.MaxFrameSkips = 1;
            settings3DS.ForceFrameRate = 0;
            settings3DS.Volume = 4;

            for (int i = 0; i < 6; i++)     // and clear all turbo buttons.
                settings3DS.Turbo[i] = 0;

            if (SNESGameFixes.PaletteCommitLine == -2)
                settings3DS.PaletteFix = 1;
            else if (SNESGameFixes.PaletteCommitLine == 1)
                settings3DS.PaletteFix = 2;
            else if (SNESGameFixes.PaletteCommitLine == -1)
                settings3DS.PaletteFix = 3;

            if (Settings.AutoSaveDelay == 60)
                settings3DS.SRAMSaveInterval = 1;
            else if (Settings.AutoSaveDelay == 600)
                settings3DS.SRAMSaveInterval = 2;
            else if (Settings.AutoSaveDelay == 3600)
                settings3DS.SRAMSaveInterval = 3;

            settingsUpdateAllSettings();

            return settingsSave();
        }
    }
}




//-------------------------------------------------------
// Load the ROM and reset the CPU.
//-------------------------------------------------------

extern SCheatData Cheat;

void emulatorLoadRom()
{
    consoleInit(GFX_BOTTOM, NULL);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    consoleClear();
    settingsSave(false);

    char romFileNameFullPath[_MAX_PATH];
    snprintf(romFileNameFullPath, _MAX_PATH, "%s%s", file3dsGetCurrentDir(), romFileName);
    impl3dsLoadROM(romFileNameFullPath);

    GPU3DS.emulatorState = EMUSTATE_EMULATE;

    consoleClear();
    settingsLoad();
    settingsUpdateAllSettings();

    if (settings3DS.AutoSavestate)
        impl3dsLoadStateAuto();

    snd3DS.generateSilence = false;
}


//----------------------------------------------------------------------
// Load all ROM file names
//----------------------------------------------------------------------
void fileGetAllFiles(std::vector<DirectoryEntry>& romFileNames)
{
    file3dsGetFiles(romFileNames, {"smc", "sfc", "fig"});
}


//----------------------------------------------------------------------
// Find the ID of the last selected file in the file list.
//----------------------------------------------------------------------
int fileFindLastSelectedFile(std::vector<SMenuItem>& fileMenu)
{
    for (int i = 0; i < fileMenu.size() && i < 1000; i++)
    {
        if (strncmp(fileMenu[i].Text.c_str(), romFileNameLastSelected, _MAX_PATH) == 0)
            return i;
    }
    return -1;
}


//----------------------------------------------------------------------
// Handle menu cheats.
//----------------------------------------------------------------------
bool menuCopyCheats(std::vector<SMenuItem>& cheatMenu, bool copyMenuToSettings)
{
    bool cheatsUpdated = false;
    for (int i = 0; (i+1) < cheatMenu.size() && i < MAX_CHEATS && i < Cheat.num_cheats; i++)
    {
        cheatMenu[i+1].Type = MenuItemType::Checkbox;
        cheatMenu[i+1].Text = Cheat.c[i].name;

        if (copyMenuToSettings)
        {
            if (Cheat.c[i].enabled != cheatMenu[i+1].Value)
            {
                Cheat.c[i].enabled = cheatMenu[i+1].Value;
                if (Cheat.c[i].enabled)
                    S9xEnableCheat(i);
                else
                    S9xDisableCheat(i);
                cheatsUpdated = true;
            }
        }
        else
            cheatMenu[i+1].SetValue(Cheat.c[i].enabled);
    }
    
    return cheatsUpdated;
}


void fillFileMenuFromFileNames(std::vector<SMenuItem>& fileMenu, const std::vector<DirectoryEntry>& romFileNames, const DirectoryEntry*& selectedEntry) {
    fileMenu.clear();
    fileMenu.reserve(romFileNames.size());

    for (size_t i = 0; i < romFileNames.size(); ++i) {
        const DirectoryEntry& entry = romFileNames[i];
        fileMenu.emplace_back( [&entry, &selectedEntry]( int val ) {
            selectedEntry = &entry;
        }, MenuItemType::Action, entry.Filename, ""s );
    }
}

//----------------------------------------------------------------------
// Start up menu.
//----------------------------------------------------------------------
void setupBootupMenu(std::vector<SMenuTab>& menuTab, std::vector<DirectoryEntry>& romFileNames, const DirectoryEntry*& selectedDirectoryEntry, bool selectPreviousFile) {
    menuTab.clear();
    menuTab.reserve(2);

    {
        menu3dsAddTab(menuTab, "Emulator", makeEmulatorNewMenu());
        menuTab.back().SubTitle.clear();
    }

    {
        std::vector<SMenuItem> fileMenu;
        fileGetAllFiles(romFileNames);
        fillFileMenuFromFileNames(fileMenu, romFileNames, selectedDirectoryEntry);
        menu3dsAddTab(menuTab, "Select ROM", fileMenu);
        menuTab.back().SubTitle.assign(file3dsGetCurrentDir());
        if (selectPreviousFile) {
            int previousFileID = fileFindLastSelectedFile(menuTab.back().MenuItems);
            menu3dsSetSelectedItemByIndex(menuTab.back(), previousFileID);
        }
    }
}

void menuSelectFile(void)
{
    std::vector<SMenuTab> menuTab;
    std::vector<DirectoryEntry> romFileNames; // needs to stay in scope, is there a better way?
    const DirectoryEntry* selectedDirectoryEntry = nullptr;
    setupBootupMenu(menuTab, romFileNames, selectedDirectoryEntry, true);

    int currentMenuTab = 1;
    bool isDialog = false;
    SMenuTab dialogTab;

    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    menu3dsSetTransferGameScreen(false);

    bool animateMenu = true;
    while (!appExiting) {
        menu3dsShowMenu(dialogTab, isDialog, currentMenuTab, menuTab, animateMenu);
        animateMenu = false;

        if (selectedDirectoryEntry) {
            if (selectedDirectoryEntry->Type == FileEntryType::File) {
                strncpy(romFileName, selectedDirectoryEntry->Filename.c_str(), _MAX_PATH);
                strncpy(romFileNameLastSelected, romFileName, _MAX_PATH);
                menu3dsHideMenu(dialogTab, isDialog, currentMenuTab, menuTab);
                emulatorLoadRom();
                return;
            } else if (selectedDirectoryEntry->Type == FileEntryType::ParentDirectory || selectedDirectoryEntry->Type == FileEntryType::ChildDirectory) {
                file3dsGoUpOrDownDirectory(*selectedDirectoryEntry);
                setupBootupMenu(menuTab, romFileNames, selectedDirectoryEntry, false);
            }
            selectedDirectoryEntry = nullptr;
        }
    }

    menu3dsHideMenu(dialogTab, isDialog, currentMenuTab, menuTab);
}


//----------------------------------------------------------------------
// Menu when the emulator is paused in-game.
//----------------------------------------------------------------------
void setupPauseMenu(std::vector<SMenuTab>& menuTab, std::vector<DirectoryEntry>& romFileNames, const DirectoryEntry*& selectedDirectoryEntry, bool selectPreviousFile, int& currentMenuTab, bool& closeMenu) {
    menuTab.clear();
    menuTab.reserve(4);

    {
        menu3dsAddTab(menuTab, "Emulator", makeEmulatorMenu(menuTab, currentMenuTab, closeMenu));
        menuTab.back().SubTitle.clear();
    }

    {
        menu3dsAddTab(menuTab, "Options", makeOptionMenu());
        menuTab.back().SubTitle.clear();
    }

    {
        menu3dsAddTab(menuTab, "Controls", makeControlsMenu());
        menuTab.back().SubTitle.clear();
    }

    {
        menu3dsAddTab(menuTab, "Cheats", makeCheatMenu());
        menuTab.back().SubTitle.clear();
    }

    {
        std::vector<SMenuItem> fileMenu;
        fileGetAllFiles(romFileNames);
        fillFileMenuFromFileNames(fileMenu, romFileNames, selectedDirectoryEntry);
        menu3dsAddTab(menuTab, "Select ROM", fileMenu);
        menuTab.back().SubTitle.assign(file3dsGetCurrentDir());
        if (selectPreviousFile) {
            int previousFileID = fileFindLastSelectedFile(menuTab.back().MenuItems);
            menu3dsSetSelectedItemByIndex(menuTab.back(), previousFileID);
        }
    }
}

void menuPause()
{
    int currentMenuTab = 0;
    bool closeMenu = false;
    std::vector<SMenuTab> menuTab;
    std::vector<DirectoryEntry> romFileNames; // needs to stay in scope, is there a better way?
    const DirectoryEntry* selectedDirectoryEntry = nullptr;
    setupPauseMenu(menuTab, romFileNames, selectedDirectoryEntry, true, currentMenuTab, closeMenu);

    bool isDialog = false;
    SMenuTab dialogTab;

    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    menu3dsSetTransferGameScreen(true);

    bool loadRomBeforeExit = false;

    std::vector<SMenuItem>& cheatMenu = menuTab[2].MenuItems;
    menuCopyCheats(cheatMenu, false);

    bool animateMenu = true;
    while (!appExiting && !closeMenu) {
        if (menu3dsShowMenu(dialogTab, isDialog, currentMenuTab, menuTab, animateMenu) < 0) {
            // user pressed B, close menu
            closeMenu = true;
        }
        animateMenu = false;

        if (selectedDirectoryEntry) {
            // Load ROM
            if (selectedDirectoryEntry->Type == FileEntryType::File) {
                bool loadRom = true;
                //if (settings3DS.Changed)settingsSave(); // should be unnecessary now?
                if (settings3DS.AutoSavestate) {
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Save State", "Autosaving...", DIALOGCOLOR_CYAN, std::vector<SMenuItem>());
                    bool result = impl3dsSaveStateAuto();
                    menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);

                    if (!result) {
                        int choice = menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Autosave failure", "Automatic savestate writing failed.\nLoad chosen game anyway?", DIALOGCOLOR_RED, makeOptionsForNoYes());
                        if (choice != 1) {
                            loadRom = false;
                        }
                    }
                }

                if (loadRom) {
                    strncpy(romFileName, selectedDirectoryEntry->Filename.c_str(), _MAX_PATH);
                    strncpy(romFileNameLastSelected, romFileName, _MAX_PATH);
                    loadRomBeforeExit = true;
                    break;
                }
            } else if (selectedDirectoryEntry->Type == FileEntryType::ParentDirectory || selectedDirectoryEntry->Type == FileEntryType::ChildDirectory) {
                file3dsGoUpOrDownDirectory(*selectedDirectoryEntry);
                setupPauseMenu(menuTab, romFileNames, selectedDirectoryEntry, false, currentMenuTab, closeMenu);
            }
            selectedDirectoryEntry = nullptr;
        }
    }

    menu3dsHideMenu(dialogTab, isDialog, currentMenuTab, menuTab);

    // Save settings and cheats
    //
    if (settings3DS.Changed)
        settingsSave();
    settingsUpdateAllSettings();

    if (menuCopyCheats(cheatMenu, true))
    {
        // Only one of these will succeeed.
        S9xSaveCheatFile (S9xGetFilename(".cht"));
        S9xSaveCheatTextFile (S9xGetFilename(".chx"));
    }

    if (closeMenu) {
        GPU3DS.emulatorState = EMUSTATE_EMULATE;
        consoleClear();
    }

    // Loads the new ROM if a ROM was selected.
    //
    if (loadRomBeforeExit)
        emulatorLoadRom();

}

//-------------------------------------------------------
// Sets up all the cheats to be displayed in the menu.
//-------------------------------------------------------
char *noCheatsText[] {
    "",
    "    No cheats available for this game ",
    "",
    "    To enable cheats:  ",
    "      Copy your .CHT/.CHX file into the same folder as  ",
    "      ROM file and make sure it has the same name. ",
    "",
    "      If your ROM filename is: ",
    "          MyGame.smc ",
    "      Then your cheat filename must be: ",
    "          MyGame.cht or MyGame.chx ",
    "",
    "    Refer to readme.md for the .CHX file format. ",
    ""
     };

void menuSetupCheats(std::vector<SMenuItem>& cheatMenu)
{
    if (Cheat.num_cheats > 0)
    {
        for (int i = 0; i < MAX_CHEATS && i < Cheat.num_cheats; i++)
        {
            cheatMenu.emplace_back(nullptr, MenuItemType::Checkbox, std::string(Cheat.c[i].name), ""s, Cheat.c[i].enabled ? 1 : 0);
        }
    }
    else
    {
        for (int i = 0; i < 14; i++)
        {
            cheatMenu.emplace_back(nullptr, MenuItemType::Disabled, std::string(noCheatsText[i]), ""s);
        }
    }
}


//--------------------------------------------------------
// Initialize the emulator engine and everything else.
// This calls the impl3dsInitializeCore, which executes
// initialization code specific to the emulation core.
//--------------------------------------------------------
void emulatorInitialize()
{
    file3dsInitialize();

    romFileNameLastSelected[0] = 0;

    if (!gpu3dsInitialize())
    {
        printf ("Unable to initialized GPU\n");
        exit(0);
    }

    printf ("Initializing...\n");

    if (!impl3dsInitializeCore())
    {
        printf ("Unable to initialize emulator core\n");
        exit(0);
    }

    if (!snd3dsInitialize())
    {
        printf ("Unable to initialize CSND\n");
        exit (0);
    }

    ui3dsInitialize();

    if (romfsInit()!=0)
    {
        printf ("Unable to initialize romfs\n");
        exit (0);
    }
    
    printf ("Initialization complete\n");

    osSetSpeedupEnable(1);    // Performance: use the higher clock speed for new 3DS.

    enableAptHooks();

    settingsLoad(false);

    // Do this one more time.
    if (file3dsGetCurrentDir()[0] == 0)
        file3dsInitialize();

    srvInit();
}


//--------------------------------------------------------
// Finalize the emulator.
//--------------------------------------------------------
void emulatorFinalize()
{
    consoleClear();

    impl3dsFinalize();

#ifndef RELEASE
    printf("gspWaitForP3D:\n");
#endif
    gspWaitForVBlank();
    gpu3dsWaitForPreviousFlush();
    gspWaitForVBlank();

#ifndef RELEASE
    printf("snd3dsFinalize:\n");
#endif
    snd3dsFinalize();

#ifndef RELEASE
    printf("gpu3dsFinalize:\n");
#endif
    gpu3dsFinalize();

#ifndef RELEASE
    printf("ptmSysmExit:\n");
#endif
    ptmSysmExit ();

    printf("romfsExit:\n");
    romfsExit();
    
#ifndef RELEASE
    printf("hidExit:\n");
#endif
	hidExit();
    
#ifndef RELEASE
    printf("aptExit:\n");
#endif
	aptExit();
    
#ifndef RELEASE
    printf("srvExit:\n");
#endif
	srvExit();
}



bool firstFrame = true;


//---------------------------------------------------------
// Counts the number of frames per second, and prints
// it to the bottom screen every 60 frames.
//---------------------------------------------------------
char frameCountBuffer[70];
void updateFrameCount()
{
    if (frameCountTick == 0)
        frameCountTick = svcGetSystemTick();

    if (frameCount60 == 0)
    {
        u64 newTick = svcGetSystemTick();
        float timeDelta = ((float)(newTick - frameCountTick))/TICKS_PER_SEC;
        int fpsmul10 = (int)((float)600 / timeDelta);

#if !defined(RELEASE) && !defined(DEBUG_CPU) && !defined(DEBUG_APU)
        consoleClear();
#endif

        if (settings3DS.HideUnnecessaryBottomScrText == 0)
        {
            if (framesSkippedCount)
                snprintf (frameCountBuffer, 69, "FPS: %2d.%1d (%d skipped)\n", fpsmul10 / 10, fpsmul10 % 10, framesSkippedCount);
            else
                snprintf (frameCountBuffer, 69, "FPS: %2d.%1d \n", fpsmul10 / 10, fpsmul10 % 10);

            ui3dsDrawRect(2, 2, 200, 16, 0x000000);
            ui3dsDrawStringWithNoWrapping(2, 2, 200, 16, 0x7f7f7f, HALIGN_LEFT, frameCountBuffer);
        }

        frameCount60 = 60;
        framesSkippedCount = 0;


#if !defined(RELEASE) && !defined(DEBUG_CPU) && !defined(DEBUG_APU)
        printf ("\n\n");
        for (int i=0; i<100; i++)
        {
            t3dsShowTotalTiming(i);
        }
        t3dsResetTimings();
#endif
        frameCountTick = newTick;

    }

    frameCount60--;
}





//----------------------------------------------------------
// This is the main emulation loop. It calls the 
//    impl3dsRunOneFrame
//   (which must be implemented for any new core)
// for the execution of the frame.
//----------------------------------------------------------
void emulatorLoop()
{
	// Main loop
    //GPU3DS.enableDebug = true;

    int snesFramesSkipped = 0;
    long snesFrameTotalActualTicks = 0;
    long snesFrameTotalAccurateTicks = 0;

    bool firstFrame = true;
    appSuspended = 0;

    snd3DS.generateSilence = false;

    gpu3dsResetState();

    frameCount60 = 60;
    frameCountTick = 0;
    framesSkippedCount = 0;

    long startFrameTick = svcGetSystemTick();

    bool skipDrawingFrame = false;

    // Reinitialize the console.
    consoleInit(GFX_BOTTOM, NULL);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    menu3dsDrawBlackScreen();
    if (settings3DS.HideUnnecessaryBottomScrText == 0)
    {
        ui3dsDrawStringWithNoWrapping(0, 100, 320, 115, 0x7f7f7f, HALIGN_CENTER, "Touch screen for menu");
    }

    snd3dsStartPlaying();

	while (true)
	{
        t3dsStartTiming(1, "aptMainLoop");

        startFrameTick = svcGetSystemTick();
        aptMainLoop();

        if (appExiting || appSuspended)
            break;

        gpu3dsStartNewFrame();
        gpu3dsCheckSlider();
        updateFrameCount();

    	input3dsScanInputForEmulation();
        impl3dsRunOneFrame(firstFrame, skipDrawingFrame);

        if (GPU3DS.emulatorState != EMUSTATE_EMULATE)
            break;

        firstFrame = false; 

        // This either waits for the next frame, or decides to skip
        // the rendering for the next frame if we are too slow.
        //
#ifndef RELEASE
        if (GPU3DS.isReal3DS)
#endif
        {

            long currentTick = svcGetSystemTick();
            long actualTicksThisFrame = currentTick - startFrameTick;

            snesFrameTotalActualTicks += actualTicksThisFrame;  // actual time spent rendering past x frames.
            snesFrameTotalAccurateTicks += settings3DS.TicksPerFrame;  // time supposed to be spent rendering past x frames.

            int isSlow = 0;


            long skew = snesFrameTotalAccurateTicks - snesFrameTotalActualTicks;

            if (skew < 0)
            {
                // We've skewed out of the actual frame rate.
                // Once we skew beyond 0.1 (10%) frames slower, skip the frame.
                //
                if (skew < -settings3DS.TicksPerFrame/10 && snesFramesSkipped < settings3DS.MaxFrameSkips)
                {
                    skipDrawingFrame = true;
                    snesFramesSkipped++;

                    framesSkippedCount++;   // this is used for the stats display every 60 frames.
                }
                else
                {
                    skipDrawingFrame = false;

                    if (snesFramesSkipped >= settings3DS.MaxFrameSkips)
                    {
                        snesFramesSkipped = 0;
                        snesFrameTotalActualTicks = actualTicksThisFrame;
                        snesFrameTotalAccurateTicks = settings3DS.TicksPerFrame;
                    }
                }
            }
            else
            {

                float timeDiffInMilliseconds = (float)skew * 1000000 / TICKS_PER_SEC;

                // Reset the counters.
                //
                snesFrameTotalActualTicks = 0;
                snesFrameTotalAccurateTicks = 0;
                snesFramesSkipped = 0;

                svcSleepThread ((long)(timeDiffInMilliseconds * 1000));

                skipDrawingFrame = false;
            }

        }

	}

    snd3dsStopPlaying();
}


//---------------------------------------------------------
// Main entrypoint.
//---------------------------------------------------------
int main()
{
    emulatorInitialize();
    clearTopScreenWithLogo();

    menuSelectFile();

    while (true)
    {
        if (appExiting)
            goto quit;

        switch (GPU3DS.emulatorState)
        {
            case EMUSTATE_PAUSEMENU:
                menuPause();
                break;

            case EMUSTATE_EMULATE:
                emulatorLoop();
                break;

            case EMUSTATE_END:
                goto quit;

        }

    }

quit:
    if (GPU3DS.emulatorState > 0 && settings3DS.AutoSavestate)
        impl3dsSaveStateAuto();

    printf("emulatorFinalize:\n");
    emulatorFinalize();
    printf ("Exiting...\n");
	exit(0);
}
