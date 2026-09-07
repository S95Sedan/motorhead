#include "ui.hpp"

#include <windowsx.h>

#include <algorithm>

namespace ui {
namespace {

constexpr wchar_t button_class[] = L"MotorheadInstallerButton";
constexpr wchar_t check_class[] = L"MotorheadInstallerCheck";

HFONT make_font(const int decipoints, const int weight, const UINT dpi) {
  return CreateFontW(-MulDiv(decipoints, static_cast<int>(dpi), 720), 0, 0, 0,
                     weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                     L"Segoe UI");
}

HBRUSH parent_backdrop(const HWND control, const HDC dc, const UINT message) {
  const auto brush = reinterpret_cast<HBRUSH>(SendMessageW(
      GetParent(control), message, reinterpret_cast<WPARAM>(dc),
      reinterpret_cast<LPARAM>(control)));
  return brush != nullptr
             ? brush
             : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
}

void notify_parent(const HWND control) {
  const auto identifier = GetWindowLongPtrW(control, GWL_ID);
  SendMessageW(GetParent(control), WM_COMMAND,
               MAKEWPARAM(static_cast<WORD>(identifier), BN_CLICKED),
               reinterpret_cast<LPARAM>(control));
}

void track_leave(const HWND control, bool &tracking) {
  if (tracking) {
    return;
  }
  TRACKMOUSEEVENT request{sizeof(request), TME_LEAVE, control, 0U};
  if (TrackMouseEvent(&request) != FALSE) {
    tracking = true;
  }
}

bool point_inside(const HWND control, const LPARAM lparam) {
  RECT client{};
  GetClientRect(control, &client);
  const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  return PtInRect(&client, point) != FALSE;
}

struct ButtonState {
  ButtonKind kind = ButtonKind::secondary;
  bool hot = false;
  bool pressed = false;
  bool tracking = false;
  HFONT font = nullptr;
  UINT dpi = 96U;
};

struct CheckState {
  bool checked = false;
  bool hot = false;
  bool pressed = false;
  bool tracking = false;
  HFONT font = nullptr;
  HFONT description_font = nullptr;
  std::wstring description;
  int right_inset = 0;
  UINT dpi = 96U;
};

template <typename State>
State *state_of(const HWND control) {
  return reinterpret_cast<State *>(GetWindowLongPtrW(control, GWLP_USERDATA));
}

void paint_button(const HWND control, ButtonState &state) {
  PAINTSTRUCT paint{};
  const auto dc = BeginPaint(control, &paint);
  RECT client{};
  GetClientRect(control, &client);
  const auto buffer = CreateCompatibleDC(dc);
  const auto bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
  const auto previous = SelectObject(buffer, bitmap);

  FillRect(buffer, &client, parent_backdrop(control, buffer, WM_CTLCOLORBTN));

  const auto enabled = IsWindowEnabled(control) != FALSE;
  const auto focused = GetFocus() == control;
  COLORREF face = colour::card;
  COLORREF edge = colour::border;
  COLORREF label = colour::text;
  if (!enabled) {
    face = RGB(0x23, 0x26, 0x2D);
    edge = RGB(0x2B, 0x2F, 0x37);
    label = colour::text_faint;
  } else if (state.kind == ButtonKind::primary) {
    face = state.pressed ? colour::accent_press
                         : (state.hot ? colour::accent_hot : colour::accent);
    edge = face;
    label = colour::on_accent;
  } else {
    face = state.pressed ? RGB(0x31, 0x37, 0x42)
                         : (state.hot ? colour::card_hot : colour::card);
    edge = state.hot ? colour::border_hot : colour::border;
  }
  const auto radius = scaled(5, state.dpi);
  fill_rounded(buffer, client, radius, face, edge);
  if (focused && enabled) {
    RECT ring = client;
    InflateRect(&ring, -scaled(3, state.dpi), -scaled(3, state.dpi));
    fill_rounded(buffer, ring, std::max(1, radius - scaled(2, state.dpi)),
                 face,
                 state.kind == ButtonKind::primary ? colour::on_accent
                                                   : colour::accent);
  }

  std::wstring text(
      static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1U, L'\0');
  const auto copied = GetWindowTextW(control, text.data(),
                                     static_cast<int>(text.size()));
  text.resize(static_cast<std::size_t>(std::max(copied, 0)));
  draw_text(buffer, client, text, state.font, label,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
  SelectObject(buffer, previous);
  DeleteObject(bitmap);
  DeleteDC(buffer);
  EndPaint(control, &paint);
}

LRESULT CALLBACK button_proc(const HWND control, const UINT message,
                             const WPARAM wparam, const LPARAM lparam) {
  auto *state = state_of<ButtonState>(control);
  switch (message) {
  case WM_NCCREATE: {
    auto *created = new ButtonState();
    SetWindowLongPtrW(control, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(created));
    break;
  }
  case WM_NCDESTROY:
    delete state;
    SetWindowLongPtrW(control, GWLP_USERDATA, 0);
    break;
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT:
    if (state != nullptr) {
      paint_button(control, *state);
      return 0;
    }
    break;
  case WM_SETFONT:
    if (state != nullptr) {
      state->font = reinterpret_cast<HFONT>(wparam);
      if (LOWORD(lparam) != 0U) {
        InvalidateRect(control, nullptr, TRUE);
      }
      return 0;
    }
    break;
  case WM_GETFONT:
    return state != nullptr ? reinterpret_cast<LRESULT>(state->font) : 0;
  case message_set_dpi:
    if (state != nullptr) {
      state->dpi = static_cast<UINT>(wparam);
      InvalidateRect(control, nullptr, TRUE);
      return 0;
    }
    break;
  case WM_SETTEXT: {
    const auto result = DefWindowProcW(control, message, wparam, lparam);
    InvalidateRect(control, nullptr, TRUE);
    return result;
  }
  case WM_ENABLE:
    if (state != nullptr && IsWindowEnabled(control) == FALSE) {
      state->pressed = false;
    }
    InvalidateRect(control, nullptr, TRUE);
    break;
  case WM_SETFOCUS:
    // Do not clear pressed here. A normal first click sets the pressed state
    // and then gives the control focus before WM_LBUTTONUP arrives.
    InvalidateRect(control, nullptr, TRUE);
    break;
  case WM_KILLFOCUS:
  case WM_CANCELMODE:
    if (state != nullptr) {
      state->pressed = false;
    }
    if (GetCapture() == control) {
      ReleaseCapture();
    }
    InvalidateRect(control, nullptr, TRUE);
    break;
  case WM_CAPTURECHANGED:
    if (state != nullptr && reinterpret_cast<HWND>(lparam) != control) {
      state->pressed = false;
      InvalidateRect(control, nullptr, TRUE);
    }
    break;
  case WM_MOUSEMOVE:
    if (state != nullptr && !state->hot) {
      state->hot = true;
      track_leave(control, state->tracking);
      InvalidateRect(control, nullptr, TRUE);
    }
    return 0;
  case WM_MOUSELEAVE:
    if (state != nullptr) {
      state->hot = false;
      state->tracking = false;
      state->pressed = false;
      InvalidateRect(control, nullptr, TRUE);
    }
    return 0;
  case WM_LBUTTONDOWN:
    if (state != nullptr && IsWindowEnabled(control) != FALSE) {
      state->pressed = true;
      SetCapture(control);
      SetFocus(control);
      InvalidateRect(control, nullptr, TRUE);
    }
    return 0;
  case WM_LBUTTONUP:
    if (state != nullptr && state->pressed) {
      state->pressed = false;
      ReleaseCapture();
      InvalidateRect(control, nullptr, TRUE);
      if (point_inside(control, lparam)) {
        notify_parent(control);
      }
    }
    return 0;
  case WM_KEYDOWN:
    if (wparam == VK_SPACE || wparam == VK_RETURN) {
      notify_parent(control);
      return 0;
    }
    break;
  case BM_CLICK:
    if (IsWindowEnabled(control) != FALSE) {
      notify_parent(control);
    }
    return 0;
  case WM_GETDLGCODE:
    return DLGC_BUTTON | (GetFocus() == control ? DLGC_DEFPUSHBUTTON
                                                : DLGC_UNDEFPUSHBUTTON);
  default:
    break;
  }
  return DefWindowProcW(control, message, wparam, lparam);
}

void paint_check(const HWND control, CheckState &state) {
  PAINTSTRUCT paint{};
  const auto dc = BeginPaint(control, &paint);
  RECT client{};
  GetClientRect(control, &client);
  const auto buffer = CreateCompatibleDC(dc);
  const auto bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
  const auto previous = SelectObject(buffer, bitmap);

  FillRect(buffer, &client,
           parent_backdrop(control, buffer, WM_CTLCOLORSTATIC));

  const auto enabled = IsWindowEnabled(control) != FALSE;
  const auto focused = GetFocus() == control;
  const auto padding = scaled(12, state.dpi);
  const auto radius = scaled(6, state.dpi);
  if (state.checked) {
    fill_rounded(buffer, client, radius,
                 state.hot && enabled ? colour::card_hot : colour::window,
                 enabled ? colour::success : colour::border);
  } else if (state.hot && enabled) {
    fill_rounded(buffer, client, radius, colour::card_hot, colour::border_hot);
  } else {
    fill_rounded(buffer, client, radius, colour::window, colour::divider);
  }
  if (focused && enabled) {
    RECT ring = client;
    InflateRect(&ring, -scaled(2, state.dpi), -scaled(2, state.dpi));
    fill_rounded(buffer, ring, radius,
                 state.hot && enabled ? colour::card_hot : colour::window,
                 state.checked ? colour::success : colour::accent);
  }

  const auto box_size = scaled(20, state.dpi);
  RECT box{client.left + padding, client.top + padding + scaled(2, state.dpi),
           client.left + padding + box_size,
           client.top + padding + scaled(2, state.dpi) + box_size};
  if (state.checked) {
    fill_box(buffer, box, enabled ? colour::success : colour::border,
             enabled ? colour::success : colour::border, 1);
    RECT mark = box;
    InflateRect(&mark, -scaled(5, state.dpi), -scaled(5, state.dpi));
    draw_check(buffer, mark, enabled ? colour::on_accent : colour::text_faint,
               std::max(2, scaled(2, state.dpi)));
  } else {
    fill_box(buffer, box, colour::window,
             state.hot && enabled ? colour::border_hot : colour::border, 1);
  }

  std::wstring text(
      static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1U, L'\0');
  const auto copied = GetWindowTextW(control, text.data(),
                                     static_cast<int>(text.size()));
  text.resize(static_cast<std::size_t>(std::max(copied, 0)));
  const auto text_left = box.right + scaled(12, state.dpi);
  const auto text_right =
      client.right - padding - scaled(state.right_inset, state.dpi);
  RECT label{text_left, client.top + padding, text_right,
             client.top + padding + scaled(22, state.dpi)};
  draw_text(buffer, label, text, state.font,
            enabled ? (state.checked ? colour::success : colour::text)
                    : colour::text_faint,
            DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  if (!state.description.empty()) {
    RECT description{text_left, label.bottom + scaled(2, state.dpi),
                     text_right, client.bottom - scaled(4, state.dpi)};
    draw_text(buffer, description, state.description, state.description_font,
              enabled ? colour::text_dim : colour::text_faint,
              DT_LEFT | DT_TOP | DT_WORDBREAK);
  }

  BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
  SelectObject(buffer, previous);
  DeleteObject(bitmap);
  DeleteDC(buffer);
  EndPaint(control, &paint);
}

void toggle_check(const HWND control, CheckState &state) {
  if (IsWindowEnabled(control) == FALSE) {
    return;
  }
  state.checked = !state.checked;
  InvalidateRect(control, nullptr, TRUE);
  notify_parent(control);
}

LRESULT CALLBACK check_proc(const HWND control, const UINT message,
                            const WPARAM wparam, const LPARAM lparam) {
  auto *state = state_of<CheckState>(control);
  switch (message) {
  case WM_NCCREATE: {
    auto *created = new CheckState();
    SetWindowLongPtrW(control, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(created));
    break;
  }
  case WM_NCDESTROY:
    delete state;
    SetWindowLongPtrW(control, GWLP_USERDATA, 0);
    break;
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT:
    if (state != nullptr) {
      paint_check(control, *state);
      return 0;
    }
    break;
  case WM_SETFONT:
    if (state != nullptr) {
      state->font = reinterpret_cast<HFONT>(wparam);
      InvalidateRect(control, nullptr, TRUE);
      return 0;
    }
    break;
  case WM_GETFONT:
    return state != nullptr ? reinterpret_cast<LRESULT>(state->font) : 0;
  case message_set_description_font:
    if (state != nullptr) {
      state->description_font = reinterpret_cast<HFONT>(wparam);
      InvalidateRect(control, nullptr, TRUE);
      return 0;
    }
    break;
  case message_set_description:
    if (state != nullptr) {
      const auto *value = reinterpret_cast<const wchar_t *>(lparam);
      state->description = value != nullptr ? value : L"";
      InvalidateRect(control, nullptr, TRUE);
      return 0;
    }
    break;
  case message_set_right_inset:
    if (state != nullptr) {
      state->right_inset = std::max(0, static_cast<int>(wparam));
      InvalidateRect(control, nullptr, TRUE);
      return 0;
    }
    break;
  case message_set_dpi:
    if (state != nullptr) {
      state->dpi = static_cast<UINT>(wparam);
      InvalidateRect(control, nullptr, TRUE);
      return 0;
    }
    break;
  case WM_SETTEXT: {
    const auto result = DefWindowProcW(control, message, wparam, lparam);
    InvalidateRect(control, nullptr, TRUE);
    return result;
  }
  case WM_ENABLE:
    if (state != nullptr && IsWindowEnabled(control) == FALSE) {
      state->pressed = false;
    }
    InvalidateRect(control, nullptr, TRUE);
    break;
  case WM_SETFOCUS:
    InvalidateRect(control, nullptr, TRUE);
    break;
  case WM_KILLFOCUS:
  case WM_CANCELMODE:
    if (state != nullptr) {
      state->pressed = false;
    }
    if (GetCapture() == control) {
      ReleaseCapture();
    }
    InvalidateRect(control, nullptr, TRUE);
    break;
  case WM_CAPTURECHANGED:
    if (state != nullptr && reinterpret_cast<HWND>(lparam) != control) {
      state->pressed = false;
      InvalidateRect(control, nullptr, TRUE);
    }
    break;
  case WM_MOUSEMOVE:
    if (state != nullptr && !state->hot) {
      state->hot = true;
      track_leave(control, state->tracking);
      InvalidateRect(control, nullptr, TRUE);
    }
    return 0;
  case WM_MOUSELEAVE:
    if (state != nullptr) {
      state->hot = false;
      state->tracking = false;
      state->pressed = false;
      InvalidateRect(control, nullptr, TRUE);
    }
    return 0;
  case WM_LBUTTONDOWN:
    if (state != nullptr && IsWindowEnabled(control) != FALSE) {
      state->pressed = true;
      SetCapture(control);
      SetFocus(control);
    }
    return 0;
  case WM_LBUTTONUP:
    if (state != nullptr && state->pressed) {
      state->pressed = false;
      ReleaseCapture();
      if (point_inside(control, lparam)) {
        toggle_check(control, *state);
      }
    }
    return 0;
  case WM_KEYDOWN:
    if (state != nullptr && (wparam == VK_SPACE || wparam == VK_RETURN)) {
      toggle_check(control, *state);
      return 0;
    }
    break;
  case BM_CLICK:
    if (state != nullptr) {
      toggle_check(control, *state);
    }
    return 0;
  case BM_SETCHECK:
    if (state != nullptr) {
      state->checked = wparam == BST_CHECKED;
      InvalidateRect(control, nullptr, TRUE);
    }
    return 0;
  case BM_GETCHECK:
    return state != nullptr && state->checked ? BST_CHECKED : BST_UNCHECKED;
  case WM_GETDLGCODE:
    return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON;
  default:
    break;
  }
  return DefWindowProcW(control, message, wparam, lparam);
}

}  // namespace

UINT window_dpi(const HWND window) {
  using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
  static const auto resolved = [] {
    const auto module = GetModuleHandleW(L"user32.dll");
    return module != nullptr
               ? reinterpret_cast<GetDpiForWindowFn>(reinterpret_cast<void *>(
                     GetProcAddress(module, "GetDpiForWindow")))
               : nullptr;
  }();
  if (resolved != nullptr && window != nullptr) {
    const auto value = resolved(window);
    if (value != 0U) {
      return value;
    }
  }
  const auto dc = GetDC(window);
  const auto value = dc != nullptr ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
  if (dc != nullptr) {
    ReleaseDC(window, dc);
  }
  return value > 0 ? static_cast<UINT>(value) : 96U;
}

int scaled(const int value, const UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi), 96);
}

void Fonts::create(const UINT dpi) {
  destroy();
  heading = make_font(205, FW_SEMIBOLD, dpi);
  subheading = make_font(105, FW_NORMAL, dpi);
  body = make_font(98, FW_NORMAL, dpi);
  body_strong = make_font(98, FW_SEMIBOLD, dpi);
  caption = make_font(88, FW_NORMAL, dpi);
  step = make_font(95, FW_NORMAL, dpi);
  step_strong = make_font(95, FW_SEMIBOLD, dpi);
  badge = make_font(150, FW_SEMIBOLD, dpi);
}

void Fonts::destroy() {
  for (auto *font : {&heading, &subheading, &body, &body_strong, &caption,
                     &step, &step_strong, &badge}) {
    if (*font != nullptr) {
      DeleteObject(*font);
      *font = nullptr;
    }
  }
}

void fill(const HDC dc, const RECT &area, const COLORREF value) {
  const auto brush = CreateSolidBrush(value);
  FillRect(dc, &area, brush);
  DeleteObject(brush);
}

void fill_box(const HDC dc, const RECT &area, const COLORREF value,
              const COLORREF border_value, const int border_width) {
  const auto brush = CreateSolidBrush(value);
  const auto pen = CreatePen(PS_SOLID, std::max(1, border_width), border_value);
  const auto old_brush = SelectObject(dc, brush);
  const auto old_pen = SelectObject(dc, pen);
  Rectangle(dc, area.left, area.top, area.right, area.bottom);
  SelectObject(dc, old_brush);
  SelectObject(dc, old_pen);
  DeleteObject(brush);
  DeleteObject(pen);
}

void fill_rounded(const HDC dc, const RECT &area, const int radius,
                  const COLORREF value, const COLORREF border_value) {
  const auto brush = CreateSolidBrush(value);
  const auto pen = CreatePen(PS_SOLID, 1, border_value);
  const auto old_brush = SelectObject(dc, brush);
  const auto old_pen = SelectObject(dc, pen);
  RoundRect(dc, area.left, area.top, area.right, area.bottom, radius * 2,
            radius * 2);
  SelectObject(dc, old_brush);
  SelectObject(dc, old_pen);
  DeleteObject(brush);
  DeleteObject(pen);
}

void fill_ellipse(const HDC dc, const RECT &area, const COLORREF value,
                  const COLORREF border_value, const int border_width) {
  const auto brush = CreateSolidBrush(value);
  const auto pen = CreatePen(PS_SOLID, std::max(1, border_width),
                             border_value);
  const auto old_brush = SelectObject(dc, brush);
  const auto old_pen = SelectObject(dc, pen);
  Ellipse(dc, area.left, area.top, area.right, area.bottom);
  SelectObject(dc, old_brush);
  SelectObject(dc, old_pen);
  DeleteObject(brush);
  DeleteObject(pen);
}

void draw_text(const HDC dc, RECT area, const std::wstring &value,
               const HFONT font, const COLORREF value_colour,
               const UINT format) {
  if (value.empty()) {
    return;
  }
  const auto old_font = SelectObject(dc, font);
  const auto old_mode = SetBkMode(dc, TRANSPARENT);
  const auto old_colour = SetTextColor(dc, value_colour);
  DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &area, format);
  SetTextColor(dc, old_colour);
  SetBkMode(dc, old_mode);
  SelectObject(dc, old_font);
}

