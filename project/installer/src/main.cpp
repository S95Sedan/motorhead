#include "retail_install.hpp"
#include "ui.hpp"

#include <windows.h>

#include <dbt.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace {

using motorhead::RetailSource;
using motorhead::RetailSourceKind;
using motorhead::InstallPhase;
using motorhead::install_phase_count;

constexpr UINT message_status = WM_APP + 1U;
constexpr UINT message_progress = WM_APP + 2U;
constexpr UINT message_phase = WM_APP + 3U;
constexpr UINT message_finished = WM_APP + 4U;

// The owned retail disc and packaged 3.0 assets are fixed inputs. Keep a
// conservative amount of headroom without scanning the cabinet or opening the
// raw CD-audio device merely to paint the Location page.
constexpr std::uint64_t estimated_content_bytes = 220U * 1024U * 1024U;
constexpr std::uint64_t estimated_audio_bytes = 430U * 1024U * 1024U;
constexpr std::uint64_t estimated_s40_bytes = 170U * 1024U * 1024U;

constexpr int id_back = 1001;
constexpr int id_next = 1002;
constexpr int id_quit = 1003;
constexpr int id_browse = 1005;
constexpr int id_destination = 1006;
constexpr int id_music = 1007;
constexpr int id_reset = 1008;
constexpr int id_open_folder = 1009;
constexpr int id_browse_image = 1010;
constexpr int id_browse_patch = 1011;
constexpr int id_s40 = 1012;
constexpr int id_browse_s40 = 1013;

constexpr UINT_PTR timer_scan = 1U;
constexpr UINT_PTR timer_elapsed = 2U;

// Logical (96 dpi) layout metrics. Everything is scaled through ui::scaled.
constexpr int layout_width = 880;
constexpr int layout_height = 660;
constexpr int rule_height = 3;
constexpr int rail_width = 232;
constexpr int footer_height = 74;
constexpr int page_padding = 34;
constexpr int button_width = 116;
constexpr int button_height = 36;
constexpr int card_height = 74;
constexpr int field_height = 38;

enum class Page {
  welcome = 0,
  disc,
  location,
  options,
  ready,
  installing,
  finished
};

enum class Outcome { none, success, cancelled, failed };

struct StepLabel {
  const wchar_t *name;
  const wchar_t *hint;
};

constexpr std::array<StepLabel, 6U> steps{{
    {L"Welcome", L"Introduction"},
    {L"Game source", L"CD-ROM or CUE/BIN"},
    {L"Location", L"Install folder"},
    {L"Options", L"Music and player data"},
    {L"Ready", L"Review your choices"},
    {L"Install", L"Install Motorhead"},
}};

constexpr std::array<const wchar_t *, install_phase_count> phase_labels{
    L"Extract the retail game data",
    L"Apply the official 3.0 content update",
    L"Copy the movie archive",
    L"Read the CD soundtrack",
    L"Install optional S40 Racing content",
    L"Validate the installed data",
    L"Install the program files",
};

int step_of(const Page page) {
  const auto index = static_cast<int>(page);
  return std::min(index, static_cast<int>(steps.size()) - 1);
}

std::wstring format_bytes(const std::uint64_t bytes) {
  std::array<wchar_t, 64U> buffer{};
  const auto value = static_cast<double>(bytes);
  if (bytes >= (1ULL << 30U)) {
    std::swprintf(buffer.data(), buffer.size(), L"%.2f GB",
                  value / (1024.0 * 1024.0 * 1024.0));
  } else {
    std::swprintf(buffer.data(), buffer.size(), L"%.0f MB",
                  value / (1024.0 * 1024.0));
  }
  return buffer.data();
}

std::wstring format_duration(const std::uint64_t seconds) {
  std::array<wchar_t, 64U> buffer{};
  std::swprintf(buffer.data(), buffer.size(), L"%llu:%02llu",
                static_cast<unsigned long long>(seconds / 60U),
                static_cast<unsigned long long>(seconds % 60U));
  return buffer.data();
}

std::wstring window_text(const HWND control) {
  const auto length = GetWindowTextLengthW(control);
  std::wstring result(static_cast<std::size_t>(length) + 1U, L'\0');
  const auto copied = GetWindowTextW(control, result.data(), length + 1);
  result.resize(static_cast<std::size_t>(std::max(copied, 0)));
  return result;
}

int CALLBACK browse_callback(const HWND dialog, const UINT message,
                             const LPARAM, const LPARAM data) {
  if (message == BFFM_INITIALIZED && data != 0) {
    SendMessageW(dialog, BFFM_SETSELECTIONW, TRUE, data);
  }
  return 0;
}

std::wstring select_installation_folder(const HWND owner,
                                        const std::wstring &initial) {
  BROWSEINFOW browse{};
  browse.hwndOwner = owner;
  browse.lpszTitle = L"Select the folder where Motorhead will be installed";
  browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX |
                   BIF_VALIDATE;
  browse.lpfn = browse_callback;
  browse.lParam = reinterpret_cast<LPARAM>(initial.c_str());
  const auto item = SHBrowseForFolderW(&browse);
  if (item == nullptr) {
    return {};
  }
  std::array<wchar_t, 32768U> path{};
  const auto valid = SHGetPathFromIDListW(item, path.data()) != FALSE;
  CoTaskMemFree(item);
  return valid ? std::wstring(path.data()) : std::wstring{};
}

std::wstring select_cue_image(const HWND owner) {
  const auto initial_directory = motorhead::executable_directory().wstring();
  std::array<wchar_t, 32768U> path{};
  constexpr wchar_t filter[] =
      L"CUE sheet (*.cue)\0*.cue\0All files (*.*)\0*.*\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = filter;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrTitle = L"Select the retail Motorhead CUE sheet";
  dialog.lpstrInitialDir = initial_directory.c_str();
  dialog.lpstrDefExt = L"cue";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                 OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
  return GetOpenFileNameW(&dialog) != FALSE ? std::wstring(path.data())
                                            : std::wstring{};
}

std::wstring select_s40_cue_image(const HWND owner) {
  const auto initial_directory = motorhead::executable_directory().wstring();
  std::array<wchar_t, 32768U> path{};
  constexpr wchar_t filter[] =
      L"S40 Racing CUE sheet (*.cue)\0*.cue\0All files (*.*)\0*.*\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = filter;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrTitle = L"Select the optional S40 Racing CUE sheet";
  dialog.lpstrInitialDir = initial_directory.c_str();
  dialog.lpstrDefExt = L"cue";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                 OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
  return GetOpenFileNameW(&dialog) != FALSE ? std::wstring(path.data())
                                            : std::wstring{};
}

std::wstring select_motorhead_patch(const HWND owner) {
  const auto initial_directory = motorhead::executable_directory().wstring();
  std::array<wchar_t, 32768U> path{};
  constexpr wchar_t filter[] =
      L"Motorhead 3.0 update (mhp30.exe)\0mhp30.exe\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = filter;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrTitle = L"Select the official Motorhead 3.0 update";
  dialog.lpstrInitialDir = initial_directory.c_str();
  dialog.lpstrDefExt = L"exe";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                 OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
  return GetOpenFileNameW(&dialog) != FALSE ? std::wstring(path.data())
                                            : std::wstring{};
}

void use_dark_title_bar(const HWND window) {
  const auto module = LoadLibraryW(L"dwmapi.dll");
  if (module == nullptr) {
    return;
  }
  using SetAttributeFn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
  const auto set_attribute = reinterpret_cast<SetAttributeFn>(
      reinterpret_cast<void *>(
          GetProcAddress(module, "DwmSetWindowAttribute")));
  if (set_attribute != nullptr) {
    const BOOL enabled = TRUE;
    if (FAILED(set_attribute(window, 20U, &enabled, sizeof(enabled)))) {
      set_attribute(window, 19U, &enabled, sizeof(enabled));
    }
  }
  FreeLibrary(module);
}

