// Copyright 2026 The Notolibra Authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Importing another browser's cookies and local storage into a profile
// (ne_profile_import_cookies, ne_profile_import_local_storage). Writes go
// through the profile's own stores, never their files: the network
// service's cookie manager (its database is encrypted with the profile's
// key) and the storage service's local storage. Runs on the UI thread.
// Never logs names, values or hosts: counts only.

#ifndef NOTOLIBRA_ENGINE_PROFILE_IMPORT_H_
#define NOTOLIBRA_ENGINE_PROFILE_IMPORT_H_

#include <cstddef>

#include "ne_engine.h"

namespace content {
class BrowserContext;
}

namespace notolibra {

// Copies `cookies` at once; `done` runs on the UI thread after the cookie
// store has flushed.
void ImportCookies(content::BrowserContext* context,
                   const ne_cookie* cookies,
                   size_t count,
                   ne_cookie_import_fn done,
                   void* user_data);

// Copies `origins` at once; `done` runs on the UI thread after local storage
// has flushed.
void ImportLocalStorage(content::BrowserContext* context,
                        const ne_storage_origin* origins,
                        size_t count,
                        bool overwrite,
                        ne_storage_import_fn done,
                        void* user_data);

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_PROFILE_IMPORT_H_
