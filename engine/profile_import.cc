// Copyright 2026 The Notolibra Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "notolibra/engine/profile_import.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/barrier_closure.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/token.h"
#include "components/services/storage/public/mojom/local_storage_control.mojom.h"
#include "components/services/storage/public/mojom/storage_usage_info.mojom.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/schemeful_site.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_constants.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_partition_key.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace notolibra {

namespace {

std::string ToString(ne_string_view view) {
  return view.data ? std::string(view.data, view.length) : std::string();
}

base::Time FromUnixMicros(int64_t micros) {
  return micros > 0 ? base::Time::UnixEpoch() + base::Microseconds(micros)
                    : base::Time();
}

// Chromium caps a new cookie's lifetime at 400 days (and checks the cap
// against its creation date).
constexpr base::TimeDelta kMaxCookieAge = base::Days(400);

// ---------------------------------------------------------------------------
// Cookies

using CookieKey = std::tuple<std::optional<net::CookiePartitionKey>,
                             std::string,
                             std::string,
                             std::string>;

CookieKey KeyOf(const net::CanonicalCookie& cookie) {
  return {cookie.PartitionKey(), cookie.Name(), cookie.Domain(),
          cookie.Path()};
}

struct ImportedCookie {
  net::CanonicalCookie cookie;
  GURL source_url;
  // Its expiry was capped (kMaxCookieAge from now), so the profile's copy
  // from an earlier import has an earlier cap.
  bool expiry_capped = false;
};

enum class MakeResult { kOk, kExpired, kInvalid };

MakeResult MakeCookie(const ne_cookie& in,
                      base::Time now,
                      std::optional<ImportedCookie>* out) {
  std::string domain = base::ToLowerASCII(ToString(in.domain));
  std::string host = domain;
  if (!host.empty() && host[0] == '.') {
    host.erase(0, 1);
  }
  if (host.empty()) {
    return MakeResult::kInvalid;
  }
  std::string path = ToString(in.path);
  if (path.empty()) {
    path = "/";
  }

  base::Time expiry = FromUnixMicros(in.expiry_us);
  if (!expiry.is_null() && expiry <= now) {
    return MakeResult::kExpired;
  }
  bool capped = false;
  if (!expiry.is_null() && expiry > now + kMaxCookieAge) {
    expiry = now + kMaxCookieAge;
    capped = true;
  }
  base::Time creation = FromUnixMicros(in.creation_us);
  if (creation.is_null() || creation > now) {
    creation = now;
  }
  // The cap is checked against the creation date: a cookie a site has kept
  // setting for years is older than its expiry allows. Chromium itself
  // checks a replacement against the time it's set and keeps the old
  // creation date, which isn't possible from outside; move it forward.
  if (!expiry.is_null() && expiry - creation > kMaxCookieAge) {
    creation = expiry - kMaxCookieAge;
  }
  base::Time last_access = std::max(FromUnixMicros(in.last_access_us), creation);
  base::Time last_update = FromUnixMicros(in.last_update_us);
  if (last_update.is_null() || last_update > now) {
    last_update = now;
  }

  net::CookieSameSite same_site;
  switch (in.same_site) {
    case NE_COOKIE_SAME_SITE_NONE:
      same_site = net::CookieSameSite::NO_RESTRICTION;
      break;
    case NE_COOKIE_SAME_SITE_LAX:
      same_site = net::CookieSameSite::LAX_MODE;
      break;
    case NE_COOKIE_SAME_SITE_STRICT:
      same_site = net::CookieSameSite::STRICT_MODE;
      break;
    default:
      same_site = net::CookieSameSite::UNSPECIFIED;
  }
  net::CookieSourceScheme source_scheme;
  switch (in.source_scheme) {
    case NE_COOKIE_SCHEME_NON_SECURE:
      source_scheme = net::CookieSourceScheme::kNonSecure;
      break;
    case NE_COOKIE_SCHEME_SECURE:
      source_scheme = net::CookieSourceScheme::kSecure;
      break;
    default:
      source_scheme = net::CookieSourceScheme::kUnset;
  }
  // Set from https unless only http ever set it (and it isn't Secure).
  const char* scheme =
      source_scheme == net::CookieSourceScheme::kNonSecure && !in.secure
          ? url::kHttpScheme
          : url::kHttpsScheme;
  GURL source_url(std::string(scheme) + "://" + host + path);
  if (!source_url.is_valid()) {
    return MakeResult::kInvalid;
  }

  std::optional<net::CookiePartitionKey> partition_key;
  std::string partition_site = ToString(in.partition_site);
  if (!partition_site.empty()) {
    GURL top_level(partition_site);
    if (!top_level.is_valid()) {
      return MakeResult::kInvalid;
    }
    // Chromium's ancestor bit is "cross-site" whenever the cookie's site
    // isn't the top-level site; Firefox records only the ABA case.
    bool cross_site =
        in.partition_cross_site_ancestor ||
        net::SchemefulSite(top_level) !=
            net::SchemefulSite(GURL(std::string(url::kHttpsScheme) + "://" +
                                    host + "/"));
    auto key = net::CookiePartitionKey::FromUntrustedInput(
        net::SchemefulSite(top_level).Serialize(), cross_site);
    if (!key.has_value()) {
      return MakeResult::kInvalid;
    }
    partition_key = std::move(key).value();
  }

  std::unique_ptr<net::CanonicalCookie> cookie =
      net::CanonicalCookie::FromStorage(
          ToString(in.name), ToString(in.value), domain, path, creation,
          expiry, last_access, last_update, in.secure, in.http_only,
          same_site, net::COOKIE_PRIORITY_DEFAULT, partition_key,
          source_scheme, url::PORT_UNSPECIFIED, net::CookieSourceType::kOther,
          net::CanonicalCookieFromStorageCallSite::kCookieManager);
  if (!cookie) {
    return MakeResult::kInvalid;
  }
  out->emplace(ImportedCookie{*cookie, std::move(source_url), capped});
  return MakeResult::kOk;
}

bool SameCookie(const net::CanonicalCookie& here,
                const ImportedCookie& imported) {
  const net::CanonicalCookie& cookie = imported.cookie;
  if (here.Value() != cookie.Value() ||
      here.SecureAttribute() != cookie.SecureAttribute() ||
      here.IsHttpOnly() != cookie.IsHttpOnly() ||
      here.SameSite() != cookie.SameSite() ||
      here.ExpiryDate().is_null() != cookie.ExpiryDate().is_null()) {
    return false;
  }
  if (here.ExpiryDate() == cookie.ExpiryDate()) {
    return true;
  }
  // Capped again from a later "now": the same cookie.
  return imported.expiry_capped &&
         here.ExpiryDate() >= cookie.ExpiryDate() - base::Days(30);
}

class CookieImport : public base::RefCounted<CookieImport> {
 public:
  CookieImport(content::BrowserContext* context,
               ne_cookie_import_fn done,
               void* user_data)
      : done_(done), user_data_(user_data) {
    manager_ = context->GetDefaultStoragePartition()
                   ->GetCookieManagerForBrowserProcess();
    result_.size = sizeof(result_);
  }

