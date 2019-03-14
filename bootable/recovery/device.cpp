/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "device.h"
#include "common.h"
#include "screen_ui.h"
#ifdef HI_REMOTE_RECOVERY
#define KEY_DOWN_ME       0x2dd2ff00
#define KEY_UP_ME         0x35caff00
#define KEY_ENTER_ME      0x31ceff00
#endif

static const char* MENU_ITEMS[] = {
    "Reboot system now",
    "Reboot to bootloader",
    "Apply update from ADB",
    "Apply update from SD card",
    "Wipe data/factory reset",
    "Wipe cache partition",
    "Mount /system",
    "View recovery logs",
    "Run graphics test",
    "Power off",
    NULL
};

const char* const* Device::GetMenuItems() {
  return MENU_ITEMS;
}

Device::BuiltinAction Device::InvokeMenuItem(int menu_position) {
  switch (menu_position) {
#ifdef HI_REMOTE_RECOVERY
    case 0: return REBOOT;
    case 1: return APPLY_SDCARD;
    case 2: return WIPE_DATA;
    case 3: return UPDATE_VOLUME;
#else
    case 0: return REBOOT;
    case 1: return REBOOT_BOOTLOADER;
    case 2: return APPLY_ADB_SIDELOAD;
    case 3: return APPLY_SDCARD;
    case 4: return WIPE_DATA;
    case 5: return WIPE_CACHE;
    case 6: return MOUNT_SYSTEM;
    case 7: return VIEW_RECOVERY_LOGS;
    case 8: return RUN_GRAPHICS_TEST;
    case 9: return SHUTDOWN;
#endif
    default: return NO_ACTION;
  }
}

int Device::HandleMenuKey(int key, int visible) {
  if (!visible) {
    return kNoAction;
  }
  switch (key) {
#ifdef HI_REMOTE_RECOVERY
    case KEY_DOWN_ME:
      return kHighlightDown;
    case KEY_UP_ME:
      return kHighlightUp;
    case KEY_ENTER_ME:
      return kInvokeItem;
#else
    case KEY_DOWN:
    case KEY_VOLUMEDOWN:
      return kHighlightDown;

    case KEY_UP:
    case KEY_VOLUMEUP:
      return kHighlightUp;

    case KEY_ENTER:
    case KEY_POWER:
      return kInvokeItem;
#endif

      // If you have all of the above buttons, any other buttons
      // are ignored. Otherwise, any button cycles the highlight.
//      return ui_->HasThreeButtons() ? kNoAction : kHighlightDown;
//        return kNoAction;
  }
  return kNoAction;
}