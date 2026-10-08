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
// the settings screen does (muse_gadget_mode.h). Nothing is sent: each chat
// hears the mode with its next message.
cJSON *gadget_set_mode_command(const cJSON *params);

// set_chat {session_id? | name?}: picks the Muse chat turns go to. A name picks
// the named chat kept on the device (muse_settings_chats), keeping a new one
// first if none has it (created: true). A session_id that's missing, empty or
// "main" is the main chat, "gadget" this gadget's own side chat
// (muse_settings_gadget_chat_sid), and a UUID that side chat. Returns
// {chat: main | gadget | named | new | custom, session_id, name?, told_mode,
// created?}, told_mode being the mode that chat last heard, or null.
cJSON *gadget_set_chat_command(const cJSON *params);

// list_chats: the named chats kept on the device, [{name, session_id,
// told_mode}], and the current one as set_chat returns it. Muse itself can't
// list its chats.
cJSON *gadget_list_chats_command(const cJSON *params);
