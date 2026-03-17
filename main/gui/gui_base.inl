// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

static int smooth_move(int dst, int src, int step) {
  static constexpr const uint8_t mul_table[] = {
    22, 31, 38, 44, 50, 54, 59, 63, 67, 71, 74, 77, 81, 84, 87, 90, 92, 95, 98, 100, 103, 105, 108, 110, 112, 114, 117, 119, 121, 123, 125, 127, 129, 131, 133, 135, 137, 138, 140, 142, 144, 146, 147, 149, 151, 152, 154, 156, 157, 159, 161, 162, 164, 165, 167, 168, 170, 171, 173, 174, 176, 177, 179, 180, 181, 183, 184, 186, 187, 188, 190, 191, 192, 194, 195, 196, 198, 199, 200, 201, 203, 204, 205, 206, 208, 209, 210, 211, 212, 214, 215, 216, 217, 218, 220, 221, 222, 223, 224, 225, 226, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255
  };
  if (dst != src) {
    if (step >= (int)(sizeof(mul_table)/sizeof(mul_table[0]))) {
      src = dst;
    } else {
      int diff = abs(dst - src);
      int mul = mul_table[step];
      // diff = (2047 + diff * mul) >> 11;
      diff = (255 + diff * mul) >> 8;
      src += (dst < src) ? -(diff) : diff;
    }
  }
  return src;
/*/
  if (dst != src && step > 0)
  {
    src += ((dst - src) * step + (dst < src ? 0 : 64)) >> 6;
    // src += ((dst - src) * step + (dst < src ? 0 : 256)) >> 8;
  }
  return src;
//*/
}

struct rect_t
{
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  constexpr rect_t(void) : x{0}, y{0}, w{0}, h{0}
  {
  }
  constexpr rect_t(int x_, int y_, int w_, int h_)
      : x{(int16_t)x_}, y{(int16_t)y_}, w{(int16_t)w_}, h{(int16_t)h_}
  {
  }
  inline constexpr int top(void) const
  {
    return y;
  }
  inline constexpr int left(void) const
  {
    return x;
  }
  inline constexpr int right(void) const
  {
    return x + w;
  }
  inline constexpr int bottom(void) const
  {
    return y + h;
  }
  inline constexpr bool empty(void) const
  {
    return w <= 0 || h <= 0;
  }
  void clear(void) { x = 0; y = 0; w = 0; h = 0; }

  inline bool operator==(const rect_t &src) const
  {
    return x == src.x && y == src.y && w == src.w && h == src.h;
  }
  inline bool operator!=(const rect_t &src) const
  {
    return !operator==(src);
  }
  inline bool isContain(int32_t target_x, int32_t target_y) const
  {
    return x <= target_x && target_x < x + w && y <= target_y && target_y < y + h;
  }
  // 矩形同士が重なっているか判定する
  inline bool isIntersect(const rect_t &src) const
  {
    return (x < src.right() && src.x < right() && y < src.bottom() && src.y < bottom());
  }

  bool smooth_move(const rect_t &src, int step) {
    if (operator==(src)) return false;
    int new_h = kanplay_ns::smooth_move(src.bottom(), bottom(), step);
    int new_y = kanplay_ns::smooth_move(src.y, y, step);
    new_h -= new_y;
    h = new_h;
    y = new_y;
    int new_w = kanplay_ns::smooth_move(src.right(), right(), step);
    int new_x = kanplay_ns::smooth_move(src.x, x, step);
    new_w -= new_x;
    w = new_w;
    x = new_x;
    return true;
  }
};

// 矩形同士のAND
static rect_t rect_and(const rect_t &a, const rect_t &b)
{
  int new_x = std::max(a.x, b.x);
  int new_y = std::max(a.y, b.y);
  int new_r = std::min(a.right(), b.right());
  int new_b = std::min(a.bottom(), b.bottom());
  return {new_x, new_y, new_r - new_x, new_b - new_y};
}

// 矩形同士の OR
static rect_t rect_or(const rect_t &a, const rect_t &b)
{
  int new_x = std::min(a.x, b.x);
  int new_y = std::min(a.y, b.y);
  int new_r = std::max(a.right(), b.right());
  int new_b = std::max(a.bottom(), b.bottom());
  return {new_x, new_y, new_r - new_x, new_b - new_y};
}