  void Start(const ne_cookie* cookies, size_t count) {
    const base::Time now = base::Time::Now();
    // The last of several cookies with one key wins, as if set in order.
    std::map<CookieKey, size_t> index;
    for (size_t i = 0; i < count; ++i) {
      std::optional<ImportedCookie> cookie;
      switch (MakeCookie(UNSAFE_BUFFERS(cookies[i]), now, &cookie)) {
        case MakeResult::kExpired:
          ++result_.expired;
          continue;
        case MakeResult::kInvalid:
          ++result_.invalid;
          continue;
        case MakeResult::kOk:
          break;
      }
      CookieKey key = KeyOf(cookie->cookie);
      auto found = index.find(key);
      if (found != index.end()) {
        cookies_[found->second] = std::move(*cookie);
      } else {
        index.emplace(std::move(key), cookies_.size());
        cookies_.push_back(std::move(*cookie));
      }
    }
    if (!manager_) {
      Finish(NE_ERR_FAILED);
      return;
    }
    manager_->GetAllCookies(
        base::BindOnce(&CookieImport::OnExisting, base::WrapRefCounted(this)));
  }

 private:
  friend class base::RefCounted<CookieImport>;
  ~CookieImport() = default;

  void OnExisting(const std::vector<net::CanonicalCookie>& existing) {
    std::map<CookieKey, const net::CanonicalCookie*> here;
    for (const net::CanonicalCookie& cookie : existing) {
      here.emplace(KeyOf(cookie), &cookie);
    }
    std::vector<std::pair<const ImportedCookie*, bool>> to_set;
    for (const ImportedCookie& imported : cookies_) {
      auto found = here.find(KeyOf(imported.cookie));
      if (found == here.end()) {
        to_set.emplace_back(&imported, false);
        continue;
      }
      if (SameCookie(*found->second, imported)) {
        ++result_.unchanged;
      } else if (found->second->LastUpdateDate() >
                 imported.cookie.LastUpdateDate()) {
        ++result_.kept_newer;
      } else {
        to_set.emplace_back(&imported, true);
      }
    }
    base::RepeatingClosure barrier = base::BarrierClosure(
        to_set.size(),
        base::BindOnce(&CookieImport::Flush, base::WrapRefCounted(this)));
    net::CookieOptions options;
    options.set_include_httponly();
    options.set_same_site_cookie_context(
        net::CookieOptions::SameSiteCookieContext::MakeInclusiveForSet());
    options.set_do_not_update_access_time();
    for (const auto& [imported, replaces] : to_set) {
      manager_->SetCanonicalCookie(
          imported->cookie, imported->source_url, options,
          base::BindOnce(&CookieImport::OnSet, base::WrapRefCounted(this),
                         replaces, barrier));
    }
  }