int text_height(const HDC dc, const std::wstring &value, const HFONT font,
                const int width, const UINT format) {
  if (value.empty()) {
    return 0;
  }
  RECT area{0, 0, width, 0};
  const auto old_font = SelectObject(dc, font);
  DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &area,
            format | DT_CALCRECT);
  SelectObject(dc, old_font);
  return area.bottom - area.top;
}

void draw_check(const HDC dc, const RECT &area, const COLORREF value,
                const int thickness) {
  const LOGBRUSH brush{BS_SOLID, value, 0};
  const auto pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND |
                                    PS_JOIN_ROUND,
                                static_cast<DWORD>(std::max(1, thickness)),
                                &brush, 0U, nullptr);
  const auto old_pen = SelectObject(dc, pen);
  const auto width = area.right - area.left;
  const auto height = area.bottom - area.top;
  const POINT points[3]{
      {area.left, area.top + height / 2},
      {area.left + width * 2 / 5, area.bottom - height / 6},
      {area.right, area.top + height / 6}};
  Polyline(dc, points, 3);
  SelectObject(dc, old_pen);
  DeleteObject(pen);
}

void draw_cross(const HDC dc, const RECT &area, const COLORREF value,
                const int thickness) {
  const LOGBRUSH brush{BS_SOLID, value, 0};
  const auto pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND,
                                static_cast<DWORD>(std::max(1, thickness)),
                                &brush, 0U, nullptr);
  const auto old_pen = SelectObject(dc, pen);
  MoveToEx(dc, area.left, area.top, nullptr);
  LineTo(dc, area.right, area.bottom);
  MoveToEx(dc, area.right, area.top, nullptr);
  LineTo(dc, area.left, area.bottom);
  SelectObject(dc, old_pen);
  DeleteObject(pen);
}

