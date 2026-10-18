// Copyright 2026 Notolibra. All rights reserved.
//
// A tab's interface surfaces as data (plan.md §2.6): file choosers, the
// page's context menu and its icon. Each request goes to the app through
// the UI delegate; the app's answer comes back through ne_ui_respond().

#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/download_request_utils.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "components/download/public/common/download_url_parameters.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "notolibra/engine/engine.h"
#include "notolibra/engine/tab.h"
#include "third_party/blink/public/common/context_menu_data/edit_flags.h"
#include "third_party/blink/public/mojom/context_menu/context_menu.mojom.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/gfx/codec/png_codec.h"

namespace notolibra {

namespace {

std::string ToString(ne_string_view view) {
  return view.data ? std::string(view.data, view.length) : std::string();
}

constexpr net::NetworkTrafficAnnotationTag kContextMenuDownload =
    net::DefineNetworkTrafficAnnotation("notolibra_context_menu_save", R"(
      semantics {
        sender: "Notolibra browser pane"
        description: "Saves a link or image the user chose in a page's context menu."
        trigger: "The user chooses Save Link or Save Image."
        data: "None."
        destination: WEBSITE
      }
      policy {
        cookies_allowed: YES
        setting: "None."
        policy_exception_justification: "Not implemented."
      })");

}  // namespace

void Tab::RunFileChooser(content::RenderFrameHost* render_frame_host,
                         scoped_refptr<content::FileSelectListener> listener,
                         const blink::mojom::FileChooserParams& params) {
  std::vector<std::string> accept;
  for (const std::u16string& type : params.accept_types) {
    accept.push_back(base::UTF16ToUTF8(type));
  }
  std::string accept_types;
  for (size_t i = 0; i < accept.size(); ++i) {
    accept_types += (i ? "," : "") + accept[i];
  }
  std::string origin = render_frame_host->GetLastCommittedOrigin().Serialize();
  std::string default_name = params.default_file_name.value();
  ne_ui_request request = {sizeof(ne_ui_request)};
  request.kind = NE_UI_FILE_CHOOSER;
  request.tab = handle_;
  request.origin = {origin.data(), origin.size()};
  request.message = {default_name.data(), default_name.size()};
  request.detail = {accept_types.data(), accept_types.size()};
  using Mode = blink::mojom::FileChooserParams::Mode;
  request.multiple = params.mode == Mode::kOpenMultiple;
  request.file_mode = params.mode == Mode::kOpenMultiple ? NE_FILE_OPEN_MULTIPLE
                      : params.mode == Mode::kUploadFolder ? NE_FILE_OPEN_FOLDER
                      : params.mode == Mode::kSave         ? NE_FILE_SAVE
                                                           : NE_FILE_OPEN;
  const Mode mode = params.mode;
  GetEngine()->SendUiRequest(
      request, this,
      base::BindOnce(
          [](scoped_refptr<content::FileSelectListener> listener, Mode mode,
             const ne_ui_response* response) {
            if (!response || !response->accept || response->path_count == 0) {
              listener->FileSelectionCanceled();
              return;
            }
            std::vector<blink::mojom::FileChooserFileInfoPtr> files;
            // SAFETY: the response carries path_count paths.
            auto paths = UNSAFE_BUFFERS(
                base::span(response->paths, response->path_count));
            for (const ne_string_view& path : paths) {
              files.push_back(blink::mojom::FileChooserFileInfo::NewNativeFile(
                  blink::mojom::NativeFileInfo::New(base::FilePath(ToString(path)),
                                                    std::u16string(), std::vector<std::u16string>())));
            }
            base::FilePath base_dir;
            if (mode == Mode::kUploadFolder && !paths.empty()) {
              base_dir = base::FilePath(ToString(paths[0]));
            }
            listener->FileSelected(std::move(files), base_dir, mode);
          },
          std::move(listener), mode));
}

bool Tab::HandleContextMenu(content::RenderFrameHost& render_frame_host,
                            const content::ContextMenuParams& params) {
  struct Item {
    int command;
    const char* label;
    bool enabled;
  };
  std::vector<Item> items;
  auto separator = [&] {
    if (!items.empty() && items.back().command != -1) {
      items.push_back({-1, "", false});
    }
  };
  const int flags = params.edit_flags;
  using MediaType = blink::mojom::ContextMenuDataMediaType;
  if (params.link_url.is_valid()) {
    items.push_back({NE_CONTEXT_OPEN_LINK_IN_NEW_TAB, "Open Link in New Tab", true});
    items.push_back({NE_CONTEXT_COPY_LINK, "Copy Link Address", true});
    items.push_back({NE_CONTEXT_SAVE_LINK, "Save Link As…", true});
  }
  if (params.media_type == MediaType::kImage && params.src_url.is_valid()) {
    separator();
    items.push_back({NE_CONTEXT_OPEN_IMAGE_IN_NEW_TAB, "Open Image in New Tab", true});
    items.push_back({NE_CONTEXT_COPY_IMAGE, "Copy Image", params.has_image_contents});
    items.push_back({NE_CONTEXT_COPY_IMAGE_URL, "Copy Image Address", true});
    items.push_back({NE_CONTEXT_SAVE_IMAGE, "Save Image As…", true});
  }
  if (params.is_editable) {
    separator();
    items.push_back({NE_CONTEXT_UNDO, "Undo", (flags & blink::ContextMenuDataEditFlags::kCanUndo) != 0});
    items.push_back({NE_CONTEXT_REDO, "Redo", (flags & blink::ContextMenuDataEditFlags::kCanRedo) != 0});
    separator();
    items.push_back({NE_CONTEXT_CUT, "Cut", (flags & blink::ContextMenuDataEditFlags::kCanCut) != 0});
    items.push_back({NE_CONTEXT_COPY, "Copy", (flags & blink::ContextMenuDataEditFlags::kCanCopy) != 0});
    items.push_back({NE_CONTEXT_PASTE, "Paste", (flags & blink::ContextMenuDataEditFlags::kCanPaste) != 0});
    items.push_back({NE_CONTEXT_PASTE_AND_MATCH_STYLE, "Paste and Match Style", (flags & blink::ContextMenuDataEditFlags::kCanPaste) != 0});
    items.push_back({NE_CONTEXT_SELECT_ALL, "Select All", (flags & blink::ContextMenuDataEditFlags::kCanSelectAll) != 0});
  } else if (!params.selection_text.empty()) {
    separator();
    items.push_back({NE_CONTEXT_COPY, "Copy", true});
  }
  if (!params.link_url.is_valid() && params.media_type == MediaType::kNone &&
      !params.is_editable && params.selection_text.empty()) {
    auto& controller = web_contents_->GetController();
    items.push_back({NE_CONTEXT_BACK, "Back", controller.CanGoBack()});
    items.push_back({NE_CONTEXT_FORWARD, "Forward", controller.CanGoForward()});
    items.push_back({NE_CONTEXT_RELOAD, "Reload", true});
  }
  if (!items.empty() && items.back().command == -1) {
    items.pop_back();
  }
  std::vector<ne_menu_item> out;
  for (const Item& item : items) {
    ne_menu_item entry = {};
    entry.command_id = item.command;
    entry.label = {item.label, strlen(item.label)};
    entry.enabled = item.enabled;
    entry.submenu_index = -1;
    entry.kind = item.command == -1 ? NE_MENU_ITEM_SEPARATOR : NE_MENU_ITEM_COMMAND;
    out.push_back(entry);
  }
  std::string link = params.link_url.is_valid() ? params.link_url.spec() : "";
  std::string src = params.src_url.is_valid() ? params.src_url.spec() : "";
  std::string selection = base::UTF16ToUTF8(params.selection_text);
  std::string origin = render_frame_host.GetLastCommittedOrigin().Serialize();
  ne_ui_request request = {sizeof(ne_ui_request)};
  request.kind = NE_UI_CONTEXT_MENU;
  request.tab = handle_;
  request.origin = {origin.data(), origin.size()};
  request.message = {selection.data(), selection.size()};
  request.url = {link.data(), link.size()};
  request.src_url = {src.data(), src.size()};
  request.anchor = {static_cast<double>(params.x), static_cast<double>(params.y), 0, 0};
  request.items = out.data();
  request.item_count = out.size();
  request.is_editable = params.is_editable;
  switch (params.media_type) {
    case MediaType::kImage:
      request.media_type = NE_MEDIA_IMAGE;
      break;
    case MediaType::kVideo:
      request.media_type = NE_MEDIA_VIDEO;
      break;
    case MediaType::kAudio:
      request.media_type = NE_MEDIA_AUDIO;
      break;
    case MediaType::kCanvas:
      request.media_type = NE_MEDIA_CANVAS;
      break;
    case MediaType::kNone:
      request.media_type = NE_MEDIA_NONE;
      break;
    default:
      request.media_type = NE_MEDIA_OTHER;
      break;
  }
  GetEngine()->SendUiRequest(
      request, this,
      base::BindOnce(
          [](base::WeakPtr<Tab> self, content::GlobalRenderFrameHostId frame,
             content::ContextMenuParams params, const ne_ui_response* response) {
            if (self && response && response->accept) {
              self->RunContextCommand(response->command_id, frame, params);
            }
          },
          weak_factory_.GetWeakPtr(), render_frame_host.GetGlobalId(), params));
  return true;
}

void Tab::RunContextCommand(int command,
                            content::GlobalRenderFrameHostId frame_id,
                            const content::ContextMenuParams& params) {
  content::RenderFrameHost* frame = content::RenderFrameHost::FromID(frame_id);
  auto& controller = web_contents_->GetController();
  auto download = [&](const GURL& url) {
    if (!url.is_valid()) {
      return;
    }
    web_contents_->GetBrowserContext()->GetDownloadManager()->DownloadUrl(
        content::DownloadRequestUtils::CreateDownloadForWebContentsMainFrame(
            web_contents_.get(), url, kContextMenuDownload));
  };
  switch (command) {
    case NE_CONTEXT_BACK:
      if (controller.CanGoBack()) {
        controller.GoBack();
      }
      break;
    case NE_CONTEXT_FORWARD:
      if (controller.CanGoForward()) {
        controller.GoForward();
      }
      break;
    case NE_CONTEXT_RELOAD:
      controller.Reload(content::ReloadType::NORMAL, false);
      break;
    case NE_CONTEXT_COPY_LINK: {
      ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
      writer.WriteText(base::UTF8ToUTF16(params.link_url.spec()));
      break;
    }
    case NE_CONTEXT_SAVE_LINK:
      download(params.link_url);
      break;
    case NE_CONTEXT_COPY_IMAGE:
      if (frame) {
        frame->CopyImageAt(params.x, params.y);
      }
      break;
    case NE_CONTEXT_COPY_IMAGE_URL: {
      ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
      writer.WriteText(base::UTF8ToUTF16(params.src_url.spec()));
      break;
    }
    case NE_CONTEXT_SAVE_IMAGE:
      download(params.src_url);
      break;
    case NE_CONTEXT_UNDO:
      web_contents_->Undo();
      break;
    case NE_CONTEXT_REDO:
      web_contents_->Redo();
      break;
    case NE_CONTEXT_CUT:
      web_contents_->Cut();
      break;
    case NE_CONTEXT_COPY:
      web_contents_->Copy();
      break;
    case NE_CONTEXT_PASTE:
      web_contents_->Paste();
      break;
    case NE_CONTEXT_PASTE_AND_MATCH_STYLE:
      web_contents_->PasteAndMatchStyle();
      break;
    case NE_CONTEXT_SELECT_ALL:
      web_contents_->SelectAll();
      break;
    default:
      // The app's own commands (opening in a new tab): nothing to do here.
      break;
  }
}

void Tab::DidUpdateFaviconURL(
    content::RenderFrameHost* render_frame_host,
    const std::vector<blink::mojom::FaviconURLPtr>& candidates,
    blink::mojom::FaviconUpdateReason reason) {
  Engine* engine = GetEngine();
  if (!engine->has_ui_delegate || !engine->ui_delegate.on_favicon) {
    return;
  }
  for (const auto& candidate : candidates) {
    if (candidate->icon_type == blink::mojom::FaviconIconType::kFavicon &&
        candidate->icon_url.is_valid()) {
      // 32 pixels: a 16-point icon at 2x.
      web_contents_->DownloadImage(
          candidate->icon_url, /*is_favicon=*/true, gfx::Size(32, 32),
          /*max_bitmap_size=*/64, /*bypass_cache=*/false,
          base::BindOnce(&Tab::FaviconDownloaded, weak_factory_.GetWeakPtr()));
      return;
    }
  }
}

void Tab::FaviconDownloaded(int id,
                            int http_status_code,
                            const GURL& image_url,
                            const std::vector<SkBitmap>& bitmaps,
                            const std::vector<gfx::Size>& sizes) {
  Engine* engine = GetEngine();
  if (bitmaps.empty() || !engine->has_ui_delegate ||
      !engine->ui_delegate.on_favicon) {
    return;
  }
  // The bitmap closest to 32 pixels.
  const SkBitmap* best = &bitmaps[0];
  for (const SkBitmap& bitmap : bitmaps) {
    if (std::abs(bitmap.width() - 32) < std::abs(best->width() - 32)) {
      best = &bitmap;
    }
  }
  std::optional<std::vector<uint8_t>> png =
      gfx::PNGCodec::EncodeBGRASkBitmap(*best, /*discard_transparency=*/false);
  if (!png) {
    return;
  }
  std::string url = image_url.spec();
  engine->ui_delegate.on_favicon(engine->ui_delegate.user_data, handle_,
                                 {url.data(), url.size()}, png->data(),
                                 png->size());
}

}  // namespace notolibra
