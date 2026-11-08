// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/objects.h (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
//
// Kernel objects behind handles (M3-T05): each object is a guest dispatcher object (its header in
// the kernel arena, so waits use dispatcher.h), a type, a reference count and an optional name
// (OBJECT_ATTRIBUTES). REFERENCE_DERIVED from Xenia (xenia-canary 67f16c4e315f: kernel/xobject.h
// X_OBJECT_ATTRIBUTES, X_ANSI_STRING; util/object_table.cc; xboxkrnl_threading.cc NtCreateEvent),
// re-derived. Handle values are this runtime's (Xenia's are its own too).
#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace runtime {

// Title: an object of a title-defined type (ObCreateObject; its OBJECT_TYPE pointer at body - 8).
enum class ObjectType { Event, Thread, Semaphore, File, Enumerator, NotifyListener, Timer, Mutant, Title };

struct KernelObject { ObjectType type; uint32_t header; };

// Creates an object with one handle; returns the handle.
uint32_t CreateObject(ObjectType type, uint32_t header, const std::string& name);
std::optional<KernelObject> LookupHandle(uint32_t handle);
std::optional<uint32_t> LookupName(const std::string& name, ObjectType& type);  // a new handle to it
bool CloseHandle(uint32_t handle);
bool IsObjectBody(uint32_t header);
std::optional<uint32_t> DuplicateHandle(uint32_t handle);   // a new handle to the same object
uint32_t HandleForObject(ObjectType type, uint32_t header);  // a new handle to an object by its body  // the guest body of an object some handle names (or named)

// The name in an X_OBJECT_ATTRIBUTES (root directory, X_ANSI_STRING*, attributes), or empty.
std::string ObjectName(const uint8_t* base, uint32_t attributes);

namespace status { constexpr uint32_t kSuccess = 0, kObjectNameExists = 0x40000000, kInvalidHandle = 0xC0000008,
                                      kObjectTypeMismatch = 0xC0000024; }

}  // namespace runtime
