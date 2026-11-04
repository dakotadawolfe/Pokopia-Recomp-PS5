// SPDX-License-Identifier: GPL-3.0-or-later
// The native application has its own CRT instead of the payload loader's CRT.
extern "C" {
void* __dso_handle = &__dso_handle;
}
