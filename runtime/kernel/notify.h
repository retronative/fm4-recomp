// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// XAM notifications (imports_notify.cpp).
#pragma once
#include <cstdint>

namespace runtime {
// Queues (id, data) on every open listener whose mask and maximum version accept it (Xenia
// XNotifyListener::EnqueueNotification, xnotifylistener.cc), and signals the listener.
void EnqueueNotification(uint8_t* base, uint32_t id, uint32_t data);
}  // namespace runtime
