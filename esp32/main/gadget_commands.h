/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "cJSON.h"

// Commands for boards with the Muse UI (CONFIG_MUSE_ENABLED), advertised in
// build_register_json() and dispatched from on_ws_command(). Each validates
// its params (NULL when there are none) and returns the command result.

// show_text {text}: shows text as the caption under the avatar and wakes the
// screen.
cJSON *gadget_show_text_command(const cJSON *params);

// set_mode {mode: "desk" | "night" | "on_the_go"}: picks the gadget mode, as
// the settings screen does (muse_gadget_mode.h).
cJSON *gadget_set_mode_command(const cJSON *params);

// set_chat {session_id?}: picks the Muse chat turns go to. Missing, empty or
// "main" is the main chat, "gadget" this gadget's own side chat
// (muse_settings_gadget_chat_sid), anything else that side chat's id.
cJSON *gadget_set_chat_command(const cJSON *params);