void draw_disc(const HDC dc, const RECT &area, const COLORREF face,
               const COLORREF ring) {
  fill_ellipse(dc, area, face, ring, 1);
  const auto width = area.right - area.left;
  RECT inner = area;
  InflateRect(&inner, -width / 4, -(area.bottom - area.top) / 4);
  fill_ellipse(dc, inner, face, ring, 1);
  RECT hub = area;
  InflateRect(&hub, -width * 2 / 5, -(area.bottom - area.top) * 2 / 5);
  fill_ellipse(dc, hub, ring, ring, 1);
}

void draw_progress(const HDC dc, const RECT &area, const int percent,
                   const COLORREF track, const COLORREF value) {
  const auto height = static_cast<int>(area.bottom - area.top);
  const auto radius = std::max(1, height / 2);
  fill_rounded(dc, area, radius, track, track);
  const auto clamped = std::clamp(percent, 0, 100);
  if (clamped <= 0) {
    return;
  }
  const auto width = static_cast<int>(area.right - area.left);
  RECT filled = area;
  filled.right = area.left + std::max(height, width * clamped / 100);
  filled.right = std::min(filled.right, area.right);
  fill_rounded(dc, filled, radius, value, value);
}

void register_classes(const HINSTANCE instance) {
  WNDCLASSEXW description{};
  description.cbSize = sizeof(description);
  description.hInstance = instance;
  description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  description.style = CS_HREDRAW | CS_VREDRAW;
  description.lpfnWndProc = &button_proc;
  description.lpszClassName = button_class;
  RegisterClassExW(&description);
  description.lpfnWndProc = &check_proc;
  description.lpszClassName = check_class;
  RegisterClassExW(&description);
}

