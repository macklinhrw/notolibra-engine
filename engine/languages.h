// Copyright 2026 The Notolibra Authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The languages pages are offered, from the system's preferred languages
// (System Settings > General > Language & Region), as Chrome derives its
// accept-languages: one list for navigator.languages and the
// Accept-Language header alike, so the two agree.

#ifndef NOTOLIBRA_ENGINE_LANGUAGES_H_
#define NOTOLIBRA_ENGINE_LANGUAGES_H_

#include <string>

namespace notolibra {

// "en-US,en,fr-FR,fr": each preferred language, then its base language, as
// Chrome's accept-languages preference reads (renderer preferences,
// navigator.languages). Computed once.
const std::string& AcceptLanguages();

// The Accept-Language header for AcceptLanguages(), with Chrome's q-values:
// "en-US,en;q=0.9,fr-FR;q=0.8,fr;q=0.7".
std::string AcceptLanguageHeader();

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_LANGUAGES_H_
