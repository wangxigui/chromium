// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef VIEWS_FOCUS_FOCUS_UTIL_WIN_H_
#define VIEWS_FOCUS_FOCUS_UTIL_WIN_H_
#pragma once

#include <windows.h>

#include "views/views_api.h"

namespace ui {
class ViewProp;
}

namespace views {

// Marks the passed |hwnd| as supporting mouse-wheel message rerouting.
// We reroute the mouse wheel messages to such HWND when they are under the
// mouse pointer (but are not the active window). Callers own the returned
// object.
VIEWS_API ui::ViewProp* SetWindowSupportsRerouteMouseWheel(HWND hwnd);

// Forwards mouse wheel messages to the window under it.
// Windows sends mouse wheel messages to the currently active window.
// This causes a window to scroll even if it is not currently under the mouse
// wheel. The following code gives mouse wheel messages to the window under the
// mouse wheel in order to scroll that window. This is arguably a better user
// experience.  The returns value says whether the mouse wheel message was
// successfully redirected.
VIEWS_API bool RerouteMouseWheel(HWND window, WPARAM w_param, LPARAM l_param);

}   // namespace views

#endif  // VIEWS_FOCUS_FOCUS_UTIL_WIN_H_