struct draw_param_t
{
  static constexpr int16_t w_d2 = disp_width / 2;
  static constexpr int16_t w_d3 = disp_width / 3;
  static constexpr int16_t w_d4 = disp_width / 4;
  static constexpr int16_t w_d5 = disp_width / 5;
  static constexpr int16_t w_d6 = disp_width / 6;
  static constexpr int16_t h_p = main_area_height / 4;
  static constexpr int16_t y_0 = header_height;
  static constexpr int16_t y_1 = y_0 + h_p;
  static constexpr int16_t y_2 = y_1 + h_p;
  static constexpr int16_t y_3 = y_2 + h_p;
  static constexpr int16_t h_s = sub_btns_height / 2;
  static constexpr int16_t y_s0 = y_0 + main_area_height;
  static constexpr int16_t y_s1 = y_s0 + h_s;
  static constexpr int16_t h_m = main_btns_height / 3;
  static constexpr int16_t y_b0 = disp_height - main_btns_height;
  static constexpr int16_t y_b1 = y_b0 + h_m;
  static constexpr int16_t y_b2 = y_b1 + h_m;

  // 部分描画のためのクリップ矩形群
  // （DMAバッファを小さく抑えつつ描画負荷を軽くするため、メニュー時と通常時で分割テーブルを分ける）
  // メニューモード時
  static constexpr rect_t clip_rect_menu[] = {
// ヘッダ部
    {        0,   0, w_d4, y_0 }, // 60 x 24 = 1440
    {     w_d4,   0, w_d4, y_0 },
    { 2 * w_d4,   0, w_d4, y_0 },
    { 3 * w_d4,   0, w_d4, y_0 },
// メニュー部
    {    0, y_0 + 0 * 7, disp_width, 7 }, // 240 x 7 = 1680
    {    0, y_0 + 1 * 7, disp_width, 7 },
    {    0, y_0 + 2 * 7, disp_width, 7 },
    {    0, y_0 + 3 * 7, disp_width, 7 },

    {    0, y_0 + 4 * 7, disp_width, 7 },
    {    0, y_0 + 5 * 7, disp_width, 7 },
    {    0, y_0 + 6 * 7, disp_width, 7 },
    {    0, y_0 + 7 * 7, disp_width, 7 },

    {    0, y_0 + 8 * 7, disp_width, 7 },
    {    0, y_0 + 9 * 7, disp_width, 7 },
    {    0, y_0 +10 * 7, disp_width, 7 },
    {    0, y_0 +11 * 7, disp_width, 7 },

    {    0, y_0 +12 * 7, disp_width, 7 },
    {    0, y_0 +13 * 7, disp_width, 7 },
    {    0, y_0 +14 * 7, disp_width, 7 },
    {    0, y_0 +15 * 7, disp_width, 7 },

    {    0, y_0 +16 * 7, disp_width, 7 },
    {    0, y_0 +17 * 7, disp_width, 7 },
    {    0, y_0 +18 * 7, disp_width, 7 },
    {    0, y_0 +19 * 7, disp_width, 7 },

    {    0, y_0 +20 * 7, disp_width, 7 },
    {    0, y_0 +21 * 7, disp_width, 7 },
    {    0, y_0 +22 * 7, disp_width, 7 },
    {    0, y_0 +23 * 7, disp_width, 7 },

// サブボタン部
    { 0 * w_d4, y_s0, w_d4, h_s }, // 60 x 16 = 960
    { 1 * w_d4, y_s0, w_d4, h_s },
    { 2 * w_d4, y_s0, w_d4, h_s },
    { 3 * w_d4, y_s0, w_d4, h_s },
    { 0 * w_d4, y_s1, w_d4, h_s },
    { 1 * w_d4, y_s1, w_d4, h_s },
    { 2 * w_d4, y_s1, w_d4, h_s },
    { 3 * w_d4, y_s1, w_d4, h_s },
// メインボタン部
    { 0       , y_b0, w_d5, h_m }, // 48 x 32 = 1536
    { 0       , y_b1, w_d5, h_m },
    { 0       , y_b2, w_d5, h_m },
    { 1 * w_d5, y_b0, w_d5, h_m },
    { 1 * w_d5, y_b1, w_d5, h_m },
    { 1 * w_d5, y_b2, w_d5, h_m },
    { 2 * w_d5, y_b0, w_d5, h_m },
    { 2 * w_d5, y_b1, w_d5, h_m },
    { 2 * w_d5, y_b2, w_d5, h_m },
    { 3 * w_d5, y_b0, w_d5, h_m },
    { 3 * w_d5, y_b1, w_d5, h_m },
    { 3 * w_d5, y_b2, w_d5, h_m },
    { 4 * w_d5, y_b0, w_d5, h_m },
    { 4 * w_d5, y_b1, w_d5, h_m },
    { 4 * w_d5, y_b2, w_d5, h_m },
  };