  void OnSet(bool replaces,
             base::RepeatingClosure barrier,
             net::CookieAccessResult access_result) {
    if (!access_result.status.IsInclude()) {
      ++result_.refused;
    } else if (replaces) {
      ++result_.updated;
    } else {
      ++result_.added;
    }
    barrier.Run();
  }

  void Flush() {
    manager_->FlushCookieStore(base::BindOnce(
        &CookieImport::Finish, base::WrapRefCounted(this), NE_OK));
  }

  void Finish(ne_status status) {
    LOG(INFO) << "NotolibraEngine: imported cookies: " << result_.added
              << " added, " << result_.updated << " updated, "
              << result_.unchanged << " unchanged, " << result_.kept_newer
              << " newer here, " << result_.expired << " expired, "
              << result_.invalid << " invalid, " << result_.refused
              << " refused";
    if (done_) {
      done_(user_data_, status, &result_);
    }
  }

  raw_ptr<network::mojom::CookieManager> manager_ = nullptr;
  std::vector<ImportedCookie> cookies_;
  ne_cookie_import_result result_ = {};
  ne_cookie_import_fn done_;
  raw_ptr<void> user_data_;
};

// ---------------------------------------------------------------------------
// Local storage

using Bytes = std::vector<uint8_t>;

// Blink's local storage format (CachedStorageArea::StringToUint8Vector,
// kLocalStorageDetectFormat), for keys and values alike: a string of only
// Latin-1 characters is a 1 and its bytes, any other a 0 and its UTF-16
// code units.
Bytes EncodeDomString(ne_utf16_view text) {
  base::span<const uint16_t> units =
      text.data ? UNSAFE_BUFFERS(base::span(text.data, text.length))
                : base::span<const uint16_t>();
  bool latin1 = std::ranges::all_of(units, [](uint16_t u) { return u <= 0xFF; });
  Bytes bytes;
  if (latin1) {
    bytes.reserve(units.size() + 1);
    bytes.push_back(1);
    for (uint16_t unit : units) {
      bytes.push_back(static_cast<uint8_t>(unit));
    }
  } else {
    bytes.reserve(units.size() * 2 + 1);
    bytes.push_back(0);
    for (uint16_t unit : units) {
      bytes.push_back(static_cast<uint8_t>(unit & 0xFF));
      bytes.push_back(static_cast<uint8_t>(unit >> 8));
    }
  }
  return bytes;
}

class StorageImport : public base::RefCounted<StorageImport> {
 public:
  StorageImport(content::BrowserContext* context,
                bool overwrite,
                ne_storage_import_fn done,
                void* user_data)
      : overwrite_(overwrite), done_(done), user_data_(user_data) {
    control_ = context->GetDefaultStoragePartition()->GetLocalStorageControl();
    result_.size = sizeof(result_);
  }

  void Start(const ne_storage_origin* origins, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      const ne_storage_origin& in = UNSAFE_BUFFERS(origins[i]);
      GURL url(ToString(in.origin));
      url::Origin origin = url::Origin::Create(url);
      if (!url.is_valid() || origin.opaque() || !url.SchemeIsHTTPOrHTTPS()) {
        ++result_.invalid_origins;
        continue;
      }
      Origin entry{blink::StorageKey::CreateFirstParty(origin), url, {}};
      entry.items.reserve(in.item_count);
      for (size_t j = 0; j < in.item_count; ++j) {
        const ne_storage_item& item = UNSAFE_BUFFERS(in.items[j]);
        entry.items.emplace_back(EncodeDomString(item.key),
                                 EncodeDomString(item.value));
      }
      origins_.push_back(std::move(entry));
    }
    if (!control_) {
      Finish(NE_ERR_FAILED);
      return;
    }
    Next();
  }