HWND create_button(const HWND parent, const int identifier,
                   const wchar_t *text, const ButtonKind kind,
                   const HFONT font, const UINT dpi) {
  const auto control = CreateWindowExW(
      0U, button_class, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0,
      parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
      reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
      nullptr);
  if (control != nullptr) {
    auto *state = state_of<ButtonState>(control);
    if (state != nullptr) {
      state->kind = kind;
      InvalidateRect(control, nullptr, TRUE);
    }
    SendMessageW(control, message_set_dpi, static_cast<WPARAM>(dpi), 0);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
  }
  return control;
}

HWND create_check(const HWND parent, const int identifier,
                  const wchar_t *text, const wchar_t *description,
                  const HFONT font, const HFONT description_font,
                  const UINT dpi) {
  const auto control = CreateWindowExW(
      0U, check_class, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0,
      parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
      reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
      nullptr);
  if (control != nullptr) {
    SendMessageW(control, message_set_dpi, static_cast<WPARAM>(dpi), 0);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    SendMessageW(control, message_set_description_font,
                 reinterpret_cast<WPARAM>(description_font), 0);
    SendMessageW(control, message_set_description, 0U,
                 reinterpret_cast<LPARAM>(description));
  }
  return control;
}

bool is_checked(const HWND control) {
  return SendMessageW(control, BM_GETCHECK, 0U, 0) == BST_CHECKED;
}

void set_checked(const HWND control, const bool value) {
  SendMessageW(control, BM_SETCHECK,
               value ? static_cast<WPARAM>(BST_CHECKED)
                     : static_cast<WPARAM>(BST_UNCHECKED),
               0);
}

void set_control_dpi(const HWND control, const UINT dpi) {
  SendMessageW(control, message_set_dpi, static_cast<WPARAM>(dpi), 0);
}

}  // namespace ui