  // 通常モード時
  static constexpr rect_t clip_rect_normal[] = {
// ヘッダ部
    {        0,   0, w_d4, y_0 }, // 60 x 24 = 1440
    {     w_d4,   0, w_d4, y_0 },
    { 2 * w_d4,   0, w_d4, y_0 },
    { 3 * w_d4,   0, w_d4, y_0 },
// 6個のパート
    {        0, y_0, w_d6, h_p }, // 40 x 42 = 1680
    {        0, y_1, w_d6, h_p },
    {        0, y_2, w_d6, h_p },
    {        0, y_3, w_d6, h_p },
    { 1 * w_d6, y_0, w_d6, h_p },
    { 1 * w_d6, y_1, w_d6, h_p },
    { 1 * w_d6, y_2, w_d6, h_p },
    { 1 * w_d6, y_3, w_d6, h_p },
    { 2 * w_d6, y_0, w_d6, h_p },
    { 2 * w_d6, y_1, w_d6, h_p },
    { 2 * w_d6, y_2, w_d6, h_p },
    { 2 * w_d6, y_3, w_d6, h_p },
    { 3 * w_d6, y_0, w_d6, h_p },
    { 3 * w_d6, y_1, w_d6, h_p },
    { 3 * w_d6, y_2, w_d6, h_p },
    { 3 * w_d6, y_3, w_d6, h_p },
    { 4 * w_d6, y_0, w_d6, h_p },
    { 4 * w_d6, y_1, w_d6, h_p },
    { 4 * w_d6, y_2, w_d6, h_p },
    { 4 * w_d6, y_3, w_d6, h_p },
    { 5 * w_d6, y_0, w_d6, h_p },
    { 5 * w_d6, y_1, w_d6, h_p },
    { 5 * w_d6, y_2, w_d6, h_p },
    { 5 * w_d6, y_3, w_d6, h_p },

// サブボタン部
    { 0 * w_d4, y_s0, w_d4, h_s }, // 60 x 16 = 960
    { 1 * w_d4, y_s0, w_d4, h_s },
    { 2 * w_d4, y_s0, w_d4, h_s },
    { 3 * w_d4, y_s0, w_d4, h_s },
    { 0 * w_d4, y_s1, w_d4, h_s },
    { 1 * w_d4, y_s1, w_d4, h_s },
    { 2 * w_d4, y_s1, w_d4, h_s },
    { 3 * w_d4, y_s1, w_d4, h_s },
// メインボタン部
    { 0       , y_b0, w_d5, h_m }, // 48 x 32 = 1536
    { 0       , y_b1, w_d5, h_m },
    { 0       , y_b2, w_d5, h_m },
    { 1 * w_d5, y_b0, w_d5, h_m },
    { 1 * w_d5, y_b1, w_d5, h_m },
    { 1 * w_d5, y_b2, w_d5, h_m },
    { 2 * w_d5, y_b0, w_d5, h_m },
    { 2 * w_d5, y_b1, w_d5, h_m },
    { 2 * w_d5, y_b2, w_d5, h_m },
    { 3 * w_d5, y_b0, w_d5, h_m },
    { 3 * w_d5, y_b1, w_d5, h_m },
    { 3 * w_d5, y_b2, w_d5, h_m },
    { 4 * w_d5, y_b0, w_d5, h_m },
    { 4 * w_d5, y_b1, w_d5, h_m },
    { 4 * w_d5, y_b2, w_d5, h_m },
  };
  // static constexpr const int max_clip_rect = 33;
  static constexpr const int max_clip_rect_normal = sizeof(clip_rect_normal) / sizeof(clip_rect_normal[0]);
  static constexpr const int max_clip_rect_menu = sizeof(clip_rect_menu) / sizeof(clip_rect_menu[0]);
  static constexpr const int max_clip_rect = std::max(max_clip_rect_normal, max_clip_rect_menu);

