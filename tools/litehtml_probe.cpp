/* litehtml_probe.cpp — 兜底 webview 选型探针(litehtml + cairo)
 *
 * 定位: 项目转向"webview 优先"后, macOS/Windows 用系统 webview(WKWebView/
 * WebView2), 而 Linux 禁用 WebKitGTK —— 兜底需要嵌入一个**现成的轻量引擎**,
 * 而不是 C99 自研。选型 litehtml(BSD, ~1.6MB, C++ HTML/CSS 排版, 渲染后端
 * 由宿主提供): 本探针验证"解析 → 排版 → 绘制 → 命中"全链路真实可跑。
 *
 * 断言(7): 背景色精确命中 / 圆角裁剪三段(出界白 + 内侧蓝 + AA过渡) / 背景白 /
 * 字形绘制 / 命中测试(on_mouse_over + get_over_element)。
 *
 * 探针踩的坑(集成时的关键约束):
 *  - **标准 CSS 不认免单位长度**: `width:80` 是非法值直接被忽略 —— 而 hn 编码
 *    恰恰全用免单位写法! 集成时必须先过 HNCSSNormalizer 同款的归一化,
 *    否则同一份页面在系统 webview 与兜底引擎下长得完全不同。
 *  - 圆角裁剪是**绘制端**职责: background_layer.border_radius 挂在 layer 上,
 *    宿主 draw_solid_fill 自己画圆角路径(不是 set_clip)。
 *  - 命中测试无 element_from_point: on_mouse_over 驱动 + get_over_element 取。
 *
 * 用法: c++ -O1 -std=c++17 -I/opt/homebrew/include \
 *           $(pkg-config --cflags cairo) litehtml_probe.cpp \
 *           -L/opt/homebrew/lib -llitehtml $(pkg-config --libs cairo) -lm -lc++
 */
#include <litehtml/litehtml.h>
#include <cairo.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <map>
#include <string>

static int fails = 0;
static void ck(int c, const char* l) { printf("%s %s\n", c ? "  v" : "  X", l); if (!c) fails++; }

class cairo_container : public litehtml::document_container {
public:
    cairo_t* cr = nullptr;
    std::map<litehtml::uint_ptr, double> sizes;

