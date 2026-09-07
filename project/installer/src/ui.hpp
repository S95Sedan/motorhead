#pragma once

#include <windows.h>

#include <string>

// Minimal dark-themed drawing helpers and owner-drawn controls shared by the
// installer wizard pages. Everything is plain GDI so the executable stays a
// single static binary with no runtime dependencies.
namespace ui {

namespace colour {
constexpr COLORREF window = RGB(0x1C, 0x1F, 0x26);
constexpr COLORREF rail = RGB(0x14, 0x16, 0x1B);
constexpr COLORREF footer = RGB(0x16, 0x18, 0x1E);
constexpr COLORREF card = RGB(0x24, 0x28, 0x31);
constexpr COLORREF card_hot = RGB(0x2C, 0x31, 0x3C);
constexpr COLORREF field = RGB(0x14, 0x16, 0x1B);
constexpr COLORREF border = RGB(0x35, 0x3B, 0x47);
constexpr COLORREF border_hot = RGB(0x4A, 0x52, 0x60);
constexpr COLORREF divider = RGB(0x25, 0x29, 0x31);
constexpr COLORREF accent = RGB(0xF2, 0x6C, 0x1F);
constexpr COLORREF accent_hot = RGB(0xFF, 0x86, 0x3A);
constexpr COLORREF accent_press = RGB(0xD2, 0x59, 0x14);
constexpr COLORREF accent_shade = RGB(0x4A, 0x2A, 0x14);
constexpr COLORREF on_accent = RGB(0x1A, 0x10, 0x06);
constexpr COLORREF text = RGB(0xEE, 0xF1, 0xF5);
constexpr COLORREF text_dim = RGB(0x9A, 0xA3, 0xB2);
constexpr COLORREF text_faint = RGB(0x66, 0x6F, 0x7D);
constexpr COLORREF success = RGB(0x56, 0xC1, 0x8B);
constexpr COLORREF warning = RGB(0xE8, 0xB3, 0x39);
constexpr COLORREF danger = RGB(0xE1, 0x5D, 0x4E);
}  // namespace colour

constexpr UINT message_set_description = WM_USER + 0x140;
constexpr UINT message_set_dpi = WM_USER + 0x141;
constexpr UINT message_set_description_font = WM_USER + 0x142;
constexpr UINT message_set_right_inset = WM_USER + 0x143;

enum class ButtonKind { primary, secondary };

UINT window_dpi(HWND window);
int scaled(int value, UINT dpi);

struct Fonts {
  HFONT heading = nullptr;
  HFONT subheading = nullptr;
  HFONT body = nullptr;
  HFONT body_strong = nullptr;
  HFONT caption = nullptr;
  HFONT step = nullptr;
  HFONT step_strong = nullptr;
  HFONT badge = nullptr;

  void create(UINT dpi);
  void destroy();
};

void fill(HDC dc, const RECT &area, COLORREF value);
void fill_box(HDC dc, const RECT &area, COLORREF value,
              COLORREF border_value, int border_width = 1);
void fill_rounded(HDC dc, const RECT &area, int radius, COLORREF value,
                  COLORREF border_value);
void fill_ellipse(HDC dc, const RECT &area, COLORREF value,
                  COLORREF border_value, int border_width);
void draw_text(HDC dc, RECT area, const std::wstring &value, HFONT font,
               COLORREF value_colour, UINT format);
int text_height(HDC dc, const std::wstring &value, HFONT font, int width,
                UINT format);
void draw_check(HDC dc, const RECT &area, COLORREF value, int thickness);
void draw_cross(HDC dc, const RECT &area, COLORREF value, int thickness);
void draw_disc(HDC dc, const RECT &area, COLORREF face, COLORREF ring);
void draw_progress(HDC dc, const RECT &area, int percent, COLORREF track,
                   COLORREF value);

void register_classes(HINSTANCE instance);
HWND create_button(HWND parent, int identifier, const wchar_t *text,
                   ButtonKind kind, HFONT font, UINT dpi);
HWND create_check(HWND parent, int identifier, const wchar_t *text,
                  const wchar_t *description, HFONT font,
                  HFONT description_font, UINT dpi);
bool is_checked(HWND control);
void set_checked(HWND control, bool value);
void set_control_dpi(HWND control, UINT dpi);

}  // namespace ui