  rect_t invalidated_rect[max_clip_rect];
  uint32_t current_msec = 0;
  uint32_t prev_msec = 0;
  uint8_t smooth_step = 0;
  bool hasInvalidated;
  bool is_menu_mode = false;
  void resetInvalidatedRect(void)
  {
    hasInvalidated = false;
    for (auto& r : invalidated_rect) {
      r.clear();
    }
  }
  void addInvalidatedRect(const rect_t &rect)
  {
    if (rect.empty()) return;
    auto &clip_rect = is_menu_mode ? clip_rect_menu : clip_rect_normal;
    auto max_clip_rect = is_menu_mode ? max_clip_rect_menu : max_clip_rect_normal;
    for (int i = 0; i < max_clip_rect; ++i) {
      auto clipped_rect = rect_and(clip_rect[i], rect);
      if (clipped_rect.empty()) continue;
      hasInvalidated = true;
      if (invalidated_rect[i].empty()) {
        invalidated_rect[i] = clipped_rect;
      }
      else
      {
        invalidated_rect[i] = rect_or(invalidated_rect[i], clipped_rect);
      }
    }
  }
};

static draw_param_t _draw_param;

static uint32_t add_color(uint32_t base_color, uint32_t table_color)
{
  uint32_t result = 0;
  for (int i = 0; i < 3; ++i) {
    int_fast16_t c = (base_color & 0xFF) + (int_fast16_t)((int8_t)table_color);
    result |= ( (c > 255) ? 255 : (c < 0) ? 0 : c ) << (i*8);
    base_color >>= 8;
    table_color >>= 8;
  }
  return result;
}

void image_dark_shift(M5Canvas *canvas, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t shift = 1)
{
  auto canvas_width = canvas->width();
  auto canvas_height = canvas->height();
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (w > canvas_width - x) {  w = canvas_width - x; }
  if (h > canvas_height - y) {  h = canvas_height - y; }
  if (w <= 0 || h <= 0) return;

  // RGB565 の各色ビットを指定ビット分シフトするためのマスク
  static constexpr const uint16_t masks[] = {
    0x7BEF, // shift 1
    0x39E7, // shift 2
    0x18E3, // shift 3
    0x0861, // shift 4
    0x0020, // shift 5
  };
  uint16_t mask = masks[shift - 1];

  auto framebuffer = (m5gfx::swap565_t*)(canvas->getBuffer());
  framebuffer += x + canvas_width * y;
  for (int j = 0; j < h; ++j) {
    auto buf = framebuffer;
    framebuffer += canvas_width;
    for (int i = 0; i < w; ++i) {
      auto raw = __builtin_bswap16(buf[0].raw);
      raw = ((raw >> shift) & mask);
      buf[0].raw = __builtin_bswap16(raw);
      ++buf;
    }
  }
}

void image_color_or(M5Canvas *canvas, int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color)
{
  auto canvas_width = canvas->width();
  auto canvas_height = canvas->height();
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (w > canvas_width - x) {  w = canvas_width - x; }
  if (h > canvas_height - y) {  h = canvas_height - y; }
  if (w <= 0 || h <= 0) return;

  color = __builtin_bswap16(color);
  auto framebuffer = (m5gfx::swap565_t*)(canvas->getBuffer());
  framebuffer += x + canvas_width * y;
  for (int j = 0; j < h; ++j) {
    auto buf = framebuffer;
    framebuffer += canvas_width;
    for (int i = 0; i < w; ++i) {
      buf[0].raw |= color;
      ++buf;
    }
  }
}

struct ui_base_t
{
  void draw(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
            int32_t offset_y, const rect_t *clip_rect);

  void update(draw_param_t *param, int offset_x, int offset_y);

  virtual bool smoothMove(int step)
  {
    return _client_rect.smooth_move(_target_rect, step);
  }

  virtual void setTargetRect(const rect_t &rect)
  {
    _target_rect = rect;
  }