 private:
  friend class base::RefCounted<StorageImport>;
  ~StorageImport() = default;

  struct Origin {
    blink::StorageKey key;
    GURL url;
    std::vector<std::pair<Bytes, Bytes>> items;
  };

  // One origin at a time, so the storage service loads one area at a time.
  void Next() {
    area_.reset();
    if (next_ >= origins_.size()) {
      // Flush has no reply; GetUsage runs on the same database sequence
      // after the commits Flush scheduled.
      control_->Flush();
      control_->GetUsage(base::BindOnce(
          [](scoped_refptr<StorageImport> self,
             std::vector<storage::mojom::StorageUsageInfoPtr>) {
            self->Finish(NE_OK);
          },
          base::WrapRefCounted(this)));
      return;
    }
    const Origin& origin = origins_[next_];
    control_->BindStorageArea(origin.key, area_.BindNewPipeAndPassReceiver());
    area_->GetAll(
        mojo::NullRemote(),
        base::BindOnce(&StorageImport::OnExisting, base::WrapRefCounted(this)));
  }

  void OnExisting(std::vector<blink::mojom::KeyValuePtr> existing) {
    const Origin& origin = origins_[next_];
    std::map<Bytes, Bytes> here;
    for (auto& entry : existing) {
      here.emplace(std::move(entry->key), std::move(entry->value));
    }
    std::vector<std::pair<const std::pair<Bytes, Bytes>*, std::optional<Bytes>>>
        to_put;
    for (const auto& item : origin.items) {
      auto found = here.find(item.first);
      if (found == here.end()) {
        to_put.emplace_back(&item, std::nullopt);
      } else if (found->second == item.second) {
        ++result_.unchanged;
      } else if (!overwrite_) {
        ++result_.kept;
      } else {
        to_put.emplace_back(&item, found->second);
      }
    }
    if (!to_put.empty()) {
      ++result_.origins;
    }
    base::RepeatingClosure barrier = base::BarrierClosure(
        to_put.size(), base::BindOnce(&StorageImport::OriginDone,
                                      base::WrapRefCounted(this)));
    for (auto& [item, old_value] : to_put) {
      bool replaces = old_value.has_value();
      auto source = blink::mojom::StorageAreaSource::New(
          origin.url, base::Token::CreateRandom());
      area_->Put(item->first, item->second, std::move(old_value),
                 std::move(source),
                 base::BindOnce(&StorageImport::OnPut,
                                base::WrapRefCounted(this), replaces, barrier));
    }
  }

  void OnPut(bool replaces, base::RepeatingClosure barrier, bool success) {
    if (!success) {
      ++result_.refused;
    } else if (replaces) {
      ++result_.updated;
    } else {
      ++result_.added;
    }
    barrier.Run();
  }

  void OriginDone() {
    ++next_;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&StorageImport::Next, base::WrapRefCounted(this)));
  }

  void Finish(ne_status status) {
    LOG(INFO) << "NotolibraEngine: imported local storage: "
              << result_.origins << " origins, " << result_.added
              << " added, " << result_.updated << " updated, "
              << result_.unchanged << " unchanged, " << result_.kept
              << " kept, " << result_.refused << " refused, "
              << result_.invalid_origins << " invalid origins";
    if (done_) {
      done_(user_data_, status, &result_);
    }
  }

  bool overwrite_;
  raw_ptr<storage::mojom::LocalStorageControl> control_ = nullptr;
  std::vector<Origin> origins_;
  size_t next_ = 0;
  mojo::Remote<blink::mojom::StorageArea> area_;
  ne_storage_import_result result_ = {};
  ne_storage_import_fn done_;
  raw_ptr<void> user_data_;
};

}  // namespace

void ImportCookies(content::BrowserContext* context,
                   const ne_cookie* cookies,
                   size_t count,
                   ne_cookie_import_fn done,
                   void* user_data) {
  auto import = base::MakeRefCounted<CookieImport>(context, done, user_data);
  import->Start(cookies, count);
}

void ImportLocalStorage(content::BrowserContext* context,
                        const ne_storage_origin* origins,
                        size_t count,
                        bool overwrite,
                        ne_storage_import_fn done,
                        void* user_data) {
  auto import = base::MakeRefCounted<StorageImport>(context, overwrite, done,
                                                    user_data);
  import->Start(origins, count);
}

}  // namespace notolibra