    litehtml::uint_ptr create_font(const litehtml::font_description& d, const litehtml::document*, litehtml::font_metrics* fm) override {
        double sz = d.size > 0 ? d.size : 16;
        cairo_font_slant_t sl = (d.style == litehtml::font_style_italic) ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL;
        cairo_font_weight_t wt = (d.weight >= 600) ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL;
        cairo_select_font_face(cr, d.family.c_str(), sl, wt);
        cairo_set_font_size(cr, sz);
        cairo_font_extents_t e; cairo_font_extents(cr, &e);
        if (fm) {
            fm->height = (int)e.height; fm->ascent = (int)e.ascent;
            fm->descent = (int)e.descent; fm->x_height = (int)(e.height * 0.5f);
        }
        litehtml::uint_ptr h = (litehtml::uint_ptr)(size_t)(sz * 1000 + d.weight);
        sizes[h] = sz;
        return h;
    }
    void delete_font(litehtml::uint_ptr) override {}
    litehtml::pixel_t text_width(const char* t, litehtml::uint_ptr hFont) override {
        cairo_set_font_size(cr, sizes.count(hFont) ? sizes[hFont] : 16);
        cairo_text_extents_t e; cairo_text_extents(cr, t, &e);
        return (litehtml::pixel_t)e.x_advance;
    }
    void draw_text(litehtml::uint_ptr, const char* t, litehtml::uint_ptr hFont, litehtml::web_color c, const litehtml::position& pos) override {
        cairo_set_font_size(cr, sizes.count(hFont) ? sizes[hFont] : 16);
        cairo_set_source_rgba(cr, c.red/255.0, c.green/255.0, c.blue/255.0, c.alpha/255.0);
        cairo_move_to(cr, pos.x, pos.y + pos.height - 3);
        cairo_show_text(cr, t);
    }
    litehtml::pixel_t pt_to_px(float pt) const override { return (litehtml::pixel_t)(pt * 96.0f / 72.0f); }
    litehtml::pixel_t get_default_font_size() const override { return 16; }
    const char* get_default_font_name() const override { return "Helvetica"; }
    void draw_list_marker(litehtml::uint_ptr, const litehtml::list_marker&) override {}
    void load_image(const char*, const char*, bool) override {}
    void get_image_size(const char*, const char*, litehtml::size& sz) override { sz.width = 0; sz.height = 0; }
    void draw_image(litehtml::uint_ptr, const litehtml::background_layer&, const std::string&, const std::string&) override {}
    void rounded_path(const litehtml::position& b, const litehtml::border_radiuses& r) {
        cairo_new_path(cr);
        cairo_move_to(cr, b.x + r.top_left_x, b.y);
        cairo_line_to(cr, b.x + b.width - r.top_right_x, b.y);
        cairo_arc(cr, b.x + b.width - r.top_right_x, b.y + r.top_right_y, r.top_right_x, -M_PI/2, 0);
        cairo_line_to(cr, b.x + b.width, b.y + b.height - r.bottom_right_x);
        cairo_arc(cr, b.x + b.width - r.bottom_right_x, b.y + b.height - r.bottom_right_y, r.bottom_right_x, 0, M_PI/2);
        cairo_line_to(cr, b.x + r.bottom_left_x, b.y + b.height);
        cairo_arc(cr, b.x + r.bottom_left_x, b.y + b.height - r.bottom_left_y, r.bottom_left_x, M_PI/2, M_PI);
        cairo_line_to(cr, b.x, b.y + r.top_left_y);
        cairo_arc(cr, b.x + r.top_left_x, b.y + r.top_left_y, r.top_left_x, M_PI, 1.5*M_PI);
        cairo_close_path(cr);
    }
    void draw_solid_fill(litehtml::uint_ptr, const litehtml::background_layer& l, const litehtml::web_color& c) override {
        cairo_set_source_rgba(cr, c.red/255.0, c.green/255.0, c.blue/255.0, c.alpha/255.0);
        const litehtml::border_radiuses& r = l.border_radius;
        if (r.top_left_x > 0 || r.top_right_x > 0 || r.bottom_right_x > 0 || r.bottom_left_x > 0) {
            /* 圆角是绘制端职责: border_radius 挂在 layer 上(0.10 的约定) */
            rounded_path(l.border_box, r);
            cairo_fill(cr);
        } else {
            cairo_rectangle(cr, l.border_box.x, l.border_box.y, l.border_box.width, l.border_box.height);
            cairo_fill(cr);
        }
    }
    void draw_linear_gradient(litehtml::uint_ptr, const litehtml::background_layer&, const litehtml::background_layer::linear_gradient&) override {}
    void draw_radial_gradient(litehtml::uint_ptr, const litehtml::background_layer&, const litehtml::background_layer::radial_gradient&) override {}
    void draw_conic_gradient(litehtml::uint_ptr, const litehtml::background_layer&, const litehtml::background_layer::conic_gradient&) override {}
    void draw_borders(litehtml::uint_ptr, const litehtml::borders& b, const litehtml::position& p, bool) override {
        if (b.top.width <= 0 || b.top.color.alpha == 0) return;
        cairo_set_source_rgba(cr, b.top.color.red/255.0, b.top.color.green/255.0, b.top.color.blue/255.0, b.top.color.alpha/255.0);
        cairo_set_line_width(cr, b.top.width);
        cairo_rectangle(cr, p.x, p.y, p.width, p.height);
        cairo_stroke(cr);
    }
    void set_caption(const char*) override {}
    void set_base_url(const char*) override {}
    void link(const std::shared_ptr<litehtml::document>&, const litehtml::element::ptr&) override {}
    void on_anchor_click(const char*, const litehtml::element::ptr&) override {}
    void on_mouse_event(const litehtml::element::ptr&, litehtml::mouse_event) override {}
    void set_cursor(const char*) override {}
    void transform_text(litehtml::string&, litehtml::text_transform) override {}
    void import_css(litehtml::string&, const litehtml::string&, litehtml::string&) override {}
    void set_clip(const litehtml::position& p, const litehtml::border_radiuses& r) override {
        cairo_reset_clip(cr);
        double tl = r.top_left_x, tr = r.top_right_x, br = r.bottom_right_x, bl = r.bottom_left_x;
        if (tl > 0 || tr > 0 || br > 0 || bl > 0) {
            double x = p.x, y = p.y, w = p.width, h = p.height;
            cairo_new_path(cr);
            cairo_move_to(cr, x + tl, y);
            cairo_line_to(cr, x + w - tr, y);  cairo_arc(cr, x + w - tr, y + tr, tr, -M_PI/2, 0);
            cairo_line_to(cr, x + w, y + h - br); cairo_arc(cr, x + w - br, y + h - br, br, 0, M_PI/2);
            cairo_line_to(cr, x + bl, y + h);  cairo_arc(cr, x + bl, y + h - bl, bl, M_PI/2, M_PI);
            cairo_line_to(cr, x, y + tl);      cairo_arc(cr, x + tl, y + tl, tl, M_PI, 1.5*M_PI);
            cairo_close_path(cr);
            cairo_clip(cr);
        } else {
            cairo_rectangle(cr, p.x, p.y, p.width, p.height);
            cairo_clip(cr);
        }
    }
    void del_clip() override { cairo_reset_clip(cr); }
    void get_viewport(litehtml::position& p) const override { p.x = 0; p.y = 0; p.width = 200; p.height = 200; }
    litehtml::element::ptr create_element(const char*, const litehtml::string_map&, const std::shared_ptr<litehtml::document>&) override { return nullptr; }
    void get_media_features(litehtml::media_features& m) const override {
        m.type = litehtml::media_type_screen; m.width = 200; m.height = 200;
        m.device_width = 200; m.device_height = 200; m.color = 8; m.color_index = 256;
        m.monochrome = 0; m.resolution = 96;
    }
    void get_language(litehtml::string& l, litehtml::string& c) const override { l = "zh"; c = "zh-CN"; }
};