  // 該当座標にコントロールが存在するか判定する
  virtual ui_base_t* searchUI(int32_t x, int32_t y)
  {
    // 移動中のコントロールは処理対象外とする
    if (_client_rect == _target_rect && _target_rect.isContain(x, y)) {
      return this;
    }
    return nullptr;
  }

  void setClientRect(const rect_t &rect)
  {
    _client_rect = rect;
  }

  const rect_t &getTargetRect(void) const
  {
    return _target_rect;
  }

  const rect_t &getClientRect(void) const
  {
    return _client_rect;
  }


  virtual void touchControl(draw_param_t *param, const m5::touch_detail_t& td)
  {
  }

  void setParent(ui_base_t *parent)
  {
    _parent = parent;
  }
protected:
  ui_base_t* _parent = nullptr;
  rect_t _client_rect;
  rect_t _target_rect;
  uint8_t _prev_update_count;

  virtual void draw_impl(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect) = 0;

  virtual void update_impl(draw_param_t *param, int offset_x, int offset_y)
  {
    if (_client_rect != _target_rect)
    {
      int px = offset_x - _client_rect.x;
      int py = offset_y - _client_rect.y;
      param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
      smoothMove(param->smooth_step);
      param->addInvalidatedRect({px + _client_rect.x, py + _client_rect.y, _client_rect.w, _client_rect.h});
    }
  }
};

void ui_base_t::draw(draw_param_t* param, M5Canvas* canvas, int32_t offset_x, 
      int32_t offset_y, const rect_t* clip_rect) {
  auto rect = getClientRect();
  if (rect.empty()) {
      return;
  }
  offset_y += rect.y;
  rect.y = offset_y;
  offset_x += rect.x;
  rect.x = offset_x;

  int32_t right  = std::min(rect.right(), clip_rect->right());
  int32_t bottom = std::min(rect.bottom(), clip_rect->bottom());
  int32_t x = std::max(rect.x, clip_rect->x);
  int32_t y = std::max(rect.y, clip_rect->y);
  rect.x = x;
  rect.y = y;
  rect.w = right - x;
  rect.h = bottom - y;
  if (rect.empty()) {
      return;
  }
  canvas->setClipRect(x, y, rect.w, rect.h);
  // int32_t dummy;
  // canvas->getClipRect(&dummy, &dummy, &wid, &hei);
  // if (wid > 0 && hei > 0) {
  draw_impl(param, canvas, offset_x, offset_y, &rect);
  // }
#if defined ( DEBUG_GUI )
  canvas->drawRect(rect.x, rect.y, rect.w, rect.h, rand());
#endif
  canvas->clearClipRect();
}

void ui_base_t::update(draw_param_t* param, int offset_x, int offset_y) {
  auto rect = getClientRect();
  offset_y += rect.y;
  offset_x += rect.x;
  update_impl(param, offset_x, offset_y);
}

struct ui_container_t : public ui_base_t {
  void addChild(ui_base_t* ui) {
    ui->setParent(this);
    _ui_child.push_back(ui);
  }
  void update_impl(draw_param_t *param, int offset_x, int offset_y) override {
    ui_base_t::update_impl(param, offset_x, offset_y);
    for (auto ui : _ui_child) {
      ui->update(param, offset_x, offset_y);
    }
  }

  ui_base_t* searchUI(int32_t x, int32_t y) override
  {
    if (this == ui_base_t::searchUI(x, y)) {
      // _ui_childを逆順に処理する
      for (auto it = _ui_child.rbegin(); it != _ui_child.rend(); ++it) {
        auto child = (*it)->searchUI(x - _client_rect.x, y - _client_rect.y);
        if (child != nullptr) {
          return child;
        }
      }
      return this;
    }
    return nullptr;
  }

  void shrink_to_fit(void) {
    _ui_child.shrink_to_fit();
  }

protected:
  std::vector<ui_base_t*> _ui_child;

  void draw_impl(draw_param_t* param, M5Canvas* canvas, int32_t offset_x, 
      int32_t offset_y, const rect_t* clip_rect) override {
    for (auto ui : _ui_child) {
      ui->draw(param, canvas, offset_x, offset_y, clip_rect);
    }
  }
};