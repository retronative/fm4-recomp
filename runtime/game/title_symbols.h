// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp: the named game functions and values the runtime's title code (runtime/game/) uses. Their numbers are FM4's, with
// evidence, in config/title_hooks.toml ([[symbol]]: a function, checked by the SHA-256 of its bytes; [[value]]: an offset, an id or a
// data address); scripts/gen_runtime_tables.py defines them in the build's fm4_hooks.cpp. No address in the runtime's source.
#pragma once
#include <cstdint>

namespace fm4::sym {
// Functions.
extern const uint32_t kScreenManager;   // () -> the screen manager
extern const uint32_t kNew;             // (size) -> memory (the game's allocator)
extern const uint32_t kCollectionNew;   // (this) -> a CDataItemCollection
extern const uint32_t kCollectionAdd;   // (collection + kCollectionItems, &item)
extern const uint32_t kListSourceNew;   // (this, 2) -> a CDynamicListDataSource
extern const uint32_t kListSourceAdd;   // (source, value, &label, 0, value, f1 -1.0, r9 enabled)
extern const uint32_t kStringAssign;    // (&field, &string): a reference stored (AddRef)
extern const uint32_t kUserString;      // (&out, UTF-16BE text, -1): a CUserString
extern const uint32_t kStringCopy;      // (&copy, &string): a reference copied (AddRef)
extern const uint32_t kSetText;         // (text element, &string)
extern const uint32_t kFireSceneEvent;  // (screen, event id)
extern const uint32_t kHudTeardown;     // the option screen's vtable slot 4: frees the rows at kHudCollection
extern const uint32_t kNameHash;        // (name) -> the id of a scene component name
extern const uint32_t kComponent;       // (screen, id) -> the component
extern const uint32_t kHelpAdd;         // (help bar, slot, &string): a button (slot = its glyph: 0 A, 1 B, 2 X, 3 Y)
extern const uint32_t kHelpRemove;      // (help bar, slot)
// Values.
extern const uint32_t kHudScreenId;            // the option screen the submenu is shown in
extern const uint32_t kScreenOwner;            // screen + this: the owner the screen manager opens and closes for
extern const uint32_t kManagerOpen;            // screen manager vtable: open (manager, owner, id, 0, -1)
extern const uint32_t kHudList;                // option screen + this: its list control
extern const uint32_t kHudCollection;          // ... its rows
extern const uint32_t kHudChanged;             // ... "values changed" (byte)
extern const uint32_t kHudDescription;         // ... the focused row's description text
extern const uint32_t kHudScreenName;          // ... the title text
extern const uint32_t kListBinding;            // list + this: its binding interface
extern const uint32_t kBindingField;           // binding vtable: the field shown (binding, name)
extern const uint32_t kBindingCollection;      // binding vtable: the rows (binding, collection)
extern const uint32_t kBindingRefresh;         // binding vtable: redraw (binding)
extern const uint32_t kListSender;             // list + this: the sender of its focus events
extern const uint32_t kItemSetValue;           // row vtable: (row, value, 0)
extern const uint32_t kItemVtable;             // a CDataSrcDataItem's vtable
extern const uint32_t kItemSize;               // its size
extern const uint32_t kCollectionSize;         // a CDataItemCollection's size
extern const uint32_t kCollectionItems;        // collection + this: its row vector (begin, end)
extern const uint32_t kListSourceSize;         // a CDynamicListDataSource's size
extern const uint32_t kDisplayNameField;       // the data address of "DisplayName"
extern const uint32_t kScreenMatchInput;       // screen vtable: (screen, input, codes, count) -> the matched code, or 0
extern const uint32_t kInputA, kInputB, kInputX, kInputY;  // input codes
extern const uint32_t kPauseInfoHidden;        // Pause menu + this: "screen info hidden" (byte; set once)
extern const uint32_t kHideScreenInfoEvent;    // the data address of the HIDE_SCREEN_INFO event id (the Pause menu's)
extern const uint32_t kPreRaceInfoHidden;      // Pre-Race screen + this: the same byte (set every time)
extern const uint32_t kPreRaceHideScreenInfoEvent;  // its HIDE_SCREEN_INFO event id's data address
extern const uint32_t kHelpBarName;            // the data address of "COMPONENT_HELP_BUTTON_BAR"
// The resource heap and the resource manager's request queues (release_wait.cpp).
extern const uint32_t kResourceHeap;           // the data address of the track resource heap object
extern const uint32_t kResourceManagerPointer; // the data address of the resource manager pointer
extern const uint32_t kRequestThreads;         // resource manager: the number of per-thread request queues
extern const uint32_t kRequestThreadTable;     // resource manager + this: their thread ids
extern const uint32_t kRequestQueueFirst;      // the queue of thread entry i is at manager + (i + this) * kRequestQueueStride
extern const uint32_t kRequestQueueStride;     // (begin and end pointers in its first two words)
extern const uint32_t kCurrentThread;          // r13 + this: the current thread's block
extern const uint32_t kThreadId;               // thread block + this: the thread id the game's "current thread id" function returns
}  // namespace fm4::sym