static cairo_surface_t* S; static cairo_t* CR;
static unsigned px(cairo_surface_t* s, int x, int y) {
    unsigned char* d = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    unsigned char* p = d + y * stride + x * 4;
    return ((unsigned)p[2] << 16) | ((unsigned)p[1] << 8) | p[0];   /* BGRA → RGB */
}

int main() {
    const int W = 200, H = 200;
    S = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    CR = cairo_create(S);
    cairo_set_source_rgba(CR, 1, 1, 1, 1);
    cairo_paint(CR);

    cairo_container cont;
    cont.cr = CR;

    const char* html =
        "<html><head><style>"
        "body{margin:0;background:#ffffff;font-family:Helvetica;}"
        "#card{width:80px;height:50px;background:#3366ff;border-radius:10px;}"
        "#t{color:#ff0000;font-size:16px;margin-top:10px;}"
        "</style></head><body>"
        "<div id=\"card\"></div><div id=\"t\">测试文本</div>"
        "</body></html>";

    auto doc = litehtml::document::createFromString(html, &cont);
    if (!doc) { printf("  X 文档创建失败\n"); return 1; }
    doc->render(W);
    litehtml::position clip(0, 0, W, H);
    doc->draw((litehtml::uint_ptr)(size_t)CR, 0, 0, &clip);
    cairo_surface_flush(S);

    printf("== litehtml: 排版 → 像素 ==\n");
    unsigned center = px(S, 50, 25);
    ck(center == 0x3366FF, "div 背景色 #3366FF 精确命中");
    /* 圆角 10 时, 角心在 (10,10): (11,11) 距角心 1.4 < 10 仍在圆角内(蓝是对的),
       (2,2) 距角心 11.3 > 10 才是被裁掉的区域 —— 断言点要选对几何位置。 */
    unsigned corner = px(S, 0, 0);
    ck(corner == 0xFFFFFF, "圆角被裁掉(完全出界处为白)");
    unsigned inner = px(S, 11, 11);
    ck(inner == 0x3366FF, "圆角内侧仍为背景色");
    /* AA: (2,2) 在边缘上, 应是白蓝之间的过渡(不许是纯白也不许是纯蓝) */
    unsigned aa = px(S, 2, 2);
    ck(aa != 0xFFFFFF && aa != 0x3366FF, "圆角边缘有抗锯齿过渡");
    unsigned outside = px(S, 95, 25);
    ck(outside == 0xFFFFFF, "div 之外为背景白");
    printf("       (中心=%06X 角=%06X 右外=%06X)\n", center, corner, outside);

    /* 文本: 有红色像素在下半区(“测试文本” 用 #ff0000) */
    int red = 0;
    for (int y = 50; y < 100; y++)
        for (int x = 0; x < W; x++) {
            unsigned c = px(S, x, y);
            if ((c >> 16) > 180 && (c & 0xFF) < 80) red++;
        }
    ck(red > 10, "文本以红色绘制(字形像素 > 10)");
    printf("       (红色字形像素 %d)\n", red);

    /* 命中测试: on_mouse_over 驱动悬停状态, get_over_element 取命中的元素 */
    litehtml::position::vector redraws;
    doc->on_mouse_over(50, 25, 50, 25, redraws);
    litehtml::element::const_ptr el = doc->get_over_element();
    std::string hit = el ? el->get_attr("id", "(无id)") : "(null)";
    ck(hit == "card", "命中测试: 悬停 (50,25) 命中 #card");
    printf("       (命中: %s)\n", hit.c_str());

    cairo_surface_write_to_png(S, "/tmp/liteprobe/out.png");
    printf("== %s (%d 项失败) ==\n", fails ? "失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