class Application final {
public:
  int run(const HINSTANCE instance, const int show) {
    instance_ = instance;
    const auto ole_result = OleInitialize(nullptr);
    const auto ole_initialized = SUCCEEDED(ole_result);
    ui::register_classes(instance);

    constexpr wchar_t class_name[] = L"MotorheadRetailInstaller";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    window_class.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(101));
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = class_name;
    window_class.lpfnWndProc = &Application::window_proc;
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    if (RegisterClassExW(&window_class) == 0U) {
      return 1;
    }
    window_ = CreateWindowExW(
        WS_EX_CONTROLPARENT, class_name, L"Motorhead Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
            WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, instance,
        this);
    if (window_ == nullptr) {
      return 1;
    }
    use_dark_title_bar(window_);
    resize_to_layout();
    ShowWindow(window_, show);
    UpdateWindow(window_);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0U, 0U) > 0) {
      if (IsDialogMessageW(window_, &message) != FALSE) {
        continue;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    if (ole_initialized) {
      OleUninitialize();
    }
    return static_cast<int>(message.wParam);
  }

private:
  static LRESULT CALLBACK window_proc(const HWND window, const UINT message,
                                      const WPARAM wparam,
                                      const LPARAM lparam) {
    auto *self = reinterpret_cast<Application *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      const auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
      self = static_cast<Application *>(create->lpCreateParams);
      self->window_ = window;
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
    }
    return self != nullptr ? self->handle(message, wparam, lparam)
                           : DefWindowProcW(window, message, wparam, lparam);
  }

  int px(const int value) const { return ui::scaled(value, dpi_); }

  // ---------------------------------------------------------------- setup

  void create_resources() {
    fonts_.create(dpi_);
    window_brush_ = CreateSolidBrush(ui::colour::window);
    rail_brush_ = CreateSolidBrush(ui::colour::rail);
    footer_brush_ = CreateSolidBrush(ui::colour::footer);
    field_brush_ = CreateSolidBrush(ui::colour::field);
    banner_ = static_cast<HBITMAP>(LoadImageW(instance_, MAKEINTRESOURCEW(102),
                                              IMAGE_BITMAP, 0, 0,
                                              LR_CREATEDIBSECTION));
    BITMAP description{};
    if (banner_ != nullptr &&
        GetObjectW(banner_, sizeof(description), &description) != 0) {
      banner_size_ = {description.bmWidth, description.bmHeight};
    }
  }

  void destroy_resources() {
    fonts_.destroy();
    for (auto *brush : {&window_brush_, &rail_brush_, &footer_brush_,
                        &field_brush_}) {
      if (*brush != nullptr) {
        DeleteObject(*brush);
        *brush = nullptr;
      }
    }
    if (banner_ != nullptr) {
      DeleteObject(banner_);
      banner_ = nullptr;
    }
  }

  void create_controls() {
    back_ = ui::create_button(window_, id_back, L"Back",
                              ui::ButtonKind::secondary, fonts_.body, dpi_);
    next_ = ui::create_button(window_, id_next, L"Next",
                              ui::ButtonKind::primary, fonts_.body_strong,
                              dpi_);
    quit_ = ui::create_button(window_, id_quit, L"Cancel",
                              ui::ButtonKind::secondary, fonts_.body, dpi_);
    browse_image_ = ui::create_button(window_, id_browse_image,
                                      L"Choose game media...",
                                      ui::ButtonKind::secondary, fonts_.body,
                                      dpi_);
    browse_patch_ = ui::create_button(window_, id_browse_patch,
                                      L"Choose update...",
                                      ui::ButtonKind::secondary, fonts_.body,
                                      dpi_);
    browse_ = ui::create_button(window_, id_browse, L"Browse...",
                                ui::ButtonKind::secondary, fonts_.body, dpi_);
    destination_ = CreateWindowExW(
        0U, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0,
        window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id_destination)),
        instance_, nullptr);
    SendMessageW(destination_, WM_SETFONT,
                 reinterpret_cast<WPARAM>(fonts_.body), FALSE);
    music_ = ui::create_check(
        window_, id_music, L"Install the original CD soundtrack",
        L"Tracks 2 to 11 are read from the CD or BIN into Music. This is the "
        L"slowest part of the installation.",
        fonts_.body_strong, fonts_.caption, dpi_);
    ui::set_checked(music_, true);
    s40_ = ui::create_check(
        window_, id_s40, L"Install optional S40 Racing content",
        L"Not selected. Adds the Volvo, S40 menu background and its three "
        L"music tracks without copying Motorhead's Nolby or Okkun tracks.",
        fonts_.body_strong, fonts_.caption, dpi_);
    SendMessageW(s40_, ui::message_set_right_inset, 178U, 0);
    browse_s40_ = ui::create_button(window_, id_browse_s40,
                                    L"Choose S40 image...",
                                    ui::ButtonKind::secondary, fonts_.body,
                                    dpi_);
    SetWindowLongPtrW(
        s40_, GWL_STYLE,
        GetWindowLongPtrW(s40_, GWL_STYLE) | WS_CLIPSIBLINGS);
    SetWindowLongPtrW(
        browse_s40_, GWL_STYLE,
        GetWindowLongPtrW(browse_s40_, GWL_STYLE) | WS_CLIPSIBLINGS);
    reset_ = ui::create_check(
        window_, id_reset, L"Reset player settings and saved games",
        L"Clears the User folder. Leave this off to keep existing profiles, "
        L"high scores and leagues.",
        fonts_.body_strong, fonts_.caption, dpi_);
    open_folder_ = ui::create_check(
        window_, id_open_folder, L"Open the installation folder", L"",
        fonts_.body_strong, fonts_.caption, dpi_);
    ui::set_checked(open_folder_, true);

    destination_path_ = motorhead::default_installation_directory().wstring();
    SetWindowTextW(destination_, destination_path_.c_str());
    detect_motorhead_patch();
    refresh_drives(false);
    apply_page();
  }

  void update_control_fonts() {
    for (const auto control :
         {back_, quit_, browse_image_, browse_patch_, browse_s40_, browse_}) {
      SendMessageW(control, WM_SETFONT,
                   reinterpret_cast<WPARAM>(fonts_.body), FALSE);
      ui::set_control_dpi(control, dpi_);
    }
    SendMessageW(next_, WM_SETFONT,
                 reinterpret_cast<WPARAM>(fonts_.body_strong), FALSE);
    ui::set_control_dpi(next_, dpi_);
    SendMessageW(destination_, WM_SETFONT,
                 reinterpret_cast<WPARAM>(fonts_.body), FALSE);
    for (const auto control : {music_, s40_, reset_, open_folder_}) {
      SendMessageW(control, WM_SETFONT,
                   reinterpret_cast<WPARAM>(fonts_.body_strong), FALSE);
      SendMessageW(control, ui::message_set_description_font,
                   reinterpret_cast<WPARAM>(fonts_.caption), 0);
      ui::set_control_dpi(control, dpi_);
    }
  }

  void resize_to_layout() {
    RECT bounds{0, 0, px(layout_width), px(layout_height)};
    AdjustWindowRectEx(&bounds,
                       static_cast<DWORD>(GetWindowLongPtrW(window_,
                                                            GWL_STYLE)),
                       FALSE, WS_EX_CONTROLPARENT);
    const auto width = bounds.right - bounds.left;
    const auto height = bounds.bottom - bounds.top;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0U, &work, 0U);
    const auto x = work.left + std::max<LONG>(0, (work.right - work.left -
                                                  width) / 2);
    const auto y = work.top + std::max<LONG>(0, (work.bottom - work.top -
                                                 height) / 2);
    SetWindowPos(window_, nullptr, static_cast<int>(x), static_cast<int>(y),
                 width, height, SWP_NOZORDER | SWP_NOACTIVATE);
  }

  // --------------------------------------------------------------- layout

  void layout() {
    RECT client{};
    GetClientRect(window_, &client);
    const auto width = client.right;
    const auto height = client.bottom;
    auto banner_height = px(150);
    if (banner_size_.cx > 0 && banner_size_.cy > 0) {
      banner_height = MulDiv(width, banner_size_.cy, banner_size_.cx);
    }
    banner_rect_ = {0, 0, width, banner_height};
    rule_rect_ = {0, banner_height, width, banner_height + px(rule_height)};
    footer_rect_ = {0, height - px(footer_height), width, height};
    rail_rect_ = {0, rule_rect_.bottom, px(rail_width), footer_rect_.top};
    body_rect_ = {rail_rect_.right, rule_rect_.bottom, width, footer_rect_.top};
    content_rect_ = body_rect_;
    InflateRect(&content_rect_, -px(page_padding), -px(page_padding));

    const auto button_w = px(button_width);
    const auto button_h = px(button_height);
    const auto top = footer_rect_.top + (px(footer_height) - button_h) / 2;
    const auto right = width - px(28);
    const auto gap = px(10);
    place(quit_, right - button_w, top, button_w, button_h);
    place(next_, right - button_w * 2 - px(24), top, button_w, button_h);
    place(back_, right - button_w * 3 - px(24) - gap, top, button_w, button_h);

    const auto source_card_top = content_rect_.top + px(119);
    place(browse_image_, content_rect_.right - px(174),
          source_card_top + px(20), px(158), button_h);
    const auto patch_top = content_rect_.bottom - px(76);
    place(browse_patch_, content_rect_.right - px(174), patch_top + px(20),
          px(158), button_h);

    const auto field_top = content_rect_.top + px(108);
    const auto browse_w = px(120);
    field_rect_ = {content_rect_.left + px(16), field_top,
                   content_rect_.right - px(16) - browse_w - gap,
                   field_top + px(field_height)};
    const auto edit_height = px(22);
    place(destination_, field_rect_.left + px(12),
          field_rect_.top + (px(field_height) - edit_height) / 2,
          field_rect_.right - field_rect_.left - px(24), edit_height);
    place(browse_, field_rect_.right + gap, field_top, browse_w,
          px(field_height));

    const auto option_height = px(68);
    place(music_, content_rect_.left, content_rect_.top + px(92),
          content_rect_.right - content_rect_.left, option_height);
    place(s40_, content_rect_.left,
          content_rect_.top + px(92) + option_height + px(8),
          content_rect_.right - content_rect_.left, option_height);
    place(browse_s40_, content_rect_.right - px(174),
          content_rect_.top + px(92) + option_height + px(24), px(158),
          button_h);
    // The S40 checkbox paints the full card background. Keep its browse
    // button above that sibling so the card cannot repaint over the button.
    SetWindowPos(browse_s40_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    place(reset_, content_rect_.left,
          content_rect_.top + px(92) + option_height * 2 + px(16),
          content_rect_.right - content_rect_.left, option_height);
    place(open_folder_, content_rect_.left, content_rect_.bottom - px(52),
          content_rect_.right - content_rect_.left, px(44));
  }

  static void place(const HWND control, const int x, const int y,
                    const int width, const int height) {
    if (control != nullptr) {
      SetWindowPos(control, nullptr, x, y, width, height,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }

  void apply_page() {
    const auto installing = page_ == Page::installing;
    show(browse_image_, page_ == Page::disc);
    show(browse_patch_, page_ == Page::disc);
    show(destination_, page_ == Page::location);
    show(browse_, page_ == Page::location);
    show(music_, page_ == Page::options);
    show(s40_, page_ == Page::options);
    show(browse_s40_, page_ == Page::options);
    show(reset_, page_ == Page::options);
    show(open_folder_,
         page_ == Page::finished && outcome_ == Outcome::success);
    show(back_, page_ == Page::location || page_ == Page::options ||
                    page_ == Page::ready || page_ == Page::disc ||
                    (page_ == Page::finished && outcome_ != Outcome::success));
    show(quit_, page_ != Page::finished);

    const wchar_t *next_text = L"Next";
    if (page_ == Page::ready) {
      next_text = L"Install";
    } else if (page_ == Page::finished) {
      next_text = L"Finish";
    } else if (page_ == Page::installing) {
      next_text = L"Installing";
    }
    SetWindowTextW(next_, next_text);
    EnableWindow(next_, next_enabled() ? TRUE : FALSE);
    EnableWindow(back_, installing ? FALSE : TRUE);
    InvalidateRect(window_, nullptr, FALSE);
  }

  [[nodiscard]] bool next_enabled() const {
    switch (page_) {
    case Page::disc:
      return selected_ >= 0 &&
             static_cast<std::size_t>(selected_) < drives_.size() &&
             !patch_path_.empty();
    case Page::location:
      return !window_text(destination_).empty();
    case Page::options:
      return !ui::is_checked(s40_) || !s40_cue_path_.empty();
    case Page::installing:
      return false;
    default:
      return true;
    }
  }

  static void show(const HWND control, const bool visible) {
    if (control != nullptr) {
      ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    }
  }

  void set_page(const Page page) {
    if (page_ == page) {
      return;
    }
    if (page_ == Page::disc) {
      KillTimer(window_, timer_scan);
    }
    page_ = page;
    if (page == Page::disc) {
      refresh_drives(false);
    } else if (page == Page::location) {
      read_destination();
    }
    apply_page();
    if (IsWindowEnabled(next_) != FALSE) {
      SetFocus(next_);
    }
  }

  // ---------------------------------------------------------------- discs

  void refresh_drives(const bool keep_selection) {
    const auto previous = keep_selection && selected_ >= 0 &&
                                  static_cast<std::size_t>(selected_) <
                                      drives_.size()
                              ? drives_[static_cast<std::size_t>(selected_)].path
                              : std::filesystem::path();
    scan_error_.clear();
    try {
      drives_ = motorhead::discover_motorhead_drives();
    } catch (const std::exception &error) {
      drives_.clear();
      scan_error_ = motorhead::widen(error.what());
    }
    if (!cue_path_.empty()) {
      try {
        drives_.push_back(motorhead::inspect_motorhead_cue(cue_path_));
      } catch (const std::exception &error) {
        scan_error_ = motorhead::widen(error.what());
      }
    }
    selected_ = drives_.empty() ? -1 : 0;
    if (!previous.empty()) {
      for (std::size_t index = 0U; index < drives_.size(); ++index) {
        if (drives_[index].path == previous) {
          selected_ = static_cast<int>(index);
          break;
        }
      }
    }
    if (page_ == Page::disc) {
      if (drives_.empty()) {
        SetTimer(window_, timer_scan, 2500U, nullptr);
      } else {
        KillTimer(window_, timer_scan);
      }
      EnableWindow(next_, next_enabled() ? TRUE : FALSE);
      InvalidateRect(window_, &body_rect_, FALSE);
    }
  }

  void detect_motorhead_patch() {
    patch_path_.clear();
    patch_error_.clear();
    const auto installer_directory = motorhead::executable_directory();
    const std::array candidates{installer_directory / "mhp30.exe",
                                installer_directory / "Install" /
                                    "mhp30.exe"};
    for (const auto &candidate : candidates) {
      if (!std::filesystem::is_regular_file(candidate)) {
        continue;
      }
      try {
        patch_path_ = motorhead::inspect_motorhead_patch(candidate);
        return;
      } catch (const std::exception &error) {
        patch_error_ = motorhead::widen(error.what());
        return;
      }
    }
  }

  [[nodiscard]] const RetailSource *selected_drive() const {
    if (selected_ < 0 ||
        static_cast<std::size_t>(selected_) >= drives_.size()) {
      return nullptr;
    }
    return &drives_[static_cast<std::size_t>(selected_)];
  }

  void choose_cue_image() {
    const auto selected = select_cue_image(window_);
    if (selected.empty()) {
      return;
    }
    try {
      auto source = motorhead::inspect_motorhead_cue(selected);
      cue_path_ = source.path;
      refresh_drives(false);
      for (std::size_t index = 0U; index < drives_.size(); ++index) {
        if (drives_[index].path == cue_path_) {
          selected_ = static_cast<int>(index);
          break;
        }
      }
      EnableWindow(next_, next_enabled() ? TRUE : FALSE);
      InvalidateRect(window_, &body_rect_, FALSE);
    } catch (const std::exception &error) {
      MessageBoxW(window_, motorhead::widen(error.what()).c_str(),
                  L"Invalid Motorhead disc image", MB_OK | MB_ICONWARNING);
    }
  }

  void choose_s40_image() {
    const auto selected = select_s40_cue_image(window_);
    if (selected.empty()) {
      return;
    }
    try {
      const auto source = motorhead::inspect_s40_cue(selected);
      s40_cue_path_ = source.path;
      s40_error_.clear();
      ui::set_checked(s40_, true);
      const auto description =
          source.path.filename().wstring() +
          L" | Verified. Installs only S40-specific files and audio.";
      SendMessageW(s40_, ui::message_set_description, 0U,
                   reinterpret_cast<LPARAM>(description.c_str()));
    } catch (const std::exception &error) {
      s40_cue_path_.clear();
      s40_error_ = motorhead::widen(error.what());
      ui::set_checked(s40_, false);
      SendMessageW(s40_, ui::message_set_description, 0U,
                   reinterpret_cast<LPARAM>(s40_error_.c_str()));
      MessageBoxW(window_, s40_error_.c_str(),
                  L"Invalid S40 Racing disc image", MB_OK | MB_ICONWARNING);
    }
    EnableWindow(next_, next_enabled() ? TRUE : FALSE);
    InvalidateRect(window_, &body_rect_, FALSE);
  }

  void choose_game_media() {
    constexpr UINT drive_command_base = 3000U;
    constexpr UINT choose_cue_command = 3200U;
    constexpr UINT refresh_command = 3201U;
    const auto menu = CreatePopupMenu();
    if (menu == nullptr) {
      return;
    }

    auto physical_count = 0U;
    for (std::size_t index = 0U; index < drives_.size(); ++index) {
      const auto &drive = drives_[index];
      if (drive.kind != RetailSourceKind::physical_disc || index >= 200U) {
        continue;
      }
      const auto flags = MF_STRING |
                         (static_cast<int>(index) == selected_ ? MF_CHECKED
                                                               : MF_UNCHECKED);
      AppendMenuW(menu, flags, drive_command_base + static_cast<UINT>(index),
                  drive.label.c_str());
      ++physical_count;
    }
    if (physical_count == 0U) {
      AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0U,
                  L"No Motorhead CD-ROM detected");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0U, nullptr);
    const auto cue_selected = selected_drive() != nullptr &&
                              selected_drive()->kind ==
                                  RetailSourceKind::cue_image;
    AppendMenuW(menu, MF_STRING | (cue_selected ? MF_CHECKED : MF_UNCHECKED),
                choose_cue_command, L"Choose CUE/BIN image...");
    AppendMenuW(menu, MF_STRING, refresh_command, L"Refresh CD-ROM drives");

    RECT anchor{};
    GetWindowRect(browse_image_, &anchor);
    SetForegroundWindow(window_);
    const auto command = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
        anchor.left, anchor.bottom + px(2), 0, window_, nullptr));
    DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0U, 0);

    if (command >= drive_command_base && command < choose_cue_command) {
      const auto index = static_cast<std::size_t>(command - drive_command_base);
      if (index < drives_.size() &&
          drives_[index].kind == RetailSourceKind::physical_disc) {
        selected_ = static_cast<int>(index);
        EnableWindow(next_, next_enabled() ? TRUE : FALSE);
        InvalidateRect(window_, &body_rect_, FALSE);
      }
    } else if (command == choose_cue_command) {
      choose_cue_image();
    } else if (command == refresh_command) {
      refresh_drives(true);
    }
  }

  // ------------------------------------------------------------- estimate

  void read_destination() {
    destination_path_ = window_text(destination_);
    free_bytes_ = 0U;
    free_known_ = false;
    replaces_existing_ = false;
    if (destination_path_.empty()) {
      return;
    }
    const std::filesystem::path path(destination_path_);
    std::uint64_t available = 0U;
    free_known_ = motorhead::free_space_bytes(path, available);
    free_bytes_ = available;
    replaces_existing_ = motorhead::installation_exists(path);
  }

  [[nodiscard]] std::uint64_t required_bytes() const {
    return estimated_content_bytes +
           (ui::is_checked(music_) ? estimated_audio_bytes : 0U) +
           (ui::is_checked(s40_) ? estimated_s40_bytes : 0U);
  }

  // -------------------------------------------------------------- install

  void start_install() {
    const auto *drive = selected_drive();
    if (drive == nullptr || patch_path_.empty()) {
      return;
    }
    read_destination();
    if (destination_path_.empty()) {
      return;
    }
    const std::filesystem::path destination(destination_path_);
    std::error_code error;
    if (std::filesystem::exists(destination, error) &&
        !std::filesystem::is_directory(destination, error)) {
      MessageBoxW(window_, L"The selected destination is not a folder.",
                  L"Motorhead Setup", MB_OK | MB_ICONWARNING);
      return;
    }
    const auto required = required_bytes();
    std::uint64_t available = 0U;
    if (required != 0U &&
        motorhead::free_space_bytes(destination, available) &&
        available < required) {
      MessageBoxW(window_,
                  L"The selected drive does not have enough free space for "
                  L"these installation options.",
                  L"Motorhead Setup", MB_OK | MB_ICONWARNING);
      return;
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    cancel_requested_.store(false);
    progress_ = 0;
    phase_ = -1;
    status_ = L"Preparing the installation...";
    outcome_ = Outcome::none;
    started_at_ = GetTickCount();
    elapsed_ = 0U;
    install_music_ = ui::is_checked(music_);
    install_s40_ = ui::is_checked(s40_) && !s40_cue_path_.empty();
    clean_profile_ = ui::is_checked(reset_);
    installed_path_ = destination_path_;
    set_page(Page::installing);
    SetTimer(window_, timer_elapsed, 1000U, nullptr);

    const auto retail_source = *drive;
    const auto patch = patch_path_;
    const auto music = install_music_;
    const auto s40_cue = install_s40_ ? s40_cue_path_
                                      : std::filesystem::path{};
    const auto clean = clean_profile_;
    worker_ = std::thread(
        [this, retail_source, patch, destination, music, s40_cue, clean] {
      bool success = false;
      bool cancelled = false;
      std::wstring message;
      try {
        motorhead::InstallReport report;
        report.phase = [this](const InstallPhase value) {
          PostMessageW(window_, message_phase,
                       static_cast<WPARAM>(value), 0);
        };
        report.status = [this](const std::wstring &value) {
          post_status(value);
        };
        report.progress = [this](const int value) {
          PostMessageW(window_, message_progress, static_cast<WPARAM>(value),
                       0);
        };
        motorhead::perform_import(retail_source, patch, destination, music,
                                  s40_cue, clean, cancel_requested_, report);
        success = true;
        message = install_s40_
                      ? L"Motorhead and the optional S40 Racing content are "
                        L"installed and ready to play."
                      : (music ? L"The game and the original CD soundtrack "
                                 L"are installed and ready to play."
                               : L"The game is installed and ready to play. "
                                 L"The optional CD soundtrack was skipped.");
      } catch (const motorhead::ImportCancelled &) {
        cancelled = true;
        message = L"The installation was cancelled. Any existing game data "
                  L"was left unchanged.";
      } catch (const std::exception &error) {
        message = motorhead::widen(error.what());
      }
      auto *text = new std::wstring(std::move(message));
      PostMessageW(window_, message_finished,
                   success ? 1U : (cancelled ? 2U : 0U),
                   reinterpret_cast<LPARAM>(text));
    });
  }

  void post_status(std::wstring value) const {
    auto text = std::make_unique<std::wstring>(std::move(value));
    if (PostMessageW(window_, message_status, 0U,
                     reinterpret_cast<LPARAM>(text.get())) != FALSE) {
      static_cast<void>(text.release());
    }
  }

  void request_cancel() {
    if (page_ != Page::installing) {
      return;
    }
    cancel_requested_.store(true);
    status_ = L"Cancelling after the current disc read...";
    EnableWindow(quit_, FALSE);
    InvalidateRect(window_, &body_rect_, FALSE);
  }

  // --------------------------------------------------------------- paint

  void paint() {
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window_, &paint);
    RECT client{};
    GetClientRect(window_, &client);
    const auto buffer = CreateCompatibleDC(dc);
    const auto bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    const auto previous = SelectObject(buffer, bitmap);

    ui::fill(buffer, body_rect_, ui::colour::window);
    ui::fill(buffer, rail_rect_, ui::colour::rail);
    ui::fill(buffer, footer_rect_, ui::colour::footer);
    ui::fill(buffer, rule_rect_, ui::colour::accent);
    RECT rail_edge{rail_rect_.right - 1, rail_rect_.top, rail_rect_.right,
                   rail_rect_.bottom};
    ui::fill(buffer, rail_edge, ui::colour::divider);
    RECT footer_edge{0, footer_rect_.top, client.right,
                     footer_rect_.top + 1};
    ui::fill(buffer, footer_edge, ui::colour::divider);

    paint_banner(buffer);
    paint_rail(buffer);
    paint_footer(buffer);
    paint_page(buffer);

    BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, previous);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    EndPaint(window_, &paint);
  }

  void paint_banner(const HDC dc) const {
    if (banner_ == nullptr || banner_size_.cx <= 0) {
      ui::fill(dc, banner_rect_, RGB(0x0D, 0x0F, 0x13));
      return;
    }
    const auto source = CreateCompatibleDC(dc);
    const auto previous = SelectObject(source, banner_);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchBlt(dc, banner_rect_.left, banner_rect_.top,
               banner_rect_.right - banner_rect_.left,
               banner_rect_.bottom - banner_rect_.top, source, 0, 0,
               banner_size_.cx, banner_size_.cy, SRCCOPY);
    SelectObject(source, previous);
    DeleteDC(source);
  }

  void paint_rail(const HDC dc) const {
    const auto current = step_of(page_);
    const auto marker = px(26);
    const auto row = px(52);
    auto y = rail_rect_.top + px(26);
    const auto x = rail_rect_.left + px(26);
    for (std::size_t index = 0U; index < steps.size(); ++index) {
      const auto position = static_cast<int>(index);
      const auto done = position < current ||
                        (page_ == Page::finished &&
                         outcome_ == Outcome::success);
      const auto active = position == current;
      RECT box{x, y, x + marker, y + marker};
      if (done) {
        ui::fill_box(dc, box, ui::colour::accent_shade, ui::colour::accent, 1);
        RECT mark = box;
        InflateRect(&mark, -px(7), -px(7));
        ui::draw_check(dc, mark, ui::colour::accent, std::max(2, px(2)));
      } else if (active) {
        ui::fill_box(dc, box, ui::colour::accent, ui::colour::accent, 1);
        ui::draw_text(dc, box, std::to_wstring(position + 1),
                      fonts_.step_strong, ui::colour::on_accent,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      } else {
        ui::fill_box(dc, box, ui::colour::rail, ui::colour::border, 1);
        ui::draw_text(dc, box, std::to_wstring(position + 1), fonts_.step,
                      ui::colour::text_faint,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
      if (index + 1U < steps.size()) {
        RECT line{x + marker / 2, box.bottom + px(3),
                  x + marker / 2 + std::max(1, px(1)), y + row - px(3)};
        ui::fill(dc, line, done ? ui::colour::accent_shade
                                : ui::colour::divider);
      }
      const auto text_left = box.right + px(12);
      RECT label{text_left, active ? y - px(1) : y, rail_rect_.right - px(14),
                 active ? y + px(16) : box.bottom};
      ui::draw_text(dc, label, steps[index].name,
                    active ? fonts_.step_strong : fonts_.step,
                    active ? ui::colour::text
                           : (done ? ui::colour::text_dim
                                    : ui::colour::text_faint),
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      if (active) {
        RECT hint{label.left, y + px(15), label.right, y + px(32)};
        ui::draw_text(dc, hint, steps[index].hint, fonts_.caption,
                       ui::colour::text_faint,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                           DT_END_ELLIPSIS);
      }
      y += row;
    }
  }

  void paint_footer(const HDC dc) const {
    RECT label{px(28), footer_rect_.top, rail_rect_.right + px(120),
               footer_rect_.bottom};
    std::wstring text = L"Step " + std::to_wstring(step_of(page_) + 1) +
                        L" of " + std::to_wstring(steps.size());
    if (page_ == Page::finished) {
      text = outcome_ == Outcome::success ? L"All steps complete"
                                          : L"Installation stopped";
    }
    ui::draw_text(dc, label, text, fonts_.caption, ui::colour::text_faint,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }

  int paint_heading(const HDC dc, const std::wstring &title,
                    const std::wstring &subtitle) const {
    RECT area{content_rect_.left, content_rect_.top, content_rect_.right,
              content_rect_.top + px(34)};
    ui::draw_text(dc, area, title, fonts_.heading, ui::colour::text,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    auto y = area.bottom + px(6);
    if (!subtitle.empty()) {
      const auto width = content_rect_.right - content_rect_.left;
      const auto height = ui::text_height(dc, subtitle, fonts_.subheading,
                                          width, DT_WORDBREAK);
      RECT body{content_rect_.left, y, content_rect_.right, y + height};
      ui::draw_text(dc, body, subtitle, fonts_.subheading, ui::colour::text_dim,
                    DT_LEFT | DT_TOP | DT_WORDBREAK);
      y = body.bottom;
    }
    return y + px(18);
  }

  void paint_page(const HDC dc) const {
    switch (page_) {
    case Page::welcome:
      paint_welcome(dc);
      break;
    case Page::disc:
      paint_disc(dc);
      break;
    case Page::location:
      paint_location(dc);
      break;
    case Page::options:
      paint_options(dc);
      break;
    case Page::ready:
      paint_ready(dc);
      break;
    case Page::installing:
      paint_installing(dc);
      break;
    case Page::finished:
      paint_finished(dc);
      break;
    }
  }

  void paint_bullet(const HDC dc, int &y, const std::wstring &title,
                    const std::wstring &body) const {
    const auto dot = px(7);
    RECT marker{content_rect_.left + px(2), y + px(6),
                content_rect_.left + px(2) + dot, y + px(6) + dot};
    ui::fill_ellipse(dc, marker, ui::colour::accent, ui::colour::accent, 1);
    const auto left = content_rect_.left + px(22);
    RECT heading{left, y, content_rect_.right, y + px(20)};
    ui::draw_text(dc, heading, title, fonts_.body_strong, ui::colour::text,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    const auto width = content_rect_.right - left;
    const auto height = ui::text_height(dc, body, fonts_.body, width,
                                        DT_WORDBREAK);
    RECT text{left, heading.bottom + px(1), content_rect_.right,
              heading.bottom + px(1) + height};
    ui::draw_text(dc, text, body, fonts_.body, ui::colour::text_dim,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
    y = text.bottom + px(16);
  }

  void paint_welcome(const HDC dc) const {
    auto y = paint_heading(
        dc, L"Welcome",
        L"Set up Motorhead using your original game media and the official "
        L"3.0 update.");
    paint_bullet(dc, y, L"Have your game files ready",
                 L"Use the original Motorhead CD-ROM or a CUE/BIN image, "
                 L"together with the official mhp30.exe update.");
    paint_bullet(dc, y, L"Everything else is handled for you",
                 L"The installer will verify your files, copy the game data "
                 L"and prepare Motorhead for you.");
  }

  void paint_disc(const HDC dc) const {
    auto y = paint_heading(
        dc, L"Game source",
        L"Choose your original game media and the official Motorhead 3.0 "
        L"update.");
    RECT source_title{content_rect_.left, y, content_rect_.right,
                      y + px(36)};
    ui::draw_text(dc, source_title, L"Original game media", fonts_.body_strong,
                  ui::colour::text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    y = source_title.bottom + px(8);
    cards_.clear();
    const auto *source = selected_drive();
    const auto valid_source = source != nullptr;
    RECT card{content_rect_.left, y, content_rect_.right,
              y + px(card_height)};
    ui::fill_rounded(dc, card, px(8), ui::colour::window,
                     valid_source ? ui::colour::success : ui::colour::border);
    RECT source_check{card.left + px(16), card.top + px(26),
                      card.left + px(40), card.top + px(50)};
    ui::fill_box(dc, source_check,
                 valid_source ? ui::colour::success : ui::colour::window,
                 valid_source ? ui::colour::success : ui::colour::border, 1);
    if (valid_source) {
      RECT mark = source_check;
      InflateRect(&mark, -px(7), -px(7));
      ui::draw_check(dc, mark, ui::colour::on_accent, std::max(2, px(2)));
    }
    RECT title{card.left + px(56), card.top + px(16),
               card.right - px(190), card.top + px(38)};
    const std::wstring media_type =
        valid_source && source->kind == RetailSourceKind::cue_image
            ? L"Original Motorhead CUE/BIN image"
            : (valid_source ? L"Original Motorhead CD-ROM"
                            : L"No game media selected");
    ui::draw_text(dc, title, media_type, fonts_.body_strong,
                  valid_source ? ui::colour::success : ui::colour::text_dim,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    const std::wstring media_detail =
        valid_source
            ? source->label + L"  |  Verified"
            : (scan_error_.empty()
                   ? L"Choose a Motorhead CD-ROM or CUE/BIN image."
                   : scan_error_);
    RECT hint{title.left, title.bottom, title.right, title.bottom + px(20)};
    ui::draw_text(dc, hint, media_detail, fonts_.caption,
                  valid_source ? ui::colour::text_dim
                               : (scan_error_.empty() ? ui::colour::text_faint
                                                     : ui::colour::danger),
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    const auto valid_patch = !patch_path_.empty();
    RECT patch{content_rect_.left, content_rect_.bottom - px(76),
               content_rect_.right, content_rect_.bottom};
    ui::fill_rounded(dc, patch, px(8), ui::colour::window,
                     valid_patch ? ui::colour::success
                                 : (patch_error_.empty() ? ui::colour::border
                                                         : ui::colour::danger));
    RECT patch_check{patch.left + px(16), patch.top + px(26),
                     patch.left + px(40), patch.top + px(50)};
    ui::fill_box(dc, patch_check,
                 valid_patch ? ui::colour::success : ui::colour::window,
                 valid_patch ? ui::colour::success : ui::colour::border, 1);
    if (valid_patch) {
      RECT mark = patch_check;
      InflateRect(&mark, -px(7), -px(7));
      ui::draw_check(dc, mark, ui::colour::on_accent, std::max(2, px(2)));
    }
    RECT patch_title{patch.left + px(56), patch.top + px(11),
                     patch.right - px(190), patch.top + px(31)};
    ui::draw_text(dc, patch_title, L"Official Motorhead 3.0 update",
                  fonts_.body_strong,
                  valid_patch ? ui::colour::success : ui::colour::text_dim,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    const std::wstring patch_detail =
        valid_patch
            ? patch_path_.filename().wstring() + L"  |  Verified"
            : (patch_error_.empty()
                   ? L"Choose the official update file named mhp30.exe."
                   : patch_error_);
    RECT patch_hint{patch_title.left, patch_title.bottom, patch_title.right,
                    patch.bottom - px(8)};
    ui::draw_text(dc, patch_hint, patch_detail, fonts_.caption,
                  valid_patch ? ui::colour::text_dim
                              : (patch_error_.empty() ? ui::colour::text_faint
                                                      : ui::colour::danger),
                   DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
  }

  void paint_location(const HDC dc) const {
    paint_heading(dc, L"Install location",
                  L"Choose where Motorhead and your user files will be "
                  L"installed.");
    RECT panel{content_rect_.left, content_rect_.top + px(72),
               content_rect_.right, content_rect_.top + px(232)};
    ui::fill_rounded(dc, panel, px(8), ui::colour::window,
                     ui::colour::border);
    RECT caption{panel.left + px(16), panel.top + px(12), panel.right - px(16),
                 panel.top + px(30)};
    ui::draw_text(dc, caption, L"Destination folder", fonts_.body_strong,
                  ui::colour::text, DT_LEFT | DT_SINGLELINE);
    const auto focused = GetFocus() == destination_;
    ui::fill_rounded(dc, field_rect_, px(6), ui::colour::field,
                     focused ? ui::colour::accent : ui::colour::border);

    int y = static_cast<int>(field_rect_.bottom) + px(20);
    const auto required = required_bytes();
    std::wstring value = L"About " + format_bytes(required);
    if (ui::is_checked(music_)) {
      value += L" (includes " + format_bytes(estimated_audio_bytes) +
               L" of CD audio)";
    }
    paint_info_row(dc, y, ui::colour::text_dim, L"Space required", value);
    if (free_known_) {
      const auto enough = required == 0U || free_bytes_ > required;
      paint_info_row(dc, y, enough ? ui::colour::success : ui::colour::danger,
                     L"Space available", format_bytes(free_bytes_) +
                                             (enough ? L""
                                                     : L" - not enough"));
    }
    if (replaces_existing_) {
      y = std::max(y + px(6), static_cast<int>(panel.bottom) + px(12));
      RECT note{content_rect_.left, y, content_rect_.right, y + px(58)};
      ui::fill_rounded(dc, note, px(6), RGB(0x2A, 0x25, 0x18),
                       ui::colour::warning);
      RECT text = note;
      InflateRect(&text, -px(14), -px(10));
      ui::draw_text(dc, text,
                    L"An existing installation was found here. Its data "
                    L"folders are replaced, and the User folder is kept "
                    L"unless the reset option is selected.",
                    fonts_.caption, ui::colour::warning,
                    DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
  }

  void paint_info_row(const HDC dc, int &y, const COLORREF marker,
                      const std::wstring &label,
                      const std::wstring &value) const {
    RECT dot{content_rect_.left + px(18), y + px(6),
             content_rect_.left + px(25), y + px(13)};
    ui::fill_box(dc, dot, marker, marker, 1);
    RECT name{content_rect_.left + px(36), y, content_rect_.left + px(164),
               y + px(20)};
    ui::draw_text(dc, name, label, fonts_.body, ui::colour::text_dim,
                  DT_LEFT | DT_SINGLELINE);
    RECT text{name.right, y, content_rect_.right - px(16), y + px(20)};
    ui::draw_text(dc, text, value, fonts_.body_strong, ui::colour::text,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    y += px(28);
  }

  void paint_options(const HDC dc) const {
    paint_heading(dc, L"Installation options",
                  L"Choose the soundtrack and optional S40 Racing add-on. "
                  L"Existing player data is preserved by default.");
  }

  void paint_summary_row(const HDC dc, int &y, const std::wstring &label,
                         const std::wstring &value,
                         const COLORREF value_colour) const {
    RECT name{content_rect_.left + px(18), y, content_rect_.left + px(170),
              y + px(22)};
    ui::draw_text(dc, name, label, fonts_.body, ui::colour::text_dim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT text{name.right, y, content_rect_.right - px(18), y + px(22)};
    ui::draw_text(dc, text, value, fonts_.body_strong, value_colour,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    y += px(30);
  }

  void paint_ready(const HDC dc) const {
    auto y = paint_heading(dc, L"Ready to install",
                           L"Check the summary below and start the "
                           L"installation.");
    const auto *drive = selected_drive();
    const auto rows = required_bytes() != 0U ? 7 : 6;
    RECT panel{content_rect_.left, y, content_rect_.right,
               y + px(24) + rows * px(30)};
    ui::fill_rounded(dc, panel, px(8), ui::colour::card, ui::colour::border);
    int row = static_cast<int>(panel.top) + px(14);
    paint_summary_row(dc, row, L"Game source",
                      drive != nullptr ? drive->label : L"None",
                      ui::colour::text);
    paint_summary_row(dc, row, L"3.0 update", patch_path_.wstring(),
                      ui::colour::text);
    paint_summary_row(dc, row, L"Install folder", destination_path_,
                      ui::colour::text);
    paint_summary_row(dc, row, L"CD soundtrack",
                      ui::is_checked(music_)
                          ? L"Tracks 2 to 11 are copied from the source"
                          : L"Not installed",
                      ui::colour::text);
    paint_summary_row(
        dc, row, L"S40 Racing",
        ui::is_checked(s40_)
            ? (s40_cue_path_.empty() ? L"No valid image selected"
                                     : s40_cue_path_.wstring())
            : L"Not installed",
        ui::is_checked(s40_) && s40_cue_path_.empty() ? ui::colour::danger
                                                       : ui::colour::text);
    paint_summary_row(dc, row, L"Player data",
                      ui::is_checked(reset_)
                          ? L"Settings and saves are reset"
                          : L"Existing settings and saves are kept",
                      ui::colour::text);
    const auto required = required_bytes();
    if (required != 0U) {
      paint_summary_row(dc, row, L"Disk space", L"about " +
                                                    format_bytes(required),
                        ui::colour::text);
    }
    RECT note{content_rect_.left, panel.bottom + px(16), content_rect_.right,
              panel.bottom + px(56)};
    ui::draw_text(dc, note,
                  ui::is_checked(music_)
                      ? L"Reading the ten audio tracks from the source can take "
                        L"several minutes. The installation can be cancelled "
                        L"at any point."
                      : L"The installation can be cancelled at any point.",
                  fonts_.caption, ui::colour::text_faint,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
  }

  void paint_installing(const HDC dc) const {
    auto y = paint_heading(dc, L"Installing Motorhead", L"");
    RECT status{content_rect_.left, y, content_rect_.right, y + px(22)};
    ui::draw_text(dc, status, status_, fonts_.body, ui::colour::text,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT percent{content_rect_.right - px(90), y, content_rect_.right,
                 y + px(22)};
    ui::draw_text(dc, percent, std::to_wstring(progress_) + L"%",
                  fonts_.body_strong, ui::colour::accent,
                  DT_RIGHT | DT_SINGLELINE);
    RECT bar{content_rect_.left, status.bottom + px(10), content_rect_.right,
             status.bottom + px(10) + px(8)};
    ui::draw_progress(dc, bar, progress_, ui::colour::card,
                      ui::colour::accent);

    auto row = bar.bottom + px(24);
    for (std::size_t index = 0U; index < phase_labels.size(); ++index) {
      const auto position = static_cast<int>(index);
      const auto done = position < phase_;
      const auto active = position == phase_;
      const auto marker = px(18);
      RECT box{content_rect_.left + px(2), row + px(1),
               content_rect_.left + px(2) + marker, row + px(1) + marker};
      if (done) {
        ui::fill_box(dc, box, ui::colour::success, ui::colour::success, 1);
        RECT mark = box;
        InflateRect(&mark, -px(4), -px(4));
        ui::draw_check(dc, mark, ui::colour::on_accent, std::max(2, px(2)));
      } else if (active) {
        ui::fill_box(dc, box, ui::colour::accent, ui::colour::accent, 1);
      } else {
        ui::fill_box(dc, box, ui::colour::window, ui::colour::border, 1);
      }
      RECT label{box.right + px(12), row, content_rect_.right,
                 row + px(20)};
      ui::draw_text(dc, label, phase_labels[index],
                    active ? fonts_.body_strong : fonts_.body,
                     active ? ui::colour::text
                            : (done ? ui::colour::success
                                    : ui::colour::text_faint),
                    DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
      row += px(26);
    }
    RECT elapsed{content_rect_.left, content_rect_.bottom - px(22),
                 content_rect_.right, content_rect_.bottom};
    ui::draw_text(dc, elapsed, L"Elapsed " + format_duration(elapsed_),
                  fonts_.caption, ui::colour::text_faint,
                  DT_LEFT | DT_SINGLELINE);
  }

  void paint_finished(const HDC dc) const {
    const auto success = outcome_ == Outcome::success;
    const auto cancelled = outcome_ == Outcome::cancelled;
    const auto accent = success ? ui::colour::success
                                : (cancelled ? ui::colour::warning
                                             : ui::colour::danger);
    const auto badge_size = px(40);
    RECT badge{content_rect_.left, content_rect_.top,
               content_rect_.left + badge_size,
               content_rect_.top + badge_size};
    ui::fill_box(dc, badge, accent, accent, 1);
    RECT mark = badge;
    InflateRect(&mark, -px(11), -px(11));
    if (success) {
      ui::draw_check(dc, mark, ui::colour::on_accent, std::max(3, px(3)));
    } else {
      ui::draw_cross(dc, mark, ui::colour::on_accent, std::max(3, px(3)));
    }

    const auto left = badge.right + px(16);
    RECT title{left, content_rect_.top, content_rect_.right,
               content_rect_.top + px(34)};
    ui::draw_text(dc, title,
                  success ? L"Installation complete"
                          : (cancelled ? L"Installation cancelled"
                                       : L"Installation failed"),
                  fonts_.heading, ui::colour::text,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    const auto width = content_rect_.right - left;
    const auto height = ui::text_height(dc, final_message_, fonts_.subheading,
                                        width, DT_WORDBREAK);
    RECT body{left, title.bottom + px(2), content_rect_.right,
              title.bottom + px(2) + height};
    ui::draw_text(dc, body, final_message_, fonts_.subheading,
                  success ? ui::colour::text_dim : accent,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);

    if (!success) {
      return;
    }
    RECT panel{content_rect_.left, body.bottom + px(20), content_rect_.right,
               body.bottom + px(20) + px(96)};
    ui::fill_rounded(dc, panel, px(8), ui::colour::window,
                     ui::colour::success);
    int row = static_cast<int>(panel.top) + px(14);
    paint_summary_row(dc, row, L"Installed to", installed_path_,
                      ui::colour::text);
    paint_summary_row(dc, row, L"Soundtrack",
                      install_music_ ? L"Installed from the retail source"
                                     : L"Not installed",
                      ui::colour::text);
    paint_summary_row(dc, row, L"Player data",
                      clean_profile_ ? L"Reset" : L"Preserved",
                      ui::colour::text);
  }

  // ------------------------------------------------------------- messages

  void on_card_click(const POINT point) {
    for (std::size_t index = 0U; index < cards_.size(); ++index) {
      if (PtInRect(&cards_[index], point) != FALSE) {
        selected_ = static_cast<int>(index);
        EnableWindow(next_, next_enabled() ? TRUE : FALSE);
        InvalidateRect(window_, &body_rect_, FALSE);
        return;
      }
    }
  }

  void on_mouse_move(const POINT point) {
    if (page_ != Page::disc) {
      return;
    }
    auto hot = -1;
    for (std::size_t index = 0U; index < cards_.size(); ++index) {
      if (PtInRect(&cards_[index], point) != FALSE) {
        hot = static_cast<int>(index);
        break;
      }
    }
    if (hot != hot_card_) {
      hot_card_ = hot;
      InvalidateRect(window_, &body_rect_, FALSE);
    }
  }

  void advance() {
    switch (page_) {
    case Page::welcome:
      set_page(Page::disc);
      break;
    case Page::disc:
      if (next_enabled()) {
        set_page(Page::location);
      }
      break;
    case Page::location:
      read_destination();
      if (!destination_path_.empty()) {
        set_page(Page::options);
      }
      break;
    case Page::options:
      set_page(Page::ready);
      break;
    case Page::ready:
      start_install();
      break;
    case Page::installing:
      break;
    case Page::finished:
      finish();
      break;
    }
  }

  void retreat() {
    switch (page_) {
    case Page::disc:
      set_page(Page::welcome);
      break;
    case Page::location:
      set_page(Page::disc);
      break;
    case Page::options:
      set_page(Page::location);
      break;
    case Page::ready:
      set_page(Page::options);
      break;
    case Page::finished:
      outcome_ = Outcome::none;
      set_page(Page::ready);
      break;
    default:
      break;
    }
  }

  void finish() {
    if (outcome_ == Outcome::success && ui::is_checked(open_folder_)) {
      ShellExecuteW(nullptr, L"open", installed_path_.c_str(), nullptr,
                    nullptr, SW_SHOWNORMAL);
    }
    DestroyWindow(window_);
  }

  void confirm_cancel() {
    const auto answer = MessageBoxW(
        window_, L"Are you sure you want to cancel setup?", L"Cancel setup",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES) {
      return;
    }
    if (page_ == Page::installing) {
      request_cancel();
    } else {
      DestroyWindow(window_);
    }
  }

  LRESULT handle(const UINT message, const WPARAM wparam,
                 const LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
      dpi_ = ui::window_dpi(window_);
      create_resources();
      create_controls();
      layout();
      return 0;
    case WM_SIZE:
      layout();
      return 0;
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wparam);
      fonts_.create(dpi_);
      update_control_fonts();
      const auto *suggested = reinterpret_cast<const RECT *>(lparam);
      SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      layout();
      InvalidateRect(window_, nullptr, TRUE);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      paint();
      return 0;
    case WM_CTLCOLORBTN: {
      const auto control = reinterpret_cast<HWND>(lparam);
      const auto footer = control == back_ || control == next_ ||
                          control == quit_;
      return reinterpret_cast<LRESULT>(footer ? footer_brush_ : window_brush_);
    }
    case WM_CTLCOLORSTATIC:
      return reinterpret_cast<LRESULT>(window_brush_);
    case WM_CTLCOLOREDIT: {
      const auto dc = reinterpret_cast<HDC>(wparam);
      SetTextColor(dc, ui::colour::text);
      SetBkColor(dc, ui::colour::field);
      return reinterpret_cast<LRESULT>(field_brush_);
    }
    case WM_MOUSEMOVE: {
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      on_mouse_move(point);
      return 0;
    }
    case WM_LBUTTONDOWN:
      if (page_ == Page::disc) {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        on_card_click(point);
      }
      SetFocus(window_);
      return 0;
    case WM_TIMER:
      if (wparam == timer_scan) {
        if (page_ == Page::disc && drives_.empty()) {
          refresh_drives(false);
        }
        return 0;
      }
      if (wparam == timer_elapsed) {
        if (page_ == Page::installing) {
          elapsed_ =
              (static_cast<std::uint64_t>(GetTickCount()) - started_at_) /
              1000U;
          InvalidateRect(window_, &body_rect_, FALSE);
        }
        return 0;
      }
      break;
    case WM_DEVICECHANGE:
      if (page_ == Page::disc &&
          (wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE ||
           wparam == DBT_DEVNODES_CHANGED)) {
        refresh_drives(true);
      }
      return TRUE;
    case WM_COMMAND:
      return on_command(wparam, lparam);
    case message_status: {
      const std::unique_ptr<std::wstring> text(
          reinterpret_cast<std::wstring *>(lparam));
      status_ = *text;
      InvalidateRect(window_, &body_rect_, FALSE);
      return 0;
    }
    case message_progress:
      progress_ = static_cast<int>(wparam);
      InvalidateRect(window_, &body_rect_, FALSE);
      return 0;
    case message_phase:
      phase_ = static_cast<int>(wparam);
      InvalidateRect(window_, &body_rect_, FALSE);
      return 0;
    case message_finished: {
      if (worker_.joinable()) {
        worker_.join();
      }
      KillTimer(window_, timer_elapsed);
      const std::unique_ptr<std::wstring> text(
          reinterpret_cast<std::wstring *>(lparam));
      outcome_ = wparam == 1U ? Outcome::success
                              : (wparam == 2U ? Outcome::cancelled
                                              : Outcome::failed);
      final_message_ = *text;
      if (outcome_ == Outcome::success) {
        progress_ = 100;
        phase_ = static_cast<int>(phase_labels.size());
      }
      EnableWindow(quit_, TRUE);
      page_ = Page::installing;
      set_page(Page::finished);
      return 0;
    }
    case WM_CLOSE:
      if (page_ == Page::finished) {
        DestroyWindow(window_);
      } else {
        confirm_cancel();
      }
      return 0;
    case WM_DESTROY:
      KillTimer(window_, timer_scan);
      KillTimer(window_, timer_elapsed);
      destroy_resources();
      PostQuitMessage(0);
      return 0;
    default:
      break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
  }

  LRESULT on_command(const WPARAM wparam, const LPARAM lparam) {
    const auto identifier = LOWORD(wparam);
    const auto code = HIWORD(wparam);
    switch (identifier) {
    case id_next:
    case IDOK:
      advance();
      return 0;
    case id_back:
      retreat();
      return 0;
    case id_quit:
    case IDCANCEL:
      if (page_ == Page::finished) {
        finish();
      } else {
        confirm_cancel();
      }
      return 0;
    case id_browse_image:
      choose_game_media();
      return 0;
    case id_browse_patch: {
      const auto selected = select_motorhead_patch(window_);
      if (selected.empty()) {
        return 0;
      }
      try {
        patch_path_ = motorhead::inspect_motorhead_patch(selected);
        patch_error_.clear();
      } catch (const std::exception &error) {
        patch_path_.clear();
        patch_error_ = motorhead::widen(error.what());
        MessageBoxW(window_, patch_error_.c_str(),
                    L"Unsupported Motorhead update",
                    MB_OK | MB_ICONWARNING);
      }
      EnableWindow(next_, next_enabled() ? TRUE : FALSE);
      InvalidateRect(window_, &body_rect_, FALSE);
      return 0;
    }
    case id_browse_s40:
      choose_s40_image();
      return 0;
    case id_browse: {
      const auto selected =
          select_installation_folder(window_, window_text(destination_));
      if (!selected.empty()) {
        SetWindowTextW(destination_, selected.c_str());
        read_destination();
        InvalidateRect(window_, &body_rect_, FALSE);
      }
      return 0;
    }
    case id_destination:
      if (code == EN_CHANGE) {
        read_destination();
        EnableWindow(next_, next_enabled() ? TRUE : FALSE);
        InvalidateRect(window_, &body_rect_, FALSE);
      } else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
        InvalidateRect(window_, &body_rect_, FALSE);
      }
      return 0;
    case id_music:
      InvalidateRect(window_, &body_rect_, FALSE);
      return 0;
    case id_s40:
      if (ui::is_checked(s40_) && s40_cue_path_.empty()) {
        choose_s40_image();
        if (s40_cue_path_.empty()) {
          ui::set_checked(s40_, false);
        }
      }
      EnableWindow(next_, next_enabled() ? TRUE : FALSE);
      InvalidateRect(window_, &body_rect_, FALSE);
      return 0;
    case id_reset:
      InvalidateRect(window_, &body_rect_, FALSE);
      return 0;
    default:
      break;
    }
    static_cast<void>(lparam);
    return 0;
  }

  HWND window_ = nullptr;
  HINSTANCE instance_ = nullptr;
  UINT dpi_ = 96U;
  ui::Fonts fonts_{};
  HBRUSH window_brush_ = nullptr;
  HBRUSH rail_brush_ = nullptr;
  HBRUSH footer_brush_ = nullptr;
  HBRUSH field_brush_ = nullptr;
  HBITMAP banner_ = nullptr;
  SIZE banner_size_{0, 0};

  HWND back_ = nullptr;
  HWND next_ = nullptr;
  HWND quit_ = nullptr;
  HWND browse_image_ = nullptr;
  HWND browse_patch_ = nullptr;
  HWND browse_s40_ = nullptr;
  HWND browse_ = nullptr;
  HWND destination_ = nullptr;
  HWND music_ = nullptr;
  HWND s40_ = nullptr;
  HWND reset_ = nullptr;
  HWND open_folder_ = nullptr;

  RECT banner_rect_{};
  RECT rule_rect_{};
  RECT rail_rect_{};
  RECT body_rect_{};
  RECT content_rect_{};
  RECT footer_rect_{};
  RECT field_rect_{};
  mutable std::vector<RECT> cards_;

  Page page_ = Page::welcome;
  Outcome outcome_ = Outcome::none;
  std::vector<RetailSource> drives_;
  std::filesystem::path cue_path_;
  std::filesystem::path patch_path_;
  std::filesystem::path s40_cue_path_;
  std::wstring scan_error_;
  std::wstring patch_error_;
  std::wstring s40_error_;
  int selected_ = -1;
  int hot_card_ = -1;

  std::wstring destination_path_;
  std::wstring installed_path_;
  std::wstring status_;
  std::wstring final_message_;
  std::uint64_t free_bytes_ = 0U;
  bool free_known_ = false;
  bool replaces_existing_ = false;
  bool install_music_ = true;
  bool install_s40_ = false;
  bool clean_profile_ = false;
  int progress_ = 0;
  int phase_ = -1;
  std::uint64_t started_at_ = 0U;
  std::uint64_t elapsed_ = 0U;

  std::thread worker_;
  std::atomic_bool cancel_requested_{false};
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  try {
    int argument_count = 0;
    auto **arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments != nullptr && argument_count == 3 &&
        _wcsicmp(arguments[1], L"--validate-cue") == 0) {
      const std::filesystem::path cue(arguments[2]);
      LocalFree(arguments);
      static_cast<void>(motorhead::inspect_motorhead_cue(cue));
      return 0;
    }
    if (arguments != nullptr && argument_count == 3 &&
        _wcsicmp(arguments[1], L"--validate-s40-cue") == 0) {
      const std::filesystem::path cue(arguments[2]);
      LocalFree(arguments);
      static_cast<void>(motorhead::inspect_s40_cue(cue));
      return 0;
    }
    if (arguments != nullptr && argument_count == 3 &&
        _wcsicmp(arguments[1], L"--validate-patch") == 0) {
      const std::filesystem::path patch(arguments[2]);
      LocalFree(arguments);
      try {
        static_cast<void>(motorhead::inspect_motorhead_patch(patch));
        return 0;
      } catch (...) {
        return 2;
      }
    }
    if (arguments != nullptr) {
      LocalFree(arguments);
    }
    Application application;
    return application.run(instance, show);
  } catch (const std::exception &error) {
    MessageBoxW(nullptr, motorhead::widen(error.what()).c_str(),
                L"Motorhead Setup failed", MB_OK | MB_ICONERROR);
    return 1;
  }
}
